/* Modern: save by itself every 10 minutes (Modern > Save by itself).
 *
 * The game saves only when the priest is asked to. Its save routine (the
 * church's, func_020aad1c in the USA version) runs over a few frames: it
 * reads the save from the chip into a work buffer, has func_020a9eb8 write
 * the adventure into that buffer (party, bag, quests, where the party is,
 * the three CRC-32s), then writes the buffer to the chip twice (at 0x10 and
 * 0x8010, 0x6FE4 bytes each).
 *
 * Here func_020a9eb8 alone runs, at the end of a field frame while the hero
 * is free to walk (not in a battle, dialogue, menu, cut scene or loading),
 * into the same buffer, filled first with the save as it is, as the game
 * does. The result goes on the chip and in the save file as the game would
 * write it: Continue then takes the party back to where it was. The buffer
 * and the game's flag are put back as they were, so the game sees nothing.
 * Ten minutes are counted in DS frames since the last save (the game's own
 * included), so replays stay exact; never in a session (every console must
 * run alike), and only once a save exists (not over an adventure log being
 * started). */
#include <string.h>
#include "nds.h"
#include "autosave.h"
#include "session.h"

int g_autosave;
int g_autosave_done;

#define EVERY       (10 * 60 * 60)   /* frames: 10 minutes */
#define WALK_FLAG   0x109DA4         /* 1 while the hero is free to walk (input.c) */
#define FIELD_PTR   0x0F33E0         /* 0 until an adventure is loaded (input.c) */
#define SAVE_TARGET 0x02108E00       /* the buffer the save routine writes into; 0: straight to the chip */
#define SAVE_DATA   0x0211E33C       /* the adventure's data, func_020a9eb8's second argument */
#define SAVE_BUF    (SAVE_DATA + 0x2901C)   /* the routine's work buffer: the chip's first 0x7000 bytes */
#define SAVE_FLAG   (0x020F33D8 + 0x5CC8)   /* GameState: set by the save routine */
#define SLOT_AT     0x10             /* what the game writes of each 32 KB slot */
#define SLOT_LEN    0x6FE4
#define SLOT_SIZE   0x8000
#define WRITE_SAVE_USA 0x020A9EB8u   /* func_020a9eb8: the adventure into the buffer */
/* the party's characters: up to 5 records of 0x964 bytes; HP, MP, max HP
 * and max MP (u16) at +0x58. An unused record has max HP 0 (or 0xFFFF). */
#define CHARS       0x0F2F24
#define CHAR_SIZE   0x964
#define N_CHARS     5
#define CHAR_HP     0x58

static uint64_t g_since;             /* the frame of the last save */

static uint8_t* ram(uint32_t a) { return &g_main_ram[a & (MAIN_RAM_SIZE - 1)]; }
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, ram(a), 4); return v; }
static uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, ram(a), 2); return v; }

/* A battle is on: the 3D on the top screen, with the party's records filled. */
static int in_battle(void) {
    int max = rd16(CHARS + (N_CHARS - 1) * CHAR_SIZE + CHAR_HP + 4);
    return (IO16(0x304) & 0x8000) && rd32(FIELD_PTR) && max > 0 && max <= 999;
}

/* Write the adventure into the game's buffer and from there to the chip.
 * Returns 1 if it saved. */
static int save_now(Cpu* c) {
    uint32_t size;
    const uint8_t* chip = backup_image(&size);
    if (size < 2 * SLOT_SIZE) return 0;
    const uint8_t* now = backup_slot_valid(chip) ? chip : NULL;       /* the first copy (the second has no name: the game writes from 0x10) */
    if (!now) { LOG_INFO("autosave: no save yet, nothing done\n"); return 0; }

    static uint8_t keep[SLOT_SIZE], slot[SLOT_SIZE];
    uint8_t* buf = ram(SAVE_BUF);
    memcpy(keep, buf, SLOT_SIZE);
    uint8_t flag = *ram(SAVE_FLAG);
    memcpy(buf, now, SLOT_SIZE);

    Cpu regs = *c;
    uint32_t sp = (c->r[13] - 16) & ~7u;          /* the routine's state, {0, buffer}, on the game's stack */
    mem_write32(sp, 0); mem_write32(sp + 4, SAVE_BUF);   /* the stack is in DTCM, not main RAM */
    mem_write32(SAVE_TARGET, SAVE_BUF);
    c->r[13] = sp; c->r[0] = sp; c->r[1] = SAVE_DATA; c->r[2] = 1; c->r[3] = 0;
    c->r[14] = regs.r[14];
    dispatch_call(c, WRITE_SAVE_USA + (rom_is_eur() ? 0x10 : 0), regs.r[14]);
    int ok = c->r[0] == 1 && !c->fault_addr;
    mem_write32(SAVE_TARGET, 0);
    *c = regs;

    memcpy(slot, buf, SLOT_SIZE);
    memcpy(buf, keep, SLOT_SIZE);
    *ram(SAVE_FLAG) = flag;
    if (!ok || !backup_slot_valid(slot)) { LOG_WARN("autosave: the game's save didn't come out right, nothing written\n"); return 0; }
    backup_store(SLOT_AT, slot + SLOT_AT, SLOT_LEN);                  /* both copies, as the game writes them */
    backup_store(SLOT_SIZE + SLOT_AT, slot + SLOT_AT, SLOT_LEN);
    backup_flush();
    LOG_INFO("autosave: saved (frame %llu)\n", (unsigned long long)g_frame);
    return 1;
}

/* func_ov017_0218d0ec, the field's frame (time of day, the map...): wrap.txt. */
void wrap_field_frame_orig(Cpu* c);
void wrap_field_frame(Cpu* c) {
    wrap_field_frame_orig(c);
    if (g_backup_written > g_since) g_since = g_backup_written;
    if (!g_since) g_since = g_frame;
    static uint64_t every;
    if (!every) { const char* e = rt_getenv("DQIX_AUTOSAVE_FRAMES"); every = e && atoi(e) > 0 ? (uint64_t)atoi(e) : EVERY; }   /* tests */
    if (!g_autosave || g_frame - g_since < every || session_requested()) return;
    if (g_main_ram[WALK_FLAG] != 1 || !rd32(FIELD_PTR) || in_battle() || rd32(SAVE_TARGET)) return;
    if (g_frame - g_backup_written < 60) return;                       /* the game is writing its own */
    g_since = g_frame;
    if (save_now(c)) g_autosave_done = 1;
}
