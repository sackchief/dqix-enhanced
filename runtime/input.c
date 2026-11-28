/* Input: buttons and the touch screen, from the frontend (keyboard, mouse,
 * gamepad) and/or a script, and a recording of everything pressed so a
 * session can be replayed exactly (reports, debugging).
 *
 * Script syntax (DQIX_INPUT="..." or a file named by DQIX_INPUT_FILE):
 *   frame:what:frames  separated by ';' or newlines.
 *   `what` is buttons and/or a touch joined by '+':
 *   A B SELECT START RIGHT LEFT UP DOWN R L X Y touch=X,Y (bottom-screen pixels)
 *   e.g. "1800:START:6;2000:touch=128,175:6;2200:A+touch=20,30:6".
 * The recording uses the same syntax. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nds.h"
#include "session.h"
#include "free_camera.h"

typedef struct { uint64_t frame; uint32_t keys; int touch; int tx, ty; uint32_t len; } Press;

static Press* g_script;
static int g_nscript, g_cap, g_head, g_loaded;

uint16_t g_keys_down;          /* bit set = pressed, KEYINPUT order (bits 0-9) + X=10, Y=11, lid closed=12 */
int g_touch_down, g_touch_x, g_touch_y;
/* set by the frontend (keyboard / mouse / gamepad) */
uint16_t g_fe_keys;
int g_fe_touch, g_fe_tx, g_fe_ty;
int g_fe_stick_x, g_fe_stick_y;
int g_fe_rstick_x;
uint16_t g_fe_stick_keys;
int g_analog_walk;

/* ---------------------------------------------------------------- Modern: walk in any direction
 * On the DS, the D-pad walks in 8 directions; the stylus walks in any: touch
 * the field and drag, and the hero walks the way the drag points (how far
 * doesn't matter). With this on, the left stick does that drag: while the
 * hero is free to walk (the game's own flag at 02109DA4 is 1; 2 in dialogue
 * and menus, 0 while loading), pushing the stick touches the middle of the
 * field and drags towards the stick's direction. Anywhere else the stick
 * stays a D-pad. To the game (and to friends in a session) it is just the
 * stylus. */
#define WALK_FLAG 0x109DA4
/* The flag is 1 on the title screen's menu too (it means "touch input
 * accepted"), so also require an adventure to be loaded: this pointer (a
 * table of the field's interface parts, static data) is 0 until the save is
 * loaded, from the title screen on (checked on the title, while loading, in
 * a room, a town and the world map). */
#define FIELD_PTR 0x0F33E0
static int free_to_walk(void) {
    uint32_t p; memcpy(&p, &g_main_ram[FIELD_PTR], 4);
    return g_main_ram[WALK_FLAG] == 1 && p != 0;
}
#define WALK_X 128
#define WALK_Y 100
/* Where the game thinks the stylus went down (two words, x and y; -1 while
 * nothing touches): the stylus walk goes the way the stylus moved from there.
 * After a menu, a battle or a change of scene the game sometimes takes that
 * point again from wherever the stylus is at that moment: with the stick
 * still held, that is the dragged point, so the hero then walks by the
 * stick's small wobbles - the wrong way - until it is let go. Seen in a
 * test: X, then B, holding the stick down: the point became (128,148).
 * Found by comparing memory with the stylus down at (128,100) and dragged
 * (static data, the same in every scene). */
#define TOUCH_START 0x114E84
#define WALK_REACH 48
#define WALK_DEAD 9000                  /* stick distance before it walks (of 32767) */
static int g_walk_frames;               /* frames the stick has been walking (0: not) */
static int stick_walk(int* tx, int* ty) {
    long long x = g_fe_stick_x, y = g_fe_stick_y;
    double m = sqrt((double)(x * x + y * y));
    if (!g_analog_walk || m < WALK_DEAD || !free_to_walk()) { g_walk_frames = 0; return 0; }
    if (g_walk_frames++ < 2) { *tx = WALK_X; *ty = WALK_Y; return 1; }   /* touch down first, then drag */
    if (g_walk_frames > 4) {                    /* the game moved the start point: lift the stylus for a frame, put it down again */
        int32_t sx, sy; memcpy(&sx, &g_main_ram[TOUCH_START], 4); memcpy(&sy, &g_main_ram[TOUCH_START + 4], 4);
        if (sx != WALK_X || sy != WALK_Y) { g_walk_frames = 0; return 0; }
    }
    int dx = (int)lround((double)x / m * WALK_REACH), dy = (int)lround((double)y / m * WALK_REACH);
    *tx = WALK_X + dx; *ty = WALK_Y + dy;
    if (*tx < 0) *tx = 0; if (*tx > 255) *tx = 255; if (*ty < 0) *ty = 0; if (*ty > 191) *ty = 191;
    return 1;
}

