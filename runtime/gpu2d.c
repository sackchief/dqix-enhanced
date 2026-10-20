/* The DS's two 2D graphics engines (A = main, B = sub), rendered one
 * scanline at a time like the hardware, so mid-frame register changes work.
 *
 * Per engine: 4 backgrounds (text, affine, extended/bitmap; BG0 can be the 3D
 * layer on engine A), 128 sprites, windows, colour effects (alpha blend,
 * brighten, darken) and master brightness. Reference: GBATEK. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "hdtext.h"
extern int g_comp_gl;                                           /* (below) GL composition: the 3D and HD text on the GPU */

uint32_t g_screen[2][256 * 192];
uint32_t g_engine_out[2][256 * 192];

/* 3D layer of engine A for the current line (gpu3d.c fills it later). */
uint32_t g_3d_line32[256];     /* 6-bit colour | alpha << 24 (alpha 0 = transparent) */
int      g_3d_present;
void gpu3d_render_line(int line);

typedef struct {
    int base;                  /* register offset in g_io: 0x000 or 0x1000 */
    uint32_t bgvram, objvram;  /* virtual VRAM addresses */
    uint32_t pal;              /* palette RAM offset */
    int32_t refx[2], refy[2];  /* BG2/BG3 affine reference points (advance every line) */
    int32_t refx_d[2], refy_d[2];  /* ... as used for drawing (held between BG mosaic lines) */
    /* mosaic counters (the hardware runs them every scanline) */
    int bg_mos_y, bg_mos_ymax, bg_mos_latch, bg_mos_line;
    int obj_mos_y, obj_mos_line;
} Engine;

static Engine g_eng[2] = {
    { 0x0000, 0x06000000, 0x06400000, 0x000, {0}, {0} },
    { 0x1000, 0x06200000, 0x06600000, 0x400, {0}, {0} },
};

static inline uint16_t R16(Engine* e, uint32_t off) { uint16_t v; memcpy(&v, &g_io[e->base + off], 2); return v; }
static inline uint32_t R32(Engine* e, uint32_t off) { uint32_t v; memcpy(&v, &g_io[e->base + off], 4); return v; }

static inline uint8_t vr8(uint32_t a) { uint8_t* p = vram_ptr(a); return p ? *p : 0; }
static inline uint16_t vr16(uint32_t a) { uint8_t* p = vram_ptr(a & ~1u); uint16_t v = 0; if (p) memcpy(&v, p, 2); return v; }
static inline uint16_t pal16(uint32_t off) { uint16_t v; memcpy(&v, &g_palette[off & 0x7FE], 2); return v; }

/* ---------------------------------------------------------------- extended palettes */
static const uint8_t* ext_bg_pal(int eng, int slot) {
    if (eng == 0) {
        uint8_t cnt = vram_bank_cnt(4);                          /* E: all 4 slots */
        if ((cnt & 0x87) == 0x84) return vram_bank(4) + slot * 0x2000;
        for (int b = 5; b <= 6; b++) {                           /* F, G: 2 slots each */
            cnt = vram_bank_cnt(b);
            if ((cnt & 0x87) == 0x84) {
                int first = ((cnt >> 3) & 1) * 2;
                if (slot == first || slot == first + 1) return vram_bank(b) + (slot - first) * 0x2000;
            }
        }
        return NULL;
    }
    if ((vram_bank_cnt(7) & 0x87) == 0x82) return vram_bank(7) + slot * 0x2000;   /* H */
    return NULL;
}
static const uint8_t* ext_obj_pal(int eng) {
    if (eng == 0) {
        if ((vram_bank_cnt(4) & 0x87) == 0x85) return vram_bank(4);
        for (int b = 5; b <= 6; b++) if ((vram_bank_cnt(b) & 0x87) == 0x85) return vram_bank(b);
        return NULL;
    }
    if ((vram_bank_cnt(8) & 0x87) == 0x83) return vram_bank(8);                   /* I */
    return NULL;
}
static inline uint16_t ext16(const uint8_t* p, uint32_t idx) { uint16_t v = 0; if (p) memcpy(&v, p + idx * 2, 2); return v; }

/* ---------------------------------------------------------------- line buffers */
#define PX_OPAQUE 0x10000u
typedef struct { uint32_t col[256]; uint32_t src[256]; uint32_t tag[256]; int tags; } BgLine;   /* raw 16-bit colour | PX_OPAQUE; src: where it came from (VRAM byte * 2 + high nibble); tag: HD text (hdtext.h) */
/* flag: 0x10 normal sprite, 0x80 semi-transparent, 0xC0 | (alpha + 1) bitmap sprite */
typedef struct { uint32_t col[256]; uint8_t prio[256]; uint8_t flag[256]; uint8_t win[256]; uint8_t mos[256]; uint32_t src[256]; } ObjLine;

