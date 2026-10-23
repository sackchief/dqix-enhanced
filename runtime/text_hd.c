/* HD text: the tags (see text_hd.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "text_hd.h"

int g_tx_active;
int g_tx_shadows;           /* draw the letters' shadows / outlines too (they are left out by default) */
int g_tx_any;

/* ---------------------------------------------------------------- tag pages
 * One TxTag per nibble, allocated a 4 KB page at a time (only canvases and
 * the VRAM they are copied to ever get any). */
#define TX_PAGE 4096u
#define TX_RAM_PAGES (MAIN_RAM_SIZE / TX_PAGE)
#define TX_VRAM_PAGES ((sizeof g_vram + TX_PAGE - 1) / TX_PAGE)
TxTag* g_tx_ram_pages[TX_RAM_PAGES];
#define g_ram_pages g_tx_ram_pages
static TxTag* g_vram_pages[TX_VRAM_PAGES];

/* the page array and byte offset for an ARM9 address (NULL: not RAM / VRAM) */
static TxTag** page_slot(uint32_t a, uint32_t* off) {
    if ((a >> 24) == 0x02) { *off = a & (MAIN_RAM_SIZE - 1); return &g_ram_pages[*off / TX_PAGE]; }
    if ((a >> 24) == 0x06) {
        uint8_t* p = vram_ptr(a);
        if (!p) return NULL;
        *off = (uint32_t)(p - g_vram);
        return &g_vram_pages[*off / TX_PAGE];
    }
    return NULL;
}
static TxTag* tags_at(uint32_t a, int alloc) {
    uint32_t off;
    TxTag** pg = page_slot(a, &off);
    if (!pg) return NULL;
    if (!*pg) {
        if (!alloc) return NULL;
        *pg = calloc(TX_PAGE * 2, sizeof(TxTag));
        if (!*pg) return NULL;
        g_tx_any = 1;
    }
    return &(*pg)[(off % TX_PAGE) * 2];
}

void tx_forget(uint32_t a, int size) {
    for (int i = 0; i < size; i++) {
        TxTag* t = tags_at(a + i, 0);
        if (t) memset(t, 0, 2 * sizeof(TxTag));
    }
}

static int g_in_glyph;                            /* inside a glyph routine: it rewrites its neighbours' bytes */
void tx_forget_changed(uint32_t a, int size, uint32_t diff) {
    for (int i = 0; i < size; i++, diff >>= 8) {
        TxTag* t = tags_at(a + i, 0);
        if (!t) continue;
        for (int k = 0; k < 2; k++) {
            if (!t[k].inst) continue;
            if (diff & (0x0Fu << (k * 4))) { t[k].inst = 0; continue; }
            /* kept as it was: the letter stays only if it was just drawn (a
             * box's border drawn into its tile right after the text); an
             * older one is a new picture that happens to have that value */
            if (g_in_glyph) continue;                  /* the routine drawing the next letter: the ones before stay */
            TxInst* in = tx_inst_of(&t[k]);
            if (!in || in->made + 2 < g_frame) t[k].inst = 0;
        }
    }
}

void tx_copy(uint32_t dst, uint32_t src, int size) {
    /* dst's tags become src's (none where src has none: a CPU write keeps the
     * tags of nibbles it doesn't change, a copy replaces the picture) */
    for (int i = 0; i < size;) {
        uint32_t so, d_o;
        TxTag** sp = page_slot(src + i, &so);
        TxTag** dp = page_slot(dst + i, &d_o);
        int run = (int)(TX_PAGE - so % TX_PAGE), rd = (int)(TX_PAGE - d_o % TX_PAGE);
        if (rd < run) run = rd;
        if (run > size - i) run = size - i;
        int s_has = sp && *sp, d_has = dp && *dp;
        if (!s_has) {
            if (d_has) memset(&(*dp)[(d_o % TX_PAGE) * 2], 0, (size_t)run * 2 * sizeof(TxTag));
            i += run; continue;
        }
        TxTag* st = &(*sp)[(so % TX_PAGE) * 2];
        int any = 0;
        for (int k = 0; k < run * 2; k++) if (st[k].inst) { any = 1; break; }
        if (!any) { if (d_has) memset(&(*dp)[(d_o % TX_PAGE) * 2], 0, (size_t)run * 2 * sizeof(TxTag)); i += run; continue; }
        TxTag* dt = tags_at(dst + i, 1);
        if (dt) memmove(dt, st, (size_t)run * 2 * sizeof(TxTag));
        i += run;
    }
}

TxTag* tx_vram_tags(const uint8_t* p) {
    uint32_t off = (uint32_t)(p - g_vram);
    if (off >= sizeof g_vram) return NULL;
    TxTag* pg = g_vram_pages[off / TX_PAGE];
    return pg ? &pg[(off % TX_PAGE) * 2] : NULL;
}

/* ---------------------------------------------------------------- instances
 * One per glyph drawn (the same glyph drawn again at the same place on the
 * same canvas in the same colour is the same instance: menus redraw a lot).
 * A ring; a tag remembers its instance's generation, so a reused slot isn't
 * mistaken for the old glyph. */
