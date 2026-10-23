/* HD text: the letters at high resolution (text_hd_glyphs.h).
 *
 * Each of the game's glyphs has a name in its font table ("A", "<'e>" for é,
 * "<heart>"...), so it can be drawn from a TrueType font. The TrueType letter
 * is fitted to the game's: the same baseline and size for every letter of a
 * font (from the game's H and x), centred where the game's letter has its
 * ink, and narrowed if it would be wider. Glyphs the font hasn't got (the
 * game's own symbols) are drawn from the game's pixels, made smooth. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "text_hd.h"
#include "text_hd_glyphs.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

/* ---------------------------------------------------------------- names -> Unicode */
static const struct { const char* name; int cp; } NAMES[] = {
    { "<tilde>", 0x02DC }, { "<C=>", 0x20AC }, { "<,>", ',' }, { "<,,>", 0x201E }, { "<OE>", 0x0152 }, { "<oe>", 0x0153 },
    { "<6>", 0x2018 }, { "<9>", 0x2019 }, { "<66>", 0x201C }, { "<99>", 0x201D }, { "<-->", 0x2014 }, { "<^!>", 0x00A1 },
    { "<po>", 0x00A3 }, { "<c>", 0x00A9 }, { "<femord>", 0x00AA }, { "<fem>", 0x00AA }, { "<<<>", 0x00AB }, { "<R>", 0x00AE },
    { "<degree>", 0x00B0 }, { "<deg>", 0x00B0 }, { "<+->", 0x00B1 }, { "<>>>", 0x00BB }, { "<^?>", 0x00BF }, { "<11>", '"' },
    { "<_%_>", '%' }, { "<1>", 0x2019 }, { "<->", '-' }, { "</>", '/' }, { "<<>", '<' }, { "<>>", '>' }, { "<]>", '[' },
    { "<x>", 0x00D7 }, { "<ss>", 0x00DF }, { "<AE>", 0x00C6 }, { "<ae>", 0x00E6 }, { "<,C>", 0x00C7 }, { "<,c>", 0x00E7 },
    { "<~N>", 0x00D1 }, { "<~n>", 0x00F1 }, { "<c/>", 0x00A2 }, { "<wave>", '~' },
    { "<l_arrow>", 0x2190 }, { "<u_arrow>", 0x2191 }, { "<r_arrow>", 0x2192 }, { "<d_arrow>", 0x2193 },
    { "<l_a>", 0x2190 }, { "<u_a>", 0x2191 }, { "<r_a>", 0x2192 }, { "<d_a>", 0x2193 },
    { "<ar-dl>", 0x2199 }, { "<ar-dr>", 0x2198 }, { "<ar-ul>", 0x2196 }, { "<ar-ur>", 0x2197 },
    { "<2way_arrow>", 0x21D4 }, { "<doub_arrow>", 0x21D2 },
    { "<blksquare>", 0x25A0 }, { "<square>", 0x25A1 }, { "<circle>", 0x25CB }, { "<blkcircle>", 0x25CF },
    { "<blackstar>", 0x2605 }, { "<star>", 0x2606 }, { "<heart>", 0x2665 }, { "<music>", 0x266A }, { "<.|>", 0x266A }, { "<.|.|>", 0x266B },
    { "<snow>", 0x203B }, { "<triangle>", 0x25B3 }, { "<blktriangle>", 0x25B2 }, { "<invtriangle>", 0x25BD }, { "<invblktriangle>", 0x25BC },
    /* the same punctuation, placed for other positions (the fitting follows the game's ink) */
    { "<!>", '!' }, { "<_66>", 0x201C }, { "<_99>", 0x201D }, { "<_6>", 0x2018 }, { "<_9>", 0x2019 }, { "<_,>", ',' },
    { "<:>", ':' }, { "<;>", ';' }, { "<%>", '%' }, { "<?>", '?' }, { "<_^?>", 0x00BF }, { "<_^!>", 0x00A1 }, { "<...>", 0x2026 },
    { "<_,,>", 0x201E }, { "<!_>", '!' }, { "<66_>", 0x201C }, { "<99_>", 0x201D }, { "<6_>", 0x2018 }, { "<9_>", 0x2019 },
    { "<,_>", ',' }, { "<:_>", ':' }, { "<;_>", ';' }, { "<%_>", '%' }, { "<?_>", '?' }, { "<^?_>", 0x00BF }, { "<^!_>", 0x00A1 },
    { "<..._>", 0x2026 }, { "<,,_>", 0x201E }, { "<DE66>", 0x201E }, { "<DE99>", 0x201C },
};
/* accents: <`A> <'A> <^A> <:A> <~A> */
static int accented(char mark, char base) {
    static const char* bases = "AEIOUaeiou";
    static const int grave[] = { 0xC0, 0xC8, 0xCC, 0xD2, 0xD9, 0xE0, 0xE8, 0xEC, 0xF2, 0xF9 };
    static const int acute[] = { 0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xE1, 0xE9, 0xED, 0xF3, 0xFA };
    static const int circ[] = { 0xC2, 0xCA, 0xCE, 0xD4, 0xDB, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB };
    static const int diaer[] = { 0xC4, 0xCB, 0xCF, 0xD6, 0xDC, 0xE4, 0xEB, 0xEF, 0xF6, 0xFC };
    const char* p = strchr(bases, base);
    if (!p || !base) {
        if (mark == '~' && base == 'A') return 0xC3;
        if (mark == '~' && base == 'a') return 0xE3;
        return 0;
    }
    int i = (int)(p - bases);
    switch (mark) {
    case '`': return grave[i];
    case '\'': return acute[i];
    case '^': return circ[i];
    case ':': return diaer[i];
    case '~': return base == 'O' ? 0xD5 : base == 'o' ? 0xF5 : 0;
    }
    return 0;
}
static int name_cp(const char* name) {
    size_t n = strlen(name);
    if (n == 1) return name[0] == '\'' ? 0x2019 : (unsigned char)name[0];
    for (unsigned i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) if (!strcmp(name, NAMES[i].name)) return NAMES[i].cp;
    if (n == 4 && name[0] == '<' && name[3] == '>') return accented(name[1], name[2]);
    return 0;
}
static int is_letter_cp(int cp) { return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x17F && cp != 0xD7 && cp != 0xF7) || cp == 0x152 || cp == 0x153; }

