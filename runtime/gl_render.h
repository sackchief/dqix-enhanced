/* OpenGL renderer interface (gl_render.c). The SDL frontend creates the GL
 * context, then calls these. */
#pragma once
#include <stdint.h>
#include "gpu3d.h"

int gl_render_init(int scale);                        /* 0 on failure (use the software path) */
void gl_render_set_scale(int scale);
#define GL_MAX_SCALE 12                               /* the highest 3D resolution offered */
int gl_render_max_scale(void);                        /* ...that this GPU can hold */
extern int g_gl_soft_shade;                           /* above 1x: the game's stepped (toon) shading as a smooth ramp */
extern int g_gl_bars;                                 /* widescreen: black sides (a scene made for the DS's width) */
extern int g_gl_soft_alpha;                           /* above 1x: textures with alpha levels drawn smooth */
/* Mods (texture packs: the textures folder; dumping; xBR upscaling): gl_render.c */
void gl_set_texture_pack(int on);
void gl_set_tex_upscale(int on);
void gl_set_texture_dump(int on);
int gl_reload_texture_pack(void);            /* how many pictures the packs have */
extern int g_tex_compare;                    /* held: the game's own textures */
extern int g_gl_outline;                              /* outlines above 1x: 1 thin, 2 medium, 0 the DS's own rule */
void gl_set_smooth(int on);                           /* smooth textures (filtered) instead of the DS's blocks */
void gl_set_hd_text(int on);                          /* HD text: the game's letters redrawn at full resolution (text_hd.h) */
void gl_hd_fonts(const char* dir);                    /* ...from dir/main.ttf, dir/small.ttf and dir/labels.bin */
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
    float badge[4];                                   /* fast forward's sign: x, y (from the top), w, h in window pixels; w 0 = none */
    int npiece;                                       /* parts of a screen drawn over the canvas (widescreen battle HUD) */
    float piece[24][10];                              /* ...each: x, y, w, h on the canvas; screen (0 top, 1 bottom); x, y, w, h on it;
                                                       * see-through: 0 solid, else how solid its fill is (its white lines and letters stay solid) */
    int held;                                         /* show the picture kept by gl_hold() instead of this frame's */
    int overlay;                                      /* draw the in-game menu's layer on top (gl_set_overlay) */
} GlPresent;
#define GL_MAX_PIECES 24
/* The in-game menu's layer: RGBA, rows top-down, the window's size; drawn over
 * the picture when GlPresent.overlay is set. rgba NULL: none. */
void gl_set_overlay(const uint8_t* rgba, int w, int h);
/* dynamic widescreen: the 3D screen's 2D windows moved right by px DS pixels (0: not); battle: only the battle messages */
void gl_set_ui_shift(int px, int battle);
int gl_ui_unshift(int* x, int y);
void gl_set_ui_hold(int on);                          /* a submenu: move nothing */
int gl_ui_area(void);                                 /* 8x8 cells under the 2D windows that would move */                     /* a moved window's point back to where it really is (1: it was one) */
void gl_hold(void);                                   /* keep this frame's picture (the map window during battles) */
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