static void render_text_bg(Engine* e, int eng, int bg, int y, BgLine* out) {
    uint32_t disp = R32(e, 0);
    uint16_t cnt = R16(e, 0x08 + bg * 2);
    uint32_t hofs = R16(e, 0x10 + bg * 4) & 0x1FF, vofs = R16(e, 0x12 + bg * 4) & 0x1FF;
    uint32_t charbase = e->bgvram + ((cnt >> 2) & 15) * 0x4000 + (eng == 0 ? ((disp >> 24) & 7) * 0x10000 : 0);
    uint32_t mapbase  = e->bgvram + ((cnt >> 8) & 31) * 0x800  + (eng == 0 ? ((disp >> 27) & 7) * 0x10000 : 0);
    int size = cnt >> 14;
    uint32_t w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    int c256 = (cnt >> 7) & 1;
    const uint8_t* ext = NULL;
    if (c256 && (disp & 0x40000000)) {
        int slot = bg;
        if (bg < 2 && (cnt & 0x2000)) slot = bg + 2;
        ext = ext_bg_pal(eng, slot);
    }
    uint32_t yy = (y + vofs) & (h - 1);
    uint32_t ty = yy >> 3;
    int hd = g_hdtext_on && g_hdtext_any && g_comp_gl && !c256;   /* the letters are 4-bit */
    out->tags = 0;
    for (int x = 0; x < 256;) {
        uint32_t xx = (x + hofs) & (w - 1);
        uint32_t tx = xx >> 3;
        uint32_t sb = (tx >> 5) + (ty >> 5) * (w >> 8);
        uint16_t ent = vr16(mapbase + sb * 0x800 + (((ty & 31) * 32 + (tx & 31)) * 2));
        uint32_t py = yy & 7;
        if (ent & 0x800) py = 7 - py;
        uint32_t tile = ent & 0x3FF;
        int flip = (ent & 0x400) != 0;
        /* one tile row: 8 (or 4) bytes, contiguous within a 16 KB VRAM page */
        const uint8_t* row = c256 ? vram_ptr(charbase + tile * 64 + py * 8) : vram_ptr(charbase + tile * 32 + py * 4);
        uint32_t palbase = c256 ? 0 : (ent >> 12) * 16;
        for (uint32_t px0 = xx & 7; px0 < 8 && x < 256; px0++, x++) {
            uint32_t px = flip ? 7 - px0 : px0;
            uint32_t idx;
            if (!row) idx = 0;
            else if (c256) idx = row[px];
            else idx = (row[px >> 1] >> ((px & 1) * 4)) & 15;
            out->src[x] = c256 ? (charbase + tile * 64 + py * 8 + px) * 2 : (charbase + tile * 32 + py * 4 + (px >> 1)) * 2 + (px & 1);
            if (hd) { uint32_t t = row ? hdtext_tag(out->src[x]) : 0; out->tag[x] = t; out->tags |= t != 0; }
            if (!idx) { out->col[x] = 0; continue; }
            uint16_t col;
            if (c256) col = ext ? ext16(ext, (ent >> 12) * 256 + idx) : pal16(e->pal + idx * 2);
            else col = pal16(e->pal + (palbase + idx) * 2);
            out->col[x] = col | PX_OPAQUE;
        }
    }
}

/* Affine / extended / bitmap backgrounds (BG2, BG3). kind: 1 affine, 2 extended. */
static void render_affine_bg(Engine* e, int eng, int bg, int kind, BgLine* out) {
    uint32_t disp = R32(e, 0);
    uint16_t cnt = R16(e, 0x08 + bg * 2);
    int16_t pa = (int16_t)R16(e, 0x20 + (bg - 2) * 16), pc = (int16_t)R16(e, 0x24 + (bg - 2) * 16);
    int32_t rx = e->refx_d[bg - 2], ry = e->refy_d[bg - 2];
    int wrap = (cnt >> 13) & 1;
    int size = cnt >> 14;
    uint32_t charbase = e->bgvram + ((cnt >> 2) & 15) * 0x4000 + (eng == 0 ? ((disp >> 24) & 7) * 0x10000 : 0);
    uint32_t mapbase  = e->bgvram + ((cnt >> 8) & 31) * 0x800  + (eng == 0 ? ((disp >> 27) & 7) * 0x10000 : 0);
    int bitmap = kind == 2 && (cnt & 0x80);
    int direct = bitmap && (cnt & 0x04);
    uint32_t w, h;
    if (bitmap) {
        static const uint32_t bw[4] = { 128, 256, 512, 512 }, bh[4] = { 128, 256, 256, 512 };
        w = bw[size]; h = bh[size];
    } else {
        w = h = 128u << size;
    }
    const uint8_t* ext = NULL;
    if (kind == 2 && !bitmap && (disp & 0x40000000)) ext = ext_bg_pal(eng, bg);
    uint32_t bmbase = e->bgvram + ((cnt >> 8) & 31) * 0x4000;
    out->tags = 0;
    for (int x = 0; x < 256; x++, rx += pa, ry += pc) {
        int32_t tx = rx >> 8, ty = ry >> 8;
        if (wrap) { tx &= (int32_t)w - 1; ty &= (int32_t)h - 1; }
        else if (tx < 0 || ty < 0 || tx >= (int32_t)w || ty >= (int32_t)h) { out->col[x] = 0; continue; }
        uint16_t col;
        if (direct) {
            col = vr16(bmbase + (ty * w + tx) * 2);
            if (!(col & 0x8000)) { out->col[x] = 0; continue; }
        } else if (bitmap) {
            uint8_t idx = vr8(bmbase + ty * w + tx);
            if (!idx) { out->col[x] = 0; continue; }
            col = pal16(e->pal + idx * 2);
        } else if (kind == 2) {                                     /* 16-bit map entries, 256 colours */
            uint16_t ent = vr16(mapbase + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2);
            uint32_t px = tx & 7, py = ty & 7;
            if (ent & 0x400) px = 7 - px;
            if (ent & 0x800) py = 7 - py;
            uint8_t idx = vr8(charbase + (ent & 0x3FF) * 64 + py * 8 + px);
            if (!idx) { out->col[x] = 0; continue; }
            col = ext ? ext16(ext, (ent >> 12) * 256 + idx) : pal16(e->pal + idx * 2);
        } else {                                                    /* 8-bit map entries */
            uint8_t tile = vr8(mapbase + (ty >> 3) * (w >> 3) + (tx >> 3));
            uint8_t idx = vr8(charbase + tile * 64 + (ty & 7) * 8 + (tx & 7));
            if (!idx) { out->col[x] = 0; continue; }
            col = pal16(e->pal + idx * 2);
        }
        out->col[x] = col | PX_OPAQUE;
    }
}