/* ---------------------------------------------------------------- fonts */
typedef struct {
    stbtt_fontinfo info;
    unsigned char* data;
    int ok;
} Ttf;
static Ttf g_ttf[2];
static char g_dir[512];

static void load_ttf(Ttf* t, const char* path) {
    free(t->data); memset(t, 0, sizeof *t);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    t->data = malloc((size_t)sz);
    if (t->data && fread(t->data, 1, (size_t)sz, f) == (size_t)sz &&
        stbtt_InitFont(&t->info, t->data, stbtt_GetFontOffsetForIndex(t->data, 0))) t->ok = 1;
    fclose(f);
    if (!t->ok) { free(t->data); t->data = NULL; }
    else fprintf(stderr, "HD text: %s\n", path);
}
void txg_load_fonts(const char* dir) {
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    char p[600];
    snprintf(p, sizeof p, "%s/small.ttf", dir); load_ttf(&g_ttf[0], p);
    snprintf(p, sizeof p, "%s/main.ttf", dir); load_ttf(&g_ttf[1], p);
    snprintf(p, sizeof p, "%s/labels.bin", dir); tx_labels_load(p);
}

/* ---------------------------------------------------------------- the game's glyphs */
typedef struct { int w, x0, x1, y0, y1; uint8_t px[12][TXG_MAXW]; } DsGlyph;   /* ink box: [x0,x1) x [y0,y1) */

static int ds_glyph(int font, int index, DsGlyph* g, char* name, int namelen) {
    int w, sx, sw, stride;
    const uint8_t* sheet = tx_font_sheet(font, &sw, &stride);
    if (!sheet || !tx_font_glyph(font, index, name, namelen, &w, &sx)) return 0;
    memset(g, 0, sizeof *g);
    g->w = w < 0 ? 0 : w > TXG_MAXW ? TXG_MAXW : w;
    g->x0 = g->y0 = 99; g->x1 = g->y1 = -1;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < g->w; x++) {
            int bx = sx + x;
            if (bx < 0 || bx >= sw) continue;
            if ((sheet[y * stride + (bx >> 3)] >> (7 - (bx & 7))) & 1) {
                g->px[y][x] = 1;
                if (x < g->x0) g->x0 = x;
                if (x + 1 > g->x1) g->x1 = x + 1;
                if (y < g->y0) g->y0 = y;
                if (y + 1 > g->y1) g->y1 = y + 1;
            }
        }
    return 1;
}

