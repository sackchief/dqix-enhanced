/* The in-game menu: every option of the menu bar (menu_items.c) in a list drawn
 * over the game, for a controller, a keyboard, a mouse or a touch screen. It
 * needs nothing of Windows, so it is the menu on Linux, the Steam Deck and
 * handhelds, and the controller's menu everywhere.
 *
 * Esc (or the controller's Home / Guide, or Select + Start together) opens
 * it; up / down choose, A or Enter picks (a submenu opens, a setting
 * changes), B or Esc goes back, and from the top it closes. The game waits
 * while it is open (not while playing with friends: there the game goes on,
 * without your buttons). */
#include "menu_ingame.h"
#include "menu.h"
#include "text.h"
#include "controls.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int kind;                 /* MK_SUB, MK_ITEM, MK_SEP; or ROW_RESUME */
    int id;                   /* the command (items) */
    int sub;                  /* a submenu: its MK_SUB entry's index */
    char label[160];
    const char* key;
} Row;
enum { ROW_RESUME = 100 };

static int g_open;
static int g_path[8], g_depth;            /* the submenus opened: their MK_SUB index (g_depth 0: the top) */
static int g_sel[9], g_top[9];            /* the chosen row and the first row shown, at each depth */
static Row g_rows[128]; static int g_nrows;
static const char* const* g_shells; static int g_nshells;
static int g_dirty = 1;
static unsigned g_gen_drawn;
static int g_hover_rows[128][2], g_panel[4];   /* where each row was drawn (y0, y1), the panel (x, y, w, h): for the mouse */
static int g_title_rows;

void ingame_menu_set_shells(const char* const* names, int n) { g_shells = names; g_nshells = n; }
int ingame_menu_is_open(void) { return g_open; }

static void build_rows(void) {
    g_nrows = 0;
    int start = g_depth ? g_path[g_depth - 1] + 1 : 0;
    if (!g_depth) { Row* r = &g_rows[g_nrows++]; memset(r, 0, sizeof *r); r->kind = ROW_RESUME; snprintf(r->label, sizeof r->label, "Back to the game"); }
    int level = 0;
    for (int i = start; g_menu_items[i].kind != MK_STOP && g_nrows < 127; i++) {
        const MenuItem* it = &g_menu_items[i];
        if (it->kind == MK_END) { if (level == 0) break; level--; continue; }
        if (level > 0) { if (it->kind == MK_SUB) level++; continue; }     /* inside a deeper submenu */
#ifndef _WIN32
        if (it->flags & MF_WINDOWS) continue;
#endif
        if (it->kind == MK_SHELLS) {
            for (int k = 0; k < g_nshells && g_nrows < 127; k++) {
                Row* r = &g_rows[g_nrows++]; memset(r, 0, sizeof *r);
                r->kind = MK_ITEM; r->id = it->id + k; snprintf(r->label, sizeof r->label, "%s", g_shells[k]);
            }
            continue;
        }
        Row* r = &g_rows[g_nrows++]; memset(r, 0, sizeof *r);
        r->kind = it->kind; r->id = it->id; r->sub = i; r->key = it->key;
        if (it->label) menu_plain_label(it->label, r->label, sizeof r->label);
        if (it->kind == MK_SUB) level++;
        if (it->kind == MK_SEP && (g_nrows == 1 || g_rows[g_nrows - 2].kind == MK_SEP)) g_nrows--;   /* (no separator first or twice) */
    }
    while (g_nrows && g_rows[g_nrows - 1].kind == MK_SEP) g_nrows--;
}

static int selectable(const Row* r) { return r->kind != MK_SEP && (r->kind != MK_ITEM || menu_is_enabled(r->id)); }

static void fix_sel(int dir) {
    int* s = &g_sel[g_depth];
    if (g_nrows == 0) { *s = 0; return; }
    if (*s < 0) *s = 0;
    if (*s >= g_nrows) *s = g_nrows - 1;
    for (int n = 0; n < g_nrows && !selectable(&g_rows[*s]); n++) *s = (*s + (dir < 0 ? g_nrows - 1 : 1)) % g_nrows;
}

