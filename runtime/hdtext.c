/* HD text (see hdtext.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nds.h"
#include "cpu.h"
#include "hdtext.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

int g_hdtext_on;
int g_hdtext_any;

/* ---------------------------------------------------------------- letters seen */
/* A letter of a font, as the game draws it: its picture (12 rows of up to 16
 * pixels) and the text it stands for ("A", "<'e>", ...). */
typedef struct { uint32_t key; int font, w; uint16_t rows[12]; char str[16]; } Glyph;
#define MAX_GLYPHS 4096
static Glyph g_glyphs[MAX_GLYPHS];
static int g_nglyphs;
static int g_glyph_hash[MAX_GLYPHS * 2];                       /* index + 1, 0 = empty */

/* One letter drawn: which, and in which colour index. */
#define INST_BITS 18
#define INST_N (1u << INST_BITS)
typedef struct { uint16_t glyph; uint8_t color, used; } Inst;
static Inst* g_inst;
static uint32_t g_inst_next = 1;

static int glyph_find(uint32_t key, int font, int w, const uint16_t* rows, const char* str) {
    uint32_t h = (key * 2654435761u) % (MAX_GLYPHS * 2);
    while (g_glyph_hash[h]) {
        Glyph* g = &g_glyphs[g_glyph_hash[h] - 1];
        if (g->key == key && !memcmp(g->rows, rows, sizeof g->rows) && !strcmp(g->str, str)) return g_glyph_hash[h] - 1;
        h = (h + 1) % (MAX_GLYPHS * 2);
    }
    if (g_nglyphs >= MAX_GLYPHS) return -1;
    Glyph* g = &g_glyphs[g_nglyphs];
    g->key = key; g->font = font; g->w = w; memcpy(g->rows, rows, sizeof g->rows);
    snprintf(g->str, sizeof g->str, "%s", str);
    g_glyph_hash[h] = ++g_nglyphs;
    return g_nglyphs - 1;
}

/* ---------------------------------------------------------------- tags in memory */
/* One 32-bit tag per 4-bit pixel, in 4 KB pages made when first needed. */
#define TPAGE 4096u
static uint32_t* g_ram_tags[MAIN_RAM_SIZE / TPAGE];
static uint32_t* g_vram_tags[sizeof g_vram / TPAGE];

static uint32_t* page_for(const uint8_t* p, int create, uint32_t* off) {
    uint32_t** pages; size_t o;
    if (p >= g_main_ram && p < g_main_ram + MAIN_RAM_SIZE) { pages = g_ram_tags; o = (size_t)(p - g_main_ram); }
    else if (p >= g_vram && p < g_vram + sizeof g_vram) { pages = g_vram_tags; o = (size_t)(p - g_vram); }
    else return NULL;
    uint32_t** pg = &pages[o / TPAGE];
    if (!*pg) { if (!create) return NULL; *pg = calloc(TPAGE * 2, sizeof(uint32_t)); g_hdtext_any = 1; }
    *off = (uint32_t)(o % TPAGE) * 2;
    return *pg;
}

static uint8_t* addr_ptr(uint32_t a) {
    if ((a >> 24) == 2) return &g_main_ram[a & (MAIN_RAM_SIZE - 1)];
    if ((a >> 24) == 6) return vram_ptr(a);
    return NULL;
}

void hdtext_forget(const uint8_t* p, int size) {
    uint32_t off; uint32_t* pg = page_for(p, 0, &off);
    if (pg) memset(pg + off, 0, (size_t)size * 2 * sizeof(uint32_t));   /* (writes never cross a 4 KB page: they're aligned) */
}

void hdtext_copy(uint32_t dst, uint32_t src, uint32_t bytes) {
    if (!g_hdtext_any) return;
    while (bytes) {
        uint8_t* s = addr_ptr(src); uint8_t* d = addr_ptr(dst);
        uint32_t n = TPAGE - (src & (TPAGE - 1)), nd = TPAGE - (dst & (TPAGE - 1));
        if (nd < n) n = nd;
        if (n > bytes) n = bytes;
        if ((dst >> 24) == 6 && 0x4000 - (dst & 0x3FFF) < n) n = 0x4000 - (dst & 0x3FFF);   /* VRAM pages map separately */
        if ((src >> 24) == 6 && 0x4000 - (src & 0x3FFF) < n) n = 0x4000 - (src & 0x3FFF);
        uint32_t so, doff;
        uint32_t* sp = s ? page_for(s, 0, &so) : NULL;
        if (sp && d) { uint32_t* dp = page_for(d, 1, &doff); if (dp) memmove(dp + doff, sp + so, (size_t)n * 2 * sizeof(uint32_t)); }
        src += n; dst += n; bytes -= n;
    }
}