/* the game font's baseline, cap height and x-height (from H and x) */
typedef struct { int ok; float base, cap, xh; float scale; } Metrics;
static Metrics g_met[2];
static int find_glyph(int font, const char* want) {
    char name[32];
    for (int i = 0; i < tx_font_count(font); i++)
        if (tx_font_glyph(font, i, name, sizeof name, NULL, NULL) && !strcmp(name, want)) return i;
    return -1;
}
static void metrics(int font) {
    Metrics* m = &g_met[font];
    if (m->ok) return;
    DsGlyph H, x;
    int ih = find_glyph(font, "H"), ix = find_glyph(font, "x");
    if (ih < 0 || ix < 0 || !ds_glyph(font, ih, &H, NULL, 0) || !ds_glyph(font, ix, &x, NULL, 0) || H.y1 < 0 || x.y1 < 0) return;
    m->base = (float)H.y1;
    m->cap = (float)(H.y1 - H.y0);
    m->xh = (float)(x.y1 - x.y0);
    m->scale = 0;
    Ttf* t = &g_ttf[font];
    if (t->ok) {
        int a0, b0, a1, b1, c0, d0, c1, d1;
        if (stbtt_GetCodepointBox(&t->info, 'H', &a0, &b0, &a1, &b1) && stbtt_GetCodepointBox(&t->info, 'x', &c0, &d0, &c1, &d1)) {
            /* between the two: the game's letters are a little taller for their x-height than most fonts */
            float s_cap = m->cap / (float)(b1 - b0), s_x = m->xh / (float)(d1 - d0);
            m->scale = (s_cap + s_x) * 0.5f;
        }
    }
    m->ok = 1;
}

/* ---------------------------------------------------------------- drawing */
static void from_pixels(const DsGlyph* g, uint8_t* cell) {
    /* the game's pixels, smoothed (a gaussian field cut at the level that keeps
     * a one-pixel line one pixel wide): round curves, clean diagonals */
    const float sigma = 0.55f, cut = 0.45f;
    static float field[(12 + 2 * TXG_M) * TXG_S + 2][(TXG_MAXW + 2 * TXG_M) * TXG_S + 2];
    for (int y = 0; y < TXG_CH; y++)
        for (int x = 0; x < TXG_CW; x++) {
            float u = (x + 0.5f) / TXG_S - TXG_M, v = (y + 0.5f) / TXG_S - TXG_M;   /* DS pixels from the glyph's top left */
            float f = 0;
            int cx = (int)floorf(u), cy = (int)floorf(v);
            for (int j = cy - 2; j <= cy + 2; j++)
                for (int i = cx - 2; i <= cx + 2; i++) {
                    if (i < 0 || j < 0 || i >= g->w || j >= 12 || !g->px[j][i]) continue;
                    /* a pixel's square, blurred: separable erf */
                    float ax = (erff((u - i) / (sigma * 1.41421356f)) - erff((u - i - 1) / (sigma * 1.41421356f))) * 0.5f;
                    float ay = (erff((v - j) / (sigma * 1.41421356f)) - erff((v - j - 1) / (sigma * 1.41421356f))) * 0.5f;
                    f += ax * ay;
                }
            field[y][x] = f;
        }
    for (int y = 0; y < TXG_CH; y++)
        for (int x = 0; x < TXG_CW; x++) {
            float f = field[y][x];
            float gx = (x + 1 < TXG_CW ? field[y][x + 1] : f) - (x > 0 ? field[y][x - 1] : f);
            float gy = (y + 1 < TXG_CH ? field[y + 1][x] : f) - (y > 0 ? field[y - 1][x] : f);
            float grad = sqrtf(gx * gx + gy * gy) * 0.5f;          /* per atlas pixel */
            float a = grad > 1e-4f ? (f - cut) / grad + 0.5f : (f > cut ? 1.f : 0.f);
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            cell[y * TXG_CW + x] = (uint8_t)(a * 255.f + 0.5f);
        }
}

