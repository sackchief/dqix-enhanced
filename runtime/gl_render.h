/* OpenGL renderer interface (gl_render.c). The SDL frontend creates the GL
 * context, then calls these. */
#pragma once
#include <stdint.h>
#include "gpu3d.h"

int gl_render_init(int scale);                        /* 0 on failure (use the software path) */
void gl_render_set_scale(int scale);
#define GL_MAX_SCALE 12                               /* the highest 3D resolution offered */
int gl_render_max_scale(void);                        /* ...that this GPU can hold */
void gl_set_smooth(int on);                           /* smooth textures (filtered) instead of the DS's blocks */
void gl_set_texture_pack(int on);                     /* HD textures: textures/<name>.png drawn instead of the game's */
void gl_set_texture_dump(int on);                     /* write every new texture to textures/dump/ (for making a pack) */
void gl_set_smooth2d(int on);                         /* Enhanced: the 2D screens smoothed (xBR) */
void gl_set_tex_upscale(int on);                      /* Enhanced: the 3D textures upscaled (xBR, 4x) */
int gl_reload_texture_pack(void);                     /* look in textures/ again; how many were found */
const Renderer3D* gl_renderer3d(void);
void gl_compose(int a_on_top, int lcd_on);           /* at VBlank: 2D + 3D -> both screens */
/* How to show the screens: canvas (DS pixels) placed in the window at
 * (ox, oy) with sc window pixels per DS pixel. */
typedef struct {
    int win_w, win_h;
    float ox, oy, sc, cw, ch;
    float top[4], bot[4];                             /* screen rects on the canvas: x, y, w, h */
    float topx[2], botx[2];                           /* each screen in the composed picture: x offset, width (DS pixels) */
    int front;                                        /* screens overlapping: 1 = the bottom one in front */
    float alpha;                                      /* ...how solid the one in front is (0 or 1 = solid) */
    int again;                                        /* another window, same frame: don't advance the motion blur */
    int lcd, lcd_color, grid, ghost;                  /* DS Lite screen filter and its parts */
    float backlight;                                  /* 0..1 */
    int shell;                                        /* draw the console around the screens */
    float lid[3], base[3], btn[3], hinge[3];          /* its colours (linear 0..1, as shown) */
    int skin;                                         /* draw the console photo (gl_set_skin) instead */
    float skin_k;                                     /* photo pixels per canvas unit */
} GlPresent;
void gl_set_skin(const uint8_t* rgba, int w, int h);
void gl_present(const GlPresent* v);
uint8_t* gl_read_window(int w, int h);
int gl_wide_shown(int* side);                         /* widescreen: the screen composed wide (0 top, 1 bottom, -1 none) */
uint8_t* gl_read_full(int* w, int* h);
void gl_read_screens(uint32_t out[2][256 * 192]);
int gl_read_3d(uint8_t* out, int* w, int* h);

/* Per-frame counters (the frontend reads and clears them). */
typedef struct { double render_ms, upload_ms; int uploads, polys; long upload_texels; } GlStats;
extern GlStats g_gl_stats;
const char* gl_renderer_name(void);
int gl_render_scale(void);
