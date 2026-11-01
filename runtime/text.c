/* Text on the DS screens with a font from the system (stb_truetype, public
 * domain / MIT, Sean Barrett). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "text.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

static stbtt_fontinfo g_font;
static unsigned char* g_data;
static int g_state;                        /* 0 not tried, 1 ok, -1 none */

static int load(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    char paths[10][300];
    int n = 0;
    snprintf(paths[n++], 300, "fonts/small.ttf");          /* the game's own (Nunito, as for HD text): the same everywhere */
    const char* win = getenv("WINDIR");
    if (win) {
        snprintf(paths[n++], 300, "%s\\Fonts\\segoeui.ttf", win);
        snprintf(paths[n++], 300, "%s\\Fonts\\arial.ttf", win);
        snprintf(paths[n++], 300, "%s\\Fonts\\tahoma.ttf", win);
    }
    snprintf(paths[n++], 300, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    snprintf(paths[n++], 300, "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    snprintf(paths[n++], 300, "/System/Library/Fonts/Supplemental/Arial.ttf");
    for (int i = 0; i < n; i++) {
        FILE* f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        g_data = malloc((size_t)sz);
        if (fread(g_data, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(g_data); g_data = NULL; continue; }
        fclose(f);
        if (stbtt_InitFont(&g_font, g_data, stbtt_GetFontOffsetForIndex(g_data, 0))) { g_state = 1; return 1; }
        free(g_data); g_data = NULL;
    }
    return 0;
}

int text_ready(void) { return load(); }

static int utf8_next(const char** s) {
    const unsigned char* p = (const unsigned char*)*s;
    int c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (*p++ & 0x3F); }
    else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    *s = (const char*)p;
    return c;
}

static int measure(float scale, const char* s, const char* end) {
    float x = 0; int prev = 0;
    while (s < end && *s) {
        int c = utf8_next(&s);
        int adv, lsb; stbtt_GetCodepointHMetrics(&g_font, c, &adv, &lsb);
        if (prev) x += scale * (float)stbtt_GetCodepointKernAdvance(&g_font, prev, c);
        x += scale * (float)adv; prev = c;
    }
    return (int)(x + 0.5f);
}

static int draw_n(uint32_t* buf, int w, int h, int x0, int y0, float px, uint32_t color, const char* s, const char* end) {
    if (!load()) return 0;
    float scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int asc, desc, gap; stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    int base = y0 + (int)(asc * scale + 0.5f);
    float x = (float)x0; int prev = 0;
    while (s < end && *s) {
        int c = utf8_next(&s);
        if (prev) x += scale * (float)stbtt_GetCodepointKernAdvance(&g_font, prev, c);
        int adv, lsb; stbtt_GetCodepointHMetrics(&g_font, c, &adv, &lsb);
        int bw = 0, bh = 0, bx = 0, by = 0;
        unsigned char* bm = stbtt_GetCodepointBitmapSubpixel(&g_font, scale, scale, x - (float)(int)x, 0, c, &bw, &bh, &bx, &by);
        for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++) {
            int X = (int)x + bx + i, Y = base + by + j;
            if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
            unsigned a = bm[j * bw + i];
            if (!a) continue;
            uint32_t d = buf[Y * w + X];
            uint32_t r = (((color >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
            uint32_t g = (((color >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
            uint32_t b = ((color & 255) * a + (d & 255) * (255 - a)) / 255;
            buf[Y * w + X] = r << 16 | g << 8 | b;
        }
        stbtt_FreeBitmap(bm, NULL);
        x += scale * (float)adv; prev = c;
    }
    return (int)x - x0;
}

int text_width(float px, const char* s) {
    if (!load()) return 0;
    return measure(stbtt_ScaleForPixelHeight(&g_font, px), s, s + strlen(s));
}

int text_draw(uint32_t* buf, int w, int h, int x, int y, float px, uint32_t color, const char* s) {
    return draw_n(buf, w, h, x, y, px, color, s, s + strlen(s));
}

int text_wrap(uint32_t* buf, int w, int h, int x, int y, int max_w, float px, uint32_t color, const char* s) {
    if (!load()) return y;
    float scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int line = (int)(px * 1.15f + 0.5f);
    while (*s) {
        if (*s == '\n') { s++; y += line; continue; }
        /* the longest run of whole words that fits */
        const char* end = s; const char* fit = NULL;
        for (;;) {
            const char* e = end;
            while (*e && *e != ' ' && *e != '\n') e++;
            if (measure(scale, s, e) > max_w && fit) break;
            fit = e;
            if (!*e || *e == '\n') break;
            end = e + 1;
        }
        draw_n(buf, w, h, x, y, px, color, s, fit);
        y += line;
        s = fit;
        if (*s == '\n') s++;
        while (*s == ' ') s++;
    }
    return y;
}
