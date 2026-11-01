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
    int value;                /* changed with left / right (MF_VALUE) */
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

static int (*g_val_get)(int id, char* out, size_t n);
static void (*g_val_adj)(int id, int dir);

/* a message or question inside the menu (instead of a window of its own) */
static struct {
    int on, n, sel, cancel, result, has_check, checked, focus_check;
    char title[128], text[4096], btn[8][64], check[200];
    int btn_rect[8][4], check_rect[4];
} g_dlg;

void ingame_menu_set_shells(const char* const* names, int n) { g_shells = names; g_nshells = n; }
void ingame_menu_set_values(int (*get)(int id, char* out, size_t n), void (*adjust)(int id, int dir)) { g_val_get = get; g_val_adj = adjust; }
static int pages_open(void);
int ingame_menu_is_open(void) { return g_open || g_dlg.on || pages_open(); }

void ingame_dialog_begin(const char* title, const char* text, const char* const* buttons, int n, int def, int cancel, const char* check, int checked) {
    memset(&g_dlg, 0, sizeof g_dlg);
    g_dlg.on = 1; g_dlg.result = -1;
    snprintf(g_dlg.title, sizeof g_dlg.title, "%s", title ? title : "");
    snprintf(g_dlg.text, sizeof g_dlg.text, "%s", text ? text : "");
    if (n > 8) n = 8;
    if (n < 1) { n = 1; snprintf(g_dlg.btn[0], 64, "OK"); }
    else for (int i = 0; i < n; i++) snprintf(g_dlg.btn[i], 64, "%s", buttons[i]);
    g_dlg.n = n; g_dlg.sel = def >= 0 && def < n ? def : 0; g_dlg.cancel = cancel;
    if (check) { g_dlg.has_check = 1; snprintf(g_dlg.check, sizeof g_dlg.check, "%s", check); g_dlg.checked = checked != 0; }
    g_dirty = 1;
}
void ingame_dialog_update(const char* title, const char* text) {     /* new words, the same buttons and choice (a live message) */
    if (!g_dlg.on) return;
    if (strcmp(g_dlg.title, title ? title : "") || strcmp(g_dlg.text, text ? text : "")) {
        snprintf(g_dlg.title, sizeof g_dlg.title, "%s", title ? title : ""); snprintf(g_dlg.text, sizeof g_dlg.text, "%s", text ? text : ""); g_dirty = 1;
    }
}
int ingame_dialog_is_on(void) { return g_dlg.on; }
void ingame_dialog_cancel(void) { if (g_dlg.on) { g_dlg.result = g_dlg.cancel; g_dlg.on = 0; g_dirty = 1; } }
int ingame_dialog_result(int* checked) {
    if (g_dlg.on) return -1;
    if (checked) *checked = g_dlg.checked;
    return g_dlg.result;
}
static void dlg_done(int r) { g_dlg.result = r; g_dlg.on = 0; g_dirty = 1; }
static void dlg_key(int k) {                 /* 'U' 'D' 'L' 'R' 'A' (choose) 'B' (cancel) */
    if (k == 'L' && !g_dlg.focus_check) g_dlg.sel = (g_dlg.sel + g_dlg.n - 1) % g_dlg.n;
    else if (k == 'R' && !g_dlg.focus_check) g_dlg.sel = (g_dlg.sel + 1) % g_dlg.n;
    else if (k == 'U' && g_dlg.has_check) g_dlg.focus_check = 1;
    else if (k == 'D') g_dlg.focus_check = 0;
    else if (k == 'A') { if (g_dlg.focus_check) g_dlg.checked = !g_dlg.checked; else dlg_done(g_dlg.sel); }
    else if (k == 'B') dlg_done(g_dlg.cancel >= 0 ? g_dlg.cancel : g_dlg.n - 1);
    g_dirty = 1;
}

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
        r->kind = it->kind; r->id = it->id; r->sub = i; r->key = it->key; r->value = (it->flags & MF_VALUE) != 0;
        if (it->label) menu_plain_label(it->label, r->label, sizeof r->label);
        if (r->value) { size_t n = strlen(r->label); while (n && r->label[n - 1] == '.') r->label[--n] = 0; r->key = NULL; }   /* "Volume..." -> "Volume" */
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
    if (r->value) { if (g_val_adj) g_val_adj(r->id, 1); g_dirty = 1; return; }   /* A on a value: up one step */
    if (r->kind == MK_ITEM) {
        *action = r->id; g_dirty = 1;
        /* things that leave the game or open a window of their own: the menu closes first */
        if (r->id == MI_EXIT || r->id == MI_RESET || r->id == MI_SES_HOST || r->id == MI_SES_JOIN || r->id == MI_SEND_REPORT ||
            r->id == MI_REPORT || r->id == MI_SHOT) ingame_menu_close();
    }
}

