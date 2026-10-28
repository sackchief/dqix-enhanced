/* Game card: ROM reads (both CPUs) and the save chip on the backup SPI bus.
 * Registers 0x040001A0-0x040001AF live in g_io and are shared by the CPUs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"

/* ---------------------------------------------------------------- ROM */
static uint8_t g_card_buf[0x4000];
static uint32_t g_card_len, g_card_pos;
static Core* g_card_owner;

/* Transfer timing, like the hardware: the command and the gap-1 delay, then
 * each data word takes 4 bus cycles, with the gap-2 delay before every
 * 0x200-byte block. A bus cycle is 5 or 8 system cycles (ROMCTRL bit 27).
 * The game reads words as they arrive, so reading early waits for them. */
static uint64_t g_card_t0;            /* when the first data word is ready */
static uint32_t g_card_xfer, g_card_gap2;
static int g_card_timing = -1;        /* DQIX_CARD_INSTANT=1 turns timing off (tests) */

static uint64_t word_ready_time(uint32_t pos) {
    uint32_t i = pos / 4;
    return g_card_t0 + (uint64_t)g_card_xfer * (4u * i + g_card_gap2 * (pos / 0x200));
}
/* Wait (emulated time passes) until the word at the read position has arrived. */
void cpu_time_sync(void);
static void wait_for_word(void) {
    if (!g_card_timing || g_card_pos >= g_card_len) return;
    cpu_time_sync();
    uint64_t ready = word_ready_time(g_card_pos), now = time_cycles();
    if (now < ready) time_spend((uint32_t)(ready - now));
}

/* ---------------------------------------------------------------- what is being loaded
 * Which scene the game is in, from the files it reads off the card: the map
 * (data/map/<NAME>.*) and a movie while one plays (data/movie/<NAME>). The
 * frontend uses it for scenes that need their own widescreen treatment. The
 * names come from the ROM's own file table (nothing kept here). */
char g_scene_map[32], g_scene_movie[32];
uint64_t g_scene_movie_frame;                 /* when the movie last read data */
typedef struct { uint32_t start, end; uint16_t kind; char name[26]; } SceneFile;   /* kind: 1 map, 2 movie */
static SceneFile* g_sfiles; static int g_nsfiles = -1;
static int sfile_cmp(const void* a, const void* b) { uint32_t x = ((const SceneFile*)a)->start, y = ((const SceneFile*)b)->start; return x < y ? -1 : x > y; }
static void scene_walk(uint32_t fnt, uint32_t fat, uint16_t dir, const char* path, int depth) {
    if (depth > 8 || fnt + (dir & 0xFFF) * 8u + 8 > g_rom_size) return;
    uint32_t sub; uint16_t first; memcpy(&sub, g_rom + fnt + (dir & 0xFFF) * 8, 4); memcpy(&first, g_rom + fnt + (dir & 0xFFF) * 8 + 4, 2);
    uint32_t p = fnt + sub; uint16_t fid = first;
    while (p < g_rom_size) {
        uint8_t b = g_rom[p++]; if (!b) break;
        int l = b & 0x7F; if (p + (uint32_t)l + 2 > g_rom_size) break;
        char name[128]; memcpy(name, g_rom + p, (size_t)l); name[l] = 0; p += (uint32_t)l;
        char full[256]; snprintf(full, sizeof full, "%s%s", path, name);
        if (b & 0x80) {
            uint16_t sid; memcpy(&sid, g_rom + p, 2); p += 2;
            char np[256]; snprintf(np, sizeof np, "%s/", full);
            scene_walk(fnt, fat, sid, np, depth + 1);
            continue;
        }
        /* places: data/map/<letter><digits>... (not its lists: ats_*, maplist, mapname); movies: *.mods */
        int kind = !strncmp(full, "data/map/", 9) && full[9] >= 'A' && full[9] <= 'Z' && full[10] >= '0' && full[10] <= '9' ? 1
                 : !strncmp(full, "data/movie/", 11) && strstr(full, ".mods") ? 2 : 0;
        if (kind && fat + fid * 8u + 8 <= g_rom_size) {
            SceneFile f; memcpy(&f.start, g_rom + fat + fid * 8, 4); memcpy(&f.end, g_rom + fat + fid * 8 + 4, 4); f.kind = (uint16_t)kind;
            const char* base = full + (kind == 1 ? 9 : 11);
            snprintf(f.name, sizeof f.name, "%s", base);
            if (kind == 1) { char* dot = strchr(f.name, '.'); if (dot) *dot = 0; }   /* a map's files share its name */
            SceneFile* n = realloc(g_sfiles, sizeof(SceneFile) * (size_t)(g_nsfiles + 1));
            if (n) { g_sfiles = n; g_sfiles[g_nsfiles++] = f; }
        }
        fid++;
    }
}
static void scene_note_read(uint32_t addr) {
    if (g_nsfiles < 0) {
        g_nsfiles = 0;
        if (g_rom_size < 0x50) return;
        uint32_t fnt, fat; memcpy(&fnt, g_rom + 0x40, 4); memcpy(&fat, g_rom + 0x48, 4);
        scene_walk(fnt, fat, 0xF000, "", 0);
        if (g_nsfiles) qsort(g_sfiles, (size_t)g_nsfiles, sizeof(SceneFile), sfile_cmp);
    }
    int lo = 0, hi = g_nsfiles - 1, at = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_sfiles[m].start <= addr) { at = m; lo = m + 1; } else hi = m - 1; }
    if (at < 0 || addr >= g_sfiles[at].end) return;
    const SceneFile* f = &g_sfiles[at];
    static int lg = -1; if (lg < 0) lg = getenv("DQIX_SCENELOG") != NULL;
    if (f->kind == 1 && strcmp(g_scene_map, f->name)) {
        snprintf(g_scene_map, sizeof g_scene_map, "%s", f->name);
        if (lg) fprintf(stderr, "scene f%llu: map %s\n", (unsigned long long)g_frame, g_scene_map);
    } else if (f->kind == 2) {
        if (strcmp(g_scene_movie, f->name) && lg) fprintf(stderr, "scene f%llu: movie %s\n", (unsigned long long)g_frame, f->name);
        snprintf(g_scene_movie, sizeof g_scene_movie, "%s", f->name);
        g_scene_movie_frame = g_frame;
    }
}

