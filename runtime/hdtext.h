/* HD text: the game's text drawn again on the graphics card at full
 * resolution, from a TrueType font or the game's own letters smoothed.
 *
 * The game draws each letter (1 bit per pixel, 12 rows) into a 4-bit tile
 * buffer in main RAM (func_0205b36c, hooked: hooks.txt), which is later
 * copied into VRAM and shown on a background. Every pixel of a letter's box
 * gets a tag (which letter, where in it); the tags follow the copies (DMA,
 * the BIOS's CpuSet, memcpy) and are forgotten when anything else writes
 * there. The 2D engines then know, per screen pixel, which letter is shown
 * there: they leave the letters out of the picture and list them, and the
 * GL renderer draws them on top at full resolution (gl_render.c). */
#pragma once
#include <stdint.h>

extern int g_hdtext_on;                       /* set by the frontend (Enhanced only) */
extern int g_hdtext_any;                      /* some memory carries tags */

/* memory */
void hdtext_forget(const uint8_t* p, int size);                    /* a write: what was there is gone */
void hdtext_copy(uint32_t dst, uint32_t src, uint32_t bytes);       /* a copy (after the bytes moved) */
uint32_t hdtext_tag(uint32_t src2);           /* the tag of a VRAM pixel: src2 = byte address * 2 + high nibble */

/* tags: bit 31 set, then the letter (18 bits), x in it (5), y (4), ink (1) */
#define HDT_INST(t)  (((t) >> 13) & 0x3FFFF)
#define HDT_LX(t)    (((t) >> 8) & 31)
#define HDT_LY(t)    (((t) >> 4) & 15)
#define HDT_INK(t)   ((t) & 1)

/* per frame, from the 2D engines */
typedef struct { int x, y, w; int font, glyph; uint32_t color; int has_color; uint32_t inst; } HdGlyph;   /* glyph: for hdtext_render */
void hdtext_frame_start(void);
void hdtext_seen(int eng, uint32_t tag, int x, int y, int ink_shown, uint32_t rgb);   /* a letter's pixel shows at (x, y) (its ink in this colour) */
extern uint8_t g_hdtext_mask[2][256 * 192];   /* 255 where a letter's box shows */
int hdtext_glyphs(int eng, const HdGlyph** out);                    /* this frame's letters, in drawing order */

/* the letters at high resolution: R pixels per DS pixel, 12 rows */
#define HDT_R 8
void hdtext_set_font(const char* name);       /* "original", or a .ttf file (fonts/, the system's fonts) */
const char* hdtext_font_name(void);
int hdtext_font_ok(void);                     /* the TrueType font was found */
/* coverage (0-255) of one letter, (w * HDT_R) x (12 * HDT_R); 0 if unknown */
int hdtext_render(int glyph, uint8_t* out, int stride, int* w);
uint32_t hdtext_font_gen(void);               /* changes when the font does (the GPU's copies are stale) */