/* ---------------------------------------------------------------- a list to pick from (files, controls) */
static struct {
    int on, n, sel, top, result;
    char title[160], foot[200];
    char (*text)[160]; char (*right)[80];
    int rows[40][2];
} g_lst;
void ingame_list_begin(const char* title, const char* const* items, const char* const* rights, int n, int sel, const char* foot) {
    free(g_lst.text); free(g_lst.right);
    memset(&g_lst, 0, sizeof g_lst);
    if (n < 0) n = 0;
    g_lst.text = calloc((size_t)(n ? n : 1), sizeof *g_lst.text); g_lst.right = calloc((size_t)(n ? n : 1), sizeof *g_lst.right);
    for (int i = 0; i < n; i++) {
        snprintf(g_lst.text[i], sizeof g_lst.text[i], "%s", items[i] ? items[i] : "");
        snprintf(g_lst.right[i], sizeof g_lst.right[i], "%s", rights && rights[i] ? rights[i] : "");
    }
    g_lst.on = 1; g_lst.n = n; g_lst.sel = sel >= 0 && sel < n ? sel : 0; g_lst.result = -2;
    snprintf(g_lst.title, sizeof g_lst.title, "%s", title ? title : "");
    snprintf(g_lst.foot, sizeof g_lst.foot, "%s", foot ? foot : "A / Enter: choose     B / Esc: back");
    g_dirty = 1;
}
int ingame_list_result(void) { return g_lst.on ? -2 : g_lst.result; }   /* -2 open, -1 back, else the row */
static void lst_move(int d) { if (g_lst.n) { g_lst.sel = (g_lst.sel + d + g_lst.n * 8) % g_lst.n; g_dirty = 1; } }
static void lst_done(int r) { g_lst.result = r; g_lst.on = 0; g_dirty = 1; }