uint32_t hdtext_tag(uint32_t src2) {
    if (!g_hdtext_any) return 0;
    uint8_t* p = vram_ptr(src2 >> 1);
    uint32_t off; uint32_t* pg = p ? page_for(p, 0, &off) : NULL;
    return pg ? pg[off + (src2 & 1)] : 0;
}

/* ---------------------------------------------------------------- the hooks (hooks.txt) */
/* func_0205b36c (USA): draws one letter into a text window's tile buffer.
 *   r0 the window's buffer: +0 the tiles, +4 its rectangles (12 bytes each:
 *      +0 offset of their tiles, +4 x, +6 y, +8 width, +10 height), +8 how many
 *   r1 the letter: +0 its text, +4 width, +5 text length (low 6 bits), +6 where
 *      its picture starts in the font's strip (in pixels)
 *   r2, r3 where (pixels in the window); [sp] colour index, [sp+4] font
 * The font (data at 0x02107834 + 4 * font): +4 the strip's width in pixels,
 * +12 the strip (12 rows, 1 bit per pixel, leftmost pixel in the top bit). */
void hook_GlyphBlit(Cpu* c, void (*orig)(Cpu*)) {
    uint32_t win = c->r[0], gl = c->r[1];
    int x0 = (int16_t)c->r[2], y0 = (int16_t)c->r[3];
    uint8_t color = mem_read8(c->r[13]) & 15, font = mem_read8(c->r[13] + 4);
    orig(c);
    if (!g_hdtext_on) return;
    if (!g_inst) g_inst = calloc(INST_N, sizeof *g_inst);
    int w = (int8_t)mem_read8(gl + 4);
    if (w <= 0 || w > 16) return;
    uint32_t fo = mem_read32(0x02107834u + 4u * font);
    if ((fo >> 24) != 2) return;
    uint32_t stride = mem_read16(fo + 4) >> 3, bm = mem_read32(fo + 12);
    uint32_t xo = mem_read16(gl + 6);
    uint16_t rows[12];
    for (int y = 0; y < 12; y++) {
        rows[y] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t bit = xo + (uint32_t)x;
            if (mem_read8(bm + (uint32_t)y * stride + (bit >> 3)) & (0x80 >> (bit & 7))) rows[y] |= (uint16_t)(1 << x);
        }
    }
    char str[16]; uint32_t sp = mem_read32(gl); int len = mem_read8(gl + 5) & 63;
    if (len > 15) len = 15;
    for (int i = 0; i < len; i++) str[i] = (char)mem_read8(sp + (uint32_t)i);
    str[len] = 0;
    int gi = glyph_find((uint32_t)font << 24 | xo << 8 | (uint32_t)w, font, w, rows, str);
    if (gi < 0) return;
    uint32_t inst = g_inst_next;
    g_inst_next = (g_inst_next + 1) & (INST_N - 1); if (!g_inst_next) g_inst_next = 1;
    g_inst[inst].glyph = (uint16_t)gi; g_inst[inst].color = color; g_inst[inst].used = 1;

    uint32_t tiles = mem_read32(win), rects = mem_read32(win + 4);
    int nrect = mem_read8(win + 8);
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < w; x++) {
            int X = x0 + x, Y = y0 + y;
            for (int r = 0; r < nrect; r++) {                      /* the rectangle holding this pixel (func_0205b1ac) */
                uint32_t ra = rects + (uint32_t)r * 12;
                int rx = (int16_t)mem_read16(ra + 4), ry = (int16_t)mem_read16(ra + 6);
                int rw = (int16_t)mem_read16(ra + 8), rh = (int16_t)mem_read16(ra + 10);
                if (X < rx || X >= rx + rw || Y < ry || Y >= ry + rh) continue;
                int lx = X - rx, ly = Y - ry;
                uint32_t a = tiles + mem_read32(ra) + (uint32_t)(((ly >> 3) * (rw >> 3) + (lx >> 3)) * 32 + (ly & 7) * 4 + ((lx & 7) >> 1));
                uint8_t* p = addr_ptr(a);
                uint32_t off; uint32_t* pg = p ? page_for(p, 1, &off) : NULL;
                if (pg) {
                    int ink = (rows[y] >> x) & 1;
                    uint32_t* t = &pg[off + (X & 1)];
                    if (ink || !(*t & 0x80000000u) || !HDT_INK(*t))   /* another letter's ink stays */
                        *t = 0x80000000u | inst << 13 | (uint32_t)x << 8 | (uint32_t)y << 4 | (uint32_t)ink;
                }
                break;
            }
        }
}

