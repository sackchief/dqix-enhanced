#include <stdio.h>
#include <stdlib.h>
/* ARM9 memory map. */
#include <string.h>
#include "nds.h"
#include "text_hd.h"

uint8_t g_main_ram[MAIN_RAM_SIZE];
uint8_t g_itcm[ITCM_SIZE];
uint8_t g_dtcm[DTCM_SIZE];
uint8_t g_swram[SWRAM_SIZE];
uint8_t g_palette[0x800];
uint8_t g_oam[0x800];
uint8_t g_vram[0xA4000];
uint32_t g_dtcm_base = 0x027E0000;
int g_itcm_enabled = 1, g_dtcm_enabled = 1;
uint32_t g_overlay_area_start, g_overlay_area_end, g_overlay_gen;

/* ---------------------------------------------------------------- VRAM
 * Nine banks (A..I). Each engine/region view is a table of 16 KB pages
 * pointing into g_vram (NULL = unmapped). Mapping follows GBATEK. */
static const uint32_t bank_off[9]  = { 0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000 };
static const uint32_t bank_size[9] = { 0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000 };
#define VPAGES (0x00A00000u / 0x4000u)   /* 0x06000000..0x069FFFFF */
static uint8_t* vram_page[VPAGES];

static void map_pages(uint32_t addr, int bank, uint32_t len) {
    for (uint32_t o = 0; o < len; o += 0x4000) {
        uint32_t idx = ((addr + o) - 0x06000000u) / 0x4000u;
        if (idx < VPAGES) vram_page[idx] = &g_vram[bank_off[bank] + (o % bank_size[bank])];
    }
}

/* 3D texture slots (banks A-D with MST=3) and texture palette slots (E/F/G). */
uint8_t* g_tex_slot[4];
uint8_t* g_texpal_slot[6];

void vram_remap(void) {
    memset(vram_page, 0, sizeof vram_page);
    memset(g_tex_slot, 0, sizeof g_tex_slot);
    memset(g_texpal_slot, 0, sizeof g_texpal_slot);
    for (int b = 0; b < 7; b++) {
        uint8_t cnt = g_io[0x240 + b];
        if ((cnt & 0x87) != 0x83) continue;
        int ofs = (cnt >> 3) & 3;
        if (b < 4) g_tex_slot[ofs] = &g_vram[bank_off[b]];
        else if (b == 4) for (int i = 0; i < 4; i++) g_texpal_slot[i] = &g_vram[bank_off[b] + 0x4000 * i];
        else g_texpal_slot[(ofs & 1) + (ofs >> 1) * 4] = &g_vram[bank_off[b]];
    }
    for (int b = 0; b < 9; b++) {
        uint8_t cnt = g_io[0x240 + b + (b >= 7 ? 1 : 0)];   /* 0x247 is WRAMCNT */
        if (!(cnt & 0x80)) continue;
        int mst = cnt & 7, ofs = (cnt >> 3) & 3;
        uint32_t sz = bank_size[b];
        if (mst == 0) {   /* LCDC */
            static const uint32_t lcdc[9] = { 0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000,
                                              0x06890000, 0x06894000, 0x06898000, 0x068A0000 };
            map_pages(lcdc[b], b, sz);
            continue;
        }
        switch (b) {
        case 0: case 1: case 2: case 3:
            if (mst == 1) map_pages(0x06000000 + 0x20000 * ofs, b, sz);              /* BG A */
            else if (mst == 2 && b < 2) map_pages(0x06400000 + 0x20000 * (ofs & 1), b, sz); /* OBJ A */
            else if (mst == 4 && b == 2) map_pages(0x06200000, b, sz);              /* BG B */
            else if (mst == 4 && b == 3) map_pages(0x06600000, b, sz);              /* OBJ B */
            /* mst 3 (texture), 2 for C/D (ARM7) are not CPU-visible here */
            break;
        case 4:
            if (mst == 1) map_pages(0x06000000, b, sz);
            else if (mst == 2) map_pages(0x06400000, b, sz);
            break;
        case 5: case 6: {
            uint32_t o = 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
            if (mst == 1) map_pages(0x06000000 + o, b, sz);
            else if (mst == 2) map_pages(0x06400000 + o, b, sz);
            break;
        }
        case 7: if (mst == 1) map_pages(0x06200000, b, sz); break;
        case 8: if (mst == 1) map_pages(0x06208000, b, sz); else if (mst == 2) map_pages(0x06600000, b, sz); break;
        }
    }
}

/* ARM7 view: banks C and D when assigned to the ARM7 (MST=2) at 0x06000000 + 0x20000*OFS. */
uint8_t* vram7_ptr(uint32_t a) {
    uint32_t o = (a - 0x06000000u) & 0x3FFFF;
    for (int b = 2; b <= 3; b++) {
        uint8_t cnt = g_io[0x240 + b];
        if ((cnt & 0x87) == 0x82 && ((cnt >> 3) & 1) == (o >> 17)) return &g_vram[bank_off[b] + (o & 0x1FFFF)];
    }
    return NULL;
}

uint8_t* vram_ptr(uint32_t a) {
    uint32_t idx = ((a & 0x00FFFFFFu) % 0x00A00000u) / 0x4000u;
    uint8_t* p = vram_page[idx];
    return p ? p + (a & 0x3FFF) : NULL;
}