#define TX_INSTS (1u << 18)
#define TX_HASH (1u << 16)
static TxInst g_inst[TX_INSTS];
static uint32_t g_inst_next = 0;
static uint32_t g_hash[TX_HASH];                  /* instance + 1 */

TxInst* tx_inst_at(uint32_t i) { return i < TX_INSTS ? &g_inst[i] : NULL; }
uint32_t tx_inst_index(const TxInst* in) { return (uint32_t)(in - g_inst); }
TxInst* tx_inst_of(const TxTag* t) {
    if (!t || !t->inst || t->inst > TX_INSTS) return NULL;
    TxInst* in = &g_inst[t->inst - 1];
    return in->gen == t->gen ? in : NULL;
}

/* the same letter shown at another place too (the game copies a drawn word to
 * several boxes): a copy of the instance for each place, made per frame */
/* the next free slot of the ring: never one whose letter is on screen (seen
 * in the last frames), or it would go back to the old font */
static uint32_t slot_next(void) {
    for (int tries = 0; tries < 4096; tries++) {
        uint32_t i = g_inst_next;
        g_inst_next = (g_inst_next + 1) % TX_INSTS;
        const TxInst* in = &g_inst[i];
        if (in->seen[0] + 4 > g_frame + 1 || in->seen[1] + 4 > g_frame + 1) continue;   /* (seen: g_frame + 1) */
        return i;
    }
    uint32_t i = g_inst_next; g_inst_next = (g_inst_next + 1) % TX_INSTS; return i;
}
TxInst* tx_inst_clone(const TxInst* src) {
    TxInst* in = &g_inst[slot_next()];
    uint8_t gen = (uint8_t)(in->gen + 1);
    *in = *src;
    in->gen = gen ? gen : 1;
    return in;
}

static uint32_t hash_key(uint32_t canvas, uint32_t glyph, int x, int y, int color, int font) {
    uint32_t h = canvas * 0x9E3779B1u ^ glyph * 0x85EBCA77u ^ (uint32_t)(x & 0xFFFF) * 0xC2B2AE3Du ^ (uint32_t)(y & 0xFFFF) * 0x27D4EB2Fu ^ (uint32_t)(color | font << 8) * 0x165667B1u;
    return (h ^ (h >> 15)) & (TX_HASH - 1);
}
static TxInst* inst_find(uint32_t canvas, uint32_t glyph, int x, int y, int color, int font) {
    uint32_t i = g_hash[hash_key(canvas, glyph, x, y, color, font)];
    if (!i) return NULL;
    TxInst* in = &g_inst[i - 1];
    return in->canvas == canvas && in->glyph == glyph && in->x == x && in->y == y && in->color == color && in->font == font ? in : NULL;
}
static uint32_t g_serial;
static TxInst* inst_get(uint32_t canvas, uint32_t glyph, int x, int y, int color, int font) {
    uint32_t h = hash_key(canvas, glyph, x, y, color, font);
    uint32_t i = g_hash[h];
    if (i) {
        TxInst* in = &g_inst[i - 1];
        if (in->canvas == canvas && in->glyph == glyph && in->x == x && in->y == y && in->color == color && in->font == font) {
            in->made = g_frame;
            if (font <= 1) in->serial = ++g_serial;   /* drawn again: now (the drawing order tells a letter from its shadow) */
            return in;
        }
    }
    uint32_t slot = slot_next();
    TxInst* in = &g_inst[slot];
    uint8_t gen = (uint8_t)(in->gen + 1);
    memset(in, 0, sizeof *in);
    in->gen = gen ? gen : 1;
    in->serial = ++g_serial;
    in->h = 12; in->made = g_frame;
    in->canvas = canvas; in->glyph = glyph; in->x = (int16_t)x; in->y = (int16_t)y; in->color = (uint8_t)color; in->font = (uint8_t)font;
    g_hash[h] = slot + 1;
    return in;
}

/* ---------------------------------------------------------------- the game's fonts
 * fonts:  [0x0210782C + font*4] -> +4 glyph count, +0x10 glyph table (8 bytes
 *         each: +0 the character's bytes, +4 width, +5 byte count (low 6
 *         bits), +6 x in the sheet)
 * sheets: [0x02107834 + font*4] -> +4 width in pixels, +0xC 1-bit pixels
 *         (12 rows, most significant bit first) */
#define FONT_TABLE 0x0210782Cu
#define SHEET_TABLE 0x02107834u
/* main RAM (with its mirrors) or the data TCM (the stack): reads without side effects */
static int ram_ok(uint32_t a) { return (a >> 24) == 0x02; }
static uint32_t rd32(uint32_t a) { return mem_read32(a); }
static uint16_t rd16(uint32_t a) { return mem_read16(a); }
static uint8_t rd8(uint32_t a) { return mem_read8(a); }