void ingame_menu_open(void) {
    if (g_open) return;
    g_open = 1; g_depth = 0; g_sel[0] = 0; g_top[0] = 0;
    build_rows(); fix_sel(1); g_dirty = 1;
}
void ingame_menu_close(void) { g_open = 0; g_dirty = 1; }

static void move(int dir) {
    if (!g_nrows) return;
    int s = g_sel[g_depth];
    for (int n = 0; n < g_nrows; n++) {
        s = (s + (dir < 0 ? g_nrows - 1 : 1)) % g_nrows;
        if (selectable(&g_rows[s])) break;
    }
    g_sel[g_depth] = s; g_dirty = 1;
}
static void back(void) {
    if (g_depth == 0) { ingame_menu_close(); return; }
    g_depth--; build_rows(); fix_sel(1); g_dirty = 1;
}
/* A or Enter: a submenu opens; an item gives its command (*action) */
static void choose(int* action) {
    if (!g_nrows) return;
    Row* r = &g_rows[g_sel[g_depth]];
    if (!selectable(r)) return;
    if (r->kind == ROW_RESUME) { ingame_menu_close(); return; }
    if (r->kind == MK_SUB && g_depth < 8) {
        g_path[g_depth++] = r->sub; g_sel[g_depth] = 0; g_top[g_depth] = 0;
        build_rows(); fix_sel(1); g_dirty = 1;
        return;
    }
    if (r->kind == MK_ITEM) {
        *action = r->id; g_dirty = 1;
        /* things that leave the game or open a window of their own: the menu closes first */
        if (r->id == MI_EXIT || r->id == MI_RESET || r->id == MI_SES_HOST || r->id == MI_SES_JOIN || r->id == MI_SEND_REPORT ||
            r->id == MI_REPORT || r->id == MI_SHOT || r->id == MI_UPDATE) ingame_menu_close();
    }
}

int ingame_menu_key(SDL_Scancode k, int* action) {
    if (!g_open) return 0;
    if (k == SDL_SCANCODE_UP) move(-1);
    else if (k == SDL_SCANCODE_DOWN) move(1);
    else if (k == SDL_SCANCODE_PAGEUP) for (int i = 0; i < 6; i++) move(-1);
    else if (k == SDL_SCANCODE_PAGEDOWN) for (int i = 0; i < 6; i++) move(1);
    else if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_SPACE || k == SDL_SCANCODE_RIGHT ||
             (g_ctl_key[CA_A][0] && (int)k == (g_ctl_key[CA_A][0] & 0xFFFF))) {
        if (k == SDL_SCANCODE_RIGHT && g_nrows && g_rows[g_sel[g_depth]].kind != MK_SUB) return 1;   /* right: only into a submenu */
        choose(action);
    }
    else if (k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_BACKSPACE || k == SDL_SCANCODE_LEFT ||
             (g_ctl_key[CA_B][0] && (int)k == (g_ctl_key[CA_B][0] & 0xFFFF))) {
        if (k == SDL_SCANCODE_LEFT && g_depth == 0) return 1;           /* left: not out of the menu */
        back();
    }
    return 1;
}

