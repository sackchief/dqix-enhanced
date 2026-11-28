/* Free camera (Modern > Free camera): the field camera turns with L/R everywhere,
 * all the way round, also where the game locks it and inside camera zones
 * (which turn L/R off and may set an angle of their own). The change is in the
 * recompiled code (patches.txt, "if free_camera"): it reads g_patch_free_camera.
 *
 * Off while playing together (the consoles' memory must stay identical, and
 * the camera's angle is in it) and in replays of bug reports (they stay exact).
 * While it is on, the 3D draws both sides of every polygon (gpu3d.c): walls,
 * bushes and trees made of one-sided polygons, meant to be seen from the
 * game's angles only, would otherwise vanish from behind. Polygons that
 * aren't modelled at all (the back of a building nobody was meant to see)
 * stay missing. free_camera_both_sides = 0 in settings.ini (or
 * DQIX_FREE_CAMERA_ONE_SIDED=1) leaves the DS's culling alone, to compare.
 *
 * Camera at its usual distance everywhere (Modern menu, g_patch_camera_no_zoom):
 * places with a camera zone of their own (a courtyard seen from far above)
 * no longer pull the camera away; patches.txt, "if camera_no_zoom". The
 * same rules: off while playing together and in replays.
 *
 * Zoom (while the free camera is on): the mouse wheel, the right stick
 * up/down and two keys (Game > Controls: "Camera closer" / "Camera further",
 * E and R) change a factor from 40% to 300%. The game places the field camera
 * with func_0202e5d8 every frame (wrap.txt, camera_orbit): afterwards, the
 * camera's offset from the point it looks at is scaled by the factor. The
 * game's own height and distance (+0x74/+0x78) stay as they are, so nothing
 * adds up from frame to frame, and only the field camera object (noted by
 * wrap_field_camera, func_020a212c with input on) is touched. Same rules: off while playing
 * together and in replays (it is off with the free camera). Zoomed out
 * further than usual, the fog is left out (gpu3d.c, gl_render.c): the game
 * sets it up for its own distance, and from further away the whole field
 * would be in it.
 *
 * L+R together (the game's own recentring, which the free camera keeps) also
 * sets the zoom back to 100%.
 *
 * Headless tests: DQIX_FREE_CAMERA=1, DQIX_CAMERA_NO_ZOOM=1,
 * DQIX_CAMERA_ZOOM=<percent> (the starting zoom). */
#include <stdlib.h>
#include <stdint.h>
#include "free_camera.h"
#include "session.h"
#include "cpu.h"
#include "nds.h"
#include <stdio.h>

int g_patch_free_camera;
int g_patch_free_camera_lr;                        /* patches.txt: L/R past the game's locks (off during scenes) */
int g_free_camera_both_sides = 1;
int g_patch_camera_no_zoom;
static int g_want = -1, g_want_nz = -1;

static int allowed(void) { return !getenv("DQIX_INPUT_FILE") && !session_running(); }
void camera_no_zoom_update(int want) {
    if (g_want_nz < 0 || want >= 0) g_want_nz = want < 0 ? getenv("DQIX_CAMERA_NO_ZOOM") && atoi(getenv("DQIX_CAMERA_NO_ZOOM")) : want;
    g_patch_camera_no_zoom = g_want_nz && allowed();
}

void free_camera_update(int want) {
    if (g_want < 0 || want >= 0) g_want = want < 0 ? getenv("DQIX_FREE_CAMERA") && atoi(getenv("DQIX_FREE_CAMERA")) : want;
    g_patch_free_camera = g_want && allowed();
    g_patch_free_camera_lr = g_patch_free_camera;
}

#if defined(__GNUC__)
__attribute__((constructor)) static void free_camera_init(void) {
    free_camera_update(-1);
    camera_no_zoom_update(-1);
    if (getenv("DQIX_FREE_CAMERA_ONE_SIDED")) g_free_camera_both_sides = 0;
}
#endif

/* ---------------------------------------------------------------- zoom */
#define ZOOM_ONE 4096                              /* 1.0, the game's own distance */
#define ZOOM_MIN (ZOOM_ONE * 40 / 100)
#define ZOOM_MAX (ZOOM_ONE * 3)
static int32_t g_zoom = ZOOM_ONE;
static uint32_t g_field_cam;