int tx_font_count(int font) {
    if (font < 0 || font > 1) return 0;
    uint32_t f = rd32(FONT_TABLE + font * 4);
    if (!ram_ok(f)) return 0;
    uint32_t n = rd32(f + 4);
    return n > 4096 ? 0 : (int)n;
}
int tx_font_glyph(int font, int index, char* name, int namelen, int* width, int* sheet_x) {
    if (index < 0 || index >= tx_font_count(font)) return 0;
    uint32_t f = rd32(FONT_TABLE + font * 4), tab = rd32(f + 0x10);
    if (!ram_ok(tab)) return 0;
    uint32_t e = tab + index * 8, s = rd32(e);
    int len = rd8(e + 5) & 0x3F;
    if (name) {
        int n = 0;
        if (ram_ok(s)) for (; n < len && n < namelen - 1; n++) name[n] = (char)rd8(s + n);
        name[n] = 0;
    }
    if (width) *width = (int8_t)rd8(e + 4);
    if (sheet_x) *sheet_x = (int16_t)rd16(e + 6);
    return 1;
}
const uint8_t* tx_font_sheet(int font, int* width_px, int* stride) {
    if (font < 0 || font > 1) return NULL;
    uint32_t s = rd32(SHEET_TABLE + font * 4);
    if (!ram_ok(s)) return NULL;
    uint32_t bm = rd32(s + 0xC);
    if (!ram_ok(bm)) return NULL;
    int w = rd16(s + 4);
    if (width_px) *width_px = w;
    if (stride) *stride = w >> 3;
    return &g_main_ram[bm & (MAIN_RAM_SIZE - 1)];
}

/* ---------------------------------------------------------------- the glyph routine
 * func_0205b36c(canvas, glyph, x, y, [sp] colour, [sp+4] font): for each set
 * pixel of the glyph (12 rows, `width` columns), the canvas region holding
 * (x+col, y+row) is found (func_0205b1ac) and that pixel's nibble set to the
 * colour. The same addresses are worked out here, before (what was there)
 * and after the call (tag what it drew). Only reads: the game is unaffected. */
void fn_main_0205b36c_orig(Cpu* c);

typedef struct { uint32_t addr; uint8_t hi, col, row, under; TxTag keep; } Px;

static int region_find(uint32_t list, int count, int px, int py, uint32_t* off, int* rx, int* ry, int* rw) {
    for (int i = 0; i < count; i++) {
        uint32_t e = list + i * 12;
        int x0 = (int16_t)rd16(e + 4), y0 = (int16_t)rd16(e + 6), w = (int16_t)rd16(e + 8), h = (int16_t)rd16(e + 10);
        if (px >= x0 && px < x0 + w && py >= y0 && py < y0 + h) { *off = rd32(e); *rx = x0; *ry = y0; *rw = w; return 1; }
    }
    return 0;
}