/* memcpy (0x02001a40): the tags go along. */
void hook_Memcpy(Cpu* c, void (*orig)(Cpu*)) {
    uint32_t d = c->r[0], s = c->r[1], n = c->r[2];
    orig(c);
    if (g_hdtext_any && n) hdtext_copy(d, s, n);
}

/* ---------------------------------------------------------------- this frame's letters */
uint8_t g_hdtext_mask[2][256 * 192];
#define MAX_SHOWN 4096
static HdGlyph g_shown[2][MAX_SHOWN];
static int g_nshown[2];
static int g_shown_hash[2][MAX_SHOWN * 2];                     /* index + 1 */

void hdtext_frame_start(void) {
    memset(g_hdtext_mask, 0, sizeof g_hdtext_mask);
    memset(g_shown_hash, 0, sizeof g_shown_hash);
    g_nshown[0] = g_nshown[1] = 0;
}

static HdGlyph* shown(int eng, uint32_t tag, int x, int y) {
    uint32_t inst = HDT_INST(tag);
    int ox = x - (int)HDT_LX(tag), oy = y - (int)HDT_LY(tag);
    uint32_t h = (inst * 2654435761u ^ (uint32_t)(ox * 31 + oy * 7919)) % (MAX_SHOWN * 2);
    while (g_shown_hash[eng][h]) {
        HdGlyph* s = &g_shown[eng][g_shown_hash[eng][h] - 1];
        if (s->inst == inst && s->x == ox && s->y == oy) return s;
        h = (h + 1) % (MAX_SHOWN * 2);
    }
    if (!g_inst || g_nshown[eng] >= MAX_SHOWN || !g_inst[inst].used) return NULL;
    HdGlyph* s = &g_shown[eng][g_nshown[eng]];
    const Glyph* g = &g_glyphs[g_inst[inst].glyph];
    s->inst = inst; s->x = ox; s->y = oy; s->w = g->w; s->font = g->font; s->glyph = g_inst[inst].glyph;
    s->color = 0; s->has_color = 0;
    g_shown_hash[eng][h] = ++g_nshown[eng];
    return s;
}

void hdtext_seen(int eng, uint32_t tag, int x, int y, int ink_shown, uint32_t rgb) {
    g_hdtext_mask[eng][y * 256 + x] = 255;
    HdGlyph* s = shown(eng, tag, x, y);
    if (s && ink_shown && !s->has_color) { s->color = rgb; s->has_color = 1; }
}

static uint32_t g_order_base;
static int by_order(const void* a, const void* b) {          /* drawing order (the ring of letters wraps) */
    uint32_t x = (((const HdGlyph*)a)->inst - g_order_base) & (INST_N - 1), y = (((const HdGlyph*)b)->inst - g_order_base) & (INST_N - 1);
    return x < y ? -1 : x > y;
}

int hdtext_glyphs(int eng, const HdGlyph** out) {
    g_order_base = g_inst_next;
    qsort(g_shown[eng], (size_t)g_nshown[eng], sizeof(HdGlyph), by_order);
    *out = g_shown[eng];
    return g_nshown[eng];
}

/* ---------------------------------------------------------------- the font */
static stbtt_fontinfo g_tt;
static unsigned char* g_tt_data;
static char g_font_name[256] = "original";
static uint32_t g_font_gen = 1;

static int load_ttf(const char* name) {
    char paths[6][600]; int n = 0;
    snprintf(paths[n++], 600, "%s", name);
    snprintf(paths[n++], 600, "fonts/%s", name);
    const char* win = getenv("WINDIR");
    if (win) snprintf(paths[n++], 600, "%s\\Fonts\\%s", win, name);
    const char* la = getenv("LOCALAPPDATA");                    /* fonts installed for one user */
    if (la) snprintf(paths[n++], 600, "%s\\Microsoft\\Windows\\Fonts\\%s", la, name);
    snprintf(paths[n++], 600, "/usr/share/fonts/truetype/dejavu/%s", name);
    snprintf(paths[n++], 600, "/usr/share/fonts/truetype/liberation/%s", name);
    for (int i = 0; i < n; i++) {
        FILE* f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char* d = malloc((size_t)sz);
        if (sz <= 0 || fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); continue; }
        fclose(f);
        if (stbtt_InitFont(&g_tt, d, stbtt_GetFontOffsetForIndex(d, 0))) { free(g_tt_data); g_tt_data = d; return 1; }
        free(d);
    }
    fprintf(stderr, "HD text: font %s not found, using the game's letters\n", name);
    return 0;
}

