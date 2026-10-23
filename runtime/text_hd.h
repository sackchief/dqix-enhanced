/* HD text: where the game's letters went, so the GPU renderer can draw them
 * again at full resolution (Graphics > HD text).
 *
 * The game draws text one glyph at a time into tiled canvases in main RAM
 * (4 bits per pixel), then copies them to VRAM by DMA. text_hd.c watches the
 * glyph routine (wrap.txt) and remembers, for every nibble a glyph set, which
 * glyph drew it and what was there before ("tags"). Tags follow the pixels
 * through DMA copies into VRAM, and any other write to a nibble forgets its
 * tag. The 2D renderer (gpu2d.c) reads the tags of the pixels it shows: so it
 * knows where each glyph is on screen, and what the picture looks like
 * without the text. The game runs exactly the same either way. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t inst;          /* glyph instance + 1 (0 = no tag) */
    uint8_t col, row;       /* this pixel inside the glyph */
    uint8_t under;          /* the nibble before any glyph was drawn on it */
    uint8_t gen;            /* the instance's generation (instances are reused) */
} TxTag;

typedef struct {
    /* what was drawn (set by the glyph routine) */
    uint32_t canvas;        /* the canvas's pixel memory */
    uint32_t glyph;         /* the glyph's table entry */
    int16_t x, y;           /* where on the canvas (the game's coordinates) */
    uint8_t font, color, w, gen;
    uint8_t h;              /* rows (12; labels may differ) */
    uint64_t lconf;         /* labels: g_frame + 1 when a tile of its own was seen */
    uint64_t made;          /* g_frame when (last) drawn */
    uint16_t index;         /* glyph number in its font */
    uint32_t serial;        /* drawing order */
    /* where it shows this frame, per engine (gpu2d.c) */
    uint64_t seen[2];       /* g_frame + 1 when seen on that engine this frame */
    int16_t ox[2], oy[2];   /* screen position of the glyph's top left */
    uint8_t layer[2];       /* the layer it shows on: BG 0-3, or 4 + sprite priority */
    uint32_t ink[2];        /* its colour as displayed (0xRRGGBB) */
    uint32_t ink2[2];       /* labels: their outline's colour as displayed (0xFF000000: not seen) */
} TxInst;

extern int g_tx_active;     /* tags are kept (HD text on, or a test asks for them) */
extern int g_tx_any;        /* some page has tags (fast exit for writes) */
extern int g_tx_shadows;    /* draw the letters' shadows and outlines in HD too (0: leave them out) */
int tx_is_shadow(const TxInst* in, int eng);

/* any write to main RAM / VRAM by the game: forget the tags of those bytes */
void tx_forget(uint32_t addr, int size);
extern TxTag* g_tx_ram_pages[];               /* per 4 KB of main RAM (NULL: no tags) */
static inline void tx_write(uint32_t addr, int size) {
    if (__builtin_expect(g_tx_any, 0)) {
        if ((addr >> 24) == 0x02) {               /* (an aligned write stays in its page) */
            TxTag* pg = g_tx_ram_pages[(addr & 0x3FFFFFu) >> 12];
            if (pg) { TxTag* t = &pg[(addr & 0xFFFu) * 2]; for (int i = 0; i < size * 2; i++) t[i].inst = 0; }
        } else if ((addr >> 24) == 0x06) tx_forget(addr, size);
    }
}
/* a CPU write that changed these bits (diff = old ^ new, little-endian): only
 * the nibbles that changed lose their tags (the game redraws a box's border
 * into the tile of a letter with a read-modify-write: the letter stays) */
void tx_forget_changed(uint32_t addr, int size, uint32_t diff);
static inline void tx_write_v(uint32_t addr, int size, uint32_t diff) {
    if (__builtin_expect(g_tx_any, 0)) {
        /* main RAM (the canvases): only what changed; VRAM: all of it (the game
         * uploads textures there with plain CPU copies: a texel that happens to
         * keep its value is still a new picture) */
        if ((addr >> 24) == 0x02) tx_forget_changed(addr, size, diff);
        else if ((addr >> 24) == 0x06) tx_forget(addr, size);
    }
}
/* DMA copied size bytes from src to dst (after the write): the tags follow */
void tx_copy(uint32_t dst, uint32_t src, int size);