static void zoom_set(double z) {
    int32_t v = (int32_t)(z + 0.5);
    g_zoom = v < ZOOM_MIN ? ZOOM_MIN : v > ZOOM_MAX ? ZOOM_MAX : v;
}
/* Cutscenes: the game's "free to walk" byte (02109da4) is 2 while a scene
 * plays (1 walking; 0 talking and the boat). It is 2 in the field menu and
 * at a battle's start too; the byte after it tells them apart: 1 in a scene
 * (as while walking), 2/3 in the menu, 0 in a battle. Only once the scene's
 * own camera has taken over (the field camera hasn't run for 20 frames;
 * saving at a priest pauses it for up to ~13)
 * is it a cutscene here: then walls are drawn one-sided as on the DS (scene
 * cameras may stand behind a wall whose back the DS doesn't draw) and the
 * zoom is left out. The scene camera sets its own view, so the player's
 * angle needn't be touched; it is the same again when walking resumes.
 * Talking to someone (also the scenes some people start, such as a priest,
 * while the field camera keeps running) changes nothing. While the bytes say
 * scene, L/R and the zoom don't move the camera (L/R follow the game's own
 * locks then: the patches 'if free_camera_lr' are off), so it can't be turned
 * into a wall during a scene's first part, which still uses the field
 * camera. */
static uint64_t g_cam_frame;                       /* the last frame the field camera ran */
static int scene_bytes(void) { return g_main_ram[0x109DA4] == 2 && g_main_ram[0x109DA5] == 1; }
int free_camera_cutscene(void) { return scene_bytes() && g_frame - g_cam_frame > 20; }
int free_camera_both_sides_now(void) { return g_patch_free_camera && g_free_camera_both_sides && !free_camera_cutscene(); }
int free_camera_no_fog(void) { return g_patch_free_camera && g_zoom > ZOOM_ONE && !free_camera_cutscene(); }
int free_camera_zoom_percent(void) { return (g_zoom * 100 + ZOOM_ONE / 2) / ZOOM_ONE; }

void free_camera_zoom_frame(int zoom_in, int zoom_out, int stick_y) {
    if (!g_patch_free_camera || scene_bytes()) return;
    double f = 1.0;
    if (zoom_in && !zoom_out) f = 1.0 / 1.018;     /* about 2.9 times closer or further in a second */
    if (zoom_out && !zoom_in) f = 1.018;
    int m = stick_y < 0 ? -stick_y : stick_y;
    if (f == 1.0 && m > 9000) {                    /* up: closer; the further pushed, the faster */
        double r = 0.018 * (m - 9000) / (32767 - 9000);
        f = stick_y < 0 ? 1.0 / (1.0 + r) : 1.0 + r;
    }
    if (f != 1.0) zoom_set(g_zoom * f);
}

void free_camera_zoom_wheel(int notches) {
    if (!g_patch_free_camera || !notches || scene_bytes()) return;
    double z = g_zoom;
    for (int i = 0; i < (notches < 0 ? -notches : notches); i++) z = notches > 0 ? z / 1.09 : z * 1.09;
    zoom_set(z);
}

/* Swinging the camera to an angle (L+R inside a camera zone), 1.25 times as fast
 * as the game's own recentring (0x17c a camera step, the camera running every
 * other frame); angles are radians, 0x1000 = 1, a full turn 0x6488. Stops if
 * something else turns the camera (L or R). */
#define TURN 0x6488
static int g_swing;
static int32_t wrap_turn(int32_t d) { d %= TURN; if (d > TURN / 2) d -= TURN; else if (d < -TURN / 2) d += TURN; return d; }
static int32_t g_swing_to, g_swing_last;
static void swing_step(uint32_t cam) {
    int32_t a = (int32_t)mem_read32(cam + 0x70);
    if (a != g_swing_last || mem_read32(cam + 0x240)) { g_swing = 0; return; }   /* turned by the player, or out of the zone */
    int32_t d = wrap_turn(g_swing_to - a);
    if (d > 0x17c) a += 0x17c; else if (d < -0x17c) a -= 0x17c; else { a = g_swing_to; g_swing = 0; }
    a = (a % TURN + TURN) % TURN;
    mem_write32(cam + 0x220, (uint32_t)a);
    mem_write32(cam + 0x70, (uint32_t)a);
    g_swing_last = a;
}