void hdtext_set_font(const char* name) {
    if (!name || !*name) name = "original";
    if (!strcmp(name, g_font_name) && g_font_gen > 1) return;
    snprintf(g_font_name, sizeof g_font_name, "%s", name);
    free(g_tt_data); g_tt_data = NULL;
    if (strcmp(name, "original") != 0) load_ttf(name);
    g_font_gen++;
}
const char* hdtext_font_name(void) { return g_font_name; }
int hdtext_font_ok(void) { return g_tt_data != NULL; }
uint32_t hdtext_font_gen(void) { return g_font_gen; }

/* The game's letter names -> Unicode ("A", "<'e>", "<99>", ...). 0: no such
 * character (drawn from the game's own picture). */
static int codepoint(const char* s) {
    if (s[0] && !s[1]) return (unsigned char)s[0];
    if (s[0] != '<') return 0;
    char k[16]; snprintf(k, sizeof k, "%s", s + 1);
    size_t n = strlen(k); if (n && k[n - 1] == '>') k[--n] = 0;
    char* q = k; if (*q == '_') q++;                           /* <_66>, <66_>: the same character, other spacing */
    n = strlen(q); if (n > 1 && q[n - 1] == '_') q[--n] = 0;
    if (!*q) return ' ';
    static const struct { const char* k; int cp; } map[] = {
        { " ", ' ' }, { "6", 0x2018 }, { "9", 0x2019 }, { "66", 0x201C }, { "99", 0x201D }, { ",", ',' }, { ",,", 0x201E },
        { "DE66", 0x201E }, { "DE99", 0x201C }, { "...", 0x2026 }, { "<<", 0xAB }, { ">>", 0xBB }, { "^!", 0xA1 }, { "^?", 0xBF },
        { "--", 0x2014 }, { "-", '-' }, { "OE", 0x152 }, { "oe", 0x153 }, { "AE", 0xC6 }, { "ae", 0xE6 }, { "ss", 0xDF },
        { "C=", 0x20AC }, { "po", 0xA3 }, { "c", 0xA9 }, { "R", 0xAE }, { "femord", 0xAA }, { "fem", 0xAA }, { "degree", 0xB0 },
        { "deg", 0xB0 }, { "+-", 0xB1 }, { "x", 0xD7 }, { "tilde", '~' }, { "!", '!' }, { "?", '?' }, { ":", ':' }, { ";", ';' },
        { "%", '%' }, { "/", '/' }, { "<", '<' }, { ">", '>' }, { "]", ']' }, { "1", '\'' }, { "11", '"' }, { "%_", '%' },
        { "l_arrow", 0x2190 }, { "u_arrow", 0x2191 }, { "r_arrow", 0x2192 }, { "d_arrow", 0x2193 },
        { "l_a", 0x2190 }, { "u_a", 0x2191 }, { "r_a", 0x2192 }, { "d_a", 0x2193 }, { "heart", 0x2665 }, { "music", 0x266A },
        { "star", 0x2606 }, { "blackstar", 0x2605 }, { "circle", 0x25CB }, { "blkcircle", 0x25CF }, { "square", 0x25A1 },
        { "blksquare", 0x25A0 }, { "triangle", 0x25B3 }, { "blktriangle", 0x25B2 }, { "invtriangle", 0x25BD },
        { "invblktriangle", 0x25BC },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(q, map[i].k)) return map[i].cp;
    if (strlen(q) == 2) {                                       /* accents: <`e> <'e> <^e> <:e> <~n> <,c> */
        static const char* base = "AEIOUaeiou";
        static const int grave[] = { 0xC0, 0xC8, 0xCC, 0xD2, 0xD9, 0xE0, 0xE8, 0xEC, 0xF2, 0xF9 };
        static const int acute[] = { 0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xE1, 0xE9, 0xED, 0xF3, 0xFA };
        static const int circ[]  = { 0xC2, 0xCA, 0xCE, 0xD4, 0xDB, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB };
        static const int uml[]   = { 0xC4, 0xCB, 0xCF, 0xD6, 0xDC, 0xE4, 0xEB, 0xEF, 0xF6, 0xFC };
        const char* b = strchr(base, q[1]);
        if (b && q[1]) {
            int i = (int)(b - base);
            switch (q[0]) { case '`': return grave[i]; case '\'': return acute[i]; case '^': return circ[i]; case ':': return uml[i]; }
        }
        if (q[0] == '~' && q[1] == 'N') return 0xD1;
        if (q[0] == '~' && q[1] == 'n') return 0xF1;
        if (q[0] == '~' && q[1] == 'O') return 0xD5;
        if (q[0] == '~' && q[1] == 'o') return 0xF5;
        if (q[0] == ',' && q[1] == 'C') return 0xC7;
        if (q[0] == ',' && q[1] == 'c') return 0xE7;
    }
    return 0;
}