/* ---------------------------------------------------------------- sprites */
static const uint8_t obj_w[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t obj_h[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static void render_objs(Engine* e, int eng, int y, ObjLine* out) {
    uint32_t disp = R32(e, 0);
    const uint8_t* oam = &g_oam[eng * 0x400];
    const uint8_t* ext = (disp & 0x80000000u) ? ext_obj_pal(eng) : NULL;
    memset(out->col, 0, sizeof out->col);
    memset(out->prio, 4, sizeof out->prio);
    memset(out->flag, 0, sizeof out->flag);
    memset(out->mos, 0, sizeof out->mos);
    memset(out->win, 0, sizeof out->win);
    for (int i = 0; i < 128; i++) {
        uint16_t a0, a1, a2;
        memcpy(&a0, oam + i * 8, 2); memcpy(&a1, oam + i * 8 + 2, 2); memcpy(&a2, oam + i * 8 + 4, 2);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) continue;                     /* hidden */
        int shape = (a0 >> 14) & 3, sz = (a1 >> 14) & 3;
        if (shape == 3) continue;
        int w = obj_w[shape][sz], h = obj_h[shape][sz];
        int bw = w, bh = h;                                         /* bounding box */
        if (affine && (a0 & 0x200)) { bw *= 2; bh *= 2; }
        int oy = a0 & 0xFF;
        int ly = (y - oy) & 0xFF;
        if (ly >= bh) continue;
        int mosaic = (a0 >> 12) & 1;
        if (mosaic && ((a0 >> 10) & 3) != 2) {                      /* OBJ mosaic: the latched line (not for OBJ-window sprites) */
            ly = (e->obj_mos_line - oy) & 0xFF;
            if (ly >= bh) ly = 0;
        }
        int ox = a1 & 0x1FF;
        if (ox >= 256) ox -= 512;
        int mode = (a0 >> 10) & 3;
        int c256 = (a0 >> 13) & 1;
        int prio = (a2 >> 10) & 3;
        uint32_t tile = a2 & 0x3FF;
        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            int p = (a1 >> 9) & 31;
            memcpy(&pa, oam + p * 32 + 6, 2); memcpy(&pb, oam + p * 32 + 14, 2);
            memcpy(&pc, oam + p * 32 + 22, 2); memcpy(&pd, oam + p * 32 + 30, 2);
        }
        for (int lx = 0; lx < bw; lx++) {
            int sx = ox + lx;
            if (sx < 0 || sx >= 256) continue;
            int tx, ty;
            if (affine) {
                int cx = lx - bw / 2, cy = ly - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            }
            uint16_t col;
            int opaque;
            uint32_t objsrc = 0;
            if (mode == 3) {                                        /* bitmap sprite */
                uint32_t addr;
                if (disp & 0x40) addr = e->objvram + tile * (128u << ((disp >> 22) & 1)) + (ty * w + tx) * 2;
                else if (disp & 0x20) addr = e->objvram + ((tile & 0x1F) * 0x10 + (tile & ~0x1Fu) * 0x80) + (ty * 256 + tx) * 2;
                else addr = e->objvram + ((tile & 0x0F) * 0x10 + (tile & ~0x0Fu) * 0x80) + (ty * 128 + tx) * 2;
                col = vr16(addr);
                opaque = (col & 0x8000) && (a2 >> 12);                  /* alpha 0: not drawn at all */
            } else {
                uint32_t base, rowstride;
                if (disp & 0x10) {                                  /* 1D mapping */
                    base = e->objvram + tile * (32u << ((disp >> 20) & 3));
                    rowstride = (w / 8) * (c256 ? 64 : 32);
                } else {
                    base = e->objvram + tile * 32;
                    rowstride = 32 * 32;
                }
                uint32_t addr = base + (ty / 8) * rowstride + (tx / 8) * (c256 ? 64 : 32);
                uint8_t idx;
                if (c256) idx = vr8(addr + (ty & 7) * 8 + (tx & 7));
                else { uint8_t b = vr8(addr + (ty & 7) * 4 + (tx & 7) / 2); idx = (tx & 1) ? b >> 4 : b & 15; }
                if (idx) objsrc = c256 ? (addr + (ty & 7) * 8 + (tx & 7)) * 2 : (addr + (ty & 7) * 4 + (tx & 7) / 2) * 2 + (tx & 1);
                opaque = idx != 0;
                if (c256) col = ext ? ext16(ext, (a2 >> 12) * 256 + idx) : pal16(e->pal + 0x200 + idx * 2);
                else col = pal16(e->pal + 0x200 + ((a2 >> 12) * 16 + idx) * 2);
            }
            if (mode == 2) { if (opaque) out->win[sx] = 1; continue; }
            if (!opaque) {                                          /* clear pixel: still carries priority / mosaic */
                if (!(out->col[sx] & PX_OPAQUE)) { out->prio[sx] = (uint8_t)prio; out->mos[sx] = (uint8_t)mosaic; }
                continue;
            }
            if (!(out->col[sx] & PX_OPAQUE) || prio < out->prio[sx]) {
                out->col[sx] = col | PX_OPAQUE;
                out->prio[sx] = (uint8_t)prio;
                out->flag[sx] = mode == 3 ? (uint8_t)(0xC0 | ((a2 >> 12) + 1)) : mode == 1 ? 0x80 : 0x10;
                out->mos[sx] = (uint8_t)mosaic;
                out->src[sx] = objsrc;
            }
        }
    }
}

/* ---------------------------------------------------------------- mosaic */
/* Sprite X mosaic, after all sprites of the line: a pixel's value is latched
 * at the start of each block, and also when mosaic starts/stops or a higher
 * priority pixel appears. */
static void obj_mosaic_x(ObjLine* o, int mosw) {
    if (!mosw) return;
    int mosx = 0;
    uint32_t lc = 0; uint8_t lp = 4, lf = 0, lm = 0;
    for (int i = 0; i < 256; i++) {
        int latch = mosx == 0 || !o->mos[i] || !lm || o->prio[i] < lp;
        if (latch) { lc = o->col[i]; lp = o->prio[i]; lf = o->flag[i]; lm = o->mos[i]; }
        o->col[i] = lc; o->prio[i] = lp; o->flag[i] = lf; o->mos[i] = lm;
        if (mosx == mosw) mosx = 0; else mosx++;
    }
}
/* BG X mosaic: each block repeats its first pixel. */
static void bg_mosaic_x(BgLine* l, int mosw) {
    if (!mosw) return;
    for (int x = 0; x < 256; x++) l->col[x] = l->col[x - x % (mosw + 1)];
}