/* ---------------------------------------------------------------- typing (a keyboard, or the one on the screen) */
static const char* const OSK[2][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm.,'" },
    { "!@#$%&*()?", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM:;/" },
};
enum { OSK_SHIFT = 10, OSK_SPACE, OSK_DEL, OSK_DONE };           /* the bottom row's keys (column numbers) */
static struct {
    int on, result, max, shift, kx, ky;                          /* kx, ky: the key chosen on the screen's keyboard */
    char title[160], prompt[600], buf[1200];
    int key_rect[5][11][4];
} g_txt;
void ingame_text_begin(const char* title, const char* prompt, const char* initial, int max) {
    memset(&g_txt, 0, sizeof g_txt);
    g_txt.on = 1; g_txt.result = -2; g_txt.max = max > 0 && max < (int)sizeof g_txt.buf ? max : (int)sizeof g_txt.buf - 1;
    snprintf(g_txt.title, sizeof g_txt.title, "%s", title ? title : "");
    snprintf(g_txt.prompt, sizeof g_txt.prompt, "%s", prompt ? prompt : "");
    snprintf(g_txt.buf, sizeof g_txt.buf, "%s", initial ? initial : "");
    g_txt.kx = 0; g_txt.ky = 1;
    g_dirty = 1;
}
int ingame_text_result(char* out, size_t n) {                   /* -2 still typing, 0 cancelled, 1 done */
    if (g_txt.on) return -2;
    if (out && n) snprintf(out, n, "%s", g_txt.buf);
    return g_txt.result;
}
void ingame_text_input(const char* utf8) {                      /* typed on a keyboard (SDL_TEXTINPUT) */
    if (!g_txt.on || !utf8) return;
    size_t l = strlen(g_txt.buf), a = strlen(utf8);
    if ((int)(l + a) > g_txt.max) return;
    memcpy(g_txt.buf + l, utf8, a + 1); g_dirty = 1;
}
static void txt_del(void) {                                     /* the last letter (a whole UTF-8 one) */
    size_t l = strlen(g_txt.buf);
    while (l > 0) { l--; if (((unsigned char)g_txt.buf[l] & 0xC0) != 0x80) break; }
    g_txt.buf[l] = 0; g_dirty = 1;
}
static void txt_press(void) {                                   /* the chosen key of the screen's keyboard */
    if (g_txt.ky < 4) { char c[2] = { OSK[g_txt.shift][g_txt.ky][g_txt.kx], 0 }; ingame_text_input(c); if (g_txt.shift == 1 && g_txt.ky > 0) g_txt.shift = 0; }
    else if (g_txt.kx == 0) g_txt.shift = !g_txt.shift;
    else if (g_txt.kx == 1) ingame_text_input(" ");
    else if (g_txt.kx == 2) txt_del();
    else { g_txt.result = 1; g_txt.on = 0; }
    g_dirty = 1;
}
static void txt_move(int dx, int dy) {
    if (dy) { g_txt.ky = (g_txt.ky + dy + 5) % 5; if (g_txt.ky == 4 && g_txt.kx > 3) g_txt.kx = 3; }
    if (dx) { int w = g_txt.ky == 4 ? 4 : 10; g_txt.kx = (g_txt.kx + dx + w) % w; }
    g_dirty = 1;
}

static int pages_open(void) { return g_lst.on || g_txt.on; }
static int on_value(void) { return g_open && g_nrows && g_rows[g_sel[g_depth]].value; }
int ingame_menu_key(SDL_Scancode k, int* action) {
    if (!g_dlg.on && g_txt.on) {                                     /* typing: the keyboard types (SDL_TEXTINPUT), these edit */
        if (k == SDL_SCANCODE_BACKSPACE) txt_del();
        else if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) { g_txt.result = 1; g_txt.on = 0; g_dirty = 1; }
        else if (k == SDL_SCANCODE_ESCAPE) { g_txt.result = 0; g_txt.on = 0; g_dirty = 1; }
        return 1;
    }
    if (!g_dlg.on && g_lst.on) {
        int isA = g_ctl_key[CA_A][0] && (int)k == (g_ctl_key[CA_A][0] & 0xFFFF), isB = g_ctl_key[CA_B][0] && (int)k == (g_ctl_key[CA_B][0] & 0xFFFF);
        if (k == SDL_SCANCODE_UP) lst_move(-1); else if (k == SDL_SCANCODE_DOWN) lst_move(1);
        else if (k == SDL_SCANCODE_PAGEUP) lst_move(-8); else if (k == SDL_SCANCODE_PAGEDOWN) lst_move(8);
        else if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_SPACE || k == SDL_SCANCODE_RIGHT || isA) { if (g_lst.n) lst_done(g_lst.sel); }
        else if (k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_BACKSPACE || k == SDL_SCANCODE_LEFT || isB) lst_done(-1);
        return 1;
    }
    if (g_dlg.on) {
        int isA = g_ctl_key[CA_A][0] && (int)k == (g_ctl_key[CA_A][0] & 0xFFFF), isB = g_ctl_key[CA_B][0] && (int)k == (g_ctl_key[CA_B][0] & 0xFFFF);
        if (k == SDL_SCANCODE_LEFT) dlg_key('L'); else if (k == SDL_SCANCODE_RIGHT || k == SDL_SCANCODE_TAB) dlg_key('R');
        else if (k == SDL_SCANCODE_UP) dlg_key('U'); else if (k == SDL_SCANCODE_DOWN) dlg_key('D');
        else if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_SPACE || isA) dlg_key('A');
        else if (k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_BACKSPACE || isB) dlg_key('B');
        return 1;
    }
    if (!g_open) return 0;
    if (on_value() && (k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT)) {
        if (g_val_adj) g_val_adj(g_rows[g_sel[g_depth]].id, k == SDL_SCANCODE_RIGHT ? 1 : -1);
        g_dirty = 1; return 1;
    }
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
    if (!(g_open || g_dlg.on) || !p) return;
    static int was_a = 1, was_b = 1, was_menu = 1, held_dir, held_x; static uint32_t next, next_x;
    int a = g_ctl_pad[CA_A] >= 0 && controls_pad_down(p, g_ctl_pad[CA_A]);
    int b = g_ctl_pad[CA_B] >= 0 && controls_pad_down(p, g_ctl_pad[CA_B]);
    int m = g_ctl_pad[CA_MENU] >= 0 && controls_pad_down(p, g_ctl_pad[CA_MENU]);
    int y = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
    int dir = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -20000 ? -1 :
              SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 20000 ? 1 : 0;
    int right = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT), left = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    static int was_r = 1, was_l = 1;
    int x = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
    int xdir = left || x < -20000 ? -1 : right || x > 20000 ? 1 : 0;
    if (!g_dlg.on && (g_txt.on || g_lst.on)) {                        /* typing or a list: the stick and D-pad move, repeating */
        int sx = xdir, sy = dir;
        static int hx, hy; static uint32_t nx, ny;
        int mx = 0, my = 0;
        if (sx != hx) { hx = sx; if (sx) { mx = sx; nx = now_ms + 380; } } else if (sx && (int32_t)(now_ms - nx) >= 0) { mx = sx; nx = now_ms + 110; }
        if (sy != hy) { hy = sy; if (sy) { my = sy; ny = now_ms + 380; } } else if (sy && (int32_t)(now_ms - ny) >= 0) { my = sy; ny = now_ms + 90; }
        int st = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_START), yb = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_Y),
            xb = SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_X);
        static int was_st = 1, was_yb = 1, was_xb = 1;
        if (g_txt.on) {
            if (mx || my) txt_move(mx, my);
            if (a && !was_a) txt_press();
            else if (b && !was_b) txt_del();                         /* B: delete a letter */
            else if (st && !was_st) { g_txt.result = 1; g_txt.on = 0; g_dirty = 1; }   /* Start: done */
            else if (yb && !was_yb) { g_txt.shift = !g_txt.shift; g_dirty = 1; }      /* (top button) capitals */
            else if (xb && !was_xb) ingame_text_input(" ");          /* (left button) a space */
            else if (m && !was_menu) { g_txt.result = 0; g_txt.on = 0; g_dirty = 1; }  /* Home: cancel */
        } else {
            if (my) lst_move(my);
            if (mx) lst_move(mx * 8);
            if (a && !was_a && g_lst.n) lst_done(g_lst.sel);
            else if (b && !was_b) lst_done(-1);
        }
        was_a = a; was_b = b; was_menu = m; was_r = right; was_l = left; was_st = st; was_yb = yb; was_xb = xb;
        held_dir = dir; held_x = xdir;
        return;
    }
    if (g_dlg.on) {                                                  /* a message: left / right between its buttons */
        if (dir != held_dir) { held_dir = dir; if (dir) dlg_key(dir < 0 ? 'U' : 'D'); }
        if (xdir != held_x) { held_x = xdir; if (xdir) dlg_key(xdir < 0 ? 'L' : 'R'); }
        if (a && !was_a) dlg_key('A'); else if (b && !was_b) dlg_key('B');
        was_a = a; was_b = b; was_menu = m; was_r = right; was_l = left;
        if (!g_dlg.on) { was_a = was_b = 1; }
        return;
    }
    if (on_value()) {                                                /* a value: left / right change it, repeating while held */
        if (xdir != held_x) { held_x = xdir; if (xdir && g_val_adj) { g_val_adj(g_rows[g_sel[g_depth]].id, xdir); g_dirty = 1; next_x = now_ms + 380; } }
        else if (xdir && (int32_t)(now_ms - next_x) >= 0 && g_val_adj) { g_val_adj(g_rows[g_sel[g_depth]].id, xdir); g_dirty = 1; next_x = now_ms + 110; }
        right = left = 0;
    } else held_x = xdir;
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
static int in_rect(const int* r, int x, int y) { return r[2] > 0 && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]; }
int ingame_menu_mouse(const SDL_Event* e, int* action) {
    if (!g_dlg.on && (g_txt.on || g_lst.on)) {
        if (e->type == SDL_MOUSEWHEEL) { if (g_lst.on) lst_move(e->wheel.y > 0 ? -3 : 3); return 1; }
        int x = e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x, y = e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y;
        int click = e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT;
        if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_RIGHT) {
            if (g_txt.on) { g_txt.result = 0; g_txt.on = 0; } else lst_done(-1);
            g_dirty = 1; return 1;
        }
        if (g_txt.on) {
            for (int ky = 0; ky < 5; ky++) for (int kx = 0; kx < 10; kx++) if (in_rect(g_txt.key_rect[ky][kx], x, y)) {
                if (g_txt.kx != kx || g_txt.ky != ky) { g_txt.kx = kx; g_txt.ky = ky; g_dirty = 1; }
                if (click) txt_press();
                return 1;
            }
        } else {
            for (int i = 0; i < 40; i++) if (g_lst.rows[i][1] > g_lst.rows[i][0] && y >= g_lst.rows[i][0] && y < g_lst.rows[i][1] &&
                                             x >= g_panel[0] && x < g_panel[0] + g_panel[2]) {
                int r = g_lst.top + i;
                if (r < g_lst.n && g_lst.sel != r) { g_lst.sel = r; g_dirty = 1; }
                if (click && r < g_lst.n) lst_done(r);
                return 1;
            }
        }
        return 1;
    }
    if (g_dlg.on) {
        if (e->type == SDL_MOUSEWHEEL) return 1;
        int x = e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x, y = e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y;
        for (int i = 0; i < g_dlg.n; i++) if (in_rect(g_dlg.btn_rect[i], x, y)) {
            if (e->type == SDL_MOUSEMOTION) { if (g_dlg.sel != i || g_dlg.focus_check) { g_dlg.sel = i; g_dlg.focus_check = 0; g_dirty = 1; } }
            else if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) dlg_done(i);
            return 1;
        }
        if (g_dlg.has_check && in_rect(g_dlg.check_rect, x, y) && e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) { g_dlg.checked = !g_dlg.checked; g_dirty = 1; }
        if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_RIGHT) dlg_key('B');
        return 1;
    }
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

