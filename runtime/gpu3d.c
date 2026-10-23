/* 3D engine: geometry (command FIFO and ports, matrix stacks, lighting,
 * polygon assembly, clipping, viewport) and the software rasterizer.
 *
 * Everything works in the DS's own fixed-point formats and follows the
 * hardware's rounding, edge, interpolation and pixel rules, so frames come out
 * pixel-identical to the DS ("Classic"). These rules were worked out by the
 * melonDS project (GPU3D.cpp, GPU3D_Soft.cpp, GPL); this is a C
 * reimplementation of the same behaviour, checked frame by frame against a
 * melonDS build (tools: the reference runner, DQIX_CRC).
 *
 * Timing: polygons are collected into one of two polygon/vertex RAM banks.
 * SWAP_BUFFERS (FLUSH) takes effect at VBlank (scanline 192): the bank becomes
 * the render list and the registers that affect rendering are latched. The
 * frame is rendered at scanline 215 and shown during the next frame, read
 * line by line by the 2D engine as BG0. Without a new FLUSH the last list is
 * rendered again with the newly latched registers, like the hardware. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "gpu3d.h"
#include <time.h>
#ifdef DQIX_TIMING   /* render timing for performance work (Linux test builds) */
static double ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#else
static double ms_now(void) { return 0; }
#endif

typedef int32_t s32; typedef uint32_t u32; typedef int64_t s64; typedef uint64_t u64;
typedef int16_t s16; typedef uint16_t u16; typedef uint8_t u8;

#define IO8_(o)  (g_io[o])
#define IO16_(o) (*(uint16_t*)&g_io[o])
#define IO32_(o) (*(uint32_t*)&g_io[o])

/* ---------------------------------------------------------------- types */
typedef struct {
    s32 pos[4];            /* clip space */
    s32 col[3];            /* 5-bit colour << 12 (kept like this through clipping) */
    s16 tex[2];            /* 12.4 texel coordinates */
    int clipped;
    s32 fpos[2];           /* screen position */
    s32 fcol[3];           /* 9-bit colour for the rasterizer */
    s32 hpos[2];           /* screen position with 4 fractional bits (GPU renderer) */
} Vtx;

typedef struct {
    Vtx* v[10];
    u32 n;
    s32 z[10], w[10];
    int wbuf;
    u32 attr, texparam, texpal;
    int degenerate, facing, translucent, shadowmask, shadow;
    u32 vtop, vbot;
    s32 ytop, ybot, xtop, xbot;
    u32 sortkey;
    int hidden;            /* drawn under a hidden tag (g_gx_hide): left out of the picture only */
} HPoly;

/* Tags (tags.txt): the recompiled code calls gx_tag_begin/end around calls
 * whose drawing can be hidden. Polygons drawn under a tag in g_gx_hide are
 * kept (the polygon count, everything the game can read) but not drawn.
 *
 * The game doesn't always send its 3D commands at once: DQIX collects them in
 * a small queue (SubmitCommandToGeometryFifo: the pointer at 0x0210CF78 ->
 * { u32 count; u32 words[192]; }) that goes to the 3D chip later. So at the
 * end of a tagged call, the textures it queued (TEXIMAGE_PARAM) are noted,
 * and until the next frame, polygons with exactly those textures carry the
 * tag too. */
int g_gx_tag;
int g_gx_hide;
#define GXQ_PTR 0x0210CF78u
#define TAGTEX_MAX 16
static u32 tagtex[TAGTEX_MAX]; static int tagtex_tag[TAGTEX_MAX], n_tagtex;
static u32 tag_q, tag_count;
static const uint8_t param_count[256];

int gx_tag_begin(int tag) {
    int prev = g_gx_tag;
    g_gx_tag = tag;
    tag_q = mem_read32(GXQ_PTR);
    tag_count = tag_q ? mem_read32(tag_q) : 0;
    return prev;
}

void gx_tag_end(int previous) {
    u32 q = mem_read32(GXQ_PTR);
    if (q && q == tag_q && g_gx_tag) {
        u32 end = mem_read32(q), i = end >= tag_count ? tag_count : 0;     /* emptied during the call: from the start */
        if (end > 192) end = 192;
        while (i < end) {
            u32 w = mem_read32(q + 4 + i * 4); i++;
            for (int k = 0; k < 4 && (w >> (8 * k)); k++) {
                uint8_t cmd = (uint8_t)(w >> (8 * k));
                if (cmd == 0x2A && i < end && n_tagtex < TAGTEX_MAX) { tagtex[n_tagtex] = mem_read32(q + 4 + i * 4); tagtex_tag[n_tagtex++] = g_gx_tag; }
                i += param_count[cmd];
            }
        }
    }
    g_gx_tag = previous;
}

static int poly_tag(u32 tp) {
    int t = g_gx_tag;
    for (int i = 0; i < n_tagtex; i++) if (tagtex[i] == tp) t |= tagtex_tag[i];
    return t;
}

/* ---------------------------------------------------------------- geometry state */
static s32 m_proj[16], m_pos[16], m_vec[16], m_tex[16], m_clip[16];
static s32 st_proj[16], st_pos[32][16], st_vec[32][16], st_tex[16];
static s32 sp_proj, sp_pos, sp_tex;
static int clip_dirty = 1;
static u32 mtx_mode;
static u32 gxstat;                      /* bit 1 box test result, bit 15 stack error, bits 30-31 IRQ mode */

static u32 poly_mode, poly_attr, cur_poly_attr, tex_param, tex_pal;
static s16 cur_vtx[3];
static u8 vtx_color[3];
static s16 tex_coords[2], raw_tex[2], normal[3];
static s16 light_dir[4][3];
static s32 spec_recip[4];
static u8 light_col[4][3];
static u8 mat_dif[3], mat_amb[3], mat_spe[3], mat_emi[3];
static int use_shine;
static u8 shine_table[128];
static u32 viewport[6];
static s32 postest[4];
static s16 vectest[3];

static Vtx tmp_vtx[4];
static u32 vtx_num, vtx_in_poly, consecutive;
static HPoly* last_strip;
static u32 n_opaque;

#define VTX_MAX 6144
#define POLY_MAX 2048
static Vtx vram_bank_v[VTX_MAX * 2];
static HPoly pram[POLY_MAX * 2];
static Vtx* cur_vram; static HPoly* cur_pram;
static u32 n_vtx, n_poly, cur_bank;
static HPoly* render_list[POLY_MAX];
static u32 render_n;
static u32 flush_request, flush_attr;

/* registers latched at VBlank for rendering */
static u32 r_disp, r_clear1, r_clear2, r_fogcolor, r_fogoffset, r_fogshift;
static u8 r_alpharef;
static u16 r_toon[32], r_edge[8];
static u8 r_fogtab[34];
static int g_3d_have_frame;

/* ---------------------------------------------------------------- matrices (m = s * m) */
static void mtx_identity(s32* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 0x1000; }
static void mtx_load4x4(s32* m, const u32* s) { for (int i = 0; i < 16; i++) m[i] = (s32)s[i]; }
static void mtx_load4x3(s32* m, const u32* s) {
    for (int r = 0; r < 4; r++) { for (int c = 0; c < 3; c++) m[r * 4 + c] = (s32)s[r * 3 + c]; m[r * 4 + 3] = r == 3 ? 0x1000 : 0; }
}
static void mtx_mult4x4(s32* m, const u32* p) {
    s32 t[16], s[16]; memcpy(t, m, 64); for (int i = 0; i < 16; i++) s[i] = (s32)p[i];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        m[r * 4 + c] = (s32)(((s64)s[r * 4] * t[c] + (s64)s[r * 4 + 1] * t[4 + c] + (s64)s[r * 4 + 2] * t[8 + c] + (s64)s[r * 4 + 3] * t[12 + c]) >> 12);
}
static void mtx_mult4x3(s32* m, const u32* p) {
    s32 t[16], s[12]; memcpy(t, m, 64); for (int i = 0; i < 12; i++) s[i] = (s32)p[i];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++)
        m[r * 4 + c] = (s32)(((s64)s[r * 3] * t[c] + (s64)s[r * 3 + 1] * t[4 + c] + (s64)s[r * 3 + 2] * t[8 + c]) >> 12);
    for (int c = 0; c < 4; c++)
        m[12 + c] = (s32)(((s64)s[9] * t[c] + (s64)s[10] * t[4 + c] + (s64)s[11] * t[8 + c] + (s64)0x1000 * t[12 + c]) >> 12);
}
static void mtx_mult3x3(s32* m, const u32* p) {
    s32 t[12], s[9]; memcpy(t, m, 48); for (int i = 0; i < 9; i++) s[i] = (s32)p[i];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++)
        m[r * 4 + c] = (s32)(((s64)s[r * 3] * t[c] + (s64)s[r * 3 + 1] * t[4 + c] + (s64)s[r * 3 + 2] * t[8 + c]) >> 12);
}
static void mtx_scale(s32* m, const u32* p) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) m[r * 4 + c] = (s32)(((s64)(s32)p[r] * m[r * 4 + c]) >> 12);
}
static void mtx_translate(s32* m, const u32* p) {
    s32 s0 = (s32)p[0], s1 = (s32)p[1], s2 = (s32)p[2];
    for (int c = 0; c < 4; c++) m[12 + c] += (s32)(((s64)s0 * m[c] + (s64)s1 * m[4 + c] + (s64)s2 * m[8 + c]) >> 12);
}
static void update_clip(void) {
    if (!clip_dirty) return;
    clip_dirty = 0;
    memcpy(m_clip, m_proj, 64);
    u32 p[16]; for (int i = 0; i < 16; i++) p[i] = (u32)m_pos[i];
    mtx_mult4x4(m_clip, p);
}

/* ---------------------------------------------------------------- clipping */
static void clip_segment(Vtx* out, const Vtx* vin, const Vtx* vout, int comp, s32 plane, int attribs) {
    s64 fnum = (s64)vin->pos[3] - (s64)plane * vin->pos[comp];
    s32 fden = (s32)(fnum - ((s64)vout->pos[3] - (s64)plane * vout->pos[comp]));
#define INTERP(var) out->var = (s32)(vin->var + ((s64)(vout->var - vin->var) * fnum) / fden)
    if (comp != 0) INTERP(pos[0]);
    if (comp != 1) INTERP(pos[1]);
    if (comp != 2) INTERP(pos[2]);
    INTERP(pos[3]);
    out->pos[comp] = plane * out->pos[3];
    if (attribs) {
        INTERP(col[0]); INTERP(col[1]); INTERP(col[2]);
        out->tex[0] = (s16)(vin->tex[0] + ((s64)(vout->tex[0] - vin->tex[0]) * fnum) / fden);
        out->tex[1] = (s16)(vin->tex[1] + ((s64)(vout->tex[1] - vin->tex[1]) * fnum) / fden);
    }
#undef INTERP
    out->clipped = 1;
}

static int clip_plane(Vtx* v, int n, int clipstart, int comp, int attribs) {
    Vtx temp[10];
    int c = clipstart;
    if (clipstart == 2) { temp[0] = v[0]; temp[1] = v[1]; }
    for (int i = clipstart; i < n; i++) {
        int prev = i - 1; if (prev < 0) prev = n - 1;
        int next = i + 1; if (next >= n) next = 0;
        Vtx vtx = v[i];
        if (vtx.pos[comp] > vtx.pos[3]) {
            if (comp == 2 && !(cur_poly_attr & (1 << 12))) return 0;   /* far plane: reject unless "render far" */
            if (v[prev].pos[comp] <= v[prev].pos[3]) clip_segment(&temp[c++], &vtx, &v[prev], comp, 1, attribs);
            if (v[next].pos[comp] <= v[next].pos[3]) clip_segment(&temp[c++], &vtx, &v[next], comp, 1, attribs);
        } else temp[c++] = vtx;
    }
    n = c; c = clipstart;
    for (int i = clipstart; i < n; i++) {
        int prev = i - 1; if (prev < 0) prev = n - 1;
        int next = i + 1; if (next >= n) next = 0;
        Vtx vtx = temp[i];
        if (vtx.pos[comp] < -vtx.pos[3]) {
            if (temp[prev].pos[comp] >= -temp[prev].pos[3]) clip_segment(&v[c++], &vtx, &temp[prev], comp, -1, attribs);
            if (temp[next].pos[comp] >= -temp[next].pos[3]) clip_segment(&v[c++], &vtx, &temp[next], comp, -1, attribs);
        } else v[c++] = vtx;
    }
    for (int i = 0; i < c; i++)
        for (int k = 0; k < 3; k++) { v[i].col[k] &= ~0xFFF; v[i].col[k] += 0xFFF; }
    return c;
}

static int clip_polygon(Vtx* v, int n, int clipstart, int attribs) {
    n = clip_plane(v, n, clipstart, 2, attribs);           /* Z, then Y, then X, like the hardware */
    n = clip_plane(v, n, clipstart, 1, attribs);
    n = clip_plane(v, n, clipstart, 0, attribs);
    return n;
}

static int clip_coords_equal(const Vtx* a, const Vtx* b) {
    return a->pos[0] == b->pos[0] && a->pos[1] == b->pos[1] && a->pos[2] == b->pos[2] && a->pos[3] == b->pos[3];
}

/* ---------------------------------------------------------------- widescreen (render only)
 * With g_wide_side > 0 (the GPU renderer's widescreen), every polygon is also
 * clipped against a wider view: x scaled by 256 / (256 + 2 * side) before
 * clipping, so the DS's picture fills the middle 256 pixels and the sides
 * show what lies beyond its edges. These polygons only go to the renderer;
 * the normal list (the polygon count, everything the game can read, the
 * software renderer) is unchanged. A polygon drawn with a smaller viewport
 * (a 3D picture inside a menu) is copied unwidened, in the middle. */