/* ---------------------------------------------------------------- composition */
/* The DS mixes layers at 6 bits per channel (18-bit colour). Pixels here are
 * packed like the hardware's line buffer: r | g << 8 | b << 16 (6 bits each),
 * with the layer flag in the top byte: 0x01-0x08 BG0-3, 0x10 sprite, 0x20
 * backdrop, 0x40 | alpha (5 bits) the 3D layer, 0x80 semi-transparent sprite,
 * 0xC0 | (alpha + 1) bitmap sprite. The rounding rules follow the hardware. */
static inline uint32_t to6(uint32_t c) {                 /* 15-bit colour -> 6-bit channels */
    uint32_t r = (c & 0x1F) << 1;
    uint32_t g = ((c & 0x3E0) >> 4) | ((c & 0x8000) >> 15);   /* bit 15 lands in green's low bit */
    uint32_t b = (c & 0x7C00) >> 9;
    return r | (g << 8) | (b << 16);
}
static inline uint32_t blend4(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb) {    /* BLDALPHA */
    uint32_t r = ((a & 0x3F) * eva + (b & 0x3F) * evb + 8) >> 4;
    uint32_t g = (((a >> 8) & 0x3F) * eva + ((b >> 8) & 0x3F) * evb + 8) >> 4;
    uint32_t bl = (((a >> 16) & 0x3F) * eva + ((b >> 16) & 0x3F) * evb + 8) >> 4;
    if (r > 63) r = 63; if (g > 63) g = 63; if (bl > 63) bl = 63;
    return r | (g << 8) | (bl << 16);
}
static inline uint32_t blend3d(uint32_t a, uint32_t b) {        /* 3D layer over what is below, by its own alpha */
    uint32_t eva = ((a >> 24) & 0x1F) + 1, evb = 32 - eva;
    if (eva == 32) return a & 0x3F3F3F;
    uint32_t r = ((a & 0x3F) * eva + (b & 0x3F) * evb + 16) >> 5;
    uint32_t g = (((a >> 8) & 0x3F) * eva + ((b >> 8) & 0x3F) * evb + 16) >> 5;
    uint32_t bl = (((a >> 16) & 0x3F) * eva + ((b >> 16) & 0x3F) * evb + 16) >> 5;
    if (r > 63) r = 63; if (g > 63) g = 63; if (bl > 63) bl = 63;
    return r | (g << 8) | (bl << 16);
}
static inline uint32_t bright_up(uint32_t c, uint32_t f, uint32_t bias) {
    uint32_t r = c & 0x3F, g = (c >> 8) & 0x3F, b = (c >> 16) & 0x3F;
    r += ((63 - r) * f + bias) >> 4; g += ((63 - g) * f + bias) >> 4; b += ((63 - b) * f + bias) >> 4;
    return r | (g << 8) | (b << 16);
}
static inline uint32_t bright_down(uint32_t c, uint32_t f, uint32_t bias) {
    uint32_t r = c & 0x3F, g = (c >> 8) & 0x3F, b = (c >> 16) & 0x3F;
    r -= (r * f + bias) >> 4; g -= (g * f + bias) >> 4; b -= (b * f + bias) >> 4;
    return r | (g << 8) | (b << 16);
}
static inline uint32_t master_bright(uint32_t c, uint16_t mb) {
    uint32_t mode = mb >> 14, f = mb & 31;
    if (f > 16) f = 16;
    if (mode == 1) return bright_up(c, f, 0);
    if (mode == 2) return bright_down(c, f, 15);
    return c;
}
static inline uint32_t to_rgb6(uint32_t c) {             /* 6-bit channels -> 0xRRGGBB like the LCD */
    uint32_t r = c & 0x3F, g = (c >> 8) & 0x3F, b = (c >> 16) & 0x3F;
    return ((r << 2 | r >> 4) << 16) | ((g << 2 | g >> 4) << 8) | (b << 2 | b >> 4);
}

/* Colour effects for one pixel: v1 = top layer, v2 = the one below it. */
static uint32_t color_composite(uint32_t v1, uint32_t v2, uint16_t bldcnt, int fx, uint32_t eva, uint32_t evb, uint32_t evy) {
    uint32_t f1 = v1 >> 24, f2 = v2 >> 24;
    uint32_t t2 = (f2 & 0x80) ? 0x1000 : (f2 & 0x40) ? 0x0100 : (f2 << 8);     /* is the lower layer a 2nd target? */
    if ((f1 & 0x80) && (bldcnt & t2)) {                                       /* semi-transparent / bitmap sprite */
        if (f1 & 0x40) { eva = f1 & 0x1F; evb = 16 - eva; }
        return blend4(v1, v2, eva, evb);
    }
    if ((f1 & 0x40) && (bldcnt & t2)) return blend3d(v1, v2);                 /* 3D layer: its own alpha */
    uint32_t layer = (f1 & 0x80) ? 0x10 : (f1 & 0x40) ? 0x01 : f1;
    if ((bldcnt & layer) && fx) {
        switch ((bldcnt >> 6) & 3) {
        case 1: if (bldcnt & t2) return blend4(v1, v2, eva, evb); break;
        case 2: return bright_up(v1, evy, 8);
        case 3: return bright_down(v1, evy, 7);
        }
    }
    return v1 & 0x3F3F3F;
}

/* GL mode outputs (engine A): see gl_render.c */
int g_comp_gl;
int g_comp_has3d;
uint32_t g_comp_c2[256 * 192];
uint8_t g_comp_info[256 * 192 * 4];

/* ---------------------------------------------------------------- display capture
 * DISPCAPCNT: if enabled when a frame starts, each line (up to the capture
 * height) of engine A's picture and/or a VRAM bank (blended) is written to a
 * VRAM bank mapped to the LCDC. The enable bit clears at VBlank. */
static int g_cap_active;
extern uint32_t g_3d_line32[256];
extern int g_3d_present;