/* the tags of the VRAM byte at p (pointer into g_vram), or NULL when none */
TxTag* tx_vram_tags(const uint8_t* p);
/* the tag of nibble hi (0 low, 1 high) of the VRAM byte at p, or NULL */
static inline const TxTag* tx_vram_tag(const uint8_t* p, int hi) {
    TxTag* t = tx_vram_tags(p);
    return t && t[hi].inst ? &t[hi] : NULL;
}
/* the instance a tag points to (NULL when it was reused since) */
TxInst* tx_inst_of(const TxTag* t);
uint32_t tx_inst_index(const TxInst* in);
TxInst* tx_inst_at(uint32_t index);
TxInst* tx_inst_clone(const TxInst* src);   /* a copy (in a new slot) */

/* glyph metrics from the game's own tables (for the HD atlas) */
int tx_font_glyph(int font, int index, char* name, int namelen, int* width, int* sheet_x);
const uint8_t* tx_font_sheet(int font, int* width_px, int* stride);   /* 1 bit per pixel, 12 rows, NULL if not loaded */
int tx_font_count(int font);

/* per frame, per engine (gpu2d.c) */
#define TX_LIST_MAX 4096
extern uint32_t g_tx_notext[2][256 * 192];   /* the picture without the letters (0xRRGGBB, like g_engine_out) */
extern uint8_t g_tx_vis[2][256 * 192];       /* layers a pixel of which would show there: BGs (bits 0-3), sprites of priority 0-3 (bits 4-7) */
extern uint32_t g_tx_list[2][TX_LIST_MAX];   /* the letters seen, in drawing order (instance indexes) */
extern int g_tx_n[2];
extern uint16_t g_tx_slot[2][256 * 192][4];  /* the letters over each pixel (up to 4, last on top): g_tx_list index + 1 */
void tx_frame_done(void);                    /* build g_tx_slot (gpu_frame_done) */

/* Labels the game has as pictures, not text ("Back", "HP", "Lv."...):
 * fonts/labels.bin (tools/labels.py) lists each label's text and the 4-bit
 * tiles its letters are in. Wherever one of those tiles is shown, its letter
 * pixels get tags of the label (made here), and the label is drawn again in HD
 * as a string (font 2 in the atlas). */
typedef struct {
    char text[48];
    uint8_t font;           /* 0 the small font, 1 the main one */
    uint16_t inkmask;       /* the colour indexes of its letters (and their outlines) */
    uint8_t bg, w, h;       /* background index; the label's box */
    uint8_t x0, top, x1, base;   /* its letters: left, top, right, baseline (in the box) */
    uint8_t main;           /* the letters' own colour index (the rest: outlines, shadows) */
    uint8_t rowbg[24];      /* per row, the colour behind the letters */
    uint8_t flags;          /* 1 bold, 2 outline drawn too (the original's look) */
    uint8_t ocell;          /* its outline's atlas cell (font 3): 64 + the label */
} TxLabel;
extern TxLabel* g_tx_labels;
extern int g_tx_nlabels;
void tx_labels_load(const char* path);
/* the label instance a 4-bit tile shown at (tsx, tsy) on that engine is part
 * of (NULL: none), and where the tile is in the label's box */
TxInst* tx_label_at(const uint8_t* tile32, int eng, int tsx, int tsy, int* dx, int* dy);
TxInst* tx_label_inst(int label, int eng, int ox, int oy);
int tx_tex_any(uint32_t teximage_param);   /* a texture has letters (tags) */
int tx_label_texture(uint32_t teximage_param, int* bx, int* by);   /* -1: not a label */