void fn_main_0205b36c(Cpu* c) {
    if (!g_tx_active) { fn_main_0205b36c_orig(c); return; }
    uint32_t canvas_obj = c->r[0], glyph = c->r[1];
    int x = (int16_t)c->r[2], y = (int16_t)c->r[3];
    uint32_t sp = c->r[13];
    int color = rd8(sp) & 15, font = rd8(sp + 4);
#ifdef DQIX_PIXDBG
    { static int lg = -1; if (lg < 0) lg = getenv("DQIX_GLYPHLOG") != NULL;
      if (lg) { uint32_t sp_ = ram_ok(glyph) ? rd32(glyph) : 0; char ch[8] = {0}; for (int k = 0; k < 4 && ram_ok(sp_ + k); k++) { ch[k] = (char)rd8(sp_ + k); if (!ch[k]) break; }
          fprintf(stderr, "[f%llu] glyph font %d '%s' x %d y %d colour %d canvas %08x lr %08x\n", (unsigned long long)g_frame, font, ch, x, y, color, ram_ok(canvas_obj) ? rd32(canvas_obj) : 0, c->r[14]);
          { void trace_dump(FILE*, int); const char* w = getenv("DQIX_GLYPHTRACE"); if (w && !strcmp(w, ch)) trace_dump(stderr, 40); } } }
#endif
    static Px px[12 * 128];
    int n = 0, gw = 0, index = -1;
    if (ram_ok(canvas_obj) && ram_ok(glyph) && font <= 1) {
        uint32_t base = rd32(canvas_obj), list = rd32(canvas_obj + 4);
        int count = rd8(canvas_obj + 8);
        uint32_t sheet = rd32(SHEET_TABLE + font * 4);
        uint32_t f = rd32(FONT_TABLE + font * 4);
        if (ram_ok(base) && ram_ok(list) && count && ram_ok(sheet) && ram_ok(f)) {
            uint32_t bm = rd32(sheet + 0xC);
            int stride = rd16(sheet + 4) >> 3;
            int gx = (int16_t)rd16(glyph + 6);
            gw = (int8_t)rd8(glyph + 4);
            uint32_t tab = rd32(f + 0x10);
            if (glyph >= tab && (glyph - tab) % 8 == 0) index = (int)((glyph - tab) / 8);
            for (int row = 0; row < 12 && ram_ok(bm); row++) {
                uint32_t src = bm + stride * row + (gx >> 3);
                int bit = 7 - (gx & 7);
                for (int col = 0; col < gw && col < 128; col++) {
                    if ((rd8(src) >> bit) & 1) {
                        int sx = (int16_t)(x + col), sy = (int16_t)(y + row);
                        uint32_t off; int rx, ry, rw;
                        if (region_find(list, count, sx, sy, &off, &rx, &ry, &rw)) {
                            int lx = (int16_t)(sx - rx), ly = (int16_t)(sy - ry);
                            int tile = (int16_t)((ly >> 3) * (rw >> 3) + (lx >> 3));
                            uint32_t a = base + off + tile * 32 + (ly & 7) * 4 + ((lx & 7) >> 1);
                            if (ram_ok(a)) {
                                Px* p = &px[n++];
                                p->addr = a; p->hi = sx & 1; p->col = (uint8_t)col; p->row = (uint8_t)row;
                                p->under = (rd8(a) >> (p->hi * 4)) & 15;
                                TxTag* t = tags_at(a, 0);
                                if (t && t[p->hi].inst) p->keep = t[p->hi]; else p->keep.inst = 0;
                            }
                        }
                    }
                    if (--bit < 0) { bit = 7; src++; }
                }
            }
        }
    }
    g_in_glyph++; fn_main_0205b36c_orig(c); g_in_glyph--;
    { static int lg = -1; if (lg < 0) lg = getenv("DQIX_TXLOG") != NULL;
      if (lg) fprintf(stderr, "[f%llu] glyph font %d idx %d at %d,%d col %d canvas %08x px %d\n", (unsigned long long)g_frame, font, index, x, y, color, ram_ok(canvas_obj) ? rd32(canvas_obj) : 0, n); }
    if (!n || index < 0) return;
    TxInst* in = inst_get(rd32(canvas_obj), glyph, x, y, color, font);
    in->w = (uint8_t)gw; in->index = (uint16_t)index;
    uint32_t id = tx_inst_index(in) + 1;
    for (int i = 0; i < n; i++) {
        Px* p = &px[i];
        { int v = (rd8(p->addr) >> (p->hi * 4)) & 15;                     /* not drawn after all (outside the canvas): left as it was */
          if (!v || (v != color && v == p->under)) continue; }             /* (coloured text can use several colours: Coup de Grâce) */
        TxTag* t = tags_at(p->addr, 1);
        if (!t) continue;
        TxTag* d = &t[p->hi];
        /* what is under: the first glyph's background (a shadow drawn first, then the letter) */
        uint8_t under = p->keep.inst && tx_inst_of(&p->keep) ? p->keep.under : p->under;
        d->inst = id; d->gen = in->gen; d->col = p->col; d->row = p->row; d->under = under;
    }
}

/* ---------------------------------------------------------------- per frame
 * Each letter seen covers its cell (its width x 12 rows, and a pixel around
 * it for the high-resolution letter's edges) wherever its BG would show. */