static void capture_start(void) {
    uint32_t cnt; memcpy(&cnt, &g_io[0x64], 4);
    g_cap_active = 0;
    if (!(cnt & 0x80000000u)) return;
    int bank = (cnt >> 16) & 3;
    if ((vram_bank_cnt(bank) & 0x87) != 0x80) return;               /* destination must be mapped to the LCDC */
    g_cap_active = 1;
}

static void capture_line(int y, const uint32_t* line6) {
    uint32_t cnt; memcpy(&cnt, &g_io[0x64], 4);
    int sz = (cnt >> 20) & 3;
    int width = sz == 0 ? 128 : 256, height = sz == 0 ? 128 : 64 * sz;
    if (y >= height) return;
    int dbank = (cnt >> 16) & 3;
    uint8_t* dvram = vram_bank(dbank);
    uint32_t daddr = (((cnt >> 18) & 3) << 14) + (uint32_t)(y * width);
    /* source A: the 2D picture, or the 3D layer alone */
    static uint32_t srca[256];
    if (cnt & (1u << 24)) for (int x = 0; x < 256; x++) srca[x] = g_3d_present ? g_3d_line32[x] : 0;
    else for (int x = 0; x < 256; x++) srca[x] = line6[x] | 0xFF000000u;
    /* source B: a VRAM bank (the display FIFO is not emulated) */
    const uint8_t* bvram = NULL; uint32_t boff = 0;
    if (!(cnt & (1u << 25))) {
        uint32_t disp; memcpy(&disp, &g_io[0], 4);
        int sbank = (disp >> 18) & 3;
        if ((vram_bank_cnt(sbank) & 0x87) == 0x80) {
            bvram = vram_bank(sbank);
            boff = (uint32_t)(y * 256);
            if (((disp >> 16) & 3) != 2) boff += ((cnt >> 26) & 3) << 14;
        }
    }
    uint32_t eva = cnt & 0x1F, evb = (cnt >> 8) & 0x1F;
    if (eva > 16) eva = 16; if (evb > 16) evb = 16;
    int mode = (cnt >> 29) & 3;
    for (int x = 0; x < width; x++) {
        uint32_t a = srca[x];
        uint32_t ra = (a >> 1) & 0x1F, ga = (a >> 9) & 0x1F, ba = (a >> 17) & 0x1F, aa = (a >> 24) != 0;
        uint16_t b = 0;
        if (bvram) memcpy(&b, bvram + (((boff + (uint32_t)x) & 0xFFFF) * 2), 2);
        uint16_t out;
        if (mode == 0) out = (uint16_t)(ra | (ga << 5) | (ba << 10) | (aa << 15));
        else if (mode == 1) out = bvram ? b : 0;
        else {
            uint32_t rb = b & 0x1F, gb = (b >> 5) & 0x1F, bb = (b >> 10) & 0x1F, ab = bvram ? b >> 15 : 0;
            uint32_t r = (ra * aa * eva + rb * ab * evb + 8) >> 4, g = (ga * aa * eva + gb * ab * evb + 8) >> 4, bl = (ba * aa * eva + bb * ab * evb + 8) >> 4;
            uint32_t ad = (eva > 0 ? aa : 0) | (evb > 0 ? ab : 0);
            if (r > 31) r = 31; if (g > 31) g = 31; if (bl > 31) bl = 31;
            out = (uint16_t)(r | (g << 5) | (bl << 10) | (ad << 15));
        }
        memcpy(dvram + (((daddr + (uint32_t)x) & 0xFFFF) * 2), &out, 2);
    }
}

static void compose_line(Engine* e, int eng, int y, uint32_t disp, uint32_t* line6, uint32_t* dst, uint16_t mb);

static void render_engine_line(int eng, int y) {
    Engine* e = &g_eng[eng];
    uint32_t* dst = &g_engine_out[eng][y * 256];
    uint32_t disp = R32(e, 0);
    if (eng == 0 && y == 0) g_comp_has3d = 0;                       /* set again below if the 3D layer shows */
    if (eng == 0 && g_comp_gl) memset(&g_comp_info[y * 256 * 4], 0, 256 * 4);
    uint16_t pow; memcpy(&pow, &g_io[0x304], 2);
    uint16_t mb = R16(e, 0x6C);
    int dmode = (disp >> 16) & 3;
    if (eng == 1) dmode &= 1;
    /* The BG/OBJ picture (before master brightness) is needed to display it,
     * or for display capture (which can grab it while a VRAM bank is shown). */
    static uint32_t line6[256];
    int capture = eng == 0 && g_cap_active;
    int need = dmode == 1 || capture;
    int composed = 0;
    if (need) {
        if (!(pow & (eng == 0 ? 0x2 : 0x200))) {                    /* engine powered off: A black, B white */
            for (int x = 0; x < 256; x++) line6[x] = eng == 0 ? 0 : 0x3F3F3F;
        } else if (disp & 0x80) {                                   /* forced blank: white */
            for (int x = 0; x < 256; x++) line6[x] = 0x3F3F3F;
        } else composed = 1;
    }
    if (composed) compose_line(e, eng, y, disp, line6, dmode == 1 ? dst : NULL, mb);
    if (dmode == 0) for (int x = 0; x < 256; x++) dst[x] = to_rgb6(0x3F3F3F);           /* screen off: white */
    else if (dmode == 2 || dmode == 3) {                            /* a VRAM bank shown directly (FIFO mode: unsupported, black) */
        int b = (disp >> 18) & 3;
        for (int x = 0; x < 256; x++) {
            uint16_t c = 0;
            if (dmode == 2) memcpy(&c, vram_bank(b) + (y * 256 + x) * 2, 2);
            dst[x] = to_rgb6(master_bright(to6(c & 0x7FFF), mb));
        }
    } else if (!composed) for (int x = 0; x < 256; x++) dst[x] = to_rgb6(master_bright(line6[x], mb));
    if (capture) capture_line(y, line6);
}

/* Compose one line of BG/OBJ layers (6-bit, before master brightness) into
 * line6; when dst is given, also write the displayed pixels (and the GPU
 * renderer's per-pixel info). */