/* the controller, every frame while open: the D-pad and the left stick repeat while held */
void ingame_menu_pad(SDL_GameController* p, int* action, uint32_t now_ms) {
    if (!g_open || !p) return;
    static int was_a = 1, was_b = 1, was_menu = 1, held_dir; static uint32_t next;
    int a = g_ctl_pad[CA_A] >= 0 && controls_pad_down(p, g_ctl_pad[CA_A]);
    int b = g_ctl_pad[CA_B] >= 0 && controls_pad_down(p, g_ctl_pad[CA_B]);
    int m = g_ctl_pad[CA_MENU] >= 0 && controls_pad_down(p, g_ctl_pad[CA_MENU]);
    int y = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
    int dir = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -20000 ? -1 :
              SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 20000 ? 1 : 0;
    int right = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT), left = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    static int was_r = 1, was_l = 1;
    if (dir != held_dir) { held_dir = dir; if (dir) { move(dir); next = now_ms + 380; } }
    else if (dir && (int32_t)(now_ms - next) >= 0) { move(dir); next = now_ms + 90; }
    if (a && !was_a) choose(action);
    else if (b && !was_b) back();
    else if (m && !was_menu) ingame_menu_close();
    else if (right && !was_r && g_nrows && g_rows[g_sel[g_depth]].kind == MK_SUB) choose(action);
    else if (left && !was_l && g_depth) back();
    was_a = a; was_b = b; was_menu = m; was_r = right; was_l = left;
    if (!g_open) { was_a = was_b = was_menu = was_r = was_l = 1; held_dir = 0; }   /* (the next opening waits for them to be let go) */
}

/* the mouse (window pixels): pointing chooses the row, a click picks it, the right button goes back */
int ingame_menu_mouse(const SDL_Event* e, int* action) {
    if (!g_open) return 0;
    if (e->type == SDL_MOUSEWHEEL) { int n = e->wheel.y > 0 ? -1 : 1; for (int i = 0; i < 3; i++) move(n); return 1; }
    int x = e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x, y = e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y;
    int row = -1;
    for (int i = 0; i < g_nrows; i++) if (g_hover_rows[i][1] > g_hover_rows[i][0] && y >= g_hover_rows[i][0] && y < g_hover_rows[i][1] &&
                                          x >= g_panel[0] && x < g_panel[0] + g_panel[2]) row = i;
    if (e->type == SDL_MOUSEMOTION) { if (row >= 0 && selectable(&g_rows[row]) && g_sel[g_depth] != row) { g_sel[g_depth] = row; g_dirty = 1; } return 1; }
    if (e->type == SDL_MOUSEBUTTONDOWN) {
        if (e->button.button == SDL_BUTTON_RIGHT) { back(); return 1; }
        if (e->button.button != SDL_BUTTON_LEFT) return 1;
        int in = x >= g_panel[0] && x < g_panel[0] + g_panel[2] && y >= g_panel[1] && y < g_panel[1] + g_panel[3];
        if (!in) { ingame_menu_close(); return 1; }                       /* a click beside it: back to the game */
        if (row >= 0 && selectable(&g_rows[row])) { g_sel[g_depth] = row; choose(action); }
        else if (y < g_panel[1] + g_title_rows && g_depth) back();        /* the title: up one level */
        return 1;
    }
    return e->type == SDL_MOUSEBUTTONUP;
}

/* ---------------------------------------------------------------- drawing */
static uint32_t* g_rgb; static uint8_t* g_alpha; static uint8_t* g_out; static int g_w, g_h;

static void fill(int x, int y, int w, int h, uint32_t c, int a) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < g_h; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < g_w; i++) { g_rgb[j * g_w + i] = c; g_alpha[j * g_w + i] = (uint8_t)a; }
}
static void frame(int x, int y, int w, int h, int t, uint32_t c) {
    fill(x, y, w, t, c, 255); fill(x, y + h - t, w, t, c, 255); fill(x, y, t, h, c, 255); fill(x + w - t, y, t, h, c, 255);
}
static void tri_right(int x, int y, int s, uint32_t c) {           /* a small pointer, pointing right */
    for (int j = 0; j < s; j++) { int half = j < s / 2 ? j : s - 1 - j; fill(x, y + j, half + 1, 1, c, 255); }
}
static void tri_left(int x, int y, int s, uint32_t c) {            /* ...and pointing left ("back") */
    for (int j = 0; j < s; j++) { int half = j < s / 2 ? j : s - 1 - j; fill(x + s / 2 - half, y + j, half + 1, 1, c, 255); }
}
static void tick(int x, int y, int s, uint32_t c) {                 /* a check mark, drawn */
    int t = s / 7 + 1;
    for (int i = 0; i <= s / 3; i++) fill(x + i, y + s / 2 + i, t, t, c, 255);
    for (int i = 0; i <= s * 2 / 3; i++) fill(x + s / 3 + i, y + s / 2 + s / 3 - i, t, t, c, 255);
}
/* text that fits in max_w (cut with "..." if not) */
static void text_fit(int x, int y, int max_w, float px, uint32_t c, const char* s) {
    if (text_width(px, s) <= max_w) { text_draw(g_rgb, g_w, g_h, x, y, px, c, s); return; }
    char t[200]; size_t n = strlen(s); if (n > 190) n = 190;
    while (n > 0) {
        memcpy(t, s, n); strcpy(t + n, "...");
        if (text_width(px, t) <= max_w) break;
        n--; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   /* (whole letters) */
    }
    text_draw(g_rgb, g_w, g_h, x, y, px, c, t);
}