/* Where the game's letters sit in their 12 rows, per font: from its "H". */
static int font_metrics(int font, int* top, int* base) {
    *top = 3; *base = 9;                                        /* the usual, until an H has been drawn */
    for (int i = 0; i < g_nglyphs; i++) {
        const Glyph* g = &g_glyphs[i];
        if (g->font != font || strcmp(g->str, "H") != 0) continue;
        int t = -1, b = -1;
        for (int y = 0; y < 12; y++) if (g->rows[y]) { if (t < 0) t = y; b = y; }
        if (t >= 0) { *top = t; *base = b; return 1; }
    }
    return 0;
}

/* The game's own letter, smoothed: the picture interpolated and cut at half,
 * so the steps of slanted and round strokes become smooth edges. */
static void render_original(const Glyph* g, uint8_t* out, int stride) {
    int W = g->w * HDT_R, H = 12 * HDT_R;
    for (int v = 0; v < H; v++)
        for (int u = 0; u < W; u++) {
            float fx = ((float)u + 0.5f) / HDT_R - 0.5f, fy = ((float)v + 0.5f) / HDT_R - 0.5f;
            int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            float ax = fx - (float)x0, ay = fy - (float)y0;
            float s = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int x = x0 + dx, y = y0 + dy;
                    int on = x >= 0 && y >= 0 && x < g->w && y < 12 && ((g->rows[y] >> x) & 1);
                    s += (float)on * (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
                }
            float k = (s - 0.5f) / 0.12f + 0.5f;                   /* a soft cut: anti-aliased edges */
            out[v * stride + u] = (uint8_t)(k <= 0 ? 0 : k >= 1 ? 255 : k * 255.0f);
        }
}

int hdtext_render(int glyph, uint8_t* out, int stride, int* w) {
    if (glyph < 0 || glyph >= g_nglyphs) return 0;
    const Glyph* g = &g_glyphs[glyph];
    *w = g->w;
    int W = g->w * HDT_R, H = 12 * HDT_R;
    for (int v = 0; v < H; v++) memset(out + v * stride, 0, (size_t)W);
    int inkx0 = 99, inkx1 = -1;
    for (int y = 0; y < 12; y++) for (int x = 0; x < g->w; x++) if ((g->rows[y] >> x) & 1) { if (x < inkx0) inkx0 = x; if (x > inkx1) inkx1 = x; }
    if (inkx1 < 0) return 1;                                    /* a space */
    int cp = g_tt_data ? codepoint(g->str) : 0;
    if (cp && cp != ' ' && stbtt_FindGlyphIndex(&g_tt, cp)) {
        int top, base; font_metrics(g->font, &top, &base);
        int hx0, hy0, hx1, hy1;
        if (!stbtt_GetCodepointBox(&g_tt, 'H', &hx0, &hy0, &hx1, &hy1) || hy1 <= 0) hy1 = 1400;
        float scale = (float)((base - top + 1) * HDT_R) / (float)hy1;
        int bx0, by0, bx1, by1;
        stbtt_GetCodepointBitmapBox(&g_tt, cp, scale, scale, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw > 0 && bh > 0 && bw <= 400 && bh <= 400) {
            unsigned char* bm = malloc((size_t)bw * (size_t)bh);
            stbtt_MakeCodepointBitmap(&g_tt, bm, bw, bh, bw, scale, scale, cp);
            float cx = (float)(inkx0 + inkx1 + 1) * HDT_R / 2.0f;    /* centred on the game's letter */
            int ox = (int)lroundf(cx - (float)bw / 2.0f), oy = (base + 1) * HDT_R + by0;
            for (int v = 0; v < bh; v++)
                for (int u = 0; u < bw; u++) {
                    int X = ox + u, Y = oy + v;
                    if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                    uint8_t c = bm[v * bw + u];
                    if (c > out[Y * stride + X]) out[Y * stride + X] = c;
                }
            free(bm);
            return 1;
        }
    }
    render_original(g, out, stride);
    return 1;
}