static int g_pixsrc_on = -1, g_pixsrc[4];                       /* DQIX_PIXSRC=frame,engine,x,y: where that pixel comes from (debugging) */
static void compose_line(Engine* e, int eng, int y, uint32_t disp, uint32_t* line6, uint32_t* dst, uint16_t mb) {
    if (g_pixsrc_on < 0) { const char* ps = getenv("DQIX_PIXSRC"); g_pixsrc_on = ps && sscanf(ps, "%d,%d,%d,%d", &g_pixsrc[0], &g_pixsrc[1], &g_pixsrc[2], &g_pixsrc[3]) == 4; }

    int mode = disp & 7;
    static BgLine bgl[4];
    static ObjLine objl;
    int bg_on[4] = { 0 };
    int bg_3d = eng == 0 && (disp & 8);
    for (int bg = 0; bg < 4; bg++) {
        if (!((disp >> (8 + bg)) & 1)) continue;
        int kind;                                                   /* 0 text, 1 affine, 2 extended, 3 = 3D */
        if (bg < 2) kind = (bg == 0 && bg_3d) ? 3 : 0;
        else if (bg == 2) kind = mode <= 1 || mode == 3 ? 0 : (mode == 2 || mode == 4) ? 1 : 2;
        else kind = mode == 0 ? 0 : (mode <= 2) ? 1 : 2;
        if (bg == 2 && mode == 1) kind = 0;
        if (bg == 3 && mode == 1) kind = 1;
        if (mode == 6 && bg != 0 && bg != 2) continue;
        if (mode == 7 && bg >= 2) continue;
        bg_on[bg] = 1;
        if (kind == 3) continue;                                    /* read per pixel below */
        int mos = (R16(e, 0x08 + bg * 2) & 0x40) != 0;
        if (kind == 0) render_text_bg(e, eng, bg, mos ? e->bg_mos_line : y, &bgl[bg]);
        else render_affine_bg(e, eng, bg, kind, &bgl[bg]);
        if (mos) bg_mosaic_x(&bgl[bg], R16(e, 0x4C) & 15);
    }
    int obj_on = (disp >> 12) & 1;
    if (obj_on) { render_objs(e, eng, y, &objl); obj_mosaic_x(&objl, (R16(e, 0x4C) >> 8) & 15); }
    { static int lg = -1; if (lg < 0) lg = getenv("DQIX_MOSAICLOG") != NULL;
      if (lg && R16(e, 0x4C)) { int use = 0; for (int bg = 0; bg < 4; bg++) if (bg_on[bg] && (R16(e, 0x08 + bg * 2) & 0x40)) use = 1;
          static uint64_t last = ~0ull; if (use && g_frame != last) { last = g_frame; fprintf(stderr, "mosaic f%llu eng %d reg %04x\n", (unsigned long long)g_frame, eng, R16(e, 0x4C)); } } }

    /* windows */
    uint8_t winmask[256];
    int win_any = (disp >> 13) & 7;
    uint16_t winin = R16(e, 0x48), winout = R16(e, 0x4A);
    if (!win_any) memset(winmask, 0x3F, sizeof winmask);
    else {
        for (int x = 0; x < 256; x++) winmask[x] = winout & 0x3F;
        if ((disp & 0x4000) && obj_on) for (int x = 0; x < 256; x++) if (objl.win[x]) winmask[x] = (winout >> 8) & 0x3F;
        for (int wi = 1; wi >= 0; wi--) {
            if (!((disp >> (13 + wi)) & 1)) continue;
            uint16_t wh = R16(e, 0x40 + wi * 2), wv = R16(e, 0x44 + wi * 2);
            int x1 = wh >> 8, x2 = wh & 0xFF, y1 = wv >> 8, y2 = wv & 0xFF;
            int iny = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
            if (!iny) continue;
            for (int x = 0; x < 256; x++) {
                int inx = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
                if (inx) winmask[x] = (winin >> (wi * 8)) & 0x3F;
            }
        }
    }

    uint16_t bldcnt = R16(e, 0x50), bldalpha = R16(e, 0x52), bldy = R16(e, 0x54);
    uint32_t eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = bldy & 31;
    if (eva > 16) eva = 16; if (evb > 16) evb = 16; if (evy > 16) evy = 16;
    uint32_t backdrop = to6(pal16(e->pal)) | 0x20000000u;

    /* drawing order, back to front (the hardware's painter order): for each
     * priority 3..0: BG3, BG2, BG1, BG0, then that priority's sprites */
    int order[20], norder = 0;                                      /* 0-3 BG; 4+prio = sprites of that priority */
    for (int prio = 3; prio >= 0; prio--) {
        for (int bg = 3; bg >= 0; bg--) if (bg_on[bg] && (R16(e, 0x08 + bg * 2) & 3) == prio) order[norder++] = bg;
        if (obj_on) order[norder++] = 4 + prio;
    }
    /* GL mode (engine A with the 3D layer on): the 3D layer is drawn on the GPU at
     * high resolution, so for each pixel record how it shows (see gl_render.c). */
    int gl3d = eng == 0 && bg_3d && bg_on[0] && g_comp_gl && dst;
    if (gl3d) g_comp_has3d = 1;
    uint32_t* c2out = &g_comp_c2[y * 256];
    uint8_t* info = &g_comp_info[y * 256 * 4];
    uint32_t mbbyte = (uint32_t)((((mb >> 14) & 3) << 5) | ((mb & 31) > 16 ? 16 : (mb & 31))) << 24;
    #define MARK3D 0xFFu                                            /* GL mode: the 3D layer, decided on the GPU */
    int hdl = 0;                                                    /* HD text: letters on this line (hdtext.h) */
    if (g_hdtext_on && dst && g_comp_gl) for (int bg = 0; bg < 4; bg++) if (bg_on[bg] && bgl[bg].tags && !(bg == 0 && bg_3d)) hdl = 1;
    for (int x = 0; x < 256; x++) {
        uint32_t v1 = backdrop, v2 = 0, v3 = 0;
        uint8_t wm = winmask[x];
        int skipL = -1;
        if (hdl) {
            /* the front-most layer here, when it shows part of a letter's box:
             * left out of the picture, the letter is drawn at full resolution */
            for (int k = norder - 1; k >= 0; k--) {
                int L = order[k];
                if (L >= 4) { if ((wm & 0x10) && (objl.col[x] & PX_OPAQUE) && objl.prio[x] == L - 4) break; continue; }
                if (L == 0 && bg_3d) { if (wm & 1) { if (gl3d) break; if (g_3d_present && (g_3d_line32[x] >> 24)) break; } continue; }
                if (!(wm & (1 << L))) continue;
                if (bgl[L].tags && bgl[L].tag[x]) { skipL = L; break; }
                if (bgl[L].col[x] & PX_OPAQUE) break;
            }
            if (skipL >= 0) {
                uint32_t t = bgl[skipL].tag[x];
                int ink = (bgl[skipL].col[x] & PX_OPAQUE) && HDT_INK(t);
                hdtext_seen(eng, t, x, y, ink, ink ? to_rgb6(master_bright(to6(bgl[skipL].col[x]), mb)) : 0);
                g_hdtext_mask[eng][y * 256 + x] = 255;
            }
        }
        for (int k = 0; k < norder; k++) {
            int L = order[k];
            uint32_t v;
            if (L == skipL) continue;
            if (L >= 4) {
                if (!(wm & 0x10) || !(objl.col[x] & PX_OPAQUE) || objl.prio[x] != L - 4) continue;
                v = to6(objl.col[x]) | ((uint32_t)objl.flag[x] << 24);
            } else if (L == 0 && bg_3d) {
                if (!(wm & 1)) continue;
                if (gl3d) v = MARK3D << 24;
                else {
                    uint32_t c = g_3d_line32[x];
                    if (!g_3d_present || !(c >> 24)) continue;
                    v = (c & 0x3F3F3F) | ((0x40u | ((c >> 24) & 0x1F)) << 24);
                }
            } else {
                if (!(wm & (1 << L)) || !(bgl[L].col[x] & PX_OPAQUE)) continue;
                v = to6(bgl[L].col[x]) | (1u << (24 + L));
            }
            v3 = v2; v2 = v1; v1 = v;
        }
        if (g_pixsrc_on && (x == g_pixsrc[2] || g_pixsrc[2] < 0) && y == g_pixsrc[3] && eng == g_pixsrc[1] && g_frame == (uint64_t)g_pixsrc[0]) {
            int L = -1;
            for (int k = norder - 1; k >= 0 && L < 0; k--) {
                int l = order[k];
                if (l >= 4) { if ((wm & 0x10) && (objl.col[x] & PX_OPAQUE) && objl.prio[x] == l - 4) L = l; }
                else if (l != 0 || !bg_3d) { if ((wm & (1 << l)) && (bgl[l].col[x] & PX_OPAQUE)) L = l; }
            }
            uint32_t sv = L < 0 ? 0 : L >= 4 ? objl.src[x] : bgl[L].src[x];
            uint8_t* vp = sv ? vram_ptr(sv >> 1) : NULL;
            fprintf(stderr, "PIXSRC f%llu eng %d (%d,%d): layer %d src %08x nib %u vram+%lx\n", (unsigned long long)g_frame, eng, x, y, L, sv >> 1, sv & 1,
                    vp ? (long)(vp - vram_bank(0)) : -1L);
        }
        int fx = (wm & 0x20) != 0;
        if (!dst) { line6[x] = color_composite(v1, v2, bldcnt, fx, eva, evb, evy); continue; }
        if (gl3d && (v1 >> 24) == MARK3D) {                         /* 3D on top */
            uint32_t f2 = v2 >> 24;
            uint32_t t2 = (f2 & 0x80) ? 0x1000 : (f2 & 0x40) ? 0x0100 : (f2 << 8);
            int sec = (bldcnt & t2) != 0;
            int eff = (fx && (bldcnt & 1)) ? (bldcnt >> 6) & 3 : 0;
            line6[x] = color_composite(v2, v3, bldcnt, fx, eva, evb, evy);
            dst[x] = to_rgb6(line6[x]);                             /* where the 3D is clear (the shader applies master brightness) */
            c2out[x] = to_rgb6(v2) | mbbyte;
            info[x * 4] = (uint8_t)(1 | (sec << 2) | (eff << 3)); info[x * 4 + 1] = (uint8_t)eva; info[x * 4 + 2] = (uint8_t)evb; info[x * 4 + 3] = (uint8_t)evy;
            continue;
        }
        if (gl3d && (v2 >> 24) == MARK3D) {                         /* 2D on top, 3D right below */
            uint32_t f1 = v1 >> 24;
            int sec = (bldcnt >> 8) & 1;
            uint32_t layer = (f1 & 0x80) ? 0x10 : f1;
            if (sec && ((f1 & 0x80) || (fx && (bldcnt & layer) && ((bldcnt >> 6) & 3) == 1))) {
                uint32_t a = eva, b = evb;
                if ((f1 & 0xC0) == 0xC0) { a = f1 & 0x1F; b = 16 - a; }
                line6[x] = v1 & 0x3F3F3F;
                dst[x] = to_rgb6(v1);
                c2out[x] = to_rgb6(v3) | mbbyte;
                info[x * 4] = 2; info[x * 4 + 1] = (uint8_t)a; info[x * 4 + 2] = (uint8_t)b; info[x * 4 + 3] = 0;
                continue;
            }
            v2 = v3;                                                /* no blending with it: treat what is below as the partner */
        }
        line6[x] = color_composite(v1, v2, bldcnt, fx, eva, evb, evy);
        dst[x] = to_rgb6(master_bright(line6[x], mb));
    }
    #undef MARK3D
}