int g_wide_side;
#define WPOLY_MAX 6144
#define WVTX_MAX (WPOLY_MAX * 6)
static HPoly wpram[WPOLY_MAX * 2];
static Vtx wvram[WVTX_MAX * 2];
static HPoly* wcur_pram = wpram; static Vtx* wcur_vram = wvram;
static u32 n_wpoly, n_wvtx;
static HPoly* wrender_list[WPOLY_MAX];
static u32 wrender_n, n_wopaque;
static int wbank, w_frame_side = -1, w_render_side;       /* the side every polygon of this frame was made with (-1: none yet, -2: mixed) */
static int poly_tag(u32 tp);

static void wide_screen_pos(Vtx* v, int side, int widen) {
    s64 w = v->pos[3];
    if (w == 0) { v->hpos[0] = v->hpos[1] = 0; return; }
    s64 W = 256 + 2 * side;
    if (widen) v->hpos[0] = (s32)((((s64)v->pos[0] + w) * W * 16) / (2 * w));
    else v->hpos[0] = (s32)((((((s64)v->pos[0] + w) * viewport[4]) << 4) / (w << 1)) + ((viewport[0] + side) << 4));
    v->hpos[1] = (s32)((((((s64)-v->pos[1] + w) * viewport[5]) << 4) / (w << 1)) + (viewport[3] << 4));
}

static void wide_submit(int nverts, int facing) {
    int side = g_wide_side;
    if (w_frame_side == -1) w_frame_side = side; else if (w_frame_side != side) w_frame_side = -2;
    if (n_wpoly >= WPOLY_MAX || n_wvtx + 10 > WVTX_MAX) return;
    int widen = viewport[0] == 0 && viewport[4] == 256;
    /* A backdrop that fills the DS's screen from edge to edge (every corner on
     * its left or right edge: a sky gradient, a fade) fills the wide picture
     * too, instead of leaving the sides bare - if it is plain colour or far
     * away (a far picture stretched a little; nearer pictures keep their shape). */
    int stretch = widen && side > 0 && nverts >= 3;
    if (stretch) {
        int left = 0, right = 0, far = 1;
        for (int i = 0; i < nverts && stretch; i++) {
            s64 x = tmp_vtx[i].pos[0], w = tmp_vtx[i].pos[3], tol = (w < 0 ? -w : w) / 256 + 1;
            if (w <= 0) stretch = 0;
            else if (x >= w - tol && x <= w + tol) right++;
            else if (x <= -w + tol && x >= -w - tol) left++;
            else stretch = 0;
            if (w > 0 && (s64)tmp_vtx[i].pos[2] * 100 < w * 98) far = 0;   /* z/w under 0.98 */
        }
        if (!left || !right || !(((tex_param >> 26) & 7) == 0 || far)) stretch = 0;
    }
    Vtx cv[10];
    for (int i = 0; i < nverts; i++) {
        cv[i] = tmp_vtx[i];
        if (widen && !stretch) cv[i].pos[0] = (s32)(((s64)cv[i].pos[0] * 256) / (256 + 2 * side));
    }
    int n = clip_polygon(cv, nverts, 0, 1);
    if (n == 0) return;
    HPoly* p = &wcur_pram[n_wpoly++];
    memset(p, 0, sizeof *p);
    p->attr = cur_poly_attr; p->texparam = tex_param; p->texpal = tex_pal;
    p->facing = facing;
    p->hidden = g_gx_hide && (poly_tag(tex_param) & g_gx_hide) != 0;
    u32 texfmt = (tex_param >> 26) & 7, polyalpha = (cur_poly_attr >> 16) & 0x1F;
    p->translucent = (texfmt == 1 || texfmt == 6) || (polyalpha > 0 && polyalpha < 31);
    if (!p->translucent) n_wopaque++;
    s32 ytop = 1 << 20, ybot = -(1 << 20);
    u32 wsize = 0;
    for (int i = 0; i < n; i++) {
        Vtx* v = &wcur_vram[n_wvtx++];
        *v = cv[i];
        v->pos[3] &= 0x00FFFFFF;
        wide_screen_pos(v, side, widen);
        for (int k = 0; k < 3; k++) { s32 c = v->col[k] >> 12; v->fcol[k] = c ? ((c << 4) + 0xF) : 0; }
        p->v[i] = v; p->n++;
        s32 y = v->hpos[1] >> 4;
        if (y < ytop) ytop = y;
        if (y > ybot) ybot = y;
        u32 w = (u32)v->pos[3];
        if (w == 0) p->degenerate = 1;
        while (wsize < 32 && (w >> wsize)) wsize += 4;
    }
    if (ybot > 192) p->degenerate = 1;
    if (ytop < 0) ytop = 0;
    p->sortkey = ((u32)(ybot & 0xFF) << 8) | (u32)(ytop & 0xFF);
    if (p->translucent) p->sortkey |= 0x10000;
    p->wbuf = (flush_attr & 2) != 0;
    for (int i = 0; i < n; i++) {
        Vtx* v = p->v[i];
        s32 w, wsh;
        if (wsize < 16) { w = v->pos[3] << (16 - wsize); wsh = w >> (16 - wsize); }
        else { w = v->pos[3] >> (wsize - 16); wsh = w << (wsize - 16); }
        s32 z;
        if (flush_attr & 2) z = wsh;
        else if (v->pos[3]) z = (s32)(((((s64)v->pos[2] * 0x4000) / v->pos[3]) + 0x3FFF) * 0x200);
        else z = 0x7FFE00;
        if (z < 0) z = 0; else if (z > 0xFFFFFF) z = 0xFFFFFF;
        p->z[i] = z; p->w[i] = w;
    }
}

/* ---------------------------------------------------------------- polygon setup */
static void submit_polygon(void) {
    Vtx cv[10];
    Vtx* reused[2] = { 0, 0 };
    int clipstart = 0, lastpolyverts = 0;
    int nverts = (poly_mode & 1) ? 4 : 3;

    /* culling: sign of the normal against the view, normalised to 32 bits */
    Vtx *v0 = &tmp_vtx[0], *v1 = &tmp_vtx[1], *v2 = &tmp_vtx[2];
    s64 nx = (s64)(v0->pos[1] - v1->pos[1]) * (v2->pos[3] - v1->pos[3]) - (s64)(v0->pos[3] - v1->pos[3]) * (v2->pos[1] - v1->pos[1]);
    s64 ny = (s64)(v0->pos[3] - v1->pos[3]) * (v2->pos[0] - v1->pos[0]) - (s64)(v0->pos[0] - v1->pos[0]) * (v2->pos[3] - v1->pos[3]);
    s64 nz = (s64)(v0->pos[0] - v1->pos[0]) * (v2->pos[1] - v1->pos[1]) - (s64)(v0->pos[1] - v1->pos[1]) * (v2->pos[0] - v1->pos[0]);
    while ((((nx >> 31) ^ (nx >> 63)) != 0) || (((ny >> 31) ^ (ny >> 63)) != 0) || (((nz >> 31) ^ (nz >> 63)) != 0)) { nx >>= 4; ny >>= 4; nz >>= 4; }
    s64 dot = (s64)v1->pos[0] * nx + (s64)v1->pos[1] * ny + (s64)v1->pos[3] * nz;
    int facing = dot <= 0;
    if (dot < 0) { if (!(cur_poly_attr & (1 << 7))) { last_strip = NULL; return; } }
    else if (dot > 0) { if (!(cur_poly_attr & (1 << 6))) { last_strip = NULL; return; } }
    if (g_wide_side > 0 && g_renderer3d) wide_submit(nverts, facing);
    else if (w_frame_side != -1) w_frame_side = -2;              /* widescreen switched off during the frame */

    /* strips: share two unclipped vertices with the previous polygon */
    if (poly_mode >= 2 && last_strip) {
        int id0, id1;
        if (poly_mode == 2) { if (consecutive & 1) { id0 = 2; id1 = 1; } else { id0 = 0; id1 = 2; } lastpolyverts = 3; }
        else { id0 = 3; id1 = 2; lastpolyverts = 4; }
        if (last_strip->n == (u32)lastpolyverts && !last_strip->v[id0]->clipped && !last_strip->v[id1]->clipped) {
            reused[0] = last_strip->v[id0]; reused[1] = last_strip->v[id1];
            cv[0] = *reused[0]; cv[1] = *reused[1];
            clipstart = 2;
        }
    }
    for (int i = clipstart; i < nverts; i++) cv[i] = tmp_vtx[i];
    (void)clip_coords_equal;

    nverts = clip_polygon(cv, nverts, clipstart, 1);
    if (nverts == 0) { last_strip = NULL; return; }
    if (n_poly >= POLY_MAX || n_vtx + (u32)nverts > VTX_MAX) {
        last_strip = NULL;
        g_io[0x61] |= 0x20;                                         /* DISP3DCNT: polygon/vertex RAM overflow */
        return;
    }

    /* viewport transform (the hardware's 32-bit divider: halve when W > 0xFFFF) */
    for (int i = clipstart; i < nverts; i++) {
        Vtx* v = &cv[i];
        v->pos[3] &= 0x00FFFFFF;
        u32 px, py, w = (u32)v->pos[3];
        if (w == 0) { px = 0; py = 0; }
        else {
            px = (u32)(v->pos[0] + (s32)w);
            py = (u32)(-v->pos[1] + (s32)w);
            u32 den = w;
            if (w > 0xFFFF) { px >>= 1; py >>= 1; den >>= 1; }
            den <<= 1;
            px = ((px * viewport[4]) / den) + viewport[0];
            py = ((py * viewport[5]) / den) + viewport[3];
        }
        v->fpos[0] = (s32)(px & 0x1FF);
        v->fpos[1] = (s32)(py & 0xFF);
        if (w != 0) {
            u32 hx = (u32)(((((s64)(v->pos[0] + (s32)w) * viewport[4]) << 4) / (((s64)w) << 1)) + (viewport[0] << 4));
            u32 hy = (u32)(((((s64)(-v->pos[1] + (s32)w) * viewport[5]) << 4) / (((s64)w) << 1)) + (viewport[3] << 4));
            v->hpos[0] = (s32)(hx & 0x1FFF); v->hpos[1] = (s32)(hy & 0xFFF);
        } else { v->hpos[0] = 0; v->hpos[1] = 0; }
    }

    /* zero-dot polygons behind the DISP_1DOT_DEPTH limit are dropped */
    if (!(cur_poly_attr & (1 << 13))) {
        u32 limit = ((u32)(IO16_(0x610) & 0x7FFF) * 0x200) + 0x1FF;
        int zerodot = 1, allbehind = 1;
        for (int i = 0; i < nverts; i++) {
            if (cv[i].fpos[0] != cv[0].fpos[0] || cv[i].fpos[1] != cv[0].fpos[1]) { zerodot = 0; break; }
            if ((u32)cv[i].pos[3] <= limit) { allbehind = 0; break; }
        }
        if (zerodot && allbehind) { last_strip = NULL; return; }
    }

    HPoly* p = &cur_pram[n_poly++];
    memset(p, 0, sizeof *p);
    p->attr = cur_poly_attr; p->texparam = tex_param; p->texpal = tex_pal;
    p->facing = facing;
    p->hidden = g_gx_hide && (poly_tag(tex_param) & g_gx_hide) != 0;
    u32 texfmt = (tex_param >> 26) & 7, polyalpha = (cur_poly_attr >> 16) & 0x1F;
    p->translucent = (texfmt == 1 || texfmt == 6) || (polyalpha > 0 && polyalpha < 31);
    p->shadowmask = (cur_poly_attr & 0x3F000030) == 0x00000030;
    p->shadow = (cur_poly_attr & 0x30) == 0x30 && !p->shadowmask;
    if (!p->translucent) n_opaque++;

    if (last_strip && clipstart > 0) {
        if (nverts == lastpolyverts) { p->v[0] = reused[0]; p->v[1] = reused[1]; }
        else {
            cur_vram[n_vtx] = *reused[0]; p->v[0] = &cur_vram[n_vtx];
            cur_vram[n_vtx + 1] = *reused[1]; p->v[1] = &cur_vram[n_vtx + 1];
            n_vtx += 2;
        }
        p->n += 2;
    }
    for (int i = clipstart; i < nverts; i++) {
        Vtx* v = &cur_vram[n_vtx++];
        *v = cv[i];
        p->v[i] = v;
        p->n++;
        for (int k = 0; k < 3; k++) { s32 c = v->col[k] >> 12; v->fcol[k] = c ? ((c << 4) + 0xF) : 0; }
    }

    u32 vtop = 0, vbot = 0;
    s32 ytop = 192, ybot = 0, xtop = 256, xbot = 0;
    u32 wsize = 0;
    for (int i = 0; i < nverts; i++) {
        Vtx* v = p->v[i];
        if (v->fpos[1] < ytop) { xtop = v->fpos[0]; ytop = v->fpos[1]; vtop = (u32)i; }
        if (v->fpos[1] > ybot || (v->fpos[1] == ybot && v->fpos[0] > xbot)) { xbot = v->fpos[0]; ybot = v->fpos[1]; vbot = (u32)i; }
        u32 w = (u32)v->pos[3];
        if (w == 0) p->degenerate = 1;
        while (wsize < 32 && (w >> wsize)) wsize += 4;
    }
    p->vtop = vtop; p->vbot = vbot; p->ytop = ytop; p->ybot = ybot; p->xtop = xtop; p->xbot = xbot;
    if (ybot > 192) p->degenerate = 1;
    p->sortkey = ((u32)ybot << 8) | (u32)ytop;
    if (p->translucent) p->sortkey |= 0x10000;
    p->wbuf = (flush_attr & 2) != 0;
    for (int i = 0; i < nverts; i++) {
        Vtx* v = p->v[i];
        s32 w, wsh;
        if (wsize < 16) { w = v->pos[3] << (16 - wsize); wsh = w >> (16 - wsize); }
        else { w = v->pos[3] >> (wsize - 16); wsh = w << (wsize - 16); }
        s32 z;
        if (flush_attr & 2) z = wsh;
        else if (v->pos[3]) z = (s32)(((((s64)v->pos[2] * 0x4000) / v->pos[3]) + 0x3FFF) * 0x200);
        else z = 0x7FFE00;
        if (z < 0) z = 0; else if (z > 0xFFFFFF) z = 0xFFFFFF;
        p->z[i] = z; p->w[i] = w;
    }
    last_strip = poly_mode >= 2 ? p : NULL;
}