static int from_ttf(int font, const DsGlyph* g, int cp, uint8_t* cell) {
    (void)0;
    Ttf* t = &g_ttf[font];
    Metrics* m = &g_met[font];
    if (!t->ok || !m->scale || !cp || !stbtt_FindGlyphIndex(&t->info, cp)) return 0;
    int b0, b1, b2, b3;
    if (!stbtt_GetCodepointBox(&t->info, cp, &b0, &b1, &b2, &b3)) return 1;   /* nothing to draw (a space) */
    float sy = m->scale * TXG_S, sx = sy;
    float tw = (b2 - b0) * m->scale, th = (b3 - b1) * m->scale;           /* DS pixels */
    float gw = (float)(g->x1 - g->x0), gh = (float)(g->y1 - g->y0);
    /* no wider than the game's letter (+1 pixel for the main font; the small one's
     * labels are set tight, so its letters keep the game's own width: a gap between them) */
    float room = font == 0 ? gw : gw + 1.0f;
    if (g->x1 > g->x0 && tw > room) { float k = room / tw; if (k < 0.7f) k = 0.7f; sx *= k; tw *= k; }
    /* where: centred on the game's ink; letters on the game's baseline, the rest on its ink's middle */
    float cxds = g->x1 > g->x0 ? (g->x0 + g->x1) * 0.5f : g->w * 0.5f;
    float ox = (TXG_M + cxds) * TXG_S - (b0 + b2) * 0.5f * sx;               /* the pen, atlas pixels */
    float oy;
    if (is_letter_cp(cp) || g->y1 <= g->y0) oy = (TXG_M + m->base) * TXG_S;
    else oy = (TXG_M + (g->y0 + g->y1) * 0.5f) * TXG_S + (b1 + b3) * 0.5f * sy;
    (void)gh; (void)th;
    int ix0, iy0, ix1, iy1;
    float fx = ox - floorf(ox), fy = oy - floorf(oy);
    stbtt_GetCodepointBitmapBoxSubpixel(&t->info, cp, sx, sy, fx, fy, &ix0, &iy0, &ix1, &iy1);
    int w = ix1 - ix0, h = iy1 - iy0;
    if (w <= 0 || h <= 0) return 1;
    uint8_t* tmp = calloc((size_t)w * h, 1);
    if (!tmp) return 1;
    stbtt_MakeCodepointBitmapSubpixel(&t->info, tmp, w, h, w, sx, sy, fx, fy, cp);
    int px = (int)floorf(ox) + ix0, py = (int)floorf(oy) + iy0;
    for (int y = 0; y < h; y++) {
        int cy = py + y;
        if (cy < 0 || cy >= TXG_CH) continue;
        for (int x = 0; x < w; x++) {
            int cx = px + x;
            if (cx < 0 || cx >= TXG_CW) continue;
            uint8_t v = tmp[y * w + x];
            if (v > cell[cy * TXG_CW + cx]) cell[cy * TXG_CW + cx] = v;
        }
    }
    free(tmp);
    return 1;
}

/* a label (text_hd.h): its string in its font, the tallest letters as tall as
 * the label's, on its baseline, from its ink's left edge, narrowed to its width */