/* ---------------------------------------------------------------- frame flow */
static void latch_affine(int eng, int bg) {
    Engine* e = &g_eng[eng];
    uint32_t x = R32(e, 0x28 + (bg - 2) * 16), y = R32(e, 0x2C + (bg - 2) * 16);
    e->refx[bg - 2] = (int32_t)(x << 4) >> 4;                       /* 28-bit signed */
    e->refy[bg - 2] = (int32_t)(y << 4) >> 4;
}

void gpu_affine_written(int eng, int bg) { latch_affine(eng, bg); }

void gpu_frame_start(void) {
    if (g_hdtext_on) hdtext_frame_start();
    for (int eng = 0; eng < 2; eng++) {
        Engine* e = &g_eng[eng];
        for (int bg = 2; bg < 4; bg++) latch_affine(eng, bg);
        uint16_t mos = R16(e, 0x4C);
        e->bg_mos_ymax = (mos >> 4) & 15; e->bg_mos_y = 0; e->bg_mos_latch = 1;
        e->obj_mos_y = 0; e->obj_mos_line = 0;
    }
    capture_start();
}

/* Per-line register updates before drawing (mosaic line latch, the affine
 * reference points used for drawing) ... */
static void line_regs_pre(int eng, int y) {
    Engine* e = &g_eng[eng];
    if (e->bg_mos_latch) e->bg_mos_line = y;
    for (int i = 0; i < 2; i++)
        if (!(R16(e, 0x08 + (2 + i) * 2) & 0x40) || e->bg_mos_latch) { e->refx_d[i] = e->refx[i]; e->refy_d[i] = e->refy[i]; }
}
/* ... and after: the mosaic counters step, the reference points advance
 * (only while the layer is enabled), the OBJ mosaic line for the next line. */