/* text in lines that fit max_w (breaks at spaces and \n); draw 0: only measures. Returns the height. */
static int text_lines(int x, int y, int max_w, float px, uint32_t c, const char* s, int draw) {
    int line = (int)(px * 1.3f), h = 0;
    char buf[400];
    while (*s) {
        if (*s == '\n') { s++; h += line / 2 + (h ? line / 2 : 0); continue; }
        size_t best = 0, i = 0;
        for (;;) {                                                      /* the longest run of whole words that fits */
            size_t j = i; while (s[j] && s[j] != ' ' && s[j] != '\n') j++;
            if (j >= sizeof buf) j = sizeof buf - 1;
            memcpy(buf, s, j); buf[j] = 0;
            if (text_width(px, buf) > max_w && best) break;
            best = j;
            if (!s[j] || s[j] == '\n' || j >= sizeof buf - 1) break;
            i = j + 1;
        }
        memcpy(buf, s, best); buf[best] = 0;
        if (draw) text_draw(g_rgb, g_w, g_h, x, y + h, px, c, buf);
        h += line;
        s += best; while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return h;
}

/* The menu's layer for a window of w x h pixels: RGBA, rows top-down; NULL
 * when nothing changed since the last call (*w and *h stay as given). */
const uint8_t* ingame_menu_draw(int w, int h, int force) {
    if (!ingame_menu_is_open() || w <= 0 || h <= 0) return NULL;
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
    if (!g_dlg.on && g_txt.on) {                                        /* typing */
        const uint32_t BG = 0x101A3A, SEL = 0x2F4C8C, WHITE = 0xFFFFFF, GREY = 0x8A93AE, GOLD = 0xF8D860;
        float px = h * 0.032f; if (px < 14) px = 14; if (px > 36) px = 36;
        int pw = (int)(px * 30); if (pw > w - 2 * (int)px) pw = w - 2 * (int)px;
        int pad = (int)(px * 1.1f), line = px > 24 ? 3 : 2, head = (int)(px * 2.1f);
        float tp = px * 0.85f;
        int th = text_lines(0, 0, pw - 2 * pad, tp, 0, g_txt.prompt, 0);
        int kh = (int)(px * 1.9f), kgap = (int)(px * 0.25f), field = (int)(px * 2.2f);
        int ph = head + pad + th + pad + field + pad + 5 * (kh + kgap) + (int)(px * 1.6f) + pad;
        if (ph > h - 8) { tp = px * 0.7f; th = text_lines(0, 0, pw - 2 * pad, tp, 0, g_txt.prompt, 0); ph = head + pad + th + pad + field + pad + 5 * (kh + kgap) + (int)(px * 1.6f) + pad; }
        int x0 = (w - pw) / 2, y0 = (h - ph) / 2; if (y0 < 4) y0 = 4;
        g_panel[0] = x0; g_panel[1] = y0; g_panel[2] = pw; g_panel[3] = ph;
        fill(x0, y0, pw, ph, BG, 245);
        frame(x0 + line, y0 + line, pw - 2 * line, ph - 2 * line, line, WHITE);
        text_fit(x0 + pad, y0 + (head - (int)px) / 2, pw - 2 * pad, px * 1.05f, GOLD, g_txt.title);
        fill(x0 + pad, y0 + head - line, pw - 2 * pad, line, 0x3A4670, 255);
        int y = y0 + head + pad;
        text_lines(x0 + pad, y, pw - 2 * pad, tp, 0xC8D0E8, g_txt.prompt, 1); y += th + pad;
        fill(x0 + pad, y, pw - 2 * pad, field, 0x060C22, 255); frame(x0 + pad, y, pw - 2 * pad, field, 1, GREY);
        {   /* the text, its end showing when long, and the caret */
            const char* t = g_txt.buf; int mw = pw - 2 * pad - (int)(px * 1.5f);
            while (*t && text_width(px, t) > mw) { t++; while (((unsigned char)*t & 0xC0) == 0x80) t++; }
            int tw = text_draw(g_rgb, g_w, g_h, x0 + pad + (int)(px * 0.5f), y + (field - (int)px) / 2, px, WHITE, t);
            fill(x0 + pad + (int)(px * 0.5f) + tw + 2, y + (int)(px * 0.4f), 2, field - (int)(px * 0.8f), GOLD, 255);
        }
        y += field + pad;
        int kw = (pw - 2 * pad - 9 * kgap) / 10;
        memset(g_txt.key_rect, 0, sizeof g_txt.key_rect);
        for (int ky = 0; ky < 5; ky++, y += kh + kgap) {
            int cols = ky == 4 ? 4 : 10;
            for (int kx = 0; kx < cols; kx++) {
                int bw = ky == 4 ? (pw - 2 * pad - 3 * kgap) / 4 : kw, bx = x0 + pad + kx * (bw + kgap);
                int on = g_txt.kx == kx && g_txt.ky == ky;
                fill(bx, y, bw, kh, on ? SEL : 0x1C2850, 255);
                frame(bx, y, bw, kh, 1, on ? WHITE : 0x3A4670);
                char lab[16];
                if (ky < 4) { lab[0] = OSK[g_txt.shift][ky][kx]; lab[1] = 0; }
                else snprintf(lab, sizeof lab, "%s", kx == 0 ? (g_txt.shift ? "abc" : "ABC") : kx == 1 ? "Space" : kx == 2 ? "Delete" : "Done");
                int lw = text_width(px, lab);
                text_draw(g_rgb, g_w, g_h, bx + (bw - lw) / 2, y + (kh - (int)px) / 2, px, ky == 4 && kx == 3 ? GOLD : WHITE, lab);
                g_txt.key_rect[ky][kx][0] = bx; g_txt.key_rect[ky][kx][1] = y; g_txt.key_rect[ky][kx][2] = bw; g_txt.key_rect[ky][kx][3] = kh;
            }
        }
        text_fit(x0 + pad, y + (int)(px * 0.2f), pw - 2 * pad, px * 0.75f, GREY,
                 "Type, or: A a letter, B delete, X space, Y capitals, Start done.   Enter: done   Esc: cancel");
        for (int j = y0; j < y0 + ph && j < h; j++) for (int i = x0; i < x0 + pw && i < w; i++) if (j >= 0 && i >= 0) g_alpha[j * w + i] = 245;
        goto out;
    }
    if (!g_dlg.on && g_lst.on) {                                        /* a list */
        const uint32_t BG = 0x101A3A, SEL = 0x2F4C8C, WHITE = 0xFFFFFF, GREY = 0x8A93AE, GOLD = 0xF8D860;
        float px = h * 0.032f; if (px < 14) px = 14; if (px > 38) px = 38;
        int row_h = (int)(px * 1.5f), pad = (int)(px * 1.1f), line = px > 24 ? 3 : 2;
        int pw = (int)(px * 34); if (pw > w - 2 * (int)px) pw = w - 2 * (int)px;
        int head = (int)(px * 2.1f), foot = (int)(px * 1.9f);
        int max_rows = (h - 2 * (int)px - head - foot - pad) / row_h; if (max_rows < 3) max_rows = 3; if (max_rows > 40) max_rows = 40;
        int shown = g_lst.n < max_rows ? g_lst.n : max_rows; if (shown < 1) shown = 1;
        int ph = head + foot + shown * row_h + pad;
        int x0 = (w - pw) / 2, y0 = (h - ph) / 2;
        g_panel[0] = x0; g_panel[1] = y0; g_panel[2] = pw; g_panel[3] = ph;
        fill(x0, y0, pw, ph, BG, 240);
        frame(x0 + line, y0 + line, pw - 2 * line, ph - 2 * line, line, WHITE);
        text_fit(x0 + pad, y0 + (head - (int)px) / 2, pw - 2 * pad, px * 1.05f, GOLD, g_lst.title);
        fill(x0 + pad, y0 + head - line, pw - 2 * pad, line, 0x3A4670, 255);
        if (g_lst.sel < g_lst.top) g_lst.top = g_lst.sel;
        if (g_lst.sel >= g_lst.top + shown) g_lst.top = g_lst.sel - shown + 1;
        if (g_lst.top > g_lst.n - shown) g_lst.top = g_lst.n - shown;
        if (g_lst.top < 0) g_lst.top = 0;
        memset(g_lst.rows, 0, sizeof g_lst.rows);
        int ry = y0 + head + pad / 2;
        if (!g_lst.n) text_draw(g_rgb, g_w, g_h, x0 + pad, ry + (row_h - (int)px) / 2, px, GREY, "(nothing here)");
        for (int k = 0; k < shown && g_lst.top + k < g_lst.n; k++, ry += row_h) {
            int i = g_lst.top + k, on = i == g_lst.sel;
            g_lst.rows[k][0] = ry; g_lst.rows[k][1] = ry + row_h;
            if (on) fill(x0 + line * 3, ry, pw - line * 6, row_h, SEL, 255);
            int ty = ry + (row_h - (int)px) / 2, right = x0 + pw - pad;
            if (on) tri_right(x0 + pad - (int)(px * 0.7f), ty + (int)(px * 0.2f), (int)(px * 0.6f), WHITE);
            if (g_lst.right[i][0]) {
                int rw = text_width(px * 0.85f, g_lst.right[i]); if (rw > pw / 2) rw = pw / 2;
                text_fit(right - rw, ty + (int)(px * 0.08f), rw, px * 0.85f, on ? GOLD : GREY, g_lst.right[i]);
                right -= rw + (int)px;
            }
            text_fit(x0 + pad, ty, right - x0 - pad, px, WHITE, g_lst.text[i]);
        }
        if (g_lst.top > 0) fill(x0 + pw - pad, y0 + head + pad / 4, (int)(px * 0.4f), line, GREY, 255);
        if (g_lst.top + shown < g_lst.n) fill(x0 + pw - pad, y0 + ph - foot - line, (int)(px * 0.4f), line, GREY, 255);
        fill(x0 + pad, y0 + ph - foot, pw - 2 * pad, line, 0x3A4670, 255);
        text_fit(x0 + pad, y0 + ph - foot + (foot - (int)(px * 0.8f)) / 2, pw - 2 * pad, px * 0.8f, GREY, g_lst.foot);
        for (int j = y0; j < y0 + ph && j < h; j++) for (int i = x0; i < x0 + pw && i < w; i++) if (j >= 0 && i >= 0) g_alpha[j * w + i] = 245;
        goto out;
    }
    if (g_dlg.on) {                                                     /* a message or question */
        const uint32_t BG = 0x101A3A, SEL = 0x2F4C8C, WHITE = 0xFFFFFF, GREY = 0x8A93AE, GOLD = 0xF8D860;
        float px = h * 0.032f; if (px < 14) px = 14; if (px > 38) px = 38;
        int pw = (int)(px * 32); if (pw > w - 2 * (int)px) pw = w - 2 * (int)px;
        int pad = (int)(px * 1.2f), line = px > 24 ? 3 : 2, head = (int)(px * 2.1f), bh = (int)(px * 1.9f);
        int chk = g_dlg.has_check ? (int)(px * 2.2f) : 0;
        int room = h - 2 * (int)px - head - bh - chk - 3 * pad;
        float tp = px;                                                  /* a long text: smaller letters, so it all shows */
        while (tp > 11 && text_lines(0, 0, pw - 2 * pad, tp, 0, g_dlg.text, 0) > room) tp -= 1;
        int th = text_lines(0, 0, pw - 2 * pad, tp, 0, g_dlg.text, 0);
        int ph = head + th + chk + bh + 3 * pad; if (ph > h - 8) ph = h - 8;
        int x0 = (w - pw) / 2, y0 = (h - ph) / 2;
        g_panel[0] = x0; g_panel[1] = y0; g_panel[2] = pw; g_panel[3] = ph;
        fill(x0, y0, pw, ph, BG, 240);
        frame(x0 + line, y0 + line, pw - 2 * line, ph - 2 * line, line, WHITE);
        text_fit(x0 + pad, y0 + (head - (int)px) / 2, pw - 2 * pad, px * 1.05f, GOLD, g_dlg.title);
        fill(x0 + pad, y0 + head - line, pw - 2 * pad, line, 0x3A4670, 255);
        text_lines(x0 + pad, y0 + head + pad, pw - 2 * pad, tp, WHITE, g_dlg.text, 1);
        int cy = y0 + head + pad + th + pad / 2;
        memset(g_dlg.check_rect, 0, sizeof g_dlg.check_rect);
        if (g_dlg.has_check) {                                          /* a check box ("Don't ask again") */
            int bs = (int)(px * 0.9f), rx = x0 + pad, ry = cy + (chk - bs) / 2;
            if (g_dlg.focus_check) fill(x0 + line * 3, cy, pw - line * 6, chk, SEL, 255);
            frame(rx, ry, bs, bs, line, WHITE);
            if (g_dlg.checked) tick(rx + bs / 6, ry, (int)(bs * 0.8f), GOLD);
            text_fit(rx + bs + (int)(px * 0.6f), cy + (chk - (int)px) / 2, pw - 2 * pad - bs, px * 0.9f, WHITE, g_dlg.check);
            g_dlg.check_rect[0] = x0; g_dlg.check_rect[1] = cy; g_dlg.check_rect[2] = pw; g_dlg.check_rect[3] = chk;
        }
        /* the buttons, side by side at the bottom (a long row: smaller) */
        float bp = px; int gap = (int)(px * 0.8f), tot;
        for (;;) { tot = 0; for (int i = 0; i < g_dlg.n; i++) tot += text_width(bp, g_dlg.btn[i]) + (int)(bp * 2) + gap; tot -= gap;
                   if (tot <= pw - 2 * pad || bp <= 11) break; bp -= 1; }
        int bx = x0 + pw - pad - tot, by = y0 + ph - pad - bh;
        if (bx < x0 + pad) bx = x0 + pad;
        for (int i = 0; i < g_dlg.n; i++) {
            int bw = text_width(bp, g_dlg.btn[i]) + (int)(bp * 2);
            int on = i == g_dlg.sel && !g_dlg.focus_check;
            fill(bx, by, bw, bh, on ? SEL : 0x1C2850, 255);
            frame(bx, by, bw, bh, line > 2 ? 2 : 1, on ? WHITE : GREY);
            text_draw(g_rgb, g_w, g_h, bx + (int)bp, by + (bh - (int)bp) / 2, bp, on ? WHITE : 0xC8D0E8, g_dlg.btn[i]);
            g_dlg.btn_rect[i][0] = bx; g_dlg.btn_rect[i][1] = by; g_dlg.btn_rect[i][2] = bw; g_dlg.btn_rect[i][3] = bh;
            bx += bw + gap;
        }
        for (int j = y0; j < y0 + ph && j < h; j++) for (int i = x0; i < x0 + pw && i < w; i++) if (j >= 0 && i >= 0) g_alpha[j * w + i] = 245;
        (void)GREY;
        goto out;
    }

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
        if (r->value && g_val_get) {                                   /* a value: < 70% > */
            char v[64]; g_val_get(r->id, v, sizeof v);
            int vw = text_width(px, v), aw = (int)(px * 0.55f);
            tri_right(right - aw, ty + (int)(px * 0.25f), aw, i == sel ? WHITE : GREY);
            text_draw(g_rgb, g_w, g_h, right - aw - (int)(px * 0.4f) - vw, ty, px, GOLD, v);
            tri_left(right - aw - (int)(px * 0.8f) - vw - aw, ty + (int)(px * 0.25f), aw, i == sel ? WHITE : GREY);
            right -= vw + 2 * aw + (int)(px * 1.6f);
        }
        else if (r->kind == MK_SUB) { tri_right(right - (int)(px * 0.4f), ty + (int)(px * 0.25f), (int)(px * 0.55f), en ? DIM : GREY); right -= (int)(px * 0.9f); }
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

out:
    for (int i = 0; i < w * h; i++) {
        uint32_t c = g_rgb[i];
        g_out[i * 4] = (uint8_t)(c >> 16); g_out[i * 4 + 1] = (uint8_t)(c >> 8); g_out[i * 4 + 2] = (uint8_t)c; g_out[i * 4 + 3] = g_alpha[i];
    }
    return g_out;
}