static int label_render(int id, uint8_t* cell) {
    if (id < 0 || id >= g_tx_nlabels) return 0;
    const TxLabel* L = &g_tx_labels[id];
    Ttf* t = &g_ttf[L->font ? 1 : 0];
    if (!t->ok) t = &g_ttf[L->font ? 0 : 1];
    if (!t->ok) return 1;
    int n = (int)strlen(L->text);
    /* the string in font units: its tallest letter, its ink's left and right */
    int top = 0, prev = 0; float pen = 0, left = 1e9f, right = -1e9f;
    for (int i = 0; i < n; i++) {
        int cp = (unsigned char)L->text[i], adv, lsb, c0, d0, c1, d1;
        if (prev) pen += stbtt_GetCodepointKernAdvance(&t->info, prev, cp);
        if (stbtt_GetCodepointBox(&t->info, cp, &c0, &d0, &c1, &d1) && c1 > c0) {
            if (d1 > top) top = d1;
            if (pen + c0 < left) left = pen + c0;
            if (pen + c1 > right) right = pen + c1;
        }
        stbtt_GetCodepointHMetrics(&t->info, cp, &adv, &lsb);
        pen += adv; prev = cp;
    }
    if (top <= 0 || right <= left || L->base <= L->top) return 1;
    float scale = (float)(L->base - L->top) / (float)top;            /* DS pixels per font unit */
    float sx = scale, room = (float)(L->x1 - L->x0);
    if ((right - left) * sx > room) sx = room / (right - left);
    float pen0 = (TXG_M + L->x0) * TXG_S - left * sx * TXG_S, base = (TXG_M + L->base) * TXG_S;
    pen = pen0; prev = 0;
    for (int i = 0; i < n; i++) {
        int cp = (unsigned char)L->text[i], adv, lsb, ix0, iy0, ix1, iy1;
        if (prev) pen += stbtt_GetCodepointKernAdvance(&t->info, prev, cp) * sx * TXG_S;
        float fx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&t->info, cp, sx * TXG_S, scale * TXG_S, fx, 0, &ix0, &iy0, &ix1, &iy1);
        int w = ix1 - ix0, h = iy1 - iy0;
        if (w > 0 && h > 0) {
            uint8_t* tmp = calloc((size_t)w * h, 1);
            if (tmp) {
                stbtt_MakeCodepointBitmapSubpixel(&t->info, tmp, w, h, w, sx * TXG_S, scale * TXG_S, fx, 0, cp);
                int px = (int)floorf(pen) + ix0, py = (int)base + iy0;
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    int cx = px + x, cy = py + y;
                    if (cx < 0 || cy < 0 || cx >= TXG_LCW || cy >= TXG_LCH) continue;
                    uint8_t v = tmp[y * w + x];
                    if (v > cell[cy * TXG_LCW + cx]) cell[cy * TXG_LCW + cx] = v;
                }
                free(tmp);
            }
        }
        stbtt_GetCodepointHMetrics(&t->info, cp, &adv, &lsb);
        pen += adv * sx * TXG_S;
        prev = cp;
    }
    return 1;
}

/* a label cell made thicker by r atlas pixels (a round max filter) */
static void label_grow(uint8_t* cell, int r) {
    static uint8_t tmp[TXG_LCW * TXG_LCH];
    memcpy(tmp, cell, sizeof tmp);
    for (int y = 0; y < TXG_LCH; y++)
        for (int x = 0; x < TXG_LCW; x++) {
            int m = tmp[y * TXG_LCW + x];
            for (int dy = -r; dy <= r && m < 255; dy++) {
                int yy = y + dy; if (yy < 0 || yy >= TXG_LCH) continue;
                for (int dx = -r; dx <= r; dx++) {
                    int xx = x + dx; if (xx < 0 || xx >= TXG_LCW || dx * dx + dy * dy > r * r) continue;
                    int v = tmp[yy * TXG_LCW + xx]; if (v > m) m = v;
                }
            }
            cell[y * TXG_LCW + x] = (uint8_t)m;
        }
}

int txg_render(int font, int index, uint8_t* cell) {
    DsGlyph g;
    char name[32];
    if (font == 2 || font == 3) {
        memset(cell, 0, TXG_LCW * TXG_LCH);
        int id = font == 2 ? index : index - 64;
        if (!label_render(id, cell)) return 0;
        if (id >= 0 && id < g_tx_nlabels) {
            const TxLabel* L = &g_tx_labels[id];
            if (L->flags & 1) label_grow(cell, TXG_S / 4);               /* bold: thicker, like the original's strokes */
            if (font == 3) label_grow(cell, TXG_S);                      /* the outline: one DS pixel around */
        }
        return 1;
    }
    memset(cell, 0, TXG_CW * TXG_CH);
    if (font < 0 || font > 1 || !ds_glyph(font, index, &g, name, sizeof name)) return 0;
    metrics(font);
    if (g.x1 <= g.x0) return 1;                                     /* blank (a space) */
    if (from_ttf(font, &g, name_cp(name), cell)) return 1;
    from_pixels(&g, cell);
    return 1;
}