/* The menu's layer for a window of w x h pixels: RGBA, rows top-down; NULL
 * when nothing changed since the last call (*w and *h stay as given). */
const uint8_t* ingame_menu_draw(int w, int h, int force) {
    if (!g_open || w <= 0 || h <= 0) return NULL;
    if (!force && !g_dirty && w == g_w && h == g_h && g_gen_drawn == menu_generation()) return NULL;
    if (w != g_w || h != g_h || !g_rgb) {
        free(g_rgb); free(g_alpha); free(g_out);
        g_rgb = malloc((size_t)w * h * 4); g_alpha = malloc((size_t)w * h); g_out = malloc((size_t)w * h * 4);
        if (!g_rgb || !g_alpha || !g_out) { g_w = g_h = 0; return NULL; }
        g_w = w; g_h = h;
    }
    g_dirty = 0; g_gen_drawn = menu_generation();
    memset(g_rgb, 0, (size_t)w * h * 4);
    memset(g_alpha, 110, (size_t)w * h);                               /* the game, dimmed behind it */

    float px = h * 0.034f; if (px < 15) px = 15; if (px > 42) px = 42;   /* readable on a handheld and a big monitor alike */
    int row_h = (int)(px * 1.55f), pad = (int)(px * 1.2f), line = px > 24 ? 3 : 2;
    int pw = (int)(px * 30); if (pw > w - 2 * (int)px) pw = w - 2 * (int)px;
    int head = (int)(px * 2.1f), foot = (int)(px * 1.9f);
    int max_rows = (h - 2 * (int)px - head - foot - 2 * pad) / row_h; if (max_rows < 3) max_rows = 3;
    int shown = g_nrows < max_rows ? g_nrows : max_rows;
    int ph = head + foot + shown * row_h + pad;
    int x0 = (w - pw) / 2, y0 = (h - ph) / 2;
    g_panel[0] = x0; g_panel[1] = y0; g_panel[2] = pw; g_panel[3] = ph; g_title_rows = head;

    const uint32_t BG = 0x101A3A, SEL = 0x2F4C8C, WHITE = 0xFFFFFF, GREY = 0x8A93AE, DIM = 0xB8C2DC, GOLD = 0xF8D860;
    fill(x0, y0, pw, ph, BG, 235);
    frame(x0 + line, y0 + line, pw - 2 * line, ph - 2 * line, line, WHITE);

    /* the title: where we are */
    char title[200] = "Menu";
    for (int d = 0; d < g_depth; d++) {
        char t[160]; menu_plain_label(g_menu_items[g_path[d]].label, t, sizeof t);
        if (d == g_depth - 1) snprintf(title, sizeof title, "%s", t);
    }
    if (g_depth) tri_left(x0 + pad / 2, y0 + head / 2 - (int)(px * 0.3f), (int)(px * 0.6f), GREY);   /* (a little "back" mark) */
    text_fit(x0 + pad + (g_depth ? (int)(px * 0.4f) : 0), y0 + (head - (int)px) / 2, pw - 2 * pad, px * 1.05f, GOLD, title);
    fill(x0 + pad, y0 + head - line, pw - 2 * pad, line, 0x3A4670, 255);

    /* the rows, scrolled so the chosen one shows */
    int* top = &g_top[g_depth], sel = g_sel[g_depth];
    if (sel < *top) *top = sel;
    if (sel >= *top + shown) *top = sel - shown + 1;
    if (*top > g_nrows - shown) *top = g_nrows - shown;
    if (*top < 0) *top = 0;
    for (int i = 0; i < g_nrows; i++) g_hover_rows[i][0] = g_hover_rows[i][1] = 0;
    int ry = y0 + head + pad / 2;
    for (int i = *top; i < *top + shown; i++, ry += row_h) {
        const Row* r = &g_rows[i];
        g_hover_rows[i][0] = ry; g_hover_rows[i][1] = ry + row_h;
        if (r->kind == MK_SEP) { fill(x0 + pad, ry + row_h / 2, pw - 2 * pad, 1, 0x3A4670, 255); continue; }
        int en = selectable(r);
        if (i == sel) fill(x0 + line * 3, ry, pw - line * 6, row_h, SEL, 255);
        int ty = ry + (row_h - (int)px) / 2;
        int cx = x0 + pad;
        if (i == sel) tri_right(cx - (int)(px * 0.7f), ty + (int)(px * 0.2f), (int)(px * 0.6f), WHITE);   /* the pointer */
        if (r->kind == MK_ITEM && menu_is_checked(r->id)) tick(cx, ty + (int)(px * 0.05f), (int)(px * 0.8f), en ? GOLD : GREY);
        int lx = cx + (int)(px * 1.2f), right = x0 + pw - pad;
        if (r->kind == MK_SUB) { tri_right(right - (int)(px * 0.4f), ty + (int)(px * 0.25f), (int)(px * 0.55f), en ? DIM : GREY); right -= (int)(px * 0.9f); }
        else if (r->key) {
            int kw = text_width(px * 0.8f, r->key);
            text_draw(g_rgb, g_w, g_h, right - kw, ty + (int)(px * 0.12f), px * 0.8f, GREY, r->key);
            right -= kw + (int)(px * 0.8f);
        }
        text_fit(lx, ty, right - lx, px, en ? (r->kind == ROW_RESUME ? GOLD : WHITE) : GREY, r->label);
        for (int y = ry; y < ry + row_h && y < g_h; y++) for (int x = x0 + line * 2; x < x0 + pw - line * 2 && x < g_w; x++)
            if (y >= 0 && x >= 0) g_alpha[y * g_w + x] = 255;          /* (the letters solid) */
    }
    /* more above / below: a small mark at the edge */
    if (*top > 0) fill(x0 + pw - pad, y0 + head + pad / 4, (int)(px * 0.4f), line, GREY, 255);
    if (*top + shown < g_nrows) fill(x0 + pw - pad, y0 + ph - foot - line, (int)(px * 0.4f), line, GREY, 255);

    /* the buttons */
    fill(x0 + pad, y0 + ph - foot, pw - 2 * pad, line, 0x3A4670, 255);
    text_fit(x0 + pad, y0 + ph - foot + (foot - (int)(px * 0.8f)) / 2, pw - 2 * pad, px * 0.8f, GREY,
             g_depth ? "A / Enter: choose     B / Esc: back" : "A / Enter: choose     B / Esc: back to the game");

    for (int i = 0; i < w * h; i++) {
        uint32_t c = g_rgb[i];
        g_out[i * 4] = (uint8_t)(c >> 16); g_out[i * 4 + 1] = (uint8_t)(c >> 8); g_out[i * 4 + 2] = (uint8_t)c; g_out[i * 4 + 3] = g_alpha[i];
    }
    return g_out;
}