static void rom_start(Core* k, uint32_t ctl) {
    const uint8_t* cmd = &g_io[0x1A8];
    uint32_t bs = (ctl >> 24) & 7;
    uint32_t len = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    g_card_len = len; g_card_pos = 0; g_card_owner = k;
    if (g_card_timing < 0) g_card_timing = getenv("DQIX_CARD_INSTANT") == NULL;
    g_card_xfer = (ctl & (1u << 27)) ? 8 : 5;
    g_card_gap2 = (ctl >> 16) & 0x3F;
    uint32_t cmddelay = 8 + (ctl & 0x1FFF) + (len ? g_card_gap2 : 0);
    g_card_t0 = time_cycles() + (uint64_t)g_card_xfer * (cmddelay + 4);
    if (cmd[0] == 0xB7) {
        uint32_t addr = ((uint32_t)cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
        if (len) scene_note_read(addr);
        for (uint32_t i = 0; i < len; i++) {
            uint32_t a = addr + i;
            if (a < 0x8000) a = 0x8000 + (a & 0x1FF);
            g_card_buf[i] = g_rom[a % g_rom_size];
        }
        int dmas = 0;
        for (int ch = 0; ch < 4; ch++) if (k->dma[ch].pending == DMA_CARD && (k->dma[ch].ctl & 0x80000000u)) {
            dmas |= 1 << ch;
            LOG_DEBUG("card: dma%d ctl %08x src %08x dst %08x\n", ch, k->dma[ch].ctl, k->dma[ch].src, k->dma[ch].dst);
        }
        LOG_DEBUG("card: %s reads %x bytes at %08x (ctl %08x, spicnt %04x, card dma %x, lr %08x)\n", k->name, len, addr, ctl, IO16(0x1A0), dmas, k->cpu->r[14]);
        { static int lg = -1; if (lg < 0) lg = getenv("DQIX_CARDLOG") != NULL;
          if (lg) { uint32_t dctl = 0; for (int ch = 0; ch < 4; ch++) if (dmas & (1 << ch)) dctl = k->dma[ch].ctl;
                    fprintf(stderr, "card f%llu %s len %x ctl %08x spicnt %04x dma %x dmactl %08x\n", (unsigned long long)g_frame, k->name, len, ctl, IO16(0x1A0), dmas, dctl); } }
    } else if (cmd[0] == 0xB8) {
        for (uint32_t i = 0; i < len; i += 4) { uint32_t id = 0x00003FC2; memcpy(&g_card_buf[i], &id, 4); }
    } else {
        memset(g_card_buf, 0xFF, len);
        LOG_WARN("card: command %02x not emulated\n", cmd[0]);
    }
    if (len == 0) { ctl &= ~0x80000000u; memcpy(&g_io[0x1A4], &ctl, 4); return; }
    ctl |= 0x00800000u;  /* data word ready (card_read reports it only once it has arrived) */
    memcpy(&g_io[0x1A4], &ctl, 4);
    /* A card DMA moves its word count per "word ready" event (usually 1, repeating):
     * keep signalling until the transfer is drained or no card DMA is armed. */
    for (;;) {
        int armed = 0;
        for (int ch = 0; ch < 4; ch++) if (k->dma[ch].pending == DMA_CARD && (k->dma[ch].ctl & 0x80000000u)) armed = 1;
        if (!armed || g_card_pos >= g_card_len) break;
        uint32_t before = g_card_pos;
        dma_on_event(k, DMA_CARD);
        if (g_card_pos == before) break;
    }
}

uint32_t card_read_data(Core* k) {
    (void)k;
    uint32_t v = 0xFFFFFFFF;
    wait_for_word();
    if (g_card_pos < g_card_len) {
        memcpy(&v, &g_card_buf[g_card_pos], 4);
        g_card_pos += 4;
        if (g_card_pos >= g_card_len) {
            uint32_t ctl = IO32(0x1A4) & ~0x80800000u;
            memcpy(&g_io[0x1A4], &ctl, 4);
            LOG_DEBUG("card: transfer done (irq %d)\n", !!(IO16(0x1A0) & 0x4000));
            if (IO16(0x1A0) & 0x4000) irq_raise(g_card_owner ? g_card_owner : k, IRQ_CARD_DONE);
        }
    }
    return v;
}

/* ---------------------------------------------------------------- save chip
 * SPI EEPROM / FLASH. Up to 64 KB it behaves as an EEPROM with 2-byte
 * addresses (what DQIX uses); larger sizes act as FLASH with 3-byte
 * addresses. The size follows an existing save file, else 64 KB. */
#define BACKUP_MAX (1u << 20)
static uint8_t g_backup[BACKUP_MAX];
static uint32_t g_backup_size = 64 * 1024;   /* DQIX: 64 KB EEPROM (2-byte addresses) */
static char g_backup_path[1024];
static int g_backup_dirty;
uint64_t g_backup_written;
static uint8_t g_bk_cmd, g_bk_status;
static uint32_t g_bk_addr, g_bk_count;

static uint8_t* g_backup_at_boot;     /* the save as it was at launch (for replays) */
static int g_backup_existed;

void backup_init(const char* path) {
    memset(g_backup, 0xFF, sizeof g_backup);
    snprintf(g_backup_path, sizeof g_backup_path, "%s", path);
    FILE* f = fopen(path, "rb");
    if (f) {
        size_t n = fread(g_backup, 1, BACKUP_MAX, f);
        fclose(f);
        if (n) { g_backup_size = (uint32_t)n; g_backup_existed = 1; }
        LOG_INFO("save: loaded %zu bytes from %s\n", n, path);
    }
    g_backup_at_boot = malloc(g_backup_size);
    memcpy(g_backup_at_boot, g_backup, g_backup_size);
}

/* For reports: the save at launch (if there was one) and as it is now. */
int backup_write_boot(FILE* f) { if (!g_backup_existed) return 0; fwrite(g_backup_at_boot, 1, g_backup_size, f); return 1; }
void backup_write_now(FILE* f) { fwrite(g_backup, 1, g_backup_size, f); }

const uint8_t* backup_image(uint32_t* size) { *size = g_backup_size; return g_backup; }

void backup_flush(void) {
    if (!g_backup_dirty || !g_backup_path[0]) return;
    FILE* f = fopen(g_backup_path, "wb");
    if (!f) { LOG_WARN("save: cannot write %s\n", g_backup_path); return; }
    fwrite(g_backup, 1, g_backup_size, f);
    fclose(f);
    g_backup_dirty = 0;
}

/* ---------------------------------------------------------------- downloadable content
 * DQIX's downloadable quests and special guests are on the cartridge; Nintendo
 * Wi-Fi Connection only switched them on. The switches are in the save: one
 * "unlocked" bit per downloadable quest in the quest status table (4 bits per
 * quest, bit 3) and a bit per special guest. Each 32 KB save slot is
 * protected by three CRC-32s (stored at 16, 36 and 132, covering 20-36,
 * 40-132 and 136-28644). Layout from the open-source DQIX save editor
 * (github.com/dqix/editor). */
static uint32_t crc32_buf(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static const uint8_t DLC_QUESTS[] = {     /* quest ids (not the numbers shown in the game) */
    2, 122, 123, 127, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 143, 145, 146, 148, 149, 150, 151, 152,
    153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 164, 165, 166, 168, 169, 170, 173, 174, 175, 176, 177, 178,
    179, 180, 181, 182, 183, 184, 185, 186, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 202 };
#define SLOT_SIZE 0x8000
#define QUEST_STATUS 12300
#define SPECIAL_GUESTS 11528
#define N_SPECIAL_GUESTS 23

static int slot_valid(const uint8_t* b) {
    static const int ck[3][3] = { { 16, 20, 36 }, { 36, 40, 132 }, { 132, 136, 28644 } };
    if (memcmp(b, "DRAGON QUEST IX", 15)) return 0;
    for (int i = 0; i < 3; i++) if (rd32(b + ck[i][0]) != crc32_buf(b + ck[i][1], (size_t)(ck[i][2] - ck[i][1]))) return 0;
    return 1;
}
int backup_slot_valid(const uint8_t* b) { return slot_valid(b); }
/* Import / export (Game > Save file): how many of a save image's two adventure
 * slots are real DQIX saves (0: not a DQIX save), and swapping the save for
 * another one (written to the save file at once). */
int backup_check_image(const uint8_t* img, uint32_t size) {
    int n = 0;
    for (uint32_t s = 0; s + SLOT_SIZE <= size && s < 2 * SLOT_SIZE; s += SLOT_SIZE) n += slot_valid(img + s);
    return n;
}
void backup_replace(const uint8_t* img, uint32_t size) {
    if (size > BACKUP_MAX) size = BACKUP_MAX;
    memset(g_backup, 0xFF, sizeof g_backup);
    memcpy(g_backup, img, size);
    if (size > g_backup_size) g_backup_size = size;
    g_backup_dirty = 1;
    backup_flush();
}
const char* backup_path(void) { return g_backup_path; }

static int unlock_image(uint8_t* img, uint32_t size) {
    int n = 0;
    for (uint32_t s = 0; s + SLOT_SIZE <= size && s < 2 * SLOT_SIZE; s += SLOT_SIZE) {
        uint8_t* b = img + s;
        if (!slot_valid(b)) continue;
        for (size_t i = 0; i < sizeof DLC_QUESTS; i++) { int id = DLC_QUESTS[i]; b[QUEST_STATUS + id / 2] |= (uint8_t)((id & 1) ? 0x80 : 0x08); }
        wr32(b + SPECIAL_GUESTS, rd32(b + SPECIAL_GUESTS) | (((1u << N_SPECIAL_GUESTS) - 1) << 1));
        wr32(b + 16, crc32_buf(b + 20, 16));
        wr32(b + 36, crc32_buf(b + 40, 92));
        wr32(b + 132, crc32_buf(b + 136, 28644 - 136));
        n++;
    }
    return n;
}

/* Switch the downloadable content on in the save as it is loaded (before
 * the game reads it). Returns how many save slots were changed. */
int backup_unlock_dlc(void) {
    int n = unlock_image(g_backup, g_backup_size);
    if (g_backup_at_boot) unlock_image(g_backup_at_boot, g_backup_size);   /* replays start from the same save */
    if (n) LOG_INFO("save: downloadable quests and special guests unlocked in %d slot(s)\n", n);
    return n;
}

/* Modern > Save by itself (autosave.c): a save made outside the game's own
 * writes, put on the chip as if the game had written it (backup_flush then
 * writes the file). */
void backup_store(uint32_t off, const uint8_t* p, uint32_t n) {
    if (off >= g_backup_size || n > g_backup_size - off) return;
    memcpy(g_backup + off, p, n);
    g_backup_dirty = 1;
}

static uint8_t backup_xfer(uint8_t in, int hold) {
    uint8_t out = 0xFF;
    if (g_bk_cmd == 0) {
        g_bk_cmd = in; g_bk_addr = 0; g_bk_count = 0;
        switch (in) {
        case 0x06: g_bk_status |= 2; g_bk_cmd = 0; break;      /* write enable */
        case 0x04: g_bk_status &= ~2; g_bk_cmd = 0; break;     /* write disable */
        default: break;
        }
    } else {
        uint32_t n = g_bk_count++;
        uint32_t abytes = g_backup_size > 0x10000 ? 3 : 2;
        switch (g_bk_cmd) {
        case 0x05: out = g_bk_status; break;                   /* read status */
        case 0x9F: out = n == 0 ? 0x20 : n == 1 ? 0x40 : 0x13; break;   /* JEDEC id */
        case 0x03: case 0x0B: case 0x02: case 0x0A: case 0xDB: case 0xD8:
            if (n < abytes) { g_bk_addr = (g_bk_addr << 8) | in; break; }
            if (g_bk_cmd == 0x0B && n == abytes) break;            /* dummy byte */
            if (g_bk_cmd == 0x03 || g_bk_cmd == 0x0B) { out = g_backup[g_bk_addr++ % g_backup_size]; break; }
            if (g_bk_cmd == 0x02 || g_bk_cmd == 0x0A) {          /* program / write */
                uint32_t a = g_bk_addr++ % g_backup_size;
                /* flash "page program" can only clear bits; EEPROM writes replace */
                g_backup[a] = (g_bk_cmd == 0x02 && g_backup_size > 0x10000) ? (g_backup[a] & in) : in;
                g_backup_dirty = 1;
                g_backup_written = g_frame;
            }
            break;
        default:
            if (n == 0) LOG_WARN("save: command %02x not emulated\n", g_bk_cmd);
            break;
        }
    }
    if (!hold) {
        if (g_bk_cmd) LOG_DEBUG("save: cmd %02x addr %06x, %u bytes\n", g_bk_cmd, g_bk_addr, g_bk_count);
        if (g_bk_cmd == 0xDB || g_bk_cmd == 0xD8) {               /* page / sector erase */
            uint32_t sz = g_bk_cmd == 0xDB ? 0x100 : 0x10000;
            uint32_t a = (g_bk_addr % g_backup_size) & ~(sz - 1);
            memset(&g_backup[a], 0xFF, sz);
            g_backup_dirty = 1;
        }
        if (g_bk_cmd == 0x02 || g_bk_cmd == 0x0A || g_bk_cmd == 0xDB || g_bk_cmd == 0xD8) g_bk_status &= ~2;
        g_bk_cmd = 0;
        backup_flush();
    }
    return out;
}

/* ---------------------------------------------------------------- registers */
uint32_t card_read(Core* k, uint32_t off, int size) {
    uint32_t v; memcpy(&v, &g_io[off & ~3u], 4);
    (void)size;
    if ((off & ~3u) == 0x1A4 && g_card_timing > 0 && (v & 0x80000000u) && g_card_pos < g_card_len) {
        /* the game polls "word ready": it is set once the next word has arrived */
        if (k == &g_c9) cpu_time_sync();
        if (time_cycles() < word_ready_time(g_card_pos)) { v &= ~0x00800000u; time_poll(k); }
    }
    return v;
}

void card_write(Core* k, uint32_t off, uint32_t v, int size) {
    memcpy(&g_io[off], &v, size);
    uint32_t end = off + size;
    if (off <= 0x1A2 && end > 0x1A2) {                            /* AUXSPIDATA: one byte over SPI */
        uint16_t cnt = IO16(0x1A0);
        if ((cnt & 0x8000) && (cnt & 0x2000)) {
            uint8_t in = (uint8_t)(size == 4 ? (v >> 16) : v);
            uint8_t out = backup_xfer(in, cnt & 0x40);
            g_io[0x1A2] = out; g_io[0x1A3] = 0;
        }
    }
    if (off <= 0x1A7 && end > 0x1A7 && (g_io[0x1A7] & 0x80)) rom_start(k, IO32(0x1A4));
}