/* ---------------------------------------------------------------- tests
 * DQIX_TXDUMP with DQIX_TXHD=scale: the screens with HD text, drawn on the
 * CPU the way gl_render.c's shader does (to compare against). */
extern uint32_t g_engine_out[2][256 * 192];
void tx_debug_render(const char* path, int scale, int a_top) {
    static uint8_t* atlas;
    static int made[TXG_NCELLS];
    if (!atlas) {
        atlas = calloc((size_t)TXG_W * TXG_H, 1);
        const char* d = getenv("DQIX_TXFONTS");
        txg_load_fonts(d ? d : "fonts");
    }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int W = 256 * scale, H = 192 * scale;
    fprintf(f, "P6\n%d %d\n255\n", W, H * 2);
    static uint8_t cell[TXG_CELLMAX];
    uint8_t* row = malloc((size_t)W * 3);
    for (int sc = 0; sc < 2; sc++) {
        int eng = (sc == 0) == (a_top != 0) ? 0 : 1;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float u = (x + 0.5f) / scale, v = (y + 0.5f) / scale;
                int px = (int)u, py = (int)v, idx = py * 256 + px;
                uint32_t c = g_engine_out[eng][idx];
                uint16_t* sl = g_tx_slot[eng][idx];
                if (sl[0]) {
                    c = g_tx_notext[eng][idx];
                    float r = (float)((c >> 16) & 255), g = (float)((c >> 8) & 255), b = (float)(c & 255);
                    for (int k = 0; k < 4 && sl[k]; k++) {
                        TxInst* in = tx_inst_at(g_tx_list[eng][sl[k] - 1]);
                        int ci = txg_cell(in->font, in->index);
                        if (ci < 0) continue;
                        int cx0, cy0, cw, ch;
                        txg_cell_rect(ci, &cx0, &cy0, &cw, &ch);
                        if (!made[ci]) { made[ci] = 1; txg_render(in->font, in->index, cell);
                            for (int yy = 0; yy < ch; yy++) memcpy(&atlas[(size_t)(cy0 + yy) * TXG_W + cx0], &cell[yy * cw], cw); }
                        /* box-filter the atlas over this output pixel */
                        float lu = u - in->ox[eng] + TXG_M, lv = v - in->oy[eng] + TXG_M;
                        float a = 0; int n = 0, st = TXG_S / scale > 0 ? TXG_S / scale : 1;
                        int ax0 = (int)((lu - 0.5f / scale) * TXG_S), ay0 = (int)((lv - 0.5f / scale) * TXG_S);
                        for (int j = 0; j < st; j++) for (int i = 0; i < st; i++) {
                            int ax = ax0 + i, ay = ay0 + j;
                            if (ax >= 0 && ay >= 0 && ax < cw && ay < ch) a += atlas[(size_t)(cy0 + ay) * TXG_W + cx0 + ax];
                            n++;
                        }
                        a /= 255.f * n;
                        uint32_t ink = in->ink[eng];
                        r += (((ink >> 16) & 255) - r) * a; g += (((ink >> 8) & 255) - g) * a; b += ((ink & 255) - b) * a;
                    }
                    c = ((uint32_t)(r + .5f) << 16) | ((uint32_t)(g + .5f) << 8) | (uint32_t)(b + .5f);
                }
                row[x * 3] = (uint8_t)(c >> 16); row[x * 3 + 1] = (uint8_t)(c >> 8); row[x * 3 + 2] = (uint8_t)c;
            }
            fwrite(row, 1, (size_t)W * 3, f);
        }
    }
    free(row);
    fclose(f);
    if (getenv("DQIX_ATLASDUMP")) { FILE* g = fopen(getenv("DQIX_ATLASDUMP"), "wb");
        if (g) { fprintf(g, "P5\n%d %d\n255\n", TXG_W, TXG_H - TXG_GH); fwrite(atlas + (size_t)TXG_GH * TXG_W, 1, (size_t)TXG_W * (TXG_H - TXG_GH), g); fclose(g); } }
}

/* ---------------------------------------------------------------- letters in 3D textures
 * The battle messages over the field are 16-colour textures (one letter, its
 * shadow or outline, each 16x16) drawn by the 3D engine. Such a texture is
 * made again TXG_S times larger: the texels without letters as they are, the
 * letters' texels as what was under them, and the letters drawn over that in
 * high resolution, in the colour the game gave them. */