static void submit_vertex(void) {
    s64 vx = cur_vtx[0], vy = cur_vtx[1], vz = cur_vtx[2], vw = 0x1000;
    Vtx* t = &tmp_vtx[vtx_in_poly];
    update_clip();
    for (int c = 0; c < 4; c++) t->pos[c] = (s32)((vx * m_clip[c] + vy * m_clip[4 + c] + vz * m_clip[8 + c] + vw * m_clip[12 + c]) >> 12);
    for (int k = 0; k < 3; k++) t->col[k] = ((s32)vtx_color[k] << 12) + 0xFFF;
    if ((tex_param >> 30) == 3) {
        t->tex[0] = (s16)(((vx * m_tex[0] + vy * m_tex[4] + vz * m_tex[8]) >> 24) + raw_tex[0]);
        t->tex[1] = (s16)(((vx * m_tex[1] + vy * m_tex[5] + vz * m_tex[9]) >> 24) + raw_tex[1]);
    } else { t->tex[0] = tex_coords[0]; t->tex[1] = tex_coords[1]; }
    t->clipped = 0;
    vtx_num++; vtx_in_poly++;
    switch (poly_mode) {
    case 0: if (vtx_in_poly == 3) { vtx_in_poly = 0; submit_polygon(); consecutive++; } break;
    case 1: if (vtx_in_poly == 4) { vtx_in_poly = 0; submit_polygon(); consecutive++; } break;
    case 2:
        if (consecutive & 1) {
            Vtx tmp = tmp_vtx[1]; tmp_vtx[1] = tmp_vtx[0]; tmp_vtx[0] = tmp;
            vtx_in_poly = 2; submit_polygon(); consecutive++;
            tmp_vtx[1] = tmp_vtx[2];
        } else if (vtx_in_poly == 3) {
            vtx_in_poly = 2; submit_polygon(); consecutive++;
            tmp_vtx[0] = tmp_vtx[1]; tmp_vtx[1] = tmp_vtx[2];
        }
        break;
    case 3:
        if (vtx_in_poly == 4) {
            Vtx tmp = tmp_vtx[3]; tmp_vtx[3] = tmp_vtx[2]; tmp_vtx[2] = tmp;
            vtx_in_poly = 2; submit_polygon(); consecutive++;
            tmp_vtx[0] = tmp_vtx[3]; tmp_vtx[1] = tmp_vtx[2];
        }
        break;
    }
}

static void calc_lighting(void) {
    if ((tex_param >> 30) == 2) {
        tex_coords[0] = (s16)(raw_tex[0] + (((s64)normal[0] * m_tex[0] + (s64)normal[1] * m_tex[4] + (s64)normal[2] * m_tex[8]) >> 21));
        tex_coords[1] = (s16)(raw_tex[1] + (((s64)normal[0] * m_tex[1] + (s64)normal[1] * m_tex[5] + (s64)normal[2] * m_tex[9]) >> 21));
    }
    s32 nt[3];
    for (int c = 0; c < 3; c++) nt[c] = (s32)((u32)(normal[0] * m_vec[c] + normal[1] * m_vec[4 + c] + normal[2] * m_vec[8 + c]) << 9) >> 21;
    u32 buf[3] = { (u32)mat_emi[0] << 14, (u32)mat_emi[1] << 14, (u32)mat_emi[2] << 14 };
    for (int i = 0; i < 4; i++) {
        if (!(cur_poly_attr & (1u << i))) continue;
        s32 dot = ((light_dir[i][0] * nt[0]) >> 9) + ((light_dir[i][1] * nt[1]) >> 9) + ((light_dir[i][2] * nt[2]) >> 9);
        s32 shine;
        if (dot > 0) {
            s32 diffdot = (s32)((u32)dot << 21) >> 21;
            for (int k = 0; k < 3; k++) buf[k] += (u32)(mat_dif[k] * light_col[i][k] * diffdot) & 0xFFFFF;
            dot += nt[2];
            dot = (s32)((u32)dot << 21) >> 21;
            dot = ((dot * dot) >> 10) & 0x3FF;
            shine = ((dot * spec_recip[i]) >> 8) - (1 << 9);
            if (shine < 0) shine = 0;
            else {
                shine = (s32)((u32)shine << 18) >> 18;
                if (shine < 0) shine = 0; else if (shine > 0x1FF) shine = 0x1FF;
            }
        } else shine = 0;
        if (use_shine) { shine >>= 2; shine = shine_table[shine]; shine <<= 1; }
        for (int k = 0; k < 3; k++) buf[k] += (u32)(((mat_spe[k] * shine) + (mat_amb[k] << 9)) * light_col[i][k]);
    }
    for (int k = 0; k < 3; k++) vtx_color[k] = (u8)((buf[k] >> 14) > 31 ? 31 : (buf[k] >> 14));
}

/* Widescreen: the game asks the DS whether a box (an object's bounds) is in
 * view before drawing it, and leaves out what isn't. Against the DS's view,
 * people, doors and monsters near the wide picture's edges vanished. With
 * g_wide_cull set (widescreen on, not in a session: there every PC must
 * decide exactly alike) the box is tested against the wide view instead. */
int g_wide_cull;
void gpu3d_debug_proj(void) {
    fprintf(stderr, "proj:"); for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", (u32)m_proj[i]); fprintf(stderr, "\n");
}
unsigned g_box_tests, g_box_in, g_box_widened;   /* tests (DQIX_BOXLOG) */
static void box_test(const u32* p) {
    Vtx cube[8], face[10];
    gxstat &= ~2u;
    s16 x0 = (s16)(p[0] & 0xFFFF), y0 = (s16)(p[0] >> 16), z0 = (s16)(p[1] & 0xFFFF);
    s16 x1 = (s16)(p[1] >> 16), y1 = (s16)(p[2] & 0xFFFF), z1 = (s16)(p[2] >> 16);
    x1 = (s16)(x1 + x0); y1 = (s16)(y1 + y0); z1 = (s16)(z1 + z0);
    const s16 cx[8] = { x0, x1, x1, x0, x0, x0, x1, x1 }, cy[8] = { y0, y0, y1, y1, y1, y0, y0, y1 }, cz[8] = { z0, z0, z0, z0, z1, z1, z1, z1 };
    update_clip();
    for (int i = 0; i < 8; i++) {
        memset(&cube[i], 0, sizeof cube[i]);
        for (int c = 0; c < 4; c++)
            cube[i].pos[c] = (s32)(((s64)cx[i] * m_clip[c] + (s64)cy[i] * m_clip[4 + c] + (s64)cz[i] * m_clip[8 + c] + (s64)0x1000 * m_clip[12 + c]) >> 12);
    }
    static const int faces[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 3, 4, 5 }, { 1, 2, 7, 6 }, { 0, 1, 6, 5 }, { 2, 3, 4, 7 } };
    g_box_tests++;
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) face[k] = cube[faces[f][k]];
        if (clip_polygon(face, 4, 0, 0) > 0) { gxstat |= 2; g_box_in++; return; }
    }
    if (g_wide_cull > 0 && viewport[0] == 0 && viewport[4] == 256) {   /* out of the DS's view: in the wide one? */
        for (int i = 0; i < 8; i++) cube[i].pos[0] = (s32)(((s64)cube[i].pos[0] * 256) / (256 + 2 * g_wide_cull));
        for (int f = 0; f < 6; f++) {
            for (int k = 0; k < 4; k++) face[k] = cube[faces[f][k]];
            if (clip_polygon(face, 4, 0, 0) > 0) { gxstat |= 2; g_box_in++; g_box_widened++; return; }
        }
    }
}