/* ---------------------------------------------------------------- core */
static inline uint8_t* resolve(uint32_t a, int write) {
    if (g_dtcm_enabled && (a & ~(DTCM_SIZE - 1)) == g_dtcm_base) return &g_dtcm[a & (DTCM_SIZE - 1)];
    switch (a >> 24) {
    case 0x00: case 0x01:
        if (g_itcm_enabled) {
            if (write && a < 0x01000000) {        /* ITCM through its low mirror: usually a null pointer */
                static int reported;
                if (g_frame >= 10 && reported++ < 8) { LOG_WARN("frame %llu: write to %08x (ITCM via the null-pointer mirror) lr=%08x pc-ish r0=%08x\n", (unsigned long long)g_frame, a, g_c9.cpu->r[14], g_c9.cpu->r[0]); if (reported == 1) trace_dump(stderr, 48); }
            }
            return &g_itcm[a & (ITCM_SIZE - 1)];
        }
        return NULL;
    case 0x02:
        if (write && a >= g_overlay_area_start && a < g_overlay_area_end) g_overlay_gen++;
        return &g_main_ram[a & (MAIN_RAM_SIZE - 1)];
    case 0x03:                       /* shared WRAM, ARM9's share per WRAMCNT */
        switch (g_io[0x247] & 3) {
        case 0: return &g_swram[a & (SWRAM_SIZE - 1)];
        case 1: return &g_swram[0x4000 + (a & 0x3FFF)];
        case 2: return &g_swram[a & 0x3FFF];
        default: return NULL;
        }
    case 0x05: return &g_palette[a & 0x7FF];
    case 0x06: return vram_ptr(a);
    case 0x07: return &g_oam[a & 0x7FF];
    default: return NULL;
    }
}

static uint32_t unmapped_read(uint32_t a, int size) {
    if ((a >> 24) == 0x08 || (a >> 24) == 0x09 || (a >> 24) == 0x0A) return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;  /* empty GBA slot */
    if ((a >> 16) == 0xFFFF) return 0;  /* BIOS */
    LOG_WARN("read%d from unmapped %08x\n", size * 8, a);
    return 0;
}

uint8_t mem_read8(uint32_t a) {
    if ((a >> 24) == 4) return (uint8_t)io9_read(a, 1);
    uint8_t* p = resolve(a, 0);
    return p ? *p : (uint8_t)unmapped_read(a, 1);
}
uint16_t mem_read16(uint32_t a) {
    a &= ~1u;
    if ((a >> 24) == 4) return (uint16_t)io9_read(a, 2);
    uint8_t* p = resolve(a, 0);
    if (!p) return (uint16_t)unmapped_read(a, 2);
    uint16_t v; memcpy(&v, p, 2); return v;
}
uint32_t mem_read32(uint32_t a) {
    a &= ~3u;
    if ((a >> 24) == 4) return io9_read(a, 4);
    uint8_t* p = resolve(a, 0);
    if (!p) return unmapped_read(a, 4);
    uint32_t v; memcpy(&v, p, 4); return v;
}
/* DQIX_WATCH=addr: log ARM9 writes to that word (debugging). */
static uint32_t g_watch = 1;
static void watch_hit(uint32_t a, uint32_t v, int size) {
    fprintf(stderr, "[f%llu l%u] watch: write%d %08x = %x (lr %08x)\n", (unsigned long long)g_frame, g_vcount, size * 8, a, v, g_arm9.r[14]);
    if (getenv("DQIX_WATCHREGS")) { for (int i = 0; i < 13; i++) fprintf(stderr, " r%d=%08x", i, g_arm9.r[i]); fprintf(stderr, "\n"); }
    trace_dump(stderr, getenv("DQIX_TRACEN") ? atoi(getenv("DQIX_TRACEN")) : 24);
}
static inline int watching(uint32_t a) {
    if (__builtin_expect(g_watch == 1, 0)) { const char* e = getenv("DQIX_WATCH"); g_watch = e ? (uint32_t)strtoul(e, NULL, 16) & ~3u : 0; }
    return g_watch && (a & ~3u) == g_watch;
}

void mem_write8(uint32_t a, uint8_t v) {
    if (watching(a)) watch_hit(a, v, 1);
    if ((a >> 24) == 4) { io9_write(a, v, 1); return; }
    /* 8-bit writes to VRAM/palette/OAM are ignored on the DS */
    if ((a >> 24) >= 5 && (a >> 24) <= 7) return;
    uint8_t* p = resolve(a, 1);
    if (p) { uint8_t o = *p; *p = v; tx_write_v(a, 1, (uint32_t)(o ^ v)); } else LOG_WARN("write8 to unmapped %08x = %02x\n", a, v);
}
void mem_write16(uint32_t a, uint16_t v) {
    a &= ~1u;
    if (watching(a)) watch_hit(a, v, 2);
    if ((a >> 24) == 4) { io9_write(a, v, 2); return; }
    uint8_t* p = resolve(a, 1);
    if (p) { uint16_t o; memcpy(&o, p, 2); memcpy(p, &v, 2); tx_write_v(a, 2, (uint32_t)(o ^ v)); } else LOG_WARN("write16 to unmapped %08x = %04x\n", a, v);
}
void mem_write32(uint32_t a, uint32_t v) {
    a &= ~3u;
    if (watching(a)) watch_hit(a, v, 4);
    if ((a >> 24) == 4) { io9_write(a, v, 4); return; }
    uint8_t* p = resolve(a, 1);
    if (p) { uint32_t o; memcpy(&o, p, 4); memcpy(p, &v, 4); tx_write_v(a, 4, o ^ v); } else LOG_WARN("write32 to unmapped %08x = %08x\n", a, v);
}

/* Bank access for the graphics engines (extended palettes, texture slots). */
uint8_t* vram_bank(int b) { return &g_vram[bank_off[b]]; }
uint32_t vram_bank_size(int b) { return bank_size[b]; }
uint8_t vram_bank_cnt(int b) { return g_io[0x240 + b + (b >= 7 ? 1 : 0)]; }