/* ---------------------------------------------------------------- the right stick turns the camera
 * The game turns the camera while L or R is held (as far as each place
 * allows) and leaves it where it is on release. The right stick presses them:
 * left = L, right = R, more often the further it is pushed, so a light push
 * turns the camera slowly. Only while the field camera would turn with L and R
 * (in menus, dialogue and battles they do other things; on the boat too);
 * not while L or R is pressed itself. To
 * the game it is just L and R. */
#define CAM_DEAD 9000
static uint32_t stick_camera(uint32_t keys) {
    static int acc;                                     /* presses owed, in 1/1000 */
    int x = g_fe_rstick_x, m = x < 0 ? -x : x;
    /* the field camera would take L/R now (free_camera.c: not in menus, dialogue or battles; also on the
     * boat, where the game's "free to walk" flag is 0), and not in a land menu (flag 2) to be sure */
    if (m < CAM_DEAD || !field_camera_takes_lr() || g_main_ram[WALK_FLAG] == 2 || (keys & (3u << 8))) { acc = 0; return 0; }
    int duty = 250 + 750 * (m - CAM_DEAD) / (32767 - CAM_DEAD);   /* a quarter of the frames .. every frame */
    acc += duty;
    if (acc < 1000) return 0;
    acc -= 1000;
    return x > 0 ? 1u << 8 : 1u << 9;                   /* R : L */
}

static const char* const k_names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y", "LID" };   /* LID = the DS is closed */

static uint32_t key_bit(const char* s, size_t n) {
    for (int i = 0; i < 13; i++) if (strlen(k_names[i]) == n && !strncmp(k_names[i], s, n)) return 1u << i;
    return 0;
}

static void push(Press** arr, int* n, int* cap, Press p) {
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *arr = realloc(*arr, sizeof(Press) * (size_t)*cap); }
    (*arr)[(*n)++] = p;
}

static void parse_script(const char* text) {
    char* buf = strdup(text);
    for (char* tok = strtok(buf, ";\r\n"); tok; tok = strtok(NULL, ";\r\n")) {
        Press p = { 0 };
        char what[160] = { 0 };
        unsigned long long fr; unsigned len = 6;
        if (sscanf(tok, "%llu:%159[^:]:%u", &fr, what, &len) < 2) continue;
        p.frame = fr; p.len = len;
        for (char* k = what; *k;) {
            char* plus = strchr(k, '+');
            size_t n = plus ? (size_t)(plus - k) : strlen(k);
            if (n > 6 && !strncmp(k, "touch=", 6)) { p.touch = 1; sscanf(k + 6, "%d,%d", &p.tx, &p.ty); }
            else p.keys |= key_bit(k, n);
            k += n + (plus ? 1 : 0);
        }
        push(&g_script, &g_nscript, &g_cap, p);
    }
    free(buf);
}

static int cmp_press(const void* a, const void* b) {
    const Press* x = a; const Press* y = b;
    return x->frame < y->frame ? -1 : x->frame > y->frame ? 1 : 0;
}

static void load_script(void) {
    g_loaded = 1;
    if (session_hidden()) return;                       /* another player's console: their presses come from the session */
    const char* env = getenv("DQIX_INPUT");
    if (env) parse_script(env);
    const char* path = getenv("DQIX_INPUT_FILE");
    if (path) {
        FILE* f = fopen(path, "rb");
        if (!f) { LOG_WARN("input: cannot open %s\n", path); }
        else {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            char* text = malloc((size_t)n + 1);
            size_t got = fread(text, 1, (size_t)n, f); text[got] = 0;
            fclose(f);
            parse_script(text);
            free(text);
        }
    }
    if (g_nscript) qsort(g_script, (size_t)g_nscript, sizeof(Press), cmp_press);
}

/* Tests: scripts for the other consoles of a local session (DQIX_INPUT_1..3),
 * as packed session inputs for a frame. */