static uint8_t* cell_cache[512];
static const uint8_t* cell_for(int font, int index) {
    int c = txg_cell(font, index);
    if (c < 0 || c >= 512) return NULL;
    if (!cell_cache[c]) {
        uint8_t* b = malloc(TXG_CW * TXG_CH);
        if (!b) return NULL;
        if (!txg_render(font, index, b)) { free(b); return NULL; }
        cell_cache[c] = b;
    }
    return cell_cache[c];
}
void txg_forget_cells(void) { for (int i = 0; i < 512; i++) { free(cell_cache[i]); cell_cache[i] = NULL; } }

static uint8_t texv8(uint32_t a) { uint8_t* p = g_tex_slot[(a >> 17) & 3]; return p ? p[a & 0x1FFFF] : 0; }
static const TxTag* tex_tag(uint32_t a, int hi) {
    uint8_t* p = g_tex_slot[(a >> 17) & 3];
    if (!p) return NULL;
    TxTag* t = tx_vram_tags(p + (a & 0x1FFFF));
    return t && t[hi].inst && tx_inst_of(&t[hi]) ? &t[hi] : NULL;
}
static void pal_rgb(uint32_t pl, int idx, uint8_t* rgb) {
    uint32_t a = (pl << 4) + (uint32_t)idx * 2;
    uint8_t* p = g_texpal_slot[(a >> 14) % 6];
    uint16_t c = p ? (uint16_t)(p[a & 0x3FFF] | p[(a & 0x3FFF) + 1] << 8) : 0;
    rgb[0] = (uint8_t)(((c & 31) * 255 + 15) / 31); rgb[1] = (uint8_t)((((c >> 5) & 31) * 255 + 15) / 31); rgb[2] = (uint8_t)((((c >> 10) & 31) * 255 + 15) / 31);
}

typedef struct { TxInst* in; int ox, oy; } TexLetter;
static int by_serial_tl(const void* a, const void* b) {
    uint32_t sa = ((const TexLetter*)a)->in->serial, sb = ((const TexLetter*)b)->in->serial;
    return sa < sb ? -1 : sa > sb;
}

int txg_texture_hd(uint32_t ti, uint32_t pl, const uint8_t* rgba, int w, int h, uint8_t* out) {
    if (((ti >> 26) & 7) != 3 || w > 64 || h > 64) return 0;      /* 16 colours, small: the letters' textures */
    uint32_t base = (ti & 0xFFFF) << 3;
    int c0clear = (ti >> 29) & 1;
    static TexLetter L[64];
    int nl = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const TxTag* t = tex_tag(base + (uint32_t)((y * w + x) >> 1), x & 1);
            if (!t) continue;
            TxInst* in = tx_inst_of(t);
            int ox = x - t->col, oy = y - (t->row & 0x3F), k;
            for (k = 0; k < nl; k++) if (L[k].in == in && L[k].ox == ox && L[k].oy == oy) break;
            if (k == nl && nl < 64) { L[nl].in = in; L[nl].ox = ox; L[nl].oy = oy; nl++; }
        }
    if (!nl) return 0;
    qsort(L, nl, sizeof L[0], by_serial_tl);
    if (!g_tx_shadows) {                                            /* shadows and outlines left out */
        int m = 0;
        for (int k = 0; k < nl; k++) {
            int shadow = 0;
            for (int j = 0; j < nl; j++) {
                if (j == k || L[j].in->serial <= L[k].in->serial || L[j].in->glyph != L[k].in->glyph) continue;
                int dx = L[j].ox - L[k].ox, dy = L[j].oy - L[k].oy;
                if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && (dx || dy)) shadow = 1;
            }
            if (!shadow) L[m++] = L[k];
        }
        nl = m;
    }
    int W = w * TXG_S, H = h * TXG_S;
    /* the texels, with the letters' own as what was under them */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t px[4];
            memcpy(px, rgba + (y * w + x) * 4, 4);
            uint32_t a = base + (uint32_t)((y * w + x) >> 1);
            const TxTag* t = tex_tag(a, x & 1);
            if (t) {
                if (t->under == 0 && c0clear) px[3] = 0;
                else { pal_rgb(pl, t->under, px); px[3] = 255; }
            }
            (void)texv8;
            for (int yy = 0; yy < TXG_S; yy++)
                for (int xx = 0; xx < TXG_S; xx++) memcpy(out + (((size_t)(y * TXG_S + yy) * W) + x * TXG_S + xx) * 4, px, 4);
        }
    /* the letters, in drawing order */
    for (int k = 0; k < nl; k++) {
        const uint8_t* cell = cell_for(L[k].in->font, L[k].in->index);
        if (!cell) continue;
        uint8_t ink[3];
        pal_rgb(pl, L[k].in->color, ink);
        int x0 = (L[k].ox - TXG_M) * TXG_S, y0 = (L[k].oy - TXG_M) * TXG_S;
        for (int cy = 0; cy < TXG_CH; cy++) {
            int Y = y0 + cy;
            if (Y < 0 || Y >= H) continue;
            for (int cx = 0; cx < TXG_CW; cx++) {
                int X = x0 + cx;
                uint8_t a = cell[cy * TXG_CW + cx];
                if (X < 0 || X >= W || !a) continue;
                uint8_t* o = out + ((size_t)Y * W + X) * 4;
                for (int ch = 0; ch < 3; ch++) o[ch] = (uint8_t)((o[ch] * (255 - a) + ink[ch] * a + 127) / 255);
                if (a > o[3]) o[3] = a;
            }
        }
    }
    return 1;
}