static int by_serial_eng;
/* a shadow or outline: the same letter drawn again a pixel away, later (the letter itself) */
int tx_is_shadow(const TxInst* a, int eng) {
    for (int j = 0; j < g_tx_n[eng]; j++) {
        const TxInst* b = &g_inst[g_tx_list[eng][j]];
        if (b == a || b->serial <= a->serial || b->glyph != a->glyph || b->canvas != a->canvas || b->font != a->font) continue;
        int dx = b->x - a->x, dy = b->y - a->y;
        if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && (dx || dy)) return 1;
    }
    return 0;
}
static int by_serial(const void* a, const void* b) {
    const TxInst* ia = &g_inst[*(const uint32_t*)a]; const TxInst* ib = &g_inst[*(const uint32_t*)b];
    (void)by_serial_eng;
    if (ia->serial != ib->serial) return ia->serial < ib->serial ? -1 : 1;
    return (ib->font == 3) - (ia->font == 3);                  /* a label's outline before its letters */
}
void tx_frame_done(void) {
    { static int st = -1; if (st < 0) st = getenv("DQIX_TXSTAT") != NULL;
      if (st && g_frame % 50 == 0) {
          int r = 0, v = 0, rt = 0, vt = 0;
          for (unsigned i = 0; i < TX_RAM_PAGES; i++) if (g_ram_pages[i]) { r++; for (unsigned k = 0; k < TX_PAGE * 2; k++) rt += g_ram_pages[i][k].inst != 0; }
          for (unsigned i = 0; i < TX_VRAM_PAGES; i++) if (g_vram_pages[i]) { v++; for (unsigned k = 0; k < TX_PAGE * 2; k++) vt += g_vram_pages[i][k].inst != 0; }
          fprintf(stderr, "[f%llu] tx: active %d any %d ram pages %d (%d tags) vram pages %d (%d tags) seen %d/%d next %u\n", (unsigned long long)g_frame, g_tx_active, g_tx_any, r, rt, v, vt, g_tx_n[0], g_tx_n[1], g_inst_next);
      } }
    if (!g_tx_active) return;
    {   extern uint8_t g_tx_line_now[2][192], g_tx_line_near[2][192];
        for (int e = 0; e < 2; e++) {
            for (int y = 0; y < 192; y++) g_tx_line_near[e][y] = g_tx_line_now[e][y] | (y > 0 ? g_tx_line_now[e][y - 1] : 0) | (y < 191 ? g_tx_line_now[e][y + 1] : 0);
            memset(g_tx_line_now[e], 0, 192);
        }
    }
    memset(g_tx_slot, 0, sizeof g_tx_slot);
    for (int eng = 0; eng < 2; eng++) {
        /* labels drawn with their outline (style outline): a second instance,
         * the outline's cell in the outline's colour, under the letters */
        for (int i = 0, n0 = g_tx_n[eng]; i < n0 && g_tx_n[eng] < TX_LIST_MAX; i++) {
            TxInst* in = &g_inst[g_tx_list[eng][i]];
            if (in->font != 2 || in->index >= g_tx_nlabels) continue;
            const TxLabel* Lb = &g_tx_labels[in->index];
            if (!(Lb->flags & 2)) continue;
            if (Lb->ocell == 0xFF || in->ink2[eng] == 0xFF000000u || in->ink[eng] == 0xFF000000u) continue;
            TxInst* o = inst_get(0xF1B00000u + (uint32_t)eng, in->index, in->ox[eng], in->oy[eng], 0, 3);
            o->index = Lb->ocell; o->w = in->w; o->h = in->h; o->serial = in->serial;
            o->ox[eng] = in->ox[eng]; o->oy[eng] = in->oy[eng]; o->layer[eng] = in->layer[eng];
            o->ink[eng] = in->ink2[eng]; o->seen[eng] = g_frame + 1;
            g_tx_list[eng][g_tx_n[eng]++] = tx_inst_index(o);
        }
        int n = g_tx_n[eng];
        qsort(g_tx_list[eng], n, sizeof g_tx_list[eng][0], by_serial);
        for (int i = 0; i < n; i++) {
            TxInst* in = &g_inst[g_tx_list[eng][i]];
            if (!g_tx_shadows && tx_is_shadow(in, eng)) continue;   /* its pixels are gone (the picture without the letters); no HD copy */
            if (in->ink[eng] == 0xFF000000u) continue;                  /* none of its own pixels shows: nothing to draw */
            int L = in->layer[eng];
            for (int yy = in->oy[eng] - 1; yy <= in->oy[eng] + in->h; yy++) {
                if (yy < 0 || yy >= 192) continue;
                for (int xx = in->ox[eng] - 1; xx <= in->ox[eng] + in->w; xx++) {
                    if (xx < 0 || xx >= 256) continue;
                    int idx = yy * 256 + xx;
                    uint8_t vis = g_tx_vis[eng][idx];
                    if (!((vis >> L) & 1)) continue;
                    uint16_t* sl = g_tx_slot[eng][idx];
                    int k = 0;
                    while (k < 4 && sl[k]) k++;
                    if (k == 4) { memmove(sl, sl + 1, 3 * sizeof *sl); k = 3; }
                    sl[k] = (uint16_t)(i + 1);
                }
            }
        }
    }
}

/* tests: DQIX_TXDUMP keeps the tags in the headless build */
__attribute__((constructor)) static void tx_env(void) {
    if (getenv("DQIX_TXSHADOWS")) g_tx_shadows = atoi(getenv("DQIX_TXSHADOWS")) != 0;   /* tests */
    if (getenv("DQIX_TXDUMP") || getenv("DQIX_TX")) {
        g_tx_active = 1;
        const char* d = getenv("DQIX_TXFONTS");
        char p[600]; snprintf(p, sizeof p, "%s/labels.bin", d ? d : "fonts"); tx_labels_load(p);
    }
}

/* ---------------------------------------------------------------- textures
 * Letters copied into texture VRAM (the game draws some text as 3D quads):
 * the tagged texels of a texture (TEXIMAGE_PARAM). */
/* does a texture have any letters (tags)? Fast: stops at the first, skips
 * the 4 KB pages that have no tags at all (the GPU renderer asks every frame) */
int tx_tex_any(uint32_t teximg) {
    if (!g_tx_any) return 0;
    uint32_t a = (teximg & 0xFFFF) << 3;
    int w = 8 << ((teximg >> 20) & 7), h = 8 << ((teximg >> 23) & 7), fmt = (teximg >> 26) & 7;
    static const int bpp[8] = { 0, 8, 2, 4, 8, 2, 8, 16 };
    if (!fmt) return 0;
    uint32_t bytes = (uint32_t)(w * h * bpp[fmt] / 8);
    for (uint32_t o = 0; o < bytes;) {
        uint32_t b = a + o;
        uint8_t* p = g_tex_slot[(b >> 17) & 3];
        uint32_t off = p ? (uint32_t)(p + (b & 0x1FFFF) - g_vram) : 0;
        uint32_t run = TX_PAGE - off % TX_PAGE;
        if (run > bytes - o) run = bytes - o;
        if (p && off < sizeof g_vram && g_vram_pages[off / TX_PAGE]) {
            const TxTag* t = &g_vram_pages[off / TX_PAGE][(off % TX_PAGE) * 2];
            for (uint32_t k = 0; k < run * 2; k++) if (t[k].inst) return 1;
        }
        o += run;
    }
    return 0;
}