static Press* g_alt[4]; static int g_nalt[4], g_altcap[4], g_altloaded[4];
uint32_t input_script_for(int slot, uint64_t frame) {
    if (slot < 1 || slot > 3) return 0;
    if (!g_altloaded[slot]) {
        g_altloaded[slot] = 1;
        char name[32]; snprintf(name, sizeof name, "DQIX_INPUT_%d", slot);
        const char* text = getenv(name);
        if (text) {
            Press* save = g_script; int n = g_nscript, cap = g_cap;
            g_script = NULL; g_nscript = g_cap = 0;
            parse_script(text);
            if (g_nscript) qsort(g_script, (size_t)g_nscript, sizeof(Press), cmp_press);
            g_alt[slot] = g_script; g_nalt[slot] = g_nscript; g_altcap[slot] = g_cap;
            g_script = save; g_nscript = n; g_cap = cap;
        }
    }
    uint16_t keys = 0; int touch = 0, tx = 0, ty = 0;
    for (int i = 0; i < g_nalt[slot] && g_alt[slot][i].frame <= frame; i++) {
        Press* p = &g_alt[slot][i];
        if (frame < p->frame + p->len) { keys |= (uint16_t)p->keys; if (p->touch) { touch = 1; tx = p->tx; ty = p->ty; } }
    }
    return session_pack(keys, touch, tx, ty);
}

/* ---------------------------------------------------------------- recording */
static Press* g_rec;
static int g_nrec, g_rec_cap;
static Press g_cur;                    /* the segment being recorded */
static int g_cur_open;

static int same_state(const Press* p, uint32_t keys, int touch, int tx, int ty) {
    return p->keys == keys && p->touch == touch && (!touch || (p->tx == tx && p->ty == ty));
}

static void record(uint32_t keys, int touch, int tx, int ty) {
    if (g_cur_open && same_state(&g_cur, keys, touch, tx, ty)) { g_cur.len++; return; }
    if (g_cur_open) push(&g_rec, &g_nrec, &g_rec_cap, g_cur);
    g_cur_open = keys || touch;
    if (g_cur_open) { g_cur.frame = g_frame; g_cur.keys = keys; g_cur.touch = touch; g_cur.tx = tx; g_cur.ty = ty; g_cur.len = 1; }
}

static void write_press(FILE* f, const Press* p) {
    fprintf(f, "%llu:", (unsigned long long)p->frame);
    int first = 1;
    for (int i = 0; i < 13; i++) if (p->keys & (1u << i)) { fprintf(f, "%s%s", first ? "" : "+", k_names[i]); first = 0; }
    if (p->touch) fprintf(f, "%stouch=%d,%d", first ? "" : "+", p->tx, p->ty);
    fprintf(f, ":%u\n", p->len);
}

/* Everything pressed since launch, in script syntax. */
void input_write_recording(FILE* f) {
    for (int i = 0; i < g_nrec; i++) write_press(f, &g_rec[i]);
    if (g_cur_open) write_press(f, &g_cur);
}