/* A 16-colour texture that is a label (text_hd.h): TXG_S times larger, the
 * label's letters (and outlines) as what is behind them, the label drawn over
 * that in high resolution in its letters' colour. */
int txg_texture_label(uint32_t ti, uint32_t pl, const uint8_t* rgba, int w, int h, int label, int bx, int by, uint8_t* out) {
    if (label < 0 || label >= g_tx_nlabels || w > 64 || h > 64) return 0;
    const TxLabel* Lb = &g_tx_labels[label];
    uint32_t base = (ti & 0xFFFF) << 3;
    int c0clear = (ti >> 29) & 1, W = w * TXG_S, H = h * TXG_S;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t px[4];
            memcpy(px, rgba + (y * w + x) * 4, 4);
            int lx = x - bx, ly = y - by;
            if (lx >= 0 && ly >= 0 && lx < Lb->w && ly < Lb->h) {
                uint32_t a = base + (uint32_t)((y * w + x) >> 1);
                int idx = (texv8(a) >> ((x & 1) * 4)) & 15;
                if (idx && (Lb->inkmask >> idx & 1) && (g_tx_shadows == 0 || idx == Lb->main)) {
                    int u = Lb->rowbg[ly < 24 ? ly : 23];
                    if (u == 0 && c0clear) px[3] = 0;
                    else { pal_rgb(pl, u, px); px[3] = 255; }
                }
            }
            for (int yy = 0; yy < TXG_S; yy++)
                for (int xx = 0; xx < TXG_S; xx++) memcpy(out + (((size_t)(y * TXG_S + yy) * W) + x * TXG_S + xx) * 4, px, 4);
        }
    static uint8_t cell[TXG_LCW * TXG_LCH];
    if (!txg_render(2, label, cell)) return 1;
    uint8_t ink[3];
    pal_rgb(pl, Lb->main, ink);
    int x0 = (bx - TXG_M) * TXG_S, y0 = (by - TXG_M) * TXG_S;
    for (int cy = 0; cy < TXG_LCH; cy++) {
        int Y = y0 + cy;
        if (Y < 0 || Y >= H) continue;
        for (int cx = 0; cx < TXG_LCW; cx++) {
            int X = x0 + cx;
            uint8_t a = cell[cy * TXG_LCW + cx];
            if (X < 0 || X >= W || !a) continue;
            uint8_t* o = out + ((size_t)Y * W + X) * 4;
            for (int ch = 0; ch < 3; ch++) o[ch] = (uint8_t)((o[ch] * (255 - a) + ink[ch] * a + 127) / 255);
            if (a > o[3]) o[3] = a;
        }
    }
    return 1;
}
