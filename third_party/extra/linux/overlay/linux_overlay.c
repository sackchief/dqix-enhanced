#include "linux_overlay.h"
#include <ctype.h>
#include <string.h>

typedef struct { uint8_t r, g, b, a; } OverlayColor;

static const char *glyph_bits(char c) {
    static const char blank[] = "00000""00000""00000""00000""00000""00000""00000";
    switch (toupper((unsigned char)c)) {
    case 'A': return "01110""10001""10001""11111""10001""10001""10001";
    case 'B': return "11110""10001""10001""11110""10001""10001""11110";
    case 'C': return "01111""10000""10000""10000""10000""10000""01111";
    case 'D': return "11110""10001""10001""10001""10001""10001""11110";
    case 'E': return "11111""10000""10000""11110""10000""10000""11111";
    case 'F': return "11111""10000""10000""11110""10000""10000""10000";
    case 'G': return "01111""10000""10000""10111""10001""10001""01111";
    case 'H': return "10001""10001""10001""11111""10001""10001""10001";
    case 'I': return "11111""00100""00100""00100""00100""00100""11111";
    case 'J': return "00111""00010""00010""00010""10010""10010""01100";
    case 'K': return "10001""10010""10100""11000""10100""10010""10001";
    case 'L': return "10000""10000""10000""10000""10000""10000""11111";
    case 'M': return "10001""11011""10101""10101""10001""10001""10001";
    case 'N': return "10001""11001""10101""10011""10001""10001""10001";
    case 'O': return "01110""10001""10001""10001""10001""10001""01110";
    case 'P': return "11110""10001""10001""11110""10000""10000""10000";
    case 'Q': return "01110""10001""10001""10001""10101""10010""01101";
    case 'R': return "11110""10001""10001""11110""10100""10010""10001";
    case 'S': return "01111""10000""10000""01110""00001""00001""11110";
    case 'T': return "11111""00100""00100""00100""00100""00100""00100";
    case 'U': return "10001""10001""10001""10001""10001""10001""01110";
    case 'V': return "10001""10001""10001""10001""10001""01010""00100";
    case 'W': return "10001""10001""10001""10101""10101""10101""01010";
    case 'X': return "10001""10001""01010""00100""01010""10001""10001";
    case 'Y': return "10001""10001""01010""00100""00100""00100""00100";
    case 'Z': return "11111""00001""00010""00100""01000""10000""11111";
    case '0': return "01110""10001""10011""10101""11001""10001""01110";
    case '1': return "00100""01100""00100""00100""00100""00100""01110";
    case '2': return "01110""10001""00001""00010""00100""01000""11111";
    case '3': return "11110""00001""00001""01110""00001""00001""11110";
    case '4': return "00010""00110""01010""10010""11111""00010""00010";
    case '5': return "11111""10000""10000""11110""00001""00001""11110";
    case '6': return "01110""10000""10000""11110""10001""10001""01110";
    case '7': return "11111""00001""00010""00100""01000""01000""01000";
    case '8': return "01110""10001""10001""01110""10001""10001""01110";
    case '9': return "01110""10001""10001""01111""00001""00001""01110";
    case ':': return "00000""00110""00110""00000""00110""00110""00000";
    case '.': return "00000""00000""00000""00000""00000""00110""00110";
    case '-': return "00000""00000""00000""11111""00000""00000""00000";
    case '/': return "00001""00010""00010""00100""01000""01000""10000";
    case '>': return "10000""01000""00100""00010""00100""01000""10000";
    case '!': return "00100""00100""00100""00100""00100""00000""00100";
    case '?': return "01110""10001""00001""00010""00100""00000""00100";
    case '+': return "00000""00100""00100""11111""00100""00100""00000";
    case '_': return "00000""00000""00000""00000""00000""00000""11111";
    case '(': return "00010""00100""01000""01000""01000""00100""00010";
    case ')': return "01000""00100""00010""00010""00010""00100""01000";
    case ' ': return blank;
    default: return blank;
    }
}

int dqix_linux_overlay_fit_rect(int viewport_width, int viewport_height,
    int *x, int *y, int *width, int *height) {
    int fit_width, fit_height;
    if (!x || !y || !width || !height || viewport_width <= 0 || viewport_height <= 0)
        return 0;
    fit_width = viewport_width;
    fit_height = viewport_height;
    if ((int64_t)viewport_width * DQIX_LINUX_OVERLAY_HEIGHT >
        (int64_t)viewport_height * DQIX_LINUX_OVERLAY_WIDTH) {
        fit_width = (int)((int64_t)viewport_height * DQIX_LINUX_OVERLAY_WIDTH /
                          DQIX_LINUX_OVERLAY_HEIGHT);
    } else {
        fit_height = (int)((int64_t)viewport_width * DQIX_LINUX_OVERLAY_HEIGHT /
                           DQIX_LINUX_OVERLAY_WIDTH);
    }
    if (fit_width <= 0 || fit_height <= 0) return 0;
    *x = (viewport_width - fit_width) / 2;
    *y = (viewport_height - fit_height) / 2;
    *width = fit_width;
    *height = fit_height;
    return 1;
}