/* ---------------------------------------------------------------- per frame */
/* Called once per frame (VBlank). */
void input_update(void) {
    if (!g_loaded) load_script();
    g_keys_down = 0; g_touch_down = 0;
    while (g_head < g_nscript && g_script[g_head].frame + g_script[g_head].len <= g_frame) g_head++;
    for (int i = g_head; i < g_nscript && g_script[i].frame <= g_frame; i++) {
        Press* p = &g_script[i];
        if (g_frame < p->frame + p->len) {
            g_keys_down |= (uint16_t)p->keys;
            if (p->touch) { g_touch_down = 1; g_touch_x = p->tx; g_touch_y = p->ty; }
        }
    }
    g_keys_down |= g_fe_keys;
    {   /* tests: DQIX_STICK=from,to,x,y[;...] holds the left stick there for those frames; DQIX_ANALOG=1 */
        static int init; static long long st[16][4]; static int nst;
        if (!init) { init = 1; const char* e = getenv("DQIX_STICK");
            for (const char* q = e; q && *q && nst < 16; ) { if (sscanf(q, "%lld,%lld,%lld,%lld", &st[nst][0], &st[nst][1], &st[nst][2], &st[nst][3]) == 4) nst++;
                q = strchr(q, ';'); if (q) q++; }
            if (getenv("DQIX_ANALOG")) g_analog_walk = 1; }
        for (int i = 0; i < nst; i++) if ((long long)g_frame >= st[i][0] && (long long)g_frame < st[i][1]) { g_fe_stick_x = (int)st[i][2]; g_fe_stick_y = (int)st[i][3]; }
        static long long rs[3] = { -1, -1, 0 }; static int rinit;      /* DQIX_RSTICK=from,to,x: the right stick */
        if (!rinit) { rinit = 1; const char* e = getenv("DQIX_RSTICK"); if (e) sscanf(e, "%lld,%lld,%lld", &rs[0], &rs[1], &rs[2]); }
        if ((long long)g_frame >= rs[0] && (long long)g_frame < rs[1]) g_fe_rstick_x = (int)rs[2];
        else if (rs[0] >= 0) g_fe_rstick_x = 0;
    }
    g_keys_down |= (uint16_t)stick_camera(g_keys_down);
    if (g_fe_touch) { g_touch_down = 1; g_touch_x = g_fe_tx; g_touch_y = g_fe_ty; g_walk_frames = 0; }
    else { int tx, ty;
        if (!g_touch_down && stick_walk(&tx, &ty)) { g_touch_down = 1; g_touch_x = tx; g_touch_y = ty; }
        else { if (g_touch_down) g_walk_frames = 0; g_keys_down |= g_fe_stick_keys; } }
    {   static long long wl = -2; if (wl == -2) { const char* e = getenv("DQIX_WALKLOG"); wl = e ? atoll(e) : -1; }   /* tests */
        if (wl >= 0 && (long long)g_frame >= wl && (long long)g_frame < wl + 200) {
            uint32_t fp; memcpy(&fp, &g_main_ram[FIELD_PTR], 4);
            fprintf(stderr, "walk f%llu flag %d field %08x touch %d %d,%d\n", (unsigned long long)g_frame, g_main_ram[WALK_FLAG], fp, g_touch_down, g_touch_x, g_touch_y);
        }
    }
    {   /* the stylus arrow (tags.txt, tag 1): hidden while the stick is walking (and a few frames
         * after), shown for the mouse and the touch screen. Only the picture: the game draws it either way. */
        extern int g_gx_hide;
        static int since = 1000;
        since = g_walk_frames > 0 ? 0 : since + 1;
        g_gx_hide = since < 8 && !g_fe_touch ? (g_gx_hide | 1) : (g_gx_hide & ~1);
    }
    if (session_running()) {                            /* lockstep: what was pressed `delay` frames ago, on every PC */
        uint32_t v = session_frame_input(g_frame, session_pack(g_keys_down, g_touch_down, g_touch_x, g_touch_y));
        session_unpack(v, &g_keys_down, &g_touch_down, &g_touch_x, &g_touch_y);
    }
    record(g_keys_down, g_touch_down, g_touch_x, g_touch_y);
}

/* Register views (active-low). */
uint16_t input_keyinput(void) { return (uint16_t)(~g_keys_down & 0x3FF); }
uint16_t input_extkeyin(void) {
    uint16_t v = 0x7F;
    if (g_keys_down & (1 << 10)) v &= ~1;            /* X */
    if (g_keys_down & (1 << 11)) v &= ~2;            /* Y */
    if (g_touch_down) v &= ~0x40;                     /* pen down */
    if (g_keys_down & (1 << 12)) v |= 0x80;           /* hinge: the DS is closed */
    return v;
}

/* Touch screen ADC values, using the calibration in our firmware image:
 * ADC 0x200 -> pixel 0x20, ADC 0xE00 -> pixel 0xE0 (x) / 0xA0 (y). */
uint16_t input_touch_adc(int channel) {
    if (!g_touch_down) return channel == 1 || channel == 5 ? 0 : 0x800;
    /* invert the firmware's touch calibration (two ADC -> pixel points) */
    const uint8_t* u = firmware_user_settings();
    int ax1 = u[0x58] | u[0x59] << 8, ay1 = u[0x5A] | u[0x5B] << 8, px1 = u[0x5C], py1 = u[0x5D];
    int ax2 = u[0x5E] | u[0x5F] << 8, ay2 = u[0x60] | u[0x61] << 8, px2 = u[0x62], py2 = u[0x63];
    /* the converter reads 0..4095; it can't go past either end */
    /* the middle of the pixel's ADC step, so the game's rounding lands on it */
    if (channel == 5) { int v = ax1 + ((g_touch_x - px1) * 2 + 1) * (ax2 - ax1) / (2 * (px2 - px1)); return (uint16_t)(v < 0 ? 0 : v > 0xFFF ? 0xFFF : v); }
    if (channel == 1) { int v = ay1 + ((g_touch_y - py1) * 2 + 1) * (ay2 - ay1) / (2 * (py2 - py1)); return (uint16_t)(v < 0 ? 0 : v > 0xFFF ? 0xFFF : v); }
    if (channel == 3) return 0x100;                   /* Z1 pressure */
    if (channel == 4) return 0xF00;                   /* Z2 */
    return 0x800;
}