static void line_regs_post(int eng, int y) {
    Engine* e = &g_eng[eng];
    uint16_t mos = R16(e, 0x4C);
    uint32_t disp = R32(e, 0);
    if (e->bg_mos_y == e->bg_mos_ymax) { e->bg_mos_ymax = (mos >> 4) & 15; e->bg_mos_y = 0; e->bg_mos_latch = 1; }
    else { e->bg_mos_y = (e->bg_mos_y + 1) & 15; e->bg_mos_latch = 0; }
    for (int i = 0; i < 2; i++) {
        if (!(disp & (0x400u << i))) continue;
        e->refx[i] += (int16_t)R16(e, 0x22 + i * 16);
        e->refy[i] += (int16_t)R16(e, 0x26 + i * 16);
    }
    if (disp & 0x1000) {
        if (e->obj_mos_y == ((mos >> 12) & 15)) { e->obj_mos_y = 0; e->obj_mos_line = y + 1; }
        else e->obj_mos_y = (e->obj_mos_y + 1) & 15;
    }
}

double g_t2d_ms;
#include <time.h>
static double ms_now2(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }   /* always on: bug reports show 2D time */
void gpu_render_line_(int line);
void gpu_render_line(int line) { double t0 = ms_now2(); gpu_render_line_(line); g_t2d_ms += ms_now2() - t0; }
void gpu_render_line_(int line) {
    if (line == 0) gpu_frame_start();
    gpu3d_render_line(line);
    for (int eng = 0; eng < 2; eng++) {
        line_regs_pre(eng, line);
        render_engine_line(eng, line);
        line_regs_post(eng, line);
    }
    if (line == 191 && g_cap_active) { g_io[0x67] &= 0x7F; g_cap_active = 0; }   /* capture done: enable bit clears */
}

/* ---------------------------------------------------------------- output */
static char g_dump_dir[512];
static int g_dump_every;
static uint64_t g_last_dump;

static void write_ppm(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n256 384\n255\n");
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 256 * 192; i++) {
            uint32_t c = g_screen[s][i];
            uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

void gpu_frame_done(void) {
    uint16_t pow; memcpy(&pow, &g_io[0x304], 2);
    int a_top = (pow >> 15) & 1;
    if (!(pow & 1)) {                                               /* LCDs off */
        memset(g_screen, 0, sizeof g_screen);
    } else {
        memcpy(g_screen[a_top ? 0 : 1], g_engine_out[0], sizeof g_engine_out[0]);
        memcpy(g_screen[a_top ? 1 : 0], g_engine_out[1], sizeof g_engine_out[1]);
    }
    {   /* DQIX_CRC=file: a fingerprint of every frame (same as the reference runner's REF_CRC) */
        static FILE* crcf; static int init;
        if (!init) { init = 1; const char* e = getenv("DQIX_CRC"); if (e) crcf = fopen(e, "w"); }
        if (crcf) {
            unsigned long long h = 1469598103934665603ULL;
            for (int sc = 0; sc < 2; sc++) for (int i = 0; i < 256 * 192; i++) { h ^= g_screen[sc][i] & 0xFFFFFF; h *= 1099511628211ULL; }
            fprintf(crcf, "%llu %016llx\n", (unsigned long long)g_frame, h);
        }
    }
    if (!g_dump_every) {
        const char* env = getenv("DQIX_DUMP");          /* "dir,every_n_frames" */
        if (env && *env) {
            const char* comma = strchr(env, ',');
            size_t n = comma ? (size_t)(comma - env) : strlen(env);
            if (n >= sizeof g_dump_dir) n = sizeof g_dump_dir - 1;
            memcpy(g_dump_dir, env, n); g_dump_dir[n] = 0;
            g_dump_every = comma ? atoi(comma + 1) : 60;
            if (g_dump_every <= 0) g_dump_every = 60;
        } else g_dump_every = -1;
    }
    { const char* at = getenv("DQIX_DUMP_AT"); if (at && g_dump_every > 0) {      /* exact frames: "f1,f2,..." */
        char key[32]; snprintf(key, sizeof key, ",%llu,", (unsigned long long)g_frame);
        char buf[4096]; snprintf(buf, sizeof buf, ",%s,", at);
        if (strstr(buf, key)) { char path[700]; snprintf(path, sizeof path, "%s/f%06llu.ppm", g_dump_dir, (unsigned long long)g_frame); write_ppm(path); }
        return; } }
    if (g_dump_every > 0 && g_frame - g_last_dump >= (uint64_t)g_dump_every) {
        g_last_dump = g_frame;
        char path[700];
        snprintf(path, sizeof path, "%s/f%06llu.ppm", g_dump_dir, (unsigned long long)g_frame);
        write_ppm(path);
    }
}