static void fill(uint8_t *p, int x, int y, int w, int h, OverlayColor c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DQIX_LINUX_OVERLAY_WIDTH) w = DQIX_LINUX_OVERLAY_WIDTH - x;
    if (y + h > DQIX_LINUX_OVERLAY_HEIGHT) h = DQIX_LINUX_OVERLAY_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy) {
        uint8_t *row = p + ((size_t)yy * DQIX_LINUX_OVERLAY_WIDTH + (size_t)x) * 4;
        for (int xx = 0; xx < w; ++xx) {
            row[xx * 4 + 0] = c.r; row[xx * 4 + 1] = c.g;
            row[xx * 4 + 2] = c.b; row[xx * 4 + 3] = c.a;
        }
    }
}

static void text(uint8_t *p, int x, int y, const char *s, int scale, int max_chars, OverlayColor c) {
    int n = 0;
    if (!s || scale < 1) return;
    for (; *s && n < max_chars; ++s, ++n) {
        const char *bits = glyph_bits(*s);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (bits[row * 5 + col] == '1')
                    fill(p, x + col * scale, y + row * scale, scale, scale, c);
        x += 6 * scale;
        if (x + 5 * scale >= DQIX_LINUX_OVERLAY_WIDTH - 16) break;
    }
}

void dqix_linux_overlay_render_rgba(uint8_t *rgba, size_t capacity,
    int selected, int classic, int fullscreen, int gl_active, const char *renderer_name) {
    const size_t need = (size_t)DQIX_LINUX_OVERLAY_WIDTH * DQIX_LINUX_OVERLAY_HEIGHT * 4;
    if (!rgba || capacity < need) return;
    if (selected < 0) selected = 0;
    if (selected > 3) selected = 3;
    memset(rgba, 0, need);
    fill(rgba, 28, 126, 456, 520, (OverlayColor){12,21,37,246});
    fill(rgba, 28, 126, 456, 5, (OverlayColor){245,190,83,255});
    fill(rgba, 36, 136, 440, 2, (OverlayColor){48,71,98,255});
    text(rgba, 56, 166, "DQIX ENHANCED", 3, 24, (OverlayColor){245,190,83,255});
    text(rgba, 58, 211, "IN-GAME MENU", 2, 32, (OverlayColor){223,231,243,255});
    const char *labels[4] = {
        "RESUME GAME",
        gl_active ? (classic ? "MODE: CLASSIC > ENHANCED" : "MODE: ENHANCED > CLASSIC") : "MODE: OPENGL REQUIRED",
        fullscreen ? "FULLSCREEN: ON" : "FULLSCREEN: OFF",
        "QUIT GAME"
    };
    for (int i = 0; i < 4; ++i) {
        int y = DQIX_LINUX_OVERLAY_ROW0 + i * DQIX_LINUX_OVERLAY_ROW_STEP;
        OverlayColor bg = i == selected ? (OverlayColor){27,91,151,255} : (OverlayColor){25,38,57,245};
        fill(rgba, DQIX_LINUX_OVERLAY_ROW_X, y - 12, DQIX_LINUX_OVERLAY_ROW_WIDTH, DQIX_LINUX_OVERLAY_ROW_HEIGHT, bg);
        fill(rgba, DQIX_LINUX_OVERLAY_ROW_X, y - 12, DQIX_LINUX_OVERLAY_ROW_WIDTH, 2,
             i == selected ? (OverlayColor){245,190,83,255} : (OverlayColor){56,74,97,255});
        text(rgba, 76, y, labels[i], 2, 32, (OverlayColor){244,247,251,255});
    }
    text(rgba, 54, 534, gl_active ? "RENDER PATH: OPENGL" : "RENDER FALLBACK: SDL2", 1, 64,
         gl_active ? (OverlayColor){142,218,180,255} : (OverlayColor){255,190,125,255});
    text(rgba, 54, 553, "RENDERER:", 1, 20, (OverlayColor){144,164,189,255});
    text(rgba, 120, 553, renderer_name && renderer_name[0] ? renderer_name : "UNAVAILABLE", 1, 56,
         (OverlayColor){218,228,241,255});
    text(rgba, 54, 583, "UP DOWN / DPAD MOVE  ENTER / A SELECT", 1, 68, (OverlayColor){171,188,210,255});
    text(rgba, 54, 600, "ESC / B RESUME", 1, 52, (OverlayColor){171,188,210,255});
}