int tx_tex_tagged(uint32_t teximg) {
    if (!g_tx_any) return 0;
    uint32_t a = (teximg & 0xFFFF) << 3;
    int w = 8 << ((teximg >> 20) & 7), h = 8 << ((teximg >> 23) & 7), fmt = (teximg >> 26) & 7;
    static const int bpp[8] = { 0, 8, 2, 4, 8, 2, 8, 16 };
    if (!fmt) return 0;
    uint32_t bytes = (uint32_t)(w * h * bpp[fmt] / 8);
    int n = 0;
    for (uint32_t o = 0; o < bytes; o++) {
        uint32_t b = a + o;
        uint8_t* p = g_tex_slot[(b >> 17) & 3];
        if (!p) continue;
        TxTag* t = tx_vram_tags(p + (b & 0x1FFFF));
        if (t) n += (t[0].inst != 0) + (t[1].inst != 0);
    }
    return n;
}

/* tests (DQIX_TXSTAT): a texture's letter texels that lost their tags (the
 * colours of its tagged texels, untagged): those would show in the old font */
int tx_tex_untagged(uint32_t teximg) {
    uint32_t a = (teximg & 0xFFFF) << 3;
    int w = 8 << ((teximg >> 20) & 7), h = 8 << ((teximg >> 23) & 7);
    if (((teximg >> 26) & 7) != 3 || w > 64 || h > 64) return 0;
    uint16_t cols = 0; int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < w * h; i++) {
            uint32_t b = a + (uint32_t)(i >> 1);
            uint8_t* p = g_tex_slot[(b >> 17) & 3];
            if (!p) continue;
            int v = (p[b & 0x1FFFF] >> ((i & 1) * 4)) & 15;
            TxTag* t = tx_vram_tags(p + (b & 0x1FFFF));
            int tagged = t && t[i & 1].inst;
            if (pass == 0 && tagged) cols |= (uint16_t)(1u << v);
            if (pass == 1 && !tagged && v && (cols >> v & 1)) n++;
        }
    return n;
}

/* ---------------------------------------------------------------- the battle messages' routine
 * func_02045864(x, y, glyph, buffer, [sp] width, [sp+4] height, [sp+8] colour,
 * [sp+12] font): a plain 4-bit buffer, (width / 2) bytes a row, the odd pixel
 * of a byte in its high nibble; pixels outside width x height are skipped. */
void fn_main_02045864_orig(Cpu* c);
void fn_main_02045864(Cpu* c) {
    if (!g_tx_active) { fn_main_02045864_orig(c); return; }
    int x = (int32_t)c->r[0], y = (int32_t)c->r[1];
    uint32_t glyph = c->r[2], buf = c->r[3], sp = c->r[13];
    int W = (int32_t)rd32(sp), H = (int32_t)rd32(sp + 4), color = rd8(sp + 8) & 15, font = rd8(sp + 12);
    static Px px[12 * 128];
    int n = 0, gw = 0, index = -1;
    if (ram_ok(glyph) && ram_ok(buf) && font <= 1 && W > 0 && H > 0 && W <= 1024 && H <= 1024) {
        uint32_t sheet = rd32(SHEET_TABLE + font * 4), f = rd32(FONT_TABLE + font * 4);
        if (ram_ok(sheet) && ram_ok(f)) {
            uint32_t bm = rd32(sheet + 0xC);
            int stride = rd16(sheet + 4) >> 3;
            int gx = (int16_t)rd16(glyph + 6);
            gw = (int8_t)rd8(glyph + 4);
            uint32_t tab = rd32(f + 0x10);
            if (glyph >= tab && (glyph - tab) % 8 == 0) index = (int)((glyph - tab) / 8);
            for (int row = 0; row < 12 && ram_ok(bm); row++) {
                int py = y + row;
                uint32_t src = bm + stride * row + (gx >> 3);
                int bit = 7 - (gx & 7);
                for (int col = 0; col < gw && col < 128; col++) {
                    int pxx = x + col;
                    if (py >= 0 && py < H && pxx >= 0 && pxx < W && ((rd8(src) >> bit) & 1)) {
                        uint32_t a = buf + (uint32_t)((W >> 1) * py) + (uint32_t)(pxx >> 1);
                        Px* p = &px[n++];
                        p->addr = a; p->hi = pxx & 1; p->col = (uint8_t)col; p->row = (uint8_t)row;
                        p->under = (rd8(a) >> (p->hi * 4)) & 15;
                        TxTag* t = tags_at(a, 0);
                        if (t && t[p->hi].inst) p->keep = t[p->hi]; else p->keep.inst = 0;
                    }
                    if (--bit < 0) { bit = 7; src++; }
                }
            }
        }
    }
    g_in_glyph++; fn_main_02045864_orig(c); g_in_glyph--;
    if (!n || index < 0) return;
    TxInst* in = inst_get(buf, glyph, x, y, color, font);
    in->w = (uint8_t)gw; in->index = (uint16_t)index;
    uint32_t id = tx_inst_index(in) + 1;
    for (int i = 0; i < n; i++) {
        Px* p = &px[i];
        { int v = (rd8(p->addr) >> (p->hi * 4)) & 15;
          if (!v || (v != color && v == p->under)) continue; }
        TxTag* t = tags_at(p->addr, 1);
        if (!t) continue;
        TxTag* d = &t[p->hi];
        uint8_t under = p->keep.inst && tx_inst_of(&p->keep) ? p->keep.under : p->under;
        d->inst = id; d->gen = in->gen; d->col = p->col; d->row = p->row; d->under = under;
    }
}