/* ---------------------------------------------------------------- commands */
static const uint8_t param_count[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12,
    [0x18] = 16, [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1,
    [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};
static int cmd_valid(uint8_t c) {
    return (c >= 0x10 && c <= 0x1C) || (c >= 0x20 && c <= 0x2B) || (c >= 0x30 && c <= 0x34) ||
           c == 0x40 || c == 0x41 || c == 0x50 || c == 0x60 || (c >= 0x70 && c <= 0x72);
}

/* the matrix a load/multiply goes to: projection, texture, or position (+ vector in mode 2) */
#define ON_MATRIX(op, both_vec)                                                     \
    do {                                                                            \
        if (mtx_mode == 0) { op(m_proj, p); clip_dirty = 1; }                       \
        else if (mtx_mode == 3) op(m_tex, p);                                       \
        else { op(m_pos, p); if (mtx_mode == 2 && (both_vec)) op(m_vec, p); clip_dirty = 1; } \
    } while (0)

/* tests: DQIX_PROJTRACE=frame: each projection load in that frame, with the functions before it */
extern void trace_dump(FILE* f, int n);
static void proj_trace(uint8_t cmd, const u32* p) {
    static long long at = -2; if (at == -2) { const char* e = getenv("DQIX_PROJTRACE"); at = e ? atoll(e) : -1; }
    if (at < 0 || g_frame != (uint64_t)at) return;
    int n = param_count[cmd];
    fprintf(stderr, "projload %02x:", cmd); for (int i = 0; i < n; i++) fprintf(stderr, " %08x", p[i]); fprintf(stderr, "\n");
    trace_dump(stderr, 12);
}

static void exec(uint8_t cmd, const u32* p) {
    if (mtx_mode == 0 && cmd >= 0x16 && cmd <= 0x19) proj_trace(cmd, p);
    switch (cmd) {
    case 0x10: mtx_mode = p[0] & 3; break;
    case 0x11:                                                       /* PUSH */
        if (mtx_mode == 0) { if (sp_proj > 0) gxstat |= 1u << 15; memcpy(st_proj, m_proj, 64); sp_proj = (sp_proj + 1) & 1; }
        else if (mtx_mode == 3) { if (sp_tex > 0) gxstat |= 1u << 15; memcpy(st_tex, m_tex, 64); sp_tex = (sp_tex + 1) & 1; }
        else {
            if (sp_pos > 30) gxstat |= 1u << 15;
            memcpy(st_pos[sp_pos & 31], m_pos, 64); memcpy(st_vec[sp_pos & 31], m_vec, 64);
            sp_pos = (sp_pos + 1) & 0x3F;
        }
        break;
    case 0x12:                                                       /* POP */
        if (mtx_mode == 0) { if (sp_proj == 0) gxstat |= 1u << 15; sp_proj = (sp_proj - 1) & 1; memcpy(m_proj, st_proj, 64); clip_dirty = 1; }
        else if (mtx_mode == 3) { if (sp_tex == 0) gxstat |= 1u << 15; sp_tex = (sp_tex - 1) & 1; memcpy(m_tex, st_tex, 64); }
        else {
            s32 off = (s32)(p[0] << 26) >> 26;
            sp_pos = (sp_pos - off) & 0x3F;
            if (sp_pos > 30) gxstat |= 1u << 15;
            memcpy(m_pos, st_pos[sp_pos & 31], 64); memcpy(m_vec, st_vec[sp_pos & 31], 64);
            clip_dirty = 1;
        }
        break;
    case 0x13:                                                       /* STORE */
        if (mtx_mode == 0) memcpy(st_proj, m_proj, 64);
        else if (mtx_mode == 3) memcpy(st_tex, m_tex, 64);
        else { u32 a = p[0] & 31; if (a > 30) gxstat |= 1u << 15; memcpy(st_pos[a], m_pos, 64); memcpy(st_vec[a], m_vec, 64); }
        break;
    case 0x14:                                                       /* RESTORE */
        if (mtx_mode == 0) { memcpy(m_proj, st_proj, 64); clip_dirty = 1; }
        else if (mtx_mode == 3) memcpy(m_tex, st_tex, 64);
        else { u32 a = p[0] & 31; if (a > 30) gxstat |= 1u << 15; memcpy(m_pos, st_pos[a], 64); memcpy(m_vec, st_vec[a], 64); clip_dirty = 1; }
        break;
    case 0x15:                                                       /* IDENTITY */
        if (mtx_mode == 0) { mtx_identity(m_proj); clip_dirty = 1; }
        else if (mtx_mode == 3) mtx_identity(m_tex);
        else { mtx_identity(m_pos); if (mtx_mode == 2) mtx_identity(m_vec); clip_dirty = 1; }
        break;
    case 0x16: ON_MATRIX(mtx_load4x4, 1); break;
    case 0x17: ON_MATRIX(mtx_load4x3, 1); break;
    case 0x18: ON_MATRIX(mtx_mult4x4, 1); break;
    case 0x19: ON_MATRIX(mtx_mult4x3, 1); break;
    case 0x1A: ON_MATRIX(mtx_mult3x3, 1); break;
    case 0x1B: ON_MATRIX(mtx_scale, 0); break;                       /* scale never touches the vector matrix */
    case 0x1C: ON_MATRIX(mtx_translate, 1); break;
    case 0x20: vtx_color[0] = p[0] & 0x1F; vtx_color[1] = (p[0] >> 5) & 0x1F; vtx_color[2] = (p[0] >> 10) & 0x1F; break;
    case 0x21:
        normal[0] = (s16)((p[0] & 0x000003FF) << 6) >> 6;
        normal[1] = (s16)((p[0] & 0x000FFC00) >> 4) >> 6;
        normal[2] = (s16)((p[0] & 0x3FF00000) >> 14) >> 6;
        calc_lighting();
        break;
    case 0x22:
        raw_tex[0] = (s16)(p[0] & 0xFFFF); raw_tex[1] = (s16)(p[0] >> 16);
        if ((tex_param >> 30) == 1) {
            tex_coords[0] = (s16)((raw_tex[0] * m_tex[0] + raw_tex[1] * m_tex[4] + m_tex[8] + m_tex[12]) >> 12);
            tex_coords[1] = (s16)((raw_tex[0] * m_tex[1] + raw_tex[1] * m_tex[5] + m_tex[9] + m_tex[13]) >> 12);
        } else { tex_coords[0] = raw_tex[0]; tex_coords[1] = raw_tex[1]; }
        break;
    case 0x23: cur_vtx[0] = (s16)(p[0] & 0xFFFF); cur_vtx[1] = (s16)(p[0] >> 16); cur_vtx[2] = (s16)(p[1] & 0xFFFF); submit_vertex(); break;
    case 0x24:
        cur_vtx[0] = (s16)((p[0] & 0x000003FF) << 6); cur_vtx[1] = (s16)((p[0] & 0x000FFC00) >> 4); cur_vtx[2] = (s16)((p[0] & 0x3FF00000) >> 14);
        submit_vertex(); break;
    case 0x25: cur_vtx[0] = (s16)(p[0] & 0xFFFF); cur_vtx[1] = (s16)(p[0] >> 16); submit_vertex(); break;
    case 0x26: cur_vtx[0] = (s16)(p[0] & 0xFFFF); cur_vtx[2] = (s16)(p[0] >> 16); submit_vertex(); break;
    case 0x27: cur_vtx[1] = (s16)(p[0] & 0xFFFF); cur_vtx[2] = (s16)(p[0] >> 16); submit_vertex(); break;
    case 0x28:
        cur_vtx[0] = (s16)(cur_vtx[0] + ((s16)((p[0] & 0x000003FF) << 6) >> 6));
        cur_vtx[1] = (s16)(cur_vtx[1] + ((s16)((p[0] & 0x000FFC00) >> 4) >> 6));
        cur_vtx[2] = (s16)(cur_vtx[2] + ((s16)((p[0] & 0x3FF00000) >> 14) >> 6));
        submit_vertex(); break;
    case 0x29: poly_attr = p[0]; break;
    case 0x2A: tex_param = p[0]; if (g_gx_tag && n_tagtex < TAGTEX_MAX) { tagtex[n_tagtex] = p[0]; tagtex_tag[n_tagtex++] = g_gx_tag; } break;
    case 0x2B: tex_pal = p[0] & 0x1FFF; break;
    case 0x30:
        for (int k = 0; k < 3; k++) { mat_dif[k] = (p[0] >> (5 * k)) & 0x1F; mat_amb[k] = (p[0] >> (16 + 5 * k)) & 0x1F; }
        if (p[0] & 0x8000) memcpy(vtx_color, mat_dif, 3);
        break;
    case 0x31:
        for (int k = 0; k < 3; k++) { mat_spe[k] = (p[0] >> (5 * k)) & 0x1F; mat_emi[k] = (p[0] >> (16 + 5 * k)) & 0x1F; }
        use_shine = (p[0] & 0x8000) != 0;
        break;
    case 0x32: {
        u32 l = p[0] >> 30;
        s16 d[3] = { (s16)((p[0] & 0x000003FF) << 6) >> 6, (s16)((p[0] & 0x000FFC00) >> 4) >> 6, (s16)((p[0] & 0x3FF00000) >> 14) >> 6 };
        for (int c = 0; c < 3; c++)
            light_dir[l][c] = (s16)((s32)((u32)(-((d[0] * m_vec[c] + d[1] * m_vec[4 + c] + d[2] * m_vec[8 + c]) >> 12)) << 21) >> 21);
        s32 den = -((s32)((u32)(d[0] * m_vec[2] + d[1] * m_vec[6] + d[2] * m_vec[10]) << 9) >> 21) + (1 << 9);
        spec_recip[l] = den == 0 ? 0 : (1 << 18) / den;
        break;
    }
    case 0x33: { u32 l = p[0] >> 30; for (int k = 0; k < 3; k++) light_col[l][k] = (p[0] >> (5 * k)) & 0x1F; break; }
    case 0x34: for (int i = 0; i < 128; i++) shine_table[i] = (u8)(p[i >> 2] >> (8 * (i & 3))); break;
    case 0x40:
        poly_mode = p[0] & 3; vtx_num = 0; vtx_in_poly = 0; consecutive = 0; last_strip = NULL;
        cur_poly_attr = poly_attr;
        break;
    case 0x41: break;
    case 0x50: flush_request = 1; flush_attr = p[0] & 3; break;
    case 0x60:
        viewport[0] = p[0] & 0xFF;
        viewport[1] = (191 - ((p[0] >> 8) & 0xFF)) & 0xFF;
        viewport[2] = (p[0] >> 16) & 0xFF;
        viewport[3] = (191 - (p[0] >> 24)) & 0xFF;
        viewport[4] = (viewport[2] - viewport[0] + 1) & 0x1FF;
        viewport[5] = (viewport[1] - viewport[3] + 1) & 0xFF;
        break;
    case 0x70: box_test(p); break;
    case 0x71: {
        cur_vtx[0] = (s16)(p[0] & 0xFFFF); cur_vtx[1] = (s16)(p[0] >> 16); cur_vtx[2] = (s16)(p[1] & 0xFFFF);
        s64 vx = cur_vtx[0], vy = cur_vtx[1], vz = cur_vtx[2];
        update_clip();
        for (int c = 0; c < 4; c++) postest[c] = (s32)((vx * m_clip[c] + vy * m_clip[4 + c] + vz * m_clip[8 + c] + (s64)0x1000 * m_clip[12 + c]) >> 12);
        break;
    }
    case 0x72: {
        s16 n[3] = { (s16)((p[0] & 0x000003FF) << 6) >> 6, (s16)((p[0] & 0x000FFC00) >> 4) >> 6, (s16)((p[0] & 0x3FF00000) >> 14) >> 6 };
        for (int c = 0; c < 3; c++) {
            vectest[c] = (s16)((n[0] * m_vec[c] + n[1] * m_vec[4 + c] + n[2] * m_vec[8 + c]) >> 9);
            if (vectest[c] & 0x1000) vectest[c] = (s16)(vectest[c] | 0xF000);
        }
        break;
    }
    default: break;
    }
}

/* Packed FIFO (GXFIFO): a command word holds up to 4 commands, then their
 * parameters follow. Commands run as soon as their parameters are complete. */
static uint8_t fifo_cmds[4];
static int fifo_ncmd, fifo_idx;
static u32 fifo_params[32];
static int fifo_np;

static void fifo_run_zero_param(void) {
    while (fifo_idx < fifo_ncmd) {
        uint8_t c = fifo_cmds[fifo_idx];
        if (!cmd_valid(c)) { fifo_idx++; continue; }
        if (param_count[c] != 0) return;
        exec(c, fifo_params);
        fifo_idx++;
    }
}

static void fifo_write(u32 v) {
    if (fifo_idx >= fifo_ncmd) {
        fifo_ncmd = 0; fifo_idx = 0; fifo_np = 0;
        for (int i = 0; i < 4; i++) fifo_cmds[fifo_ncmd++] = (uint8_t)(v >> (8 * i));
        while (fifo_ncmd > 0 && fifo_cmds[fifo_ncmd - 1] == 0) fifo_ncmd--;
        fifo_run_zero_param();
        return;
    }
    uint8_t c = fifo_cmds[fifo_idx];
    fifo_params[fifo_np++] = v;
    if (fifo_np >= param_count[c]) {
        exec(c, fifo_params);
        fifo_np = 0;
        fifo_idx++;
        fifo_run_zero_param();
    }
}

/* Direct command ports (0x04000440-0x040005FF) */
static uint8_t port_cmd;
static u32 port_params[32];
static int port_np;

static void port_write(uint8_t cmd, u32 v) {
    if (!cmd_valid(cmd)) return;
    if (param_count[cmd] == 0) { exec(cmd, port_params); return; }
    if (port_cmd != cmd) { port_cmd = cmd; port_np = 0; }
    port_params[port_np++] = v;
    if (port_np >= param_count[cmd]) { exec(cmd, port_params); port_np = 0; port_cmd = 0; }
}

void gpu3d_init(void);
/* The GXFIFO interrupt is level-triggered: with our always-empty FIFO it is
 * asserted whenever an interrupt condition (less than half full / empty) is selected. */
int gx_irq_line(void) { u32 m = gxstat >> 30; return m == 1 || m == 2; }

void gx_write(u32 off, u32 v) {
    static int inited;
    if (!inited) { inited = 1; gpu3d_init(); }
    if (off >= 0x400 && off < 0x440) { fifo_write(v); return; }
    if (off >= 0x440 && off < 0x600) { port_write((uint8_t)(0x10 + (off - 0x440) / 4), v); return; }
    if (off == 0x600) {
        if (v & 0x8000) { gxstat &= ~0x8000u; sp_proj = 0; sp_tex = 0; }
        gxstat = (gxstat & 0x3FFFFFFFu) | (v & 0xC0000000u);
        if (gx_irq_line()) irq_raise(&g_c9, IRQ_GXFIFO);
    }
}

u32 gx_read(u32 off) {
    off &= ~3u;
    if (off == 0x600) {
        u32 v = gxstat & 0xC0008002u;
        v |= (u32)(sp_pos & 31) << 8;
        v |= (u32)(sp_proj & 1) << 13;
        v |= 1u << 25 | 1u << 26;                                    /* FIFO less than half full, empty */
        return v;
    }
    if (off == 0x604) return n_poly | (n_vtx << 16);
    if (off >= 0x620 && off < 0x630) return (u32)postest[(off - 0x620) / 4];
    if (off == 0x630) return (u32)(u16)vectest[0] | ((u32)(u16)vectest[1] << 16);
    if (off == 0x634) return (u32)(u16)vectest[2];
    if (off >= 0x640 && off < 0x680) { update_clip(); return (u32)m_clip[(off - 0x640) / 4]; }
    if (off >= 0x680 && off < 0x6A4) { int i = (off - 0x680) / 4; return (u32)m_vec[(i / 3) * 4 + i % 3]; }
    u32 v; memcpy(&v, &g_io[off], 4); return v;
}

/* ================================================================ rasterizer */
/* Interpolation along an edge (dir 1, 9-bit factor) or a span (dir 0, 8-bit):
 * a perspective-correct factor between 0 and 1, then a linear blend of the
 * attribute with it. Equal, round W values interpolate exactly linearly. */
typedef struct {
    s32 x0, x1, xdiff, x;
    int shift, linear, wbuffer, dir;
    s32 xrecip_z, w0n, w0d, w1d;
    u32 yfactor;
} Interp;

static void interp_setup(Interp* it, int dir, s32 x0, s32 x1, s32 w0, s32 w1, int wbuffer) {
    it->dir = dir;
    it->x0 = x0; it->x1 = x1; it->xdiff = x1 - x0; it->wbuffer = wbuffer; it->x = 0; it->yfactor = 0;
    it->xrecip_z = it->xdiff != 0 ? (1 << 22) / it->xdiff : 0;
    u32 mask = dir ? 0x7E : 0x7F;
    it->linear = (w0 == w1) && !((u32)w0 & mask) && !((u32)w1 & mask);
    if (dir) { it->w0n = w0 >> 1; it->w0d = (w0 + ((w0 & ~w1) & 1)) >> 1; it->w1d = w1 >> 1; it->shift = 9; }
    else { it->w0n = w0; it->w0d = w0; it->w1d = w1; it->shift = 8; }
}
static inline void interp_setx(Interp* it, s32 x) {
    x -= it->x0;
    it->x = x;
    if (it->xdiff != 0 && (!it->linear || it->wbuffer)) {
        u32 num = ((u32)x * (u32)it->w0n) << it->shift;
        u32 den = (u32)x * (u32)it->w0d + (u32)(it->xdiff - x) * (u32)it->w1d;
        it->yfactor = den == 0 ? 0 : num / den;
    }
}
static inline s32 interp_value(const Interp* it, s32 y0, s32 y1) {
    if (it->xdiff == 0 || y0 == y1) return y0;
    if (!it->linear) {
        if (y0 < y1) return y0 + (s32)(((u32)(y1 - y0) * it->yfactor) >> it->shift);
        return y1 + (s32)(((u32)(y0 - y1) * ((1u << it->shift) - it->yfactor)) >> it->shift);
    }
    if (y0 < y1) return y0 + (s32)((s64)(y1 - y0) * it->x / it->xdiff);
    return y1 + (s32)((s64)(y0 - y1) * (it->xdiff - it->x) / it->xdiff);
}
static inline s32 interp_z(const Interp* it, s32 z0, s32 z1) {
    if (it->xdiff == 0 || z0 == z1) return z0;
    if (it->wbuffer) {
        if (z0 < z1) return z0 + (s32)(((s64)(z1 - z0) * it->yfactor) >> it->shift);
        return z1 + (s32)(((s64)(z0 - z1) * ((1 << it->shift) - (s64)it->yfactor)) >> it->shift);
    }
    s32 base, disp, factor;
    if (z0 < z1) { base = z0; disp = z1 - z0; factor = it->x; }
    else { base = z1; disp = z0 - z1; factor = it->xdiff - it->x; }
    if (it->dir) {
        int sh = 0;
        while (disp > 0x3FF) { disp >>= 1; sh++; }
        return base + (s32)((((s64)disp * factor * it->xrecip_z) >> 22) << sh);
    }
    disp >>= 9;
    return base + (s32)(((s64)disp * factor * it->xrecip_z) >> 13);
}

/* One polygon edge, stepped one scanline at a time (18-bit fractional X). */
typedef struct {
    s32 increment; int negative, xmajor;
    Interp interp;
    s32 x0, xmin, xmax, xlen, ylen, dx, y, xcov_incr;
    int side;                                       /* 0 left, 1 right */
} Slope;

static inline s32 slope_xval(const Slope* s) {
    s32 r = s->negative ? s->x0 - (s->dx >> 18) : s->x0 + (s->dx >> 18);
    if (r < s->xmin) r = s->xmin; else if (r > s->xmax) r = s->xmax;
    return r;
}
static s32 slope_setup_dummy(Slope* s, int side, s32 x0, int wbuffer) {
    memset(s, 0, sizeof *s);
    s->side = side; s->x0 = x0; s->xmin = x0; s->xmax = x0;
    interp_setup(&s->interp, 1, 0, 0, 0, 0, wbuffer);
    interp_setx(&s->interp, 0);
    return x0;
}
static s32 slope_setup(Slope* s, int side, s32 x0, s32 x1, s32 y0, s32 y1, s32 w0, s32 w1, s32 y, int wbuffer) {
    s->side = side; s->x0 = x0; s->y = y;
    if (x1 > x0) { s->xmin = x0; s->xmax = x1 - 1; s->negative = 0; }
    else if (x1 < x0) { s->xmin = x1; s->xmax = x0 - 1; s->negative = 1; }
    else { s->xmin = x0; s->xmax = x0; s->negative = 0; }
    s->xlen = s->xmax + 1 - s->xmin;
    s->ylen = y1 - y0;
    if (s->ylen == 0) s->increment = 0;
    else if (s->ylen == s->xlen && s->xlen != 1) s->increment = 0x40000;
    else {
        s32 yrecip = (1 << 18) / s->ylen;
        s->increment = (x1 - x0) * yrecip;
        if (s->increment < 0) s->increment = -s->increment;
    }
    s->xmajor = s->increment > 0x40000;
    if (side) {
        if (s->xmajor) s->dx = s->negative ? (0x20000 + 0x40000) : (s->increment - 0x20000);
        else if (s->increment != 0) s->dx = s->negative ? 0x40000 : 0;
        else s->dx = 0;
    } else {
        if (s->xmajor) s->dx = s->negative ? ((s->increment - 0x20000) + 0x40000) : 0x20000;
        else if (s->increment != 0) s->dx = s->negative ? 0x40000 : 0;
        else s->dx = 0;
    }
    s->dx += (y - y0) * s->increment;
    s32 x = slope_xval(s);
    int interpoffset = (s->increment >= 0x40000) && (side ^ s->negative);
    interp_setup(&s->interp, 1, y0 - interpoffset, y1 - interpoffset, w0, w1, wbuffer);
    interp_setx(&s->interp, y);
    if (s->xmajor) s->xcov_incr = (s->ylen << 10) / s->xlen;
    return x;
}
static inline s32 slope_step(Slope* s) {
    s->dx += s->increment;
    s->y++;
    s32 x = slope_xval(s);
    interp_setx(&s->interp, s->y);
    return x;
}
/* edge span length and anti-aliasing coverage for the current scanline */
static void slope_edge_params(const Slope* s, int swapped, s32* length, s32* coverage) {
    if (s->xmajor) {
        if (!swapped || s->side) {
            if (s->side ^ s->negative) *length = (s->dx >> 18) - ((s->dx - s->increment) >> 18);
            else *length = ((s->dx + s->increment) >> 18) - (s->dx >> 18);
        }
        s32 startx = s->dx >> 18;
        if (s->negative) startx = s->xlen - startx;
        if (s->side) startx = startx - *length + 1;
        s32 startcov = (s32)((((s64)(startx << 10) + 0x1FF) * s->ylen) / s->xlen);
        *coverage = (s32)((1u << 31) | ((u32)(startcov & 0x3FF) << 12) | (u32)(s->xcov_incr & 0x3FF));
        if (swapped) *length = 1;
    } else {
        *length = 1;
        if (s->increment == 0) *coverage = swapped ? 0 : 31;
        else {
            s32 cov = ((s->dx >> 9) + (s->increment >> 10)) >> 4;
            if ((cov >> 5) != (s->dx >> 18)) cov = 31;
            cov &= 0x1F;
            if (swapped) { if (s->side ^ s->negative) cov = 0x1F - cov; }
            else { if (!(s->side ^ s->negative)) cov = 0x1F - cov; }
            *coverage = cov;
        }
    }
}

/* ---------------------------------------------------------------- buffers
 * 258x194 with a 1-pixel border (simplifies edge marking); each buffer is
 * doubled to keep the two topmost pixels (anti-aliasing, shadows).
 * attr: bit0-3 edge flags (left/right/top/bottom), bit4 back-facing,
 * bit8-12 AA coverage, bit15 fog, bit16-21 translucent polygon ID,
 * bit22 translucent, bit24-29 opaque polygon ID. Colours are 6-bit, alpha 5-bit. */
#define SW 258
#define SH 194
#define BUFSZ (SW * SH)
#define FIRSTPX (SW + 1)
static u32 cbuf[BUFSZ * 2], zbuf[BUFSZ * 2], abuf[BUFSZ * 2];
static u8 stencil[256 * 2];
static int prev_shadowmask;

typedef struct {
    HPoly* p;
    Slope sl, sr;
    s32 xl, xr;
    u32 curvl, curvr, nextvl, nextvr;
} RPoly;
static RPoly rpolys[POLY_MAX];

/* ---------------------------------------------------------------- textures */
static inline u8 tex8(u32 a) { uint8_t* p = g_tex_slot[(a >> 17) & 3]; return p ? p[a & 0x1FFFF] : 0; }
static inline u16 tex16(u32 a) { return (u16)(tex8(a) | (tex8(a + 1) << 8)); }
static inline u16 pal16(u32 a) {
    u32 slot = a >> 14;
    uint8_t* p = slot < 6 ? g_texpal_slot[slot] : NULL;
    if (!p) return 0;
    u16 v; memcpy(&v, p + (a & 0x3FFE), 2); return v;
}

static void texture_lookup(u32 texparam, u32 texpal, s32 s, s32 t, u16* color, u8* alpha) {
    u32 vramaddr = (texparam & 0xFFFF) << 3;
    s32 width = 8 << ((texparam >> 20) & 7), height = 8 << ((texparam >> 23) & 7);
    s >>= 4; t >>= 4;
    if (texparam & (1 << 16)) {
        if (texparam & (1 << 18)) s = (s & width) ? (width - 1) - (s & (width - 1)) : (s & (width - 1));
        else s &= width - 1;
    } else { if (s < 0) s = 0; else if (s >= width) s = width - 1; }
    if (texparam & (1 << 17)) {
        if (texparam & (1 << 19)) t = (t & height) ? (height - 1) - (t & (height - 1)) : (t & (height - 1));
        else t &= height - 1;
    } else { if (t < 0) t = 0; else if (t >= height) t = height - 1; }
    u8 alpha0 = (texparam & (1 << 29)) ? 0 : 31;
    switch ((texparam >> 26) & 7) {
    case 1: {                                                       /* A3I5 */
        u8 px = tex8(vramaddr + (u32)(t * width + s));
        *color = pal16((texpal << 4) + ((px & 0x1F) << 1));
        *alpha = (u8)(((px >> 3) & 0x1C) + (px >> 6));
        break;
    }
    case 2: {                                                       /* 4 colours */
        u8 px = tex8(vramaddr + ((u32)(t * width + s) >> 2));
        px = (px >> ((s & 3) << 1)) & 3;
        *color = pal16((texpal << 3) + (px << 1));
        *alpha = px == 0 ? alpha0 : 31;
        break;
    }
    case 3: {                                                       /* 16 colours */
        u8 px = tex8(vramaddr + ((u32)(t * width + s) >> 1));
        px = (s & 1) ? px >> 4 : px & 0xF;
        *color = pal16((texpal << 4) + (px << 1));
        *alpha = px == 0 ? alpha0 : 31;
        break;
    }
    case 4: {                                                       /* 256 colours */
        u8 px = tex8(vramaddr + (u32)(t * width + s));
        *color = pal16((texpal << 4) + (px << 1));
        *alpha = px == 0 ? alpha0 : 31;
        break;
    }
    case 5: {                                                       /* 4x4 compressed */
        vramaddr += (u32)((t & 0x3FC) * (width >> 2)) + (u32)(s & 0x3FC);
        vramaddr += (u32)(t & 3);
        vramaddr &= 0x7FFFF;
        u32 slot1 = 0x20000 + ((vramaddr & 0x1FFFC) >> 1);
        if (vramaddr >= 0x40000) slot1 += 0x10000;
        u8 val;
        if (vramaddr >= 0x20000 && vramaddr < 0x40000) val = 0;
        else val = (u8)(tex8(vramaddr) >> (2 * (s & 3)));
        u16 palinfo = tex16(slot1);
        u32 po = (u32)(palinfo & 0x3FFF) << 2, pb = texpal << 4;
        u32 mode = palinfo >> 14;
        u16 c0 = pal16(pb + po), c1 = pal16(pb + po + 2);
        *alpha = 31;
        switch (val & 3) {
        case 0: *color = c0; break;
        case 1: *color = c1; break;
        case 2:
            if (mode == 1) *color = (u16)((((c0 & 0x1F) + (c1 & 0x1F)) >> 1) | ((((c0 & 0x3E0) + (c1 & 0x3E0)) >> 1) & 0x3E0) | ((((c0 & 0x7C00) + (c1 & 0x7C00)) >> 1) & 0x7C00));
            else if (mode == 3) *color = (u16)((((c0 & 0x1F) * 5 + (c1 & 0x1F) * 3) >> 3) | ((((c0 & 0x3E0) * 5 + (c1 & 0x3E0) * 3) >> 3) & 0x3E0) | ((((c0 & 0x7C00) * 5 + (c1 & 0x7C00) * 3) >> 3) & 0x7C00));
            else *color = pal16(pb + po + 4);
            break;
        case 3:
            if (mode == 2) *color = pal16(pb + po + 6);
            else if (mode == 3) *color = (u16)((((c0 & 0x1F) * 3 + (c1 & 0x1F) * 5) >> 3) | ((((c0 & 0x3E0) * 3 + (c1 & 0x3E0) * 5) >> 3) & 0x3E0) | ((((c0 & 0x7C00) * 3 + (c1 & 0x7C00) * 5) >> 3) & 0x7C00));
            else { *color = 0; *alpha = 0; }
            break;
        }
        break;
    }
    case 6: {                                                       /* A5I3 */
        u8 px = tex8(vramaddr + (u32)(t * width + s));
        *color = pal16((texpal << 4) + ((px & 7) << 1));
        *alpha = px >> 3;
        break;
    }
    case 7: {                                                       /* direct colour */
        *color = tex16(vramaddr + ((u32)(t * width + s) << 1));
        *alpha = (*color & 0x8000) ? 31 : 0;
        break;
    }
    default: *color = 0; *alpha = 0; break;
    }
}

/* ---------------------------------------------------------------- pixels */
static inline int depth_test(int mode, s32 dstz, s32 z, u32 dstattr) {
    switch (mode) {
    case 0: return z < dstz;                                        /* less */
    case 1: if ((dstattr & 0x00400010) == 0x00000010) return z <= dstz; return z < dstz;   /* front over opaque back-facing */
    case 2: return (u32)((dstz - z) + 0x200) <= 0x400;              /* equal (Z) */
    default: return (u32)((dstz - z) + 0xFF) <= 0x1FE;              /* equal (W) */
    }
}

static inline u32 alpha_blend(u32 src, u32 dst, u32 alpha) {
    u32 dsta = dst >> 24;
    if (dsta == 0) return src;
    u32 r = src & 0x3F, g = (src >> 8) & 0x3F, b = (src >> 16) & 0x3F;
    if (r_disp & (1 << 3)) {
        alpha++;
        r = (r * alpha + (dst & 0x3F) * (32 - alpha)) >> 5;
        g = (g * alpha + ((dst >> 8) & 0x3F) * (32 - alpha)) >> 5;
        b = (b * alpha + ((dst >> 16) & 0x3F) * (32 - alpha)) >> 5;
        alpha--;
    }
    if (alpha > dsta) dsta = alpha;
    return r | (g << 8) | (b << 16) | (dsta << 24);
}

static inline u32 expand5(u32 c, u32* r, u32* g, u32* b) {        /* 15-bit -> 6-bit channels (+1 if non-zero) */
    *r = (c << 1) & 0x3E; if (*r) (*r)++;
    *g = (c >> 4) & 0x3E; if (*g) (*g)++;
    *b = (c >> 9) & 0x3E; if (*b) (*b)++;
    return 0;
}

static u32 render_pixel(const HPoly* p, u8 vr, u8 vg, u8 vb, s16 s, s16 t) {
    u32 r, g, b, a;
    u32 blendmode = (p->attr >> 4) & 3, polyalpha = (p->attr >> 16) & 0x1F;
    int wire = polyalpha == 0;
    if (blendmode == 2) {
        if (r_disp & (1 << 1)) { vg = vr; vb = vr; }                /* highlight */
        else { u32 tr, tg, tb; expand5(r_toon[vr >> 1], &tr, &tg, &tb); vr = (u8)tr; vg = (u8)tg; vb = (u8)tb; }   /* toon */
    }
    if ((r_disp & 1) && ((p->texparam >> 26) & 7) != 0) {
        u16 tc; u8 ta;
        texture_lookup(p->texparam, p->texpal, s, t, &tc, &ta);
        u32 tr, tg, tb; expand5(tc, &tr, &tg, &tb);
        if (blendmode & 1) {                                        /* decal */
            if (ta == 0) { r = vr; g = vg; b = vb; }
            else if (ta == 31) { r = tr; g = tg; b = tb; }
            else { r = (tr * ta + vr * (31u - ta)) >> 5; g = (tg * ta + vg * (31u - ta)) >> 5; b = (tb * ta + vb * (31u - ta)) >> 5; }
            a = polyalpha;
        } else {                                                    /* modulate */
            r = ((tr + 1) * (vr + 1u) - 1) >> 6;
            g = ((tg + 1) * (vg + 1u) - 1) >> 6;
            b = ((tb + 1) * (vb + 1u) - 1) >> 6;
            a = ((ta + 1u) * (polyalpha + 1) - 1) >> 5;
        }
    } else { r = vr; g = vg; b = vb; a = polyalpha; }
    if (blendmode == 2 && (r_disp & (1 << 1))) {
        u32 hr, hg, hb; expand5(r_toon[vr >> 1], &hr, &hg, &hb);
        r += hr; g += hg; b += hb;
        if (r > 63) r = 63; if (g > 63) g = 63; if (b > 63) b = 63;
    }
    if (wire) a = 31;
    return (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16) | (a << 24);
}

static void plot_translucent(u32 pa, u32 color, u32 z, u32 polyattr, int shadow) {
    u32 dstattr = abuf[pa];
    u32 attr = (polyattr & 0xE0F0) | ((polyattr >> 8) & 0xFF0000) | (1u << 22) | (dstattr & 0xFF001F0F);
    if (shadow) {
        if (dstattr & (1u << 22)) { if ((dstattr & 0x007F0000) == (attr & 0x007F0000)) return; }
        else if ((dstattr & 0x3F000000) == (polyattr & 0x3F000000)) return;
    } else if ((dstattr & 0x007F0000) == (attr & 0x007F0000)) return;
    if (!(dstattr & (1u << 15))) attr &= ~(1u << 15);
    color = alpha_blend(color, cbuf[pa], color >> 24);
    if (z != 0xFFFFFFFFu) zbuf[pa] = z;
    cbuf[pa] = color;
    abuf[pa] = attr;
}

/* ---------------------------------------------------------------- polygon edges */
static void setup_left_edge(RPoly* rp, s32 y) {
    HPoly* p = rp->p;
    while (y >= p->v[rp->nextvl]->fpos[1] && rp->curvl != p->vbot) {
        rp->curvl = rp->nextvl;
        if (p->facing) { rp->nextvl = rp->curvl + 1; if (rp->nextvl >= p->n) rp->nextvl = 0; }
        else { rp->nextvl = rp->curvl - 1; if ((s32)rp->nextvl < 0) rp->nextvl = p->n - 1; }
    }
    rp->xl = slope_setup(&rp->sl, 0, p->v[rp->curvl]->fpos[0], p->v[rp->nextvl]->fpos[0], p->v[rp->curvl]->fpos[1], p->v[rp->nextvl]->fpos[1],
                         p->w[rp->curvl], p->w[rp->nextvl], y, p->wbuf);
}
static void setup_right_edge(RPoly* rp, s32 y) {
    HPoly* p = rp->p;
    while (y >= p->v[rp->nextvr]->fpos[1] && rp->curvr != p->vbot) {
        rp->curvr = rp->nextvr;
        if (p->facing) { rp->nextvr = rp->curvr - 1; if ((s32)rp->nextvr < 0) rp->nextvr = p->n - 1; }
        else { rp->nextvr = rp->curvr + 1; if (rp->nextvr >= p->n) rp->nextvr = 0; }
    }
    rp->xr = slope_setup(&rp->sr, 1, p->v[rp->curvr]->fpos[0], p->v[rp->nextvr]->fpos[0], p->v[rp->curvr]->fpos[1], p->v[rp->nextvr]->fpos[1],
                         p->w[rp->curvr], p->w[rp->nextvr], y, p->wbuf);
}
static void setup_polygon(RPoly* rp, HPoly* p) {
    u32 n = p->n, vtop = p->vtop, vbot = p->vbot;
    rp->p = p;
    rp->curvl = vtop; rp->curvr = vtop;
    if (p->facing) {
        rp->nextvl = rp->curvl + 1; if (rp->nextvl >= n) rp->nextvl = 0;
        rp->nextvr = rp->curvr - 1; if ((s32)rp->nextvr < 0) rp->nextvr = n - 1;
    } else {
        rp->nextvl = rp->curvl - 1; if ((s32)rp->nextvl < 0) rp->nextvl = n - 1;
        rp->nextvr = rp->curvr + 1; if (rp->nextvr >= n) rp->nextvr = 0;
    }
    if (p->ybot == p->ytop) {
        vtop = 0; vbot = 0;
        u32 i = 1;
        if (p->v[i]->fpos[0] < p->v[vtop]->fpos[0]) vtop = i;
        if (p->v[i]->fpos[0] > p->v[vbot]->fpos[0]) vbot = i;
        i = n - 1;
        if (p->v[i]->fpos[0] < p->v[vtop]->fpos[0]) vtop = i;
        if (p->v[i]->fpos[0] > p->v[vbot]->fpos[0]) vbot = i;
        rp->curvl = vtop; rp->nextvl = vtop; rp->curvr = vbot; rp->nextvr = vbot;
        rp->xl = slope_setup_dummy(&rp->sl, 0, p->v[rp->curvl]->fpos[0], p->wbuf);
        rp->xr = slope_setup_dummy(&rp->sr, 1, p->v[rp->curvr]->fpos[0], p->wbuf);
    } else {
        setup_left_edge(rp, p->ytop);
        setup_right_edge(rp, p->ytop);
    }
}

/* Common per-scanline setup: span ends, swapped edges, fill rules. */
typedef struct {
    s32 xstart, xend, wl, wr, zl, zr;
    int l_fill, r_fill;
    s32 l_len, r_len, l_cov, r_cov;
    Interp *istart, *iend;
    Vtx *vlcur, *vlnext, *vrcur, *vrnext;
    int yedge;
} Span;

static void span_setup(RPoly* rp, s32 y, Span* sp, int wire, u32 polyalpha) {
    HPoly* p = rp->p;
    if (p->ytop != p->ybot) {
        if (y >= p->v[rp->nextvl]->fpos[1] && rp->curvl != p->vbot) setup_left_edge(rp, y);
        if (y >= p->v[rp->nextvr]->fpos[1] && rp->curvr != p->vbot) setup_right_edge(rp, y);
    }
    sp->xstart = rp->xl; sp->xend = rp->xr;
    sp->wl = interp_value(&rp->sl.interp, p->w[rp->curvl], p->w[rp->nextvl]);
    sp->wr = interp_value(&rp->sr.interp, p->w[rp->curvr], p->w[rp->nextvr]);
    sp->zl = interp_z(&rp->sl.interp, p->z[rp->curvl], p->z[rp->nextvl]);
    sp->zr = interp_z(&rp->sr.interp, p->z[rp->curvr], p->z[rp->nextvr]);
    /* right vertical edges are pushed 1px left (unless the span is a single pixel at x 0) */
    if (rp->sr.increment == 0 && (rp->sl.increment != 0 || sp->xstart != sp->xend) && sp->xend != 0) sp->xend--;
    int always = (r_disp & ((1 << 4) | (1 << 5))) || (polyalpha < 31 && (r_disp & (1 << 3))) || wire;
    if (sp->xstart > sp->xend) {                                    /* swapped: draw from the right edge */
        sp->vlcur = p->v[rp->curvr]; sp->vlnext = p->v[rp->nextvr];
        sp->vrcur = p->v[rp->curvl]; sp->vrnext = p->v[rp->nextvl];
        sp->istart = &rp->sr.interp; sp->iend = &rp->sl.interp;
        slope_edge_params(&rp->sr, 1, &sp->l_len, &sp->l_cov);
        slope_edge_params(&rp->sl, 1, &sp->r_len, &sp->r_cov);
        s32 t;
        t = sp->xstart; sp->xstart = sp->xend; sp->xend = t;
        t = sp->wl; sp->wl = sp->wr; sp->wr = t;
        t = sp->zl; sp->zl = sp->zr; sp->zr = t;
        if (always) { sp->l_fill = 1; sp->r_fill = 1; }
        else {
            sp->l_fill = (rp->sr.negative || !rp->sr.xmajor)
                || (y == p->ybot - 1 && rp->sr.xmajor && sp->vlnext->fpos[0] != sp->vrnext->fpos[0]);
            sp->r_fill = (!rp->sl.negative && rp->sl.xmajor)
                || (!(rp->sl.negative && rp->sl.xmajor) && rp->sr.increment == 0)
                || (y == p->ybot - 1 && rp->sl.xmajor && sp->vlnext->fpos[0] != sp->vrnext->fpos[0]);
        }
    } else {
        sp->vlcur = p->v[rp->curvl]; sp->vlnext = p->v[rp->nextvl];
        sp->vrcur = p->v[rp->curvr]; sp->vrnext = p->v[rp->nextvr];
        sp->istart = &rp->sl.interp; sp->iend = &rp->sr.interp;
        slope_edge_params(&rp->sl, 0, &sp->l_len, &sp->l_cov);
        slope_edge_params(&rp->sr, 0, &sp->r_len, &sp->r_cov);
        if (always) { sp->l_fill = 1; sp->r_fill = 1; }
        else {
            sp->l_fill = ((rp->sl.negative || !rp->sl.xmajor)
                || (y == p->ybot - 1 && rp->sl.xmajor && sp->vlnext->fpos[0] != sp->vrnext->fpos[0]))
                || (rp->sl.increment == rp->sr.increment && sp->xstart + sp->l_len == sp->xend + 1);
            sp->r_fill = (!rp->sr.negative && rp->sr.xmajor) || rp->sr.increment == 0
                || (y == p->ybot - 1 && rp->sr.xmajor && sp->vlnext->fpos[0] != sp->vrnext->fpos[0]);
        }
    }
    sp->yedge = y == p->ytop ? 0x4 : y == p->ybot - 1 ? 0x8 : 0;
}

static int depth_mode(const HPoly* p) {
    if (p->attr & (1 << 14)) return p->wbuf ? 3 : 2;
    return p->facing ? 1 : 0;
}

static void render_shadowmask_scanline(RPoly* rp, s32 y) {
    HPoly* p = rp->p;
    u32 polyalpha = (p->attr >> 16) & 0x1F;
    int wire = polyalpha == 0, dm = depth_mode(p);
    if (!prev_shadowmask) memset(&stencil[256 * (y & 1)], 0, 256);
    prev_shadowmask = 1;
    Span sp; span_setup(rp, y, &sp, wire, polyalpha);
    if (wire) polyalpha = 31;
    if (polyalpha <= r_alpharef) return;                            /* (the edges are not stepped either) */
    Interp ix; interp_setup(&ix, 0, sp.xstart, sp.xend + 1, sp.wl, sp.wr, p->wbuf);
    s32 x = sp.xstart; if (x < 0) x = 0;
    for (int part = 0; part < 3; part++) {
        s32 xlimit;
        if (part == 0) { xlimit = sp.xstart + sp.l_len; if (xlimit > sp.xend + 1) xlimit = sp.xend + 1; if (xlimit > 256) xlimit = 256; if (!sp.l_fill) { x = xlimit; continue; } }
        else if (part == 1) { xlimit = sp.xend - sp.r_len + 1; if (xlimit > sp.xend + 1) xlimit = sp.xend + 1; if (xlimit > 256) xlimit = 256; if (wire && !sp.yedge) { if (x < xlimit) x = xlimit; continue; } }
        else { xlimit = sp.xend + 1; if (xlimit > 256) xlimit = 256; if (!sp.r_fill) continue; }
        for (; x < xlimit; x++) {
            u32 pa = FIRSTPX + (u32)y * SW + (u32)x;
            interp_setx(&ix, x);
            s32 z = interp_z(&ix, sp.zl, sp.zr);
            u32 dstattr = abuf[pa];
            if (!depth_test(dm, (s32)zbuf[pa], z, dstattr)) stencil[256 * (y & 1) + x] = 1;
            if (dstattr & 0xF) {
                if (!depth_test(dm, (s32)zbuf[pa + BUFSZ], z, abuf[pa + BUFSZ])) stencil[256 * (y & 1) + x] |= 2;
            }
        }
    }
    rp->xl = slope_step(&rp->sl);
    rp->xr = slope_step(&rp->sr);
}

static void render_polygon_scanline(RPoly* rp, s32 y) {
    HPoly* p = rp->p;
    u32 polyattr = p->attr & 0x3F008000;
    if (!p->facing) polyattr |= 1u << 4;
    u32 polyalpha = (p->attr >> 16) & 0x1F;
    int wire = polyalpha == 0, dm = depth_mode(p);
    prev_shadowmask = 0;
    Span sp; span_setup(rp, y, &sp, wire, polyalpha);
    s32 rl = interp_value(sp.istart, sp.vlcur->fcol[0], sp.vlnext->fcol[0]);
    s32 gl = interp_value(sp.istart, sp.vlcur->fcol[1], sp.vlnext->fcol[1]);
    s32 bl = interp_value(sp.istart, sp.vlcur->fcol[2], sp.vlnext->fcol[2]);
    s32 sl = interp_value(sp.istart, sp.vlcur->tex[0], sp.vlnext->tex[0]);
    s32 tl = interp_value(sp.istart, sp.vlcur->tex[1], sp.vlnext->tex[1]);
    s32 rr = interp_value(sp.iend, sp.vrcur->fcol[0], sp.vrnext->fcol[0]);
    s32 gr = interp_value(sp.iend, sp.vrcur->fcol[1], sp.vrnext->fcol[1]);
    s32 br = interp_value(sp.iend, sp.vrcur->fcol[2], sp.vrnext->fcol[2]);
    s32 sr = interp_value(sp.iend, sp.vrcur->tex[0], sp.vrnext->tex[0]);
    s32 tr = interp_value(sp.iend, sp.vrcur->tex[1], sp.vrnext->tex[1]);
    Interp ix; interp_setup(&ix, 0, sp.xstart, sp.xend + 1, sp.wl, sp.wr, p->wbuf);
    s32 x = sp.xstart; if (x < 0) x = 0;
    s32 xcov = 0;
    int aa = (r_disp & (1 << 4)) != 0;
    for (int part = 0; part < 3; part++) {
        s32 xlimit, edge, ecov = 0;
        if (part == 0) {
            edge = sp.yedge | 1;
            xlimit = sp.xstart + sp.l_len; if (xlimit > sp.xend + 1) xlimit = sp.xend + 1; if (xlimit > 256) xlimit = 256;
            ecov = sp.l_cov;
            if (ecov & (1 << 31)) { xcov = (ecov >> 12) & 0x3FF; if (xcov == 0x3FF) xcov = 0; }
            if (!sp.l_fill) { x = xlimit; continue; }
        } else if (part == 1) {
            edge = sp.yedge;
            xlimit = sp.xend - sp.r_len + 1; if (xlimit > sp.xend + 1) xlimit = sp.xend + 1; if (xlimit > 256) xlimit = 256;
            if (wire && !edge) { if (x < xlimit) x = xlimit; continue; }
        } else {
            edge = sp.yedge | 2;
            xlimit = sp.xend + 1; if (xlimit > 256) xlimit = 256;
            ecov = sp.r_cov;
            if (ecov & (1 << 31)) { xcov = (ecov >> 12) & 0x3FF; if (xcov == 0x3FF) xcov = 0; }
            if (!sp.r_fill) continue;
        }
        for (; x < xlimit; x++) {
            u32 pa = FIRSTPX + (u32)y * SW + (u32)x;
            u32 dstattr = abuf[pa];
            if (p->shadow) {
                u8 st = stencil[256 * (y & 1) + x];
                if (!st) continue;
                if (!(st & 1)) pa += BUFSZ;
                if (!(st & 2)) dstattr &= ~0xFu;
            }
            interp_setx(&ix, x);
            s32 z = interp_z(&ix, sp.zl, sp.zr);
            if (!depth_test(dm, (s32)zbuf[pa], z, dstattr)) {          /* failing the top pixel: try the one under it */
                if (!(dstattr & 0xF) || pa >= BUFSZ) continue;
                pa += BUFSZ;
                dstattr = abuf[pa];
                if (!depth_test(dm, (s32)zbuf[pa], z, dstattr)) continue;
            }
            u32 vr = (u32)interp_value(&ix, rl, rr), vg = (u32)interp_value(&ix, gl, gr), vb = (u32)interp_value(&ix, bl, br);
            s16 s = (s16)interp_value(&ix, sl, sr), t = (s16)interp_value(&ix, tl, tr);
            u32 color = render_pixel(p, (u8)(vr >> 3), (u8)(vg >> 3), (u8)(vb >> 3), s, t);
            u8 alpha = (u8)(color >> 24);
            if (alpha <= r_alpharef) continue;
            if (alpha == 31) {
                u32 attr = polyattr | (u32)edge;
                if (aa && (part != 1 || (attr & 0xF))) {
                    s32 cov;
                    if (part == 1) cov = 0x1F;
                    else {
                        cov = ecov;
                        if (cov & (1 << 31)) {
                            if (part == 0) { cov = xcov >> 5; if (cov > 31) cov = 31; }
                            else { cov = 0x1F - (xcov >> 5); if (cov < 0) cov = 0; }
                            xcov += ecov & 0x3FF;
                        }
                    }
                    attr |= (u32)cov << 8;
                    if (pa < BUFSZ) { cbuf[pa + BUFSZ] = cbuf[pa]; zbuf[pa + BUFSZ] = zbuf[pa]; abuf[pa + BUFSZ] = abuf[pa]; }
                }
                zbuf[pa] = (u32)z; cbuf[pa] = color; abuf[pa] = attr;
            } else {
                u32 zz = (p->attr & (1 << 11)) ? (u32)z : 0xFFFFFFFFu;
                plot_translucent(pa, color, zz, polyattr, p->shadow);
                if ((dstattr & 0xF) && pa < BUFSZ) plot_translucent(pa + BUFSZ, color, zz, polyattr, p->shadow);
            }
        }
    }
    rp->xl = slope_step(&rp->sl);
    rp->xr = slope_step(&rp->sr);
}

static void render_scanline(s32 y, int n) {
    for (int i = 0; i < n; i++) {
        RPoly* rp = &rpolys[i];
        HPoly* p = rp->p;
        if (y >= p->ytop && (y < p->ybot || (y == p->ytop && p->ybot == p->ytop))) {
            if (p->shadowmask) render_shadowmask_scanline(rp, y);
            else render_polygon_scanline(rp, y);
        }
    }
}

/* ---------------------------------------------------------------- final pass: edges, fog, anti-aliasing */
static u32 fog_density(u32 pa) {
    u32 z = zbuf[pa], id, frac;
    if (z < r_fogoffset) { id = 0; frac = 0; }
    else {
        z -= r_fogoffset;
        z = (z >> 2) << r_fogshift;
        id = z >> 17;
        if (id >= 32) { id = 32; frac = 0; } else frac = z & 0x1FFFF;
    }
    u32 d = (r_fogtab[id] * (0x20000 - frac) + r_fogtab[id + 1] * frac) >> 17;
    if (d >= 127) d = 128;
    return d;
}

static void apply_fog(u32 pa, u32 fr, u32 fg, u32 fb, u32 fa, int fogcolor) {
    u32 d = fog_density(pa), c = cbuf[pa];
    u32 r = c & 0x3F, g = (c >> 8) & 0x3F, b = (c >> 16) & 0x3F, a = (c >> 24) & 0x1F;
    if (fogcolor) { r = (fr * d + r * (128 - d)) >> 7; g = (fg * d + g * (128 - d)) >> 7; b = (fb * d + b * (128 - d)) >> 7; }
    a = (fa * d + a * (128 - d)) >> 7;
    cbuf[pa] = r | (g << 8) | (b << 16) | (a << 24);
}

static void final_pass(s32 y) {
    if (r_disp & (1 << 5)) {                                        /* edge marking (topmost pixels) */
        for (int x = 0; x < 256; x++) {
            u32 pa = FIRSTPX + (u32)y * SW + (u32)x;
            u32 attr = abuf[pa];
            if (!(attr & 0xF)) continue;
            u32 id = attr >> 24, z = zbuf[pa];
            if ((id != (abuf[pa - 1] >> 24) && z < zbuf[pa - 1]) || (id != (abuf[pa + 1] >> 24) && z < zbuf[pa + 1]) ||
                (id != (abuf[pa - SW] >> 24) && z < zbuf[pa - SW]) || (id != (abuf[pa + SW] >> 24) && z < zbuf[pa + SW])) {
                u32 er, eg, eb; expand5(r_edge[id >> 3], &er, &eg, &eb);
                cbuf[pa] = er | (eg << 8) | (eb << 16) | (cbuf[pa] & 0xFF000000);
                abuf[pa] = (abuf[pa] & 0xFFFFE0FF) | 0x00001000;
            }
        }
    }
    if (r_disp & (1 << 7)) {                                        /* fog (top two pixels) */
        int fogcolor = !(r_disp & (1 << 6));
        u32 fr, fg, fb; expand5(r_fogcolor, &fr, &fg, &fb);
        u32 fa = (r_fogcolor >> 16) & 0x1F;
        for (int x = 0; x < 256; x++) {
            u32 pa = FIRSTPX + (u32)y * SW + (u32)x;
            u32 attr = abuf[pa];
            if (attr & (1u << 15)) apply_fog(pa, fr, fg, fb, fa, fogcolor);
            if (!(attr & 0xF)) continue;
            if (abuf[pa + BUFSZ] & (1u << 15)) apply_fog(pa + BUFSZ, fr, fg, fb, fa, fogcolor);
        }
    }
    if (r_disp & (1 << 4)) {                                        /* anti-aliasing: blend edge pixels with the one under */
        for (int x = 0; x < 256; x++) {
            u32 pa = FIRSTPX + (u32)y * SW + (u32)x;
            u32 attr = abuf[pa];
            if (!(attr & 0xF)) continue;
            u32 cov = (attr >> 8) & 0x1F;
            if (cov == 0x1F) continue;
            if (cov == 0) { cbuf[pa] = cbuf[pa + BUFSZ]; continue; }
            u32 top = cbuf[pa], bot = cbuf[pa + BUFSZ];
            u32 tr = top & 0x3F, tg = (top >> 8) & 0x3F, tb = (top >> 16) & 0x3F, ta = (top >> 24) & 0x1F;
            u32 br = bot & 0x3F, bg = (bot >> 8) & 0x3F, bb = (bot >> 16) & 0x3F, ba = (bot >> 24) & 0x1F;
            cov++;
            if (ba > 0) { tr = (tr * cov + br * (32 - cov)) >> 5; tg = (tg * cov + bg * (32 - cov)) >> 5; tb = (tb * cov + bb * (32 - cov)) >> 5; }
            ta = (ta * cov + ba * (32 - cov)) >> 5;
            cbuf[pa] = tr | (tg << 8) | (tb << 16) | (ta << 24);
        }
    }
}

static void clear_buffers(void) {
    u32 clearz = ((r_clear2 & 0x7FFF) * 0x200) + 0x1FF;
    u32 polyid = r_clear1 & 0x3F000000;
    for (int x = 0; x < SW; x++) { cbuf[x] = 0; zbuf[x] = clearz; abuf[x] = polyid; }
    for (int x = SW; x < SW * 193; x += SW) {
        cbuf[x] = 0; zbuf[x] = clearz; abuf[x] = polyid;
        cbuf[x + 257] = 0; zbuf[x + 257] = clearz; abuf[x + 257] = polyid;
    }
    for (int x = SW * 193; x < SW * 194; x++) { cbuf[x] = 0; zbuf[x] = clearz; abuf[x] = polyid; }
    if (r_disp & (1 << 14)) {                                       /* clear image from texture slots 2 and 3 */
        u8 xoff = (r_clear2 >> 16) & 0xFF, yoff = (r_clear2 >> 24) & 0xFF;
        for (int y = 0; y < 192; y++, yoff++) {
            u8 xo = xoff;
            for (int x = 0; x < 256; x++, xo++) {
                u16 v2 = tex16(0x40000 + ((u32)yoff << 9) + ((u32)xo << 1));
                u16 v3 = tex16(0x60000 + ((u32)yoff << 9) + ((u32)xo << 1));
                u32 r, g, b; expand5(v2, &r, &g, &b);
                u32 pa = FIRSTPX + (u32)y * SW + (u32)x;
                cbuf[pa] = r | (g << 8) | (b << 16) | ((v2 & 0x8000) ? 0x1F000000u : 0);
                zbuf[pa] = ((u32)(v3 & 0x7FFF) * 0x200) + 0x1FF;
                abuf[pa] = polyid | (v3 & 0x8000);
            }
        }
    } else {
        u32 r, g, b; expand5(r_clear1, &r, &g, &b);
        u32 color = r | (g << 8) | (b << 16) | (((r_clear1 >> 16) & 0x1F) << 24);
        polyid |= r_clear1 & 0x8000;
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++) {
                u32 pa = FIRSTPX + (u32)y * SW + (u32)x;
                cbuf[pa] = color; zbuf[pa] = clearz; abuf[pa] = polyid;
            }
    }
}

double g_t3d_ms;
double g_t3d_part[4];
static void render_frame(void) {
    double t0 = ms_now();
    clear_buffers();
    int j = 0;
    for (u32 i = 0; i < render_n; i++) {
        if (render_list[i]->degenerate || render_list[i]->hidden) continue;
        setup_polygon(&rpolys[j++], render_list[i]);
    }
    prev_shadowmask = 0;
    render_scanline(0, j);
    for (s32 y = 1; y < 192; y++) { render_scanline(y, j); final_pass(y - 1); }
    final_pass(191);
    g_3d_have_frame = 1;
    g_t3d_part[1] += ms_now() - t0;
}

/* ================================================================ frame flow */
const Renderer3D* g_renderer3d;
int g_renderer3d_verify;
static int g_render_pending;                                        /* a render is due at line 215 */

static int ysort_cmp(const void* a, const void* b) {
    const HPoly* x = *(HPoly* const*)a; const HPoly* y = *(HPoly* const*)b;
    if (x->sortkey != y->sortkey) return x->sortkey < y->sortkey ? -1 : 1;
    return x < y ? -1 : x > y ? 1 : 0;                              /* stable: keep submission order */
}

/* GPU renderer: the render list as screen-space polygons with float attributes. */
static Poly g_gl_polys[POLY_MAX];
static const Poly* g_gl_order[POLY_MAX];
static void gl_frame(void) {
    int n = 0, nop = 0;
    HPoly** list = render_list; u32 count = render_n;
    if (w_render_side > 0) { list = wrender_list; count = wrender_n; }
    for (u32 i = 0; i < count; i++) {
        HPoly* h = list[i];
        if (h->degenerate || h->hidden) continue;
        Poly* q = &g_gl_polys[n];
        q->n = (int)h->n; q->attr = h->attr; q->teximg = h->texparam; q->pltt = h->texpal; q->translucent = h->translucent;
        q->ymin = 1e9f; q->ymax = -1e9f;
        for (u32 k = 0; k < h->n; k++) {
            const Vtx* v = h->v[k];
            SVtx* o = &q->v[k];
            o->x = v->hpos[0] / 16.0f; o->y = v->hpos[1] / 16.0f;
            o->zn = h->wbuf ? (float)h->z[k] / 16777215.0f : (float)h->z[k] / 16777215.0f;
            o->iw = v->pos[3] ? 4096.0f / (float)v->pos[3] : 1e6f;
            o->r = v->fcol[0] / 16.0f; o->g = v->fcol[1] / 16.0f; o->b = v->fcol[2] / 16.0f;
            o->s = v->tex[0] / 16.0f; o->t = v->tex[1] / 16.0f;
            if (o->y < q->ymin) q->ymin = o->y;
            if (o->y > q->ymax) q->ymax = o->y;
        }
        if (!h->translucent) nop++;
        g_gl_order[n] = q;
        n++;
    }
    Frame3D f;
    f.order = g_gl_order; f.n = n; f.n_opaque = nop;
    f.wbuf = count ? list[0]->wbuf : 0;
    f.width = 256 + 2 * (w_render_side > 0 ? w_render_side : 0);
    g_renderer3d->render(&f);
    g_3d_have_frame = 1;
}

/* VBlank (scanline 192): latch the rendering registers; a pending FLUSH swaps
 * the polygon RAM banks and makes the new list the one to render. */
void gpu3d_vblank(void) {
    if (gx_irq_line()) irq_raise(&g_c9, IRQ_GXFIFO);
    if (g_renderer3d) g_renderer3d->vblank();
    int new_list = 0;
    if (flush_request) {
        u32 io = 0, it = n_opaque;
        for (u32 i = 0; i < n_poly; i++) { HPoly* p = &cur_pram[i]; if (p->translucent) render_list[it++] = p; else render_list[io++] = p; }
        u32 nsort = (flush_attr & 1) ? n_opaque : n_poly;
        qsort(render_list, nsort, sizeof render_list[0], ysort_cmp);
        render_n = n_poly;
        new_list = 1;
        { static int st = -1; if (st < 0) st = getenv("DQIX_TXSTAT") != NULL;   /* HD text debugging: textures with letters, the polygon under a pixel */
          { static int px = -2, py, pf; if (px == -2) { const char* e = getenv("DQIX_POLYAT"); px = -1; if (e) sscanf(e, "%d,%d,%d", &pf, &px, &py); }
            if (px >= 0 && (int)g_frame >= pf - 3 && (int)g_frame <= pf) for (u32 i = 0; i < n_poly; i++) { HPoly* q = render_list[i]; s32 x0 = 9999, x1 = -9999, y0 = 9999, y1 = -9999;
                for (u32 k = 0; k < q->n; k++) { s32 X = q->v[k]->fpos[0], Y = q->v[k]->fpos[1]; if (X < x0) x0 = X; if (X > x1) x1 = X; if (Y < y0) y0 = Y; if (Y > y1) y1 = Y; }
                if (px >= x0 && px <= x1 && py >= y0 && py <= y1) fprintf(stderr, "POLYAT f%d poly %u tex %08x pal %08x box %d,%d-%d,%d attr %08x\n", (int)g_frame, i, q->texparam, q->texpal, x0, y0, x1, y1, q->attr);
                  if (px >= x0 && px <= x1 && py >= y0 && py <= y1) { u32 a = (q->texparam & 0xFFFF) << 3; uint8_t* sl = g_tex_slot[(a >> 17) & 3];
                    if (sl) { fprintf(stderr, "  vram off %06x:", (unsigned)(sl + (a & 0x1FFFF) - g_vram)); for (int k = 0; k < 32; k++) fprintf(stderr, " %02x", sl[(a & 0x1FFFF) + k]); fprintf(stderr, "\n"); } } } }
          if (st) { int tx_tex_tagged(uint32_t), tx_tex_untagged(uint32_t); for (u32 i = 0; i < n_poly; i++) { int k = tx_tex_tagged(render_list[i]->texparam);
              if (k) fprintf(stderr, "[f%llu] poly %u tex %08x pal %08x: %d tagged texels, %d untagged\n", (unsigned long long)g_frame, i, render_list[i]->texparam, render_list[i]->texpal, k, tx_tex_untagged(render_list[i]->texparam)); } } }
        /* the widescreen list, if the whole frame was made with it */
        /* (a frame without polygons - the black of a scene change - keeps the
         * current width: switching to 256 and back rebuilt the renderer's
         * buffers, leaving one frame without 3D: the backdrop's magenta) */
        w_render_side = w_frame_side > 0 && g_wide_side == w_frame_side ? w_frame_side : w_frame_side == -1 ? g_wide_side : 0;
        if (w_render_side) {
            u32 wo = 0, wt = n_wopaque;
            for (u32 i = 0; i < n_wpoly; i++) { HPoly* p = &wcur_pram[i]; if (p->translucent) wrender_list[wt++] = p; else wrender_list[wo++] = p; }
            qsort(wrender_list, (flush_attr & 1) ? n_wopaque : n_wpoly, sizeof wrender_list[0], ysort_cmp);
            wrender_n = n_wpoly;
        }
        static int lg = -1; if (lg < 0) lg = getenv("DQIX_SWAPLOG") != NULL;
        if (lg) fprintf(stderr, "swap %llu\n", (unsigned long long)g_frame);
    }
    r_disp = IO16_(0x060);
    r_alpharef = (r_disp & (1 << 2)) ? (IO8_(0x340) & 0x1F) : 0;
    memcpy(r_edge, &g_io[0x330], 16);
    memcpy(r_toon, &g_io[0x380], 64);
    r_fogcolor = IO32_(0x358);
    r_fogoffset = (u32)(IO16_(0x35C) & 0x7FFF) * 0x200;
    r_fogshift = (r_disp >> 8) & 0xF;
    for (int i = 0; i < 32; i++) r_fogtab[i + 1] = g_io[0x360 + i] & 0x7F;
    r_fogtab[0] = r_fogtab[1]; r_fogtab[33] = r_fogtab[32];
    r_clear1 = IO32_(0x350); r_clear2 = IO32_(0x354);
    if (flush_request) {
        n_tagtex = 0;                                               /* tagged textures: per frame */
        wbank ^= 1;
        wcur_pram = &wpram[wbank ? WPOLY_MAX : 0]; wcur_vram = &wvram[wbank ? WVTX_MAX : 0];
        n_wpoly = n_wvtx = n_wopaque = 0; w_frame_side = -1;
        cur_bank ^= 1;
        cur_vram = &vram_bank_v[cur_bank ? VTX_MAX : 0];
        cur_pram = &pram[cur_bank ? POLY_MAX : 0];
        n_vtx = 0; n_poly = 0; n_opaque = 0;
        flush_request = 0;
    }
    if (g_renderer3d && new_list) gl_frame();
    g_render_pending = 1;
}

/* Scanline 215: draw the frame (shown during the next frame). */
void gpu3d_render_start(void) {
    if (!g_render_pending) return;
    g_render_pending = 0;
    if (g_renderer3d && !g_renderer3d_verify) return;
    double t0 = ms_now();
    render_frame();
    g_t3d_ms += ms_now() - t0;
}

/* ---------------------------------------------------------------- output to the 2D engine */
extern u32 g_3d_line32[256];                                         /* 6-bit colour | alpha << 24 (0 = clear) */
extern int g_3d_present;

void gpu3d_render_line(int line) {
    if (!g_3d_have_frame || g_renderer3d) { g_3d_present = 0; return; }
    g_3d_present = 1;
    const u32* raw = &cbuf[FIRSTPX + (u32)line * SW];
    u32 xpos = IO16_(0x010) & 0x1FF;                                 /* BG0HOFS scrolls the 3D layer */
    if (xpos == 0) { memcpy(g_3d_line32, raw, 256 * 4); return; }
    if (xpos & 0x100) {
        int i = 0, j = (int)xpos;
        for (; j < 512; i++, j++) g_3d_line32[i] = 0;
        for (j = 0; i < 256; i++, j++) g_3d_line32[i] = raw[j];
    } else {
        int i = 0, j = (int)xpos;
        for (; j < 256; i++, j++) g_3d_line32[i] = raw[j];
        for (; i < 256; i++) g_3d_line32[i] = 0;
    }
}

/* Software renderer output (tests, GPU-renderer verification). */
const u32* gpu3d_soft_line(int line) { return &cbuf[FIRSTPX + (u32)line * SW]; }

/* ---------------------------------------------------------------- texture helpers for the GPU renderer */
int gpu3d_tex_size(u32 ti, int* w, int* h) {
    *w = 8 << ((ti >> 20) & 7); *h = 8 << ((ti >> 23) & 7);
    return ((ti >> 26) & 7) != 0;
}

void gpu3d_decode_texture(u32 ti, u32 pl, uint8_t* rgba) {
    int w, h; gpu3d_tex_size(ti, &w, &h);
    u32 tp = ti & ~(0xFu << 16);                                    /* no repeat/flip: texels are inside anyway */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u16 c; u8 a;
            texture_lookup(tp, pl, x << 4, y << 4, &c, &a);
            uint8_t* o = rgba + (y * w + x) * 4;
            int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            o[0] = (uint8_t)(r << 3 | r >> 2); o[1] = (uint8_t)(g << 3 | g >> 2); o[2] = (uint8_t)(b << 3 | b >> 2);
            o[3] = (uint8_t)(a << 3 | a >> 2);
        }
}