void wrap_field_camera_orig(Cpu* c);
void wrap_field_camera(Cpu* c) {
    uint32_t r1 = c->r[1], cam = c->r[0];
    if (r1) { g_field_cam = c->r[0]; g_cam_frame = g_frame; }   /* r1 = 0: func_020a2b38's default view */
    int32_t before = (int32_t)mem_read32(cam + 0x70);
    g_patch_free_camera_lr = g_patch_free_camera && !scene_bytes();   /* a scene: L/R as the game has them */
    wrap_field_camera_orig(c);
    /* the game's recentring (L+R, +0x245 bit 1) 1.25 times as fast: a quarter more
     * of each of its steps, as long as that doesn't go past its target */
    if (r1 && g_patch_free_camera && (mem_read8(cam + 0x245) & 2) && mem_read32(cam + 0x240)) {
        int32_t after = (int32_t)mem_read32(cam + 0x70), to = (int16_t)mem_read16(cam + 0x246);
        int32_t d = wrap_turn(after - before), left = wrap_turn(to - after), e = d / 4;
        if (e && ((e > 0 && left > e) || (e < 0 && left < e))) {
            int32_t a = ((after + e) % TURN + TURN) % TURN;
            mem_write32(cam + 0x70, (uint32_t)a); mem_write32(cam + 0x220, (uint32_t)a);
        }
    }
    /* L+R together: the game swings the camera back to its usual angle (behind
     * the hero, or the map's own angle where it is fixed; +0x245 bit 1 while
     * it turns). The zoom goes back to the game's distance with it. */
    if (r1 && g_patch_free_camera && (mem_read8(cam + 0x245) & 2)) {
        g_zoom = ZOOM_ONE;
        /* Inside a camera zone (+0x240 0) the game's recentring doesn't run (on
         * the DS L/R do nothing there): the zone's own angle (+0x2ac) becomes
         * the camera's again: swing_step turns the camera there as the
         * game's recentring would. */
        if (!mem_read32(cam + 0x240)) { g_swing = 1; g_swing_to = (int32_t)mem_read32(cam + 0x2ac); g_swing_last = (int32_t)mem_read32(cam + 0x70); }
    }
    if (r1 && g_swing) swing_step(cam);
    static int log = -1; if (log < 0) log = getenv("DQIX_CAMLOG") != NULL;   /* tests */
    if (log) {
        uint32_t o = g_field_cam;
        fprintf(stderr, "cam f%llu r1=%u o=%08x 23c=%x 240=%x 244=%02x 245=%02x walk=%d takes_lr=%d yaw=%04x d=%x zoom=%d%% 220=%x 246=%x 2ac=%x\n", (unsigned long long)g_frame, (unsigned)r1, o,
                mem_read32(o + 0x23c), mem_read32(o + 0x240), mem_read8(o + 0x244), mem_read8(o + 0x245), g_main_ram[0x109DA4], field_camera_takes_lr(), mem_read32(o + 0x70), mem_read32(o + 0x78), free_camera_zoom_percent(), mem_read32(o + 0x220), mem_read16(o + 0x246), mem_read32(o + 0x2ac));
    }
}

/* L/R would turn the field camera now (input.c: the right stick presses
 * them only then): it ran in the last few frames, and isn't held still by
 * the game (+0x244 bit 1; +0x23c 0) or, without the free camera, by the
 * map's lock (+0x244 bit 4) or a camera zone (+0x240 0). On the boat the
 * game's "free to walk" flag is 0, but the camera turns with L/R. */
int field_camera_takes_lr(void) {
    uint32_t o = g_field_cam;
    if (!o || g_frame - g_cam_frame > 4 || scene_bytes()) return 0;     /* (the boat runs at half rate: every other frame) */
    uint8_t f = mem_read8(o + 0x244);
    if ((f & 2) || !mem_read32(o + 0x23c)) return 0;
    if (!g_patch_free_camera && ((f & 0x10) || !mem_read32(o + 0x240))) return 0;
    return 1;
}

void wrap_camera_orbit_orig(Cpu* c);
void wrap_camera_orbit(Cpu* c) {
    uint32_t cam = c->r[0];
    wrap_camera_orbit_orig(c);
    static int test = -1;
    if (test < 0) { const char* e = getenv("DQIX_CAMERA_ZOOM"); test = e != NULL; if (e) zoom_set(atof(e) * ZOOM_ONE / 100.0); }
    if (!g_patch_free_camera || g_zoom == ZOOM_ONE || cam != g_field_cam || !cam || free_camera_cutscene()) return;
    for (uint32_t i = 0; i < 12; i += 4) {          /* position = point looked at + offset * zoom */
        int32_t t = (int32_t)mem_read32(cam + 0x10 + i), p = (int32_t)mem_read32(cam + 0x04 + i);
        int64_t d = (int64_t)(p - t) * g_zoom;
        mem_write32(cam + 0x04 + i, (uint32_t)(t + (int32_t)(d >> 12)));
    }
}