/* memcpy(dst, src, n): the game's own copies carry the tags along */
void fn_main_02001a40_orig(Cpu* c);
void fn_main_02001a40(Cpu* c) {
    uint32_t dst = c->r[0], src = c->r[1], len = c->r[2];
#ifdef DQIX_PIXDBG
    { static long lg = -2; if (lg == -2) { const char* e = getenv("DQIX_MEMCPYLOG"); lg = e ? strtol(e, NULL, 16) : -1; }
      if (lg >= 0 && (lg == 0 || (c->r[14] & ~1u) - (uint32_t)lg < 0x400)) fprintf(stderr, "[f%llu] memcpy %08x <- %08x x%u lr %08x\n", (unsigned long long)g_frame, dst, src, len, c->r[14]); }
#endif
    fn_main_02001a40_orig(c);
    if (g_tx_any && len && len < (8u << 20)) tx_copy(dst, src, (int)len);
}

/* ---------------------------------------------------------------- labels (pictures of text) */
TxLabel* g_tx_labels;
int g_tx_nlabels;
/* a label's tile: known by a 64-bit hash of its 32 bytes (the file holds no
 * graphics of the game) */
typedef struct { uint64_t hash; uint16_t label; int8_t dx, dy; } LabelKey;
static LabelKey* g_lkeys; static int g_nlkeys;
#define LHASH 4096
static int g_lhash[LHASH];                    /* key + 1 */
static uint64_t fnv64(const uint8_t* p, int n) { uint64_t h = 14695981039346656037ull; for (int i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
typedef struct { uint64_t hash; uint16_t w, h, label; uint8_t bx, by; } TexKey;
static TexKey* g_tkeys; static int g_ntkeys;
/* a 16-colour texture that is a label ("X Menu" on the field): which, and where in it */
int tx_label_texture(uint32_t ti, int* bx, int* by) {
    if (!g_ntkeys || ((ti >> 26) & 7) != 3) return -1;
    int w = 8 << ((ti >> 20) & 7), h = 8 << ((ti >> 23) & 7), k;
    for (k = 0; k < g_ntkeys; k++) if (g_tkeys[k].w == w && g_tkeys[k].h == h) break;
    if (k == g_ntkeys) return -1;
    uint32_t a = (ti & 0xFFFF) << 3;
    uint64_t hs = 14695981039346656037ull;
    for (int i = 0; i < w * h / 2; i++) {
        uint32_t b = a + (uint32_t)i;
        uint8_t* p = g_tex_slot[(b >> 17) & 3];
        hs ^= p ? p[b & 0x1FFFF] : 0; hs *= 1099511628211ull;
    }
    for (k = 0; k < g_ntkeys; k++)
        if (g_tkeys[k].hash == hs && g_tkeys[k].w == w && g_tkeys[k].h == h && g_tkeys[k].label < g_tx_nlabels) { *bx = g_tkeys[k].bx; *by = g_tkeys[k].by; return g_tkeys[k].label; }
    return -1;
}
void tx_labels_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char magic[4]; uint32_t n[3];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "DQL4", 4) || fread(n, 4, 3, f) != 3 || n[0] > 4096 || n[1] > 16384 || n[2] > 4096) { fclose(f); return; }
    TxLabel* L = calloc(n[0], sizeof *L); LabelKey* K = calloc(n[1], sizeof *K);
    int ok = L && K;
    for (uint32_t i = 0; ok && i < n[0]; i++) {
        uint8_t b[84];
        if (fread(b, 1, 84, f) != 84) { ok = 0; break; }
        memcpy(L[i].rowbg, b + 60, 24);
        memcpy(L[i].text, b, 47); L[i].text[47] = 0;
        L[i].font = b[48]; L[i].main = b[49]; memcpy(&L[i].inkmask, b + 50, 2);
        L[i].bg = b[52]; L[i].w = b[53]; L[i].h = b[54]; L[i].x0 = b[55]; L[i].top = b[56]; L[i].x1 = b[57]; L[i].base = b[58]; L[i].flags = b[59]; L[i].ocell = 0xFF;
    }
    for (uint32_t i = 0; ok && i < n[1]; i++) {
        uint8_t b[12];
        if (fread(b, 1, 12, f) != 12) { ok = 0; break; }
        memcpy(&K[i].hash, b, 8); memcpy(&K[i].label, b + 8, 2); K[i].dx = (int8_t)b[10]; K[i].dy = (int8_t)b[11];
    }
    TexKey* T = calloc(n[2] ? n[2] : 1, sizeof *T);
    if (!T) ok = 0;
    for (uint32_t i = 0; ok && i < n[2]; i++) {
        uint8_t b[16];
        if (fread(b, 1, 16, f) != 16) { ok = 0; break; }
        memcpy(&T[i].hash, b, 8); memcpy(&T[i].w, b + 8, 2); memcpy(&T[i].h, b + 10, 2); memcpy(&T[i].label, b + 12, 2); T[i].bx = b[14]; T[i].by = b[15];
    }
    fclose(f);
    if (!ok) { free(L); free(K); free(T); return; }
    free(g_tkeys); g_tkeys = T; g_ntkeys = (int)n[2];
    for (uint32_t i = 0; i < n[0] && i < 64; i++) L[i].ocell = (uint8_t)(64 + i);   /* every label can have its outline drawn */
    if (n[0] > 64) n[0] = 64;
    free(g_tx_labels); free(g_lkeys);
    g_tx_labels = L; g_tx_nlabels = (int)n[0]; g_lkeys = K; g_nlkeys = (int)n[1];
    memset(g_lhash, 0, sizeof g_lhash);
    for (int i = 0; i < g_nlkeys; i++) {
        uint32_t h = (uint32_t)K[i].hash & (LHASH - 1);
        while (g_lhash[h]) h = (h + 1) & (LHASH - 1);
        g_lhash[h] = i + 1;
    }
    fprintf(stderr, "HD text: %d labels\n", g_tx_nlabels);
}
TxInst* tx_label_inst(int label, int eng, int ox, int oy) {
    TxInst* in = inst_get(0xF1A00000u + (uint32_t)eng, (uint32_t)label, ox, oy, g_tx_labels[label].main, 2);
    in->w = g_tx_labels[label].w; in->h = g_tx_labels[label].h; in->index = (uint16_t)label;
    return in;
}
/* Labels can share tiles ("Change C" of "Change Character" and "Change
 * Category"): a tile of one label only confirms it; a shared one goes to the
 * label at that place a tile of its own confirmed (this frame or the last). */
