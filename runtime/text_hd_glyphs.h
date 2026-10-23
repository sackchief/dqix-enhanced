/* HD text: the game's letters drawn at high resolution (alpha cells of an
 * atlas), from a TrueType font where the letter is one it has, or from the
 * game's own pixels made smooth. */
#pragma once
#include <stdint.h>

#define TXG_S 8                                   /* atlas pixels per DS pixel */
#define TXG_M 1                                   /* margin around a glyph (DS pixels) */
#define TXG_MAXW 16                               /* widest glyph (DS pixels) */
#define TXG_CW ((TXG_MAXW + 2 * TXG_M) * TXG_S)   /* a cell: 144 x 112 */
#define TXG_CH ((12 + 2 * TXG_M) * TXG_S)
#define TXG_COLS 28
#define TXG_ROWS 19                               /* 2 fonts x 256 glyphs */
#define TXG_W (TXG_COLS * TXG_CW)
#define TXG_GH (TXG_ROWS * TXG_CH)                /* the glyphs' part */
/* below them, cells for labels (text_hd.h): up to 120 x 18 DS pixels; 64 labels, then their outlines */
#define TXG_LMAXW 120
#define TXG_LMAXH 18
#define TXG_LCW ((TXG_LMAXW + 2 * TXG_M) * TXG_S)
#define TXG_LCH ((TXG_LMAXH + 2 * TXG_M) * TXG_S)
#define TXG_LCOLS (TXG_W / TXG_LCW)
#define TXG_LROWS 32
#define TXG_NLAB (TXG_LCOLS * TXG_LROWS)
#define TXG_H (TXG_GH + TXG_LROWS * TXG_LCH)
#define TXG_NCELLS (512 + TXG_NLAB)
#define TXG_CELLMAX (TXG_LCW * TXG_LCH)           /* the largest cell's bytes */

/* the atlas cell of a glyph (-1: none), and where it is (atlas pixels; its
 * width is also the stride of txg_render's cell) */
static inline int txg_cell(int font, int index) {
    if (font >= 0 && font <= 1 && index >= 0 && index < 256) return font * 256 + index;
    if (font == 2 && index >= 0 && index < 64) return 512 + index;          /* labels (text_hd.h) */
    if (font == 3 && index >= 64 && index < TXG_NLAB) return 512 + index;   /* ...their outlines (index: 64 + the label) */
    return -1;
}
static inline void txg_cell_rect(int c, int* x, int* y, int* w, int* h) {
    if (c < 512) { *x = (c % TXG_COLS) * TXG_CW; *y = (c / TXG_COLS) * TXG_CH; *w = TXG_CW; *h = TXG_CH; }
    else { c -= 512; *x = (c % TXG_LCOLS) * TXG_LCW; *y = TXG_GH + (c / TXG_LCOLS) * TXG_LCH; *w = TXG_LCW; *h = TXG_LCH; }
}

/* The TrueType fonts: dir/main.ttf for the game's main font (font 1, the
 * serif one), dir/small.ttf for the small one (font 0). Missing files: those
 * letters are made from the game's own pixels. */
void txg_load_fonts(const char* dir);
/* Draw a glyph into a cell (txg_cell_rect's size, 1 byte per pixel, 255 = ink);
 * the glyph's (0,0) is at (TXG_M, TXG_M) DS pixels. 0 if the game's font isn't loaded. */
int txg_render(int font, int index, uint8_t* cell);

/* A 16-colour texture with letters in it (TEXIMAGE_PARAM ti, palette pl,
 * decoded as rgba, w x h), TXG_S times larger with the letters in high
 * resolution, into out ((w*TXG_S) x (h*TXG_S) RGBA). 0 if it has no letters. */
int txg_texture_hd(uint32_t ti, uint32_t pl, const uint8_t* rgba, int w, int h, uint8_t* out);
void txg_forget_cells(void);
int txg_texture_label(uint32_t ti, uint32_t pl, const uint8_t* rgba, int w, int h, int label, int bx, int by, uint8_t* out);