/* Hash of everything a texture is made from: its texels, the 4x4 format's
 * index data, and its palette (with the current VRAM slot mapping). */
static u64 hash_range(u64 h, u32 addr, u32 len, int pal) {
    while (len) {
        uint8_t* base = pal ? g_texpal_slot[(addr >> 14) < 6 ? addr >> 14 : 0] : g_tex_slot[(addr >> 17) & 3];
        u32 span = pal ? 0x4000 - (addr & 0x3FFF) : 0x20000 - (addr & 0x1FFFF);
        if (span > len) span = len;
        h = (h ^ (u64)(uintptr_t)base) * 0x100000001B3ull;
        if (base) {
            const uint8_t* q = base + (pal ? (addr & 0x3FFF) : (addr & 0x1FFFF));
            u32 i = 0;
            for (; i + 8 <= span; i += 8) { u64 v; memcpy(&v, q + i, 8); h = (h ^ v) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
            for (; i < span; i++) h = (h ^ q[i]) * 0x100000001B3ull;
        }
        addr += span; len -= span;
    }
    return h;
}

u64 gpu3d_texture_hash(u32 ti, u32 pl) {
    int w, h; gpu3d_tex_size(ti, &w, &h);
    int fmt = (ti >> 26) & 7;
    u32 base = (ti & 0xFFFF) * 8, n = (u32)(w * h);
    u64 hs = 0xCBF29CE484222325ull ^ ((u64)(ti & 0x3FF0FFFFu) << 16) ^ pl;
    static const u32 bits[8] = { 0, 8, 2, 4, 8, 2, 8, 16 };
    hs = hash_range(hs, base, n * bits[fmt] / 8, 0);
    u32 pb = pl * 16;
    switch (fmt) {
    case 1: hs = hash_range(hs, pb, 64, 1); break;
    case 2: hs = hash_range(hs, pl * 8, 8, 1); break;
    case 3: hs = hash_range(hs, pb, 32, 1); break;
    case 4: hs = hash_range(hs, pb, 512, 1); break;
    case 6: hs = hash_range(hs, pb, 16, 1); break;
    case 5: {
        u32 ia = 0x20000 + ((base & 0x1FFFF) >> 1) + (base >= 0x40000 ? 0x10000 : 0);
        hs = hash_range(hs, ia, n / 8, 0);
        u32 maxoff = 0;
        for (u32 i = 0; i < n / 16; i++) { u32 o = (u32)(tex16(ia + i * 2) & 0x3FFF) * 4; if (o > maxoff) maxoff = o; }
        hs = hash_range(hs, pb, maxoff + 8, 1);
        break;
    }
    default: break;
    }
    return hs;
}

void gpu3d_init(void) {
    mtx_identity(m_proj); mtx_identity(m_pos); mtx_identity(m_vec); mtx_identity(m_tex);
    clip_dirty = 1;
    cur_bank = 0; cur_vram = vram_bank_v; cur_pram = pram;
}