TxInst* tx_label_at(const uint8_t* t, int eng, int tsx, int tsy, int* dx, int* dy) {
    if (!g_nlkeys) return NULL;
    /* the tiles seen recently, by place in VRAM, with what they matched: a
     * tile is looked at on every line it shows, its contents rarely change */
    enum { NC = 8192 };
    static struct { const uint8_t* p; uint8_t bytes[32]; uint8_t n; uint16_t cand[4]; int8_t cdx[4], cdy[4]; } cache[NC];
    static const TxLabel* cache_db;
    if (cache_db != g_tx_labels) { memset(cache, 0, sizeof cache); cache_db = g_tx_labels; }
    uint32_t ci = (uint32_t)(((uintptr_t)t >> 5) & (NC - 1));
    int cand[8], cdx[8], cdy[8], n = 0;
    if (cache[ci].p == t && !memcmp(cache[ci].bytes, t, 32)) {
        n = cache[ci].n;
        for (int i = 0; i < n; i++) { cand[i] = cache[ci].cand[i]; cdx[i] = cache[ci].cdx[i]; cdy[i] = cache[ci].cdy[i]; }
    } else {
        uint64_t th = fnv64(t, 32);
        for (uint32_t h = (uint32_t)th & (LHASH - 1), k = 0; g_lhash[h] && k < LHASH; h = (h + 1) & (LHASH - 1), k++) {
            LabelKey* K = &g_lkeys[g_lhash[h] - 1];
            if (K->hash == th && n < 4) { cand[n] = K->label; cdx[n] = K->dx; cdy[n] = K->dy; n++; }
        }
        cache[ci].p = t; memcpy(cache[ci].bytes, t, 32); cache[ci].n = (uint8_t)n;
        for (int i = 0; i < n; i++) { cache[ci].cand[i] = (uint16_t)cand[i]; cache[ci].cdx[i] = (int8_t)cdx[i]; cache[ci].cdy[i] = (int8_t)cdy[i]; }
    }
    if (!n) return NULL;
    if (n == 1) {
        TxInst* in = tx_label_inst(cand[0], eng, tsx - cdx[0], tsy - cdy[0]);
        in->lconf = g_frame + 1;
        *dx = cdx[0]; *dy = cdy[0];
        return in;
    }
    for (int i = 0; i < n; i++) {
        TxInst* in = inst_find(0xF1A00000u + (uint32_t)eng, (uint32_t)cand[i], tsx - cdx[i], tsy - cdy[i], g_tx_labels[cand[i]].main, 2);
        if (in && in->lconf + 1 >= g_frame) { *dx = cdx[i]; *dy = cdy[i]; return in; }
    }
    return NULL;
}

