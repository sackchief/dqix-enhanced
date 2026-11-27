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
int free_camera_no_fog(void) { return g_patch_free_camera && g_zoom > ZOOM_ONE; }
int free_camera_zoom_percent(void) { return (g_zoom * 100 + ZOOM_ONE / 2) / ZOOM_ONE; }

void free_camera_zoom_frame(int zoom_in, int zoom_out, int stick_y) {
    if (!g_patch_free_camera) return;
    double f = 1.0;
    if (zoom_in && !zoom_out) f = 1.0 / 1.02;      /* about 3 times closer or further in a second */
    if (zoom_out && !zoom_in) f = 1.02;
    int m = stick_y < 0 ? -stick_y : stick_y;
    if (f == 1.0 && m > 9000) {                    /* up: closer; the further pushed, the faster */
        double r = 0.02 * (m - 9000) / (32767 - 9000);
        f = stick_y < 0 ? 1.0 / (1.0 + r) : 1.0 + r;
    }
    if (f != 1.0) zoom_set(g_zoom * f);
}

void free_camera_zoom_wheel(int notches) {
    if (!g_patch_free_camera || !notches) return;
    double z = g_zoom;
    for (int i = 0; i < (notches < 0 ? -notches : notches); i++) z = notches > 0 ? z / 1.1 : z * 1.1;
    zoom_set(z);
}

void wrap_field_camera_orig(Cpu* c);
static uint64_t g_cam_frame;                       /* the last frame the field camera ran */
void wrap_field_camera(Cpu* c) {
    if (c->r[1]) { g_field_cam = c->r[0]; g_cam_frame = g_frame; }   /* r1 = 0: func_020a2b38's default view */
    wrap_field_camera_orig(c);
    static int log = -1; if (log < 0) log = getenv("DQIX_CAMLOG") != NULL;   /* tests */
    if (log) {
        uint32_t o = g_field_cam;
        fprintf(stderr, "cam f%llu o=%08x 23c=%x 240=%x 244=%02x 245=%02x walk=%d takes_lr=%d yaw=%04x d=%x\n", (unsigned long long)g_frame, o,
                mem_read32(o + 0x23c), mem_read32(o + 0x240), mem_read8(o + 0x244), mem_read8(o + 0x245), g_main_ram[0x109DA4], field_camera_takes_lr(), mem_read32(o + 0x70), mem_read32(o + 0x78));
    }
}

/* L/R would turn the field camera now (input.c: the right stick presses
 * them only then): it ran in the last few frames, and isn't held still by
 * the game (+0x244 bit 1; +0x23c 0) or, without the free camera, by the
 * map's lock (+0x244 bit 4) or a camera zone (+0x240 0). On the boat the
 * game's "free to walk" flag is 0, but the camera turns with L/R. */
int field_camera_takes_lr(void) {
    uint32_t o = g_field_cam;
    if (!o || g_frame - g_cam_frame > 4) return 0;     /* (the boat runs at half rate: every other frame) */
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
    if (!g_patch_free_camera || g_zoom == ZOOM_ONE || cam != g_field_cam || !cam) return;
    for (uint32_t i = 0; i < 12; i += 4) {          /* position = point looked at + offset * zoom */
        int32_t t = (int32_t)mem_read32(cam + 0x10 + i), p = (int32_t)mem_read32(cam + 0x04 + i);
        int64_t d = (int64_t)(p - t) * g_zoom;
        mem_write32(cam + 0x04 + i, (uint32_t)(t + (int32_t)(d >> 12)));
    }
}
