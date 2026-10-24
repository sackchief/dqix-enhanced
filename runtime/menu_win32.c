/* Menu bar for the game window on Windows (see menu.h). */
#include "menu.h"
#include "controls.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "SDL_syswm.h"
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>

static HWND g_hwnd;
static HMENU g_bar;

static void item(HMENU m, int id, const wchar_t* text) { AppendMenuW(m, MF_STRING, (UINT_PTR)id, text); }
static void sep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); }
static HMENU sub(HMENU parent, const wchar_t* text) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)m, text);
    return m;
}

void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    g_hwnd = info.info.win.window;
    g_bar = CreateMenu();

    HMENU game = sub(g_bar, L"&Game");
    item(game, MI_PAUSE, L"&Pause\tP");
    item(game, MI_TURBO, L"&Fast forward\t(or hold Tab)");
    item(game, MI_RESET, L"&Reset...\tCtrl+R");
    sep(game);
    item(game, MI_VOLUME, L"&Volume...\t- / =");
    item(game, MI_MUTE, L"&Mute");
    item(game, MI_CONTROLS_EDIT, L"&Controls...");
    sep(game);
    item(game, MI_SHOT, L"Save &screenshot\tF12");
    item(game, MI_REPORT, L"Save &bug report\tF10");
    sep(game);
    item(game, MI_EXIT, L"E&xit\tEsc");

    HMENU gfx = sub(g_bar, L"G&raphics");
    item(gfx, MI_CLASSIC, L"&Classic: exactly like the DS\tF5");
    item(gfx, MI_ENHANCED, L"&Enhanced: sharper 3D\tF5");
    sep(gfx);
    HMENU res = sub(gfx, L"3D &resolution (Enhanced)\tF4");
    item(res, MI_RES1, L"1x (DS)");
    item(res, MI_RES2, L"2x");
    item(res, MI_RES3, L"3x");
    item(res, MI_RES4, L"4x");
    item(res, MI_RES6, L"6x");
    item(res, MI_RES8, L"8x");
    sep(res);
    item(res, MI_RES_AUTO, L"&Auto: as sharp as your screen (up to 12x)");
    item(gfx, MI_SMOOTH, L"&Smooth textures (Enhanced)");
    item(gfx, MI_TEXUP, L"&Upscaled textures, xBR (Enhanced)");
    item(gfx, MI_SMOOTH2D, L"Smooth &2D: menus, maps, sprites (Enhanced)");
    sep(gfx);
    item(gfx, MI_TEXPACK, L"&HD textures from the textures folder (Enhanced)");
    item(gfx, MI_TEXRELOAD, L"&Reload the HD textures");
    item(gfx, MI_TEXDUMP, L"&Dump textures to textures\\dump (for making a pack)");
    sep(gfx);
    item(gfx, MI_HDTEXT, L"HD te&xt (Enhanced)");
    HMENU fnt = sub(gfx, L"Text &font");
    item(fnt, MI_FONT0, L"The game's own letters, &smoothed");
    item(fnt, MI_FONT0 + 1, L"&Georgia");
    item(fnt, MI_FONT0 + 2, L"&Palatino Linotype");
    item(fnt, MI_FONT0 + 3, L"&Constantia");
    item(fnt, MI_FONT0 + 4, L"&Times New Roman");
    item(fnt, MI_FONT0 + 5, L"Segoe &UI");

    HMENU scr = sub(g_bar, L"&Screen");
    item(scr, MI_LCD, L"&DS Lite screen\tF6");
    sep(scr);
    item(scr, MI_LCDCOL, L"LCD &colours");
    item(scr, MI_GRID, L"&Pixel grid (at 3x and larger)");
    item(scr, MI_GHOST, L"&Motion blur");
    HMENU bl = sub(scr, L"&Backlight");
    item(bl, MI_BL1, L"&Low");
    item(bl, MI_BL2, L"&Medium");
    item(bl, MI_BL3, L"&High");
    item(bl, MI_BL4, L"Ma&x");

    HMENU view = sub(g_bar, L"&View");
    item(view, MI_SHELL, L"Show the DS &Lite\tF7");
    HMENU col = sub(view, L"Console &colour");
    for (int i = 0; i < n_shells; i++) {
        wchar_t w[64];
        MultiByteToWideChar(CP_UTF8, 0, shell_names[i], -1, w, 64);
        item(col, MI_SHELLC0 + i, w);
    }
    item(view, MI_GAP, L"&Gap between the screens");
    item(view, MI_ACTUAL, L"&Actual size (as big as a real DS Lite)\tF8");
    sep(view);
    item(view, MI_STACKED, L"S&tacked\tF2");
    item(view, MI_SIDE, L"&Side by side\tF2");
    item(view, MI_CORNER, L"Top screen in the &corner\tF2");
    item(view, MI_WIDE, L"&Widescreen (the action wide, the other screen in a corner)\tF2");
    item(view, MI_WIDE_DYN, L"Widescreen, d&ynamic (experimental: makes room for menus)\tF2");
    item(view, MI_TWO, L"Two wi&ndows (one screen in each, for two monitors)\tF2");
    sep(view);
    item(view, MI_SINGLE, L"Single scr&een (widescreen: hide the small screen)\tU");
    item(view, MI_SWAP, L"Swa&p the screens (widescreen: hold to see the other)\tF9");
    HMENU sm = sub(view, L"Widescreen: the small scr&een");
    item(sm, MI_CORNER0, L"Top left\tK");
    item(sm, MI_CORNER0 + 1, L"Top right\tK");
    item(sm, MI_CORNER0 + 2, L"Bottom right\tK");
    item(sm, MI_CORNER0 + 3, L"Bottom left\tK");
    sep(sm);
    item(sm, MI_SSIZE0, L"Small\tI");
    item(sm, MI_SSIZE0 + 1, L"Medium\tI");
    item(sm, MI_SSIZE0 + 2, L"Large\tI");
    sep(sm);
    item(sm, MI_OPACITY1, L"100% (solid)\tO");
    item(sm, MI_OPACITY1 + 1, L"75%\tO");
    item(sm, MI_OPACITY1 + 2, L"50%\tO");
    item(sm, MI_OPACITY1 + 3, L"25%\tO");
    sep(sm);
    item(sm, MI_POPOUT, L"In its own &window (a clean field)");
    sep(view);
    HMENU size = sub(view, L"&Window size\tF3");
    item(size, MI_SIZE1, L"1x");
    item(size, MI_SIZE2, L"2x");
    item(size, MI_SIZE3, L"3x");
    item(size, MI_SIZE4, L"4x");
    item(view, MI_INTEGER, L"&Integer scaling");
    item(view, MI_FULL, L"&Fullscreen\tF11");

    HMENU wl = sub(g_bar, L"&Wireless");
    item(wl, MI_NET, L"&Connect (Nintendo Wi-Fi Connection replacement)");
    HMENU srv = sub(wl, L"&Server");
    item(srv, MI_SRV_BUILTIN, L"&Built-in (offline: DQIX's preserved downloads)");
    sep(srv);
    item(srv, MI_SRV_ALTWFC, L"&AltWFC (172.104.88.237)");
    item(srv, MI_SRV_WIIMMFI, L"&Wiimmfi (164.132.44.106): not for DQIX");
    item(srv, MI_SRV_CUSTOM, L"&Custom (wfc_server in settings.ini)");
    sep(wl);
    HMENU friends = sub(wl, L"&Play with friends far away (tag mode, visiting)");
    item(friends, MI_SES_HOST, L"&Host: your friends join you...");
    item(friends, MI_SES_JOIN, L"&Join a friend...");
    item(friends, MI_SES_START, L"&Start now (everyone is here)\tEnter");
    item(friends, MI_SES_LEAVE, L"&Leave and play alone");
    sep(friends);
    item(friends, MI_SES_NAME, L"Your &name...");
    item(friends, MI_SES_HELP, L"&How does this work?");
    sep(wl);
    item(wl, MI_DLC, L"&Unlock all downloadable content now (without Sellma)");
    sep(wl);
    item(wl, MI_NET_HELP, L"&How does this work?");

    HMENU modern = sub(g_bar, L"&Modern");
    item(modern, MI_ANALOG, L"Walk in any &direction with the left stick");
    item(modern, MI_WIDE, L"&Widescreen: the action wide, the other screen in a corner");
    item(modern, MI_WIDE_DYN, L"Widescreen, d&ynamic (experimental)");
    item(modern, MI_CORNER, L"Top screen in the &corner (the field as on the DS)");
    item(modern, MI_POPOUT, L"Widescreen: the small screen in its own &window (a clean field)");
    sep(modern);
    item(modern, MI_MODERN_HELP, L"&What is this?");
    HMENU lang = sub(g_bar, L"&Language");
    item(lang, MI_LANG1, L"&English");
    item(lang, MI_LANG1 + 1, L"&Fran\u00e7ais");
    item(lang, MI_LANG1 + 2, L"&Deutsch");
    item(lang, MI_LANG1 + 3, L"&Italiano");
    item(lang, MI_LANG1 + 4, L"E&spa\u00f1ol");
    HMENU help = sub(g_bar, L"&Help");
    item(help, MI_CONTROLS, L"&Controls");
    item(help, MI_UPDATE, L"Check for &updates...");
    item(help, MI_UPDATE_AUTO, L"Check for updates at &start");
    sep(help);
    item(help, MI_SEND_REPORT, L"&Send a bug report...");
    item(help, MI_ABOUT, L"&About");

    SetMenu(g_hwnd, g_bar);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
}

void menu_show(int on) {
    if (g_hwnd) SetMenu(g_hwnd, on ? g_bar : NULL);
}

/* The second window (two windows): a bar with one command, fullscreen. */
static HWND g_hwnd2;
static HMENU g_bar2;
void menu_init_second(SDL_Window* win) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    g_hwnd2 = NULL;
    if (!win || !SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    g_hwnd2 = info.info.win.window;
    if (!g_bar2) { g_bar2 = CreateMenu(); item(g_bar2, MI_FULL2, L"&Fullscreen (F11)"); }
    SetMenu(g_hwnd2, g_bar2);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
}
void menu_show_second(int on) {
    if (g_hwnd2 && IsWindow(g_hwnd2)) SetMenu(g_hwnd2, on ? g_bar2 : NULL);
}

void menu_check(int id, int on) {
    if (g_bar) CheckMenuItem(g_bar, (UINT)id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

void menu_enable(int id, int on) {
    if (g_bar) EnableMenuItem(g_bar, (UINT)id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
}

int menu_command(const SDL_Event* e) {
    if (e->type != SDL_SYSWMEVENT || (!g_bar && !g_bar2)) return 0;
    const SDL_SysWMmsg* m = e->syswm.msg;
    if (m->subsystem != SDL_SYSWM_WINDOWS || m->msg.win.msg != WM_COMMAND || HIWORD(m->msg.win.wParam) != 0) return 0;
    return LOWORD(m->msg.win.wParam);
}

/* The monitor's EDID (in the registry under its device id) holds its
 * physical size: the first detailed timing block in mm, else the basic
 * size in cm. Divided by the monitor's width in pixels. */
float display_mm_per_pixel(SDL_Window* win) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return 0;
    HMONITOR mon = MonitorFromWindow(info.info.win.window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO*)&mi)) return 0;
    DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth) return 0;
    int px_w = (int)dm.dmPelsWidth, px_h = (int)dm.dmPelsHeight;
    DISPLAY_DEVICEW dd; dd.cb = sizeof dd;
    for (DWORD k = 0; EnumDisplayDevicesW(mi.szDevice, k, &dd, EDD_GET_DEVICE_INTERFACE_NAME); k++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        /* \\?\DISPLAY#<model>#<instance>#{guid} -> SYSTEM\CurrentControlSet\Enum\DISPLAY\<model>\<instance> */
        wchar_t id[256]; wcsncpy(id, dd.DeviceID, 255); id[255] = 0;
        wchar_t* parts[4] = { 0 }; int n = 0;
        for (wchar_t *ctx = NULL, *t = wcstok(id, L"#", &ctx); t && n < 4; t = wcstok(NULL, L"#", &ctx)) parts[n++] = t;
        if (n < 3) continue;
        wchar_t key[512];
        _snwprintf(key, 511, L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%ls\\%ls\\Device Parameters", parts[1], parts[2]);
        key[511] = 0;
        BYTE edid[256]; DWORD len = sizeof edid;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EDID", RRF_RT_REG_BINARY, NULL, edid, &len) != ERROR_SUCCESS || len < 128) continue;
        if (edid[0] != 0 || edid[1] != 0xFF || edid[7] != 0) continue;
        int w_mm = 0, h_mm = 0;
        if (edid[54] | edid[55]) { w_mm = edid[66] | ((edid[68] & 0xF0) << 4); h_mm = edid[67] | ((edid[68] & 0x0F) << 8); }
        if (w_mm < 100 || h_mm < 50) { w_mm = edid[21] * 10; h_mm = edid[22] * 10; }    /* cm */
        if (w_mm < 100 || h_mm < 50) continue;
        if ((px_w < px_h) != (w_mm < h_mm)) { int t = w_mm; w_mm = h_mm; h_mm = t; } /* rotated monitor */
        float mm = (float)w_mm / (float)px_w;
        if (mm > 0.05f && mm < 1.5f) return mm;
    }
    return 0;
}

/* ---------------------------------------------------------------- text box (bug descriptions)
 * A dialog built in memory: a prompt, a large multi-line edit box, and
 * Save / Skip buttons. */
#define ID_TEXT 1001
static wchar_t* g_text_out;

static INT_PTR CALLBACK text_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SendDlgItemMessageW(h, ID_TEXT, EM_SETLIMITTEXT, 1 << 20, 0);
        SetFocus(GetDlgItem(h, ID_TEXT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int n = GetWindowTextLengthW(GetDlgItem(h, ID_TEXT));
            g_text_out = calloc((size_t)n + 1, sizeof(wchar_t));
            if (g_text_out) GetDlgItemTextW(h, ID_TEXT, g_text_out, n + 1);
            EndDialog(h, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static WORD* align4(WORD* p) { return (WORD*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }
static WORD* put_str(WORD* p, const wchar_t* s) { do { *p++ = (WORD)*s; } while (*s++); return p; }
static WORD* put_item(WORD* p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
    p = align4(p);
    DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
    it->style = style | WS_CHILD | WS_VISIBLE; it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD*)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;                                   /* 0x80 button, 0x81 edit, 0x82 static */
    p = put_str(p, text);
    *p++ = 0;                                                    /* no creation data */
    return p;
}


/* ---------------------------------------------------------------- a message with buttons that fit */
#define CB_TEXT 2000
#define CB_ICON 2001
#define CB_CHECK 2002
#define CB_BUTTON 3000
typedef struct { int n, def, cancel, has_check, checked; } ChoiceInfo;

static INT_PTR CALLBACK choice_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    ChoiceInfo* ci = (ChoiceInfo*)GetWindowLongPtrW(h, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        ci = (ChoiceInfo*)lp;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)ci);
        SendDlgItemMessageW(h, CB_ICON, STM_SETICON, (WPARAM)LoadIconW(NULL, (LPCWSTR)IDI_INFORMATION), 0);
        if (ci->has_check) CheckDlgButton(h, CB_CHECK, ci->checked ? BST_CHECKED : BST_UNCHECKED);
        /* sizes in dialog units -> pixels for this font and monitor */
        RECT u = { 7, 7, 280, 14 }; MapDialogRect(h, &u);
        int margin = u.left, textw = u.right, btnh = u.bottom;
        RECT g = { 4, 50, 0, 0 }; MapDialogRect(h, &g);
        int gap = g.left, minbtn = g.top, icon = GetSystemMetrics(SM_CXICON);
        HDC dc = GetDC(h);
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t t[4096];
        GetDlgItemTextW(h, CB_TEXT, t, 4096);
        RECT tr = { 0, 0, textw, 0 };
        DrawTextW(dc, t, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        int bw[8], total = 0;
        for (int i = 0; i < ci->n; i++) {
            wchar_t b[200]; SIZE sz;
            GetDlgItemTextW(h, CB_BUTTON + i, b, 200);
            GetTextExtentPoint32W(dc, b, (int)wcslen(b), &sz);
            bw[i] = sz.cx + 2 * margin + gap; if (bw[i] < minbtn) bw[i] = minbtn;
            total += bw[i] + (i ? gap : 0);
        }
        int checkh = 0, checkw = 0;
        if (ci->has_check) {
            wchar_t c[300]; SIZE sz; GetDlgItemTextW(h, CB_CHECK, c, 300);
            GetTextExtentPoint32W(dc, c, (int)wcslen(c), &sz);
            checkw = sz.cx + GetSystemMetrics(SM_CXMENUCHECK) + 3 * gap; checkh = btnh;
        }
        SelectObject(dc, old);
        ReleaseDC(h, dc);
        int tx = margin + icon + 2 * gap, th = tr.bottom;
        int cw = tx + tr.right + margin;
        if (cw < total + 2 * margin) cw = total + 2 * margin;
        if (ci->has_check && cw < tx + checkw + margin) cw = tx + checkw + margin;
        int y = margin;
        SetWindowPos(GetDlgItem(h, CB_ICON), NULL, margin, y, icon, icon, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(h, CB_TEXT), NULL, tx, y, cw - tx - margin, th, SWP_NOZORDER);
        y += (th > icon ? th : icon) + margin;
        if (ci->has_check) { SetWindowPos(GetDlgItem(h, CB_CHECK), NULL, tx, y, cw - tx - margin, checkh, SWP_NOZORDER); y += checkh + margin; }
        int bx = cw - margin - total;
        for (int i = 0; i < ci->n; i++) { SetWindowPos(GetDlgItem(h, CB_BUTTON + i), NULL, bx, y, bw[i], btnh, SWP_NOZORDER); bx += bw[i] + gap; }
        int ch = y + btnh + margin;
        RECT wr = { 0, 0, cw, ch };
        AdjustWindowRectEx(&wr, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
        HWND parent = GetParent(h); RECT pr;
        if (!parent || !GetWindowRect(parent, &pr)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &pr, 0);
        int w = wr.right - wr.left, hh = wr.bottom - wr.top;
        SetWindowPos(h, NULL, pr.left + (pr.right - pr.left - w) / 2, pr.top + (pr.bottom - pr.top - hh) / 2, w, hh, SWP_NOZORDER);
        SetFocus(GetDlgItem(h, CB_BUTTON + ci->def));
        return FALSE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= CB_BUTTON && id < CB_BUTTON + ci->n) {
            if (ci->has_check) ci->checked = IsDlgButtonChecked(h, CB_CHECK) == BST_CHECKED;
            EndDialog(h, id - CB_BUTTON); return TRUE;
        }
        if (id == IDCANCEL) { if (ci->has_check) ci->checked = IsDlgButtonChecked(h, CB_CHECK) == BST_CHECKED; EndDialog(h, ci->cancel); return TRUE; }
        if (id == IDOK) { EndDialog(h, ci->def); return TRUE; }
        break;
    }
    }
    return FALSE;
}

int choice_box(SDL_Window* win, const char* title, const char* text, const char* const* buttons, int n, int def, int cancel,
               const char* check, int* checked) {
    if (n < 1 || n > 8) return -1;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    HWND parent = NULL;
    if (win && SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_WINDOWS) parent = info.info.win.window;
    static WORD buf[16384];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cdit = (WORD)(2 + n + (check ? 1 : 0)); d->cx = 200; d->cy = 100;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    static wchar_t w[8192];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, w, 8192);
    p = put_str(p, w);
    *p++ = 9; p = put_str(p, L"Segoe UI");
    p = put_item(p, SS_ICON, 0, 0, 0, 0, CB_ICON, 0x82, L"");
    /* the text: \n -> \r\n for the static control */
    static wchar_t t[8192]; int k = 0;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 8192);
    for (int i = 0; w[i] && k < 8190; i++) { if (w[i] == L'\n') t[k++] = L'\r'; t[k++] = w[i]; }
    t[k] = 0;
    p = put_item(p, SS_NOPREFIX, 0, 0, 10, 10, CB_TEXT, 0x82, t);
    if (check) { MultiByteToWideChar(CP_UTF8, 0, check, -1, w, 8192); p = put_item(p, WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10, CB_CHECK, 0x80, w); }
    for (int i = 0; i < n; i++) {
        MultiByteToWideChar(CP_UTF8, 0, buttons[i], -1, w, 8192);
        p = put_item(p, WS_TABSTOP | (i == def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), 0, 0, 10, 10, (WORD)(CB_BUTTON + i), 0x80, w);
    }
    ChoiceInfo ci = { n, def, cancel, check != NULL, checked ? *checked : 0 };
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), d, parent, choice_proc, (LPARAM)&ci);
    if (r < 0) return -1;
    if (checked) *checked = ci.checked;
    return (int)r;
}

char* ask_text(SDL_Window* win, const char* title, const char* prompt) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return NULL;
    wchar_t wtitle[200], wprompt[1000];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 200);
    MultiByteToWideChar(CP_UTF8, 0, prompt, -1, wprompt, 1000);
    static WORD buf[4096];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->cdit = 4; d->x = 0; d->y = 0; d->cx = 340; d->cy = 230;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;                                          /* no menu, default class */
    p = put_str(p, wtitle);
    *p++ = 9; p = put_str(p, L"Segoe UI");                       /* font */
    p = put_item(p, 0, 7, 7, 326, 34, 0xFFFF, 0x82, wprompt);
    p = put_item(p, WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 7, 44, 326, 158, ID_TEXT, 0x81, L"");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 213, 208, 60, 15, IDOK, 0x80, L"Save");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 277, 208, 56, 15, IDCANCEL, 0x80, L"Skip");
    g_text_out = NULL;
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, text_proc, 0);
    if (r != 1 || !g_text_out) { free(g_text_out); g_text_out = NULL; return NULL; }
    int n = WideCharToMultiByte(CP_UTF8, 0, g_text_out, -1, NULL, 0, NULL, NULL);
    char* out = malloc((size_t)(n > 0 ? n : 1));
    if (out) { out[0] = 0; if (n > 0) WideCharToMultiByte(CP_UTF8, 0, g_text_out, -1, out, n, NULL, NULL); }
    free(g_text_out); g_text_out = NULL;
    return out;
}


/* ---------------------------------------------------------------- the session window (menu.h) */
enum { ID_ADDR = 1101, ID_COPY, ID_HINT, ID_LIST, ID_STATUS, ID_START, ID_HEAD };
static HWND g_ses;
static int g_ses_action;
static SessionWindow g_ses_shown;

static void set_text(HWND h, int id, const char* utf8) {
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 1024);
    w[1023] = 0;
    SetDlgItemTextW(h, id, w);
}

static void copy_address(HWND h) {
    int n = GetWindowTextLengthW(GetDlgItem(h, ID_ADDR));
    if (n <= 0 || !OpenClipboard(h)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(wchar_t));
    if (mem) {
        wchar_t* p = GlobalLock(mem);
        GetDlgItemTextW(h, ID_ADDR, p, n + 1);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    SetDlgItemTextW(h, ID_COPY, L"Copied!");
    SetTimer(h, 1, 2000, NULL);
}

static INT_PTR CALLBACK ses_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: return TRUE;
    case WM_TIMER: KillTimer(h, 1); SetDlgItemTextW(h, ID_COPY, L"Copy"); return TRUE;
    case WM_CLOSE: g_ses_action = 2; return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_COPY: copy_address(h); return TRUE;
        case ID_START: if (IsWindowEnabled(GetDlgItem(h, ID_START))) g_ses_action = 1; return TRUE;
        case IDCANCEL: g_ses_action = 2; return TRUE;
        }
        break;
    }
    return FALSE;
}

int session_window_open(SDL_Window* win) {
    if (g_ses) return 1;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return 0;
    static WORD buf[4096];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    d->cdit = 9; d->x = 0; d->y = 0; d->cx = 270; d->cy = 278;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    p = put_str(p, L"Play with friends far away");
    *p++ = 10; p = put_str(p, L"Segoe UI");
    p = put_item(p, 0, 8, 8, 254, 10, ID_HEAD, 0x82, L"");
    p = put_item(p, WS_BORDER | ES_READONLY | ES_AUTOHSCROLL | ES_CENTER, 8, 21, 196, 16, ID_ADDR, 0x81, L"");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 210, 21, 52, 16, ID_COPY, 0x80, L"Copy");
    p = put_item(p, 0, 8, 42, 254, 92, ID_HINT, 0x82, L"");
    p = put_item(p, 0, 8, 138, 254, 10, 0xFFFF, 0x82, L"Players:");
    p = put_item(p, WS_BORDER | WS_VSCROLL | LBS_NOSEL, 8, 150, 254, 54, ID_LIST, 0x83, L"");
    p = put_item(p, 0, 8, 210, 254, 34, ID_STATUS, 0x82, L"");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 118, 254, 84, 17, ID_START, 0x80, L"Start the game");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 208, 254, 54, 17, IDCANCEL, 0x80, L"Cancel");
    g_ses = CreateDialogIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, ses_proc, 0);
    if (!g_ses) return 0;
    /* a bigger, bolder address */
    HFONT f = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (f) SendDlgItemMessageW(g_ses, ID_ADDR, WM_SETFONT, (WPARAM)f, TRUE);
    memset(&g_ses_shown, 0, sizeof g_ses_shown);
    g_ses_shown.can_start = -1;
    g_ses_action = 0;
    return 1;
}

void session_window_update(const SessionWindow* w) {
    if (!g_ses) return;
    SessionWindow* o = &g_ses_shown;
    if (strcmp(o->heading, w->heading)) set_text(g_ses, ID_HEAD, w->heading);
    if (strcmp(o->address, w->address)) set_text(g_ses, ID_ADDR, w->address);
    if (strcmp(o->hint, w->hint)) set_text(g_ses, ID_HINT, w->hint);
    if (strcmp(o->status, w->status)) set_text(g_ses, ID_STATUS, w->status);
    int list_changed = o->n_players != w->n_players;
    for (int i = 0; i < w->n_players && !list_changed; i++) list_changed = strcmp(o->players[i], w->players[i]) != 0;
    if (list_changed) {
        HWND l = GetDlgItem(g_ses, ID_LIST);
        SendMessageW(l, LB_RESETCONTENT, 0, 0);
        for (int i = 0; i < w->n_players; i++) {
            wchar_t t[128]; MultiByteToWideChar(CP_UTF8, 0, w->players[i], -1, t, 128); t[127] = 0;
            SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)t);
        }
    }
    int start = w->host && w->can_start && !w->failed;
    if (o->can_start != start || o->host != w->host || o->failed != w->failed) {
        EnableWindow(GetDlgItem(g_ses, ID_START), start);
        ShowWindow(GetDlgItem(g_ses, ID_START), w->host && !w->failed ? SW_SHOW : SW_HIDE);
        SetDlgItemTextW(g_ses, IDCANCEL, w->failed ? L"Back" : L"Cancel");
        EnableWindow(GetDlgItem(g_ses, ID_COPY), w->address[0] != 0);
    }
    *o = *w;
    o->can_start = start;
}


/* ---------------------------------------------------------------- volume (Game > Volume...) */
#define ID_VOL_BAR 4000
#define ID_VOL_TEXT 4001
#define ID_VOL_MUTE 4002
static HWND g_vol;
static int* g_vol_val; static int* g_vol_mute;
static void vol_show(HWND h) {
    wchar_t t[32]; swprintf(t, 32, L"%d%%", *g_vol_val); SetDlgItemTextW(h, ID_VOL_TEXT, t);
    CheckDlgButton(h, ID_VOL_MUTE, *g_vol_mute ? BST_CHECKED : BST_UNCHECKED);
}
static INT_PTR CALLBACK vol_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SCROLLINFO si = { sizeof si, SIF_RANGE | SIF_POS | SIF_PAGE, 0, 100 + 9, 10, *g_vol_val, 0 };
        SetScrollInfo(GetDlgItem(h, ID_VOL_BAR), SB_CTL, &si, TRUE);
        vol_show(h);
        return TRUE;
    }
    case WM_HSCROLL: {
        HWND bar = GetDlgItem(h, ID_VOL_BAR);
        int v = *g_vol_val;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: v -= 1; break; case SB_LINERIGHT: v += 1; break;
        case SB_PAGELEFT: v -= 10; break; case SB_PAGERIGHT: v += 10; break;
        case SB_LEFT: v = 0; break; case SB_RIGHT: v = 100; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: v = HIWORD(wp); break;
        default: return TRUE;
        }
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        *g_vol_val = v; if (v > 0) *g_vol_mute = 0;
        SetScrollPos(bar, SB_CTL, v, TRUE);
        vol_show(h);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_VOL_MUTE) { *g_vol_mute = IsDlgButtonChecked(h, ID_VOL_MUTE) == BST_CHECKED; return TRUE; }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { DestroyWindow(h); g_vol = NULL; return TRUE; }
        break;
    case WM_CLOSE: DestroyWindow(h); g_vol = NULL; return TRUE;
    }
    (void)lp;
    return FALSE;
}
void volume_window_open(SDL_Window* win, int* volume, int* mute) {
    g_vol_val = volume; g_vol_mute = mute;
    if (g_vol) { vol_show(g_vol); SetForegroundWindow(g_vol); return; }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    static WORD buf[1024];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    d->cdit = 4; d->cx = 220; d->cy = 70;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    p = put_str(p, L"Volume");
    *p++ = 10; p = put_str(p, L"Segoe UI");
    p = put_item(p, WS_TABSTOP | SBS_HORZ, 8, 10, 168, 12, ID_VOL_BAR, 0x84, L"");
    p = put_item(p, 0, 182, 11, 32, 10, ID_VOL_TEXT, 0x82, L"");
    p = put_item(p, WS_TABSTOP | BS_AUTOCHECKBOX, 8, 30, 120, 12, ID_VOL_MUTE, 0x80, L"Mute");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 160, 48, 54, 15, IDOK, 0x80, L"Close");
    g_vol = CreateDialogIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, vol_proc, 0);
}
void volume_window_refresh(void) { if (g_vol) { SetScrollPos(GetDlgItem(g_vol, ID_VOL_BAR), SB_CTL, *g_vol_val, TRUE); vol_show(g_vol); } }

/* ---------------------------------------------------------------- controls (Game > Controls...) */
#define ID_CTL_LIST 4100
#define ID_CTL_K1 4101
#define ID_CTL_K2 4102
#define ID_CTL_PAD 4103
#define ID_CTL_CLEAR 4104
#define ID_CTL_DEFAULTS 4105
#define ID_CTL_STATUS 4106
static HWND g_ctlw;
static int g_ctl_req = -1, g_ctl_req_what;
static void ctl_fill(HWND h) {
    HWND l = GetDlgItem(h, ID_CTL_LIST);
    int sel = (int)SendMessageW(l, LB_GETCURSEL, 0, 0), top = (int)SendMessageW(l, LB_GETTOPINDEX, 0, 0);
    SendMessageW(l, LB_RESETCONTENT, 0, 0);
    for (int a = 0; a < CA_N; a++) {
        char k0[64], k1[64], pd[64], row[300];
        controls_key_name(g_ctl_key[a][0], k0, sizeof k0); controls_key_name(g_ctl_key[a][1], k1, sizeof k1);
        controls_pad_name(g_ctl_pad[a], pd, sizeof pd);
        snprintf(row, sizeof row, "%s\t%s\t%s\t%s", g_ctl[a].name, k0, k1, pd);
        wchar_t w[300]; MultiByteToWideChar(CP_UTF8, 0, row, -1, w, 300); w[299] = 0;
        SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)w);
    }
    SendMessageW(l, LB_SETTOPINDEX, (WPARAM)top, 0);
    if (sel >= 0) SendMessageW(l, LB_SETCURSEL, (WPARAM)sel, 0);
}
static INT_PTR CALLBACK ctl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        int tabs[3] = { 172, 228, 284 };                          /* dialog units: action | key | key | controller */
        SendDlgItemMessageW(h, ID_CTL_LIST, LB_SETTABSTOPS, 3, (LPARAM)tabs);
        ctl_fill(h);
        SendDlgItemMessageW(h, ID_CTL_LIST, LB_SETCURSEL, 0, 0);
        return TRUE;
    }
    case WM_COMMAND: {
        int sel = (int)SendDlgItemMessageW(h, ID_CTL_LIST, LB_GETCURSEL, 0, 0);
        switch (LOWORD(wp)) {
        case ID_CTL_LIST: if (HIWORD(wp) == LBN_DBLCLK && sel >= 0) { g_ctl_req = sel; g_ctl_req_what = 0; } return TRUE;
        case ID_CTL_K1: if (sel >= 0) { g_ctl_req = sel; g_ctl_req_what = 0; } return TRUE;
        case ID_CTL_K2: if (sel >= 0) { g_ctl_req = sel; g_ctl_req_what = 1; } return TRUE;
        case ID_CTL_PAD: if (sel >= 0) { g_ctl_req = sel; g_ctl_req_what = 2; } return TRUE;
        case ID_CTL_CLEAR:
            if (sel >= 0) {
                if (g_ctl_key[sel][1]) g_ctl_key[sel][1] = 0;
                else if (g_ctl_key[sel][0]) g_ctl_key[sel][0] = 0;
                else g_ctl_pad[sel] = CTL_NONE;
                controls_save(); ctl_fill(h);
            }
            return TRUE;
        case ID_CTL_DEFAULTS:
            if (MessageBoxW(h, L"Put every key and button back to how the game came?", L"Controls", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                controls_defaults(); controls_save(); ctl_fill(h);
            }
            return TRUE;
        case IDOK: case IDCANCEL: DestroyWindow(h); g_ctlw = NULL; return TRUE;
        }
        break;
    }
    case WM_CLOSE: DestroyWindow(h); g_ctlw = NULL; return TRUE;
    }
    return FALSE;
}
void controls_window_open(SDL_Window* win) {
    if (g_ctlw) { SetForegroundWindow(g_ctlw); return; }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    static WORD buf[4096];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE* d = (DLGTEMPLATE*)align4(buf);
    d->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    d->cdit = 10; d->cx = 400; d->cy = 290;
    WORD* p = (WORD*)(d + 1);
    *p++ = 0; *p++ = 0;
    p = put_str(p, L"Controls");
    *p++ = 9; p = put_str(p, L"Segoe UI");
    p = put_item(p, 0, 8, 6, 384, 18, 0xFFFF, 0x82, L"Pick an action, then change one of its keys or its controller button. A key or button that "
                 "another action has moves over to it (they swap). Esc and Alt+Enter, and the sticks, stay as they are.");
    p = put_item(p, 0, 8, 28, 170, 9, 0xFFFF, 0x82, L"Action");
    p = put_item(p, 0, 180, 28, 210, 9, 0xFFFF, 0x82, L"Key          Second key    Controller");
    p = put_item(p, WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_USETABSTOPS | LBS_NOINTEGRALHEIGHT, 8, 39, 384, 196, ID_CTL_LIST, 0x83, L"");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 8, 241, 72, 15, ID_CTL_K1, 0x80, L"Change key...");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 84, 241, 82, 15, ID_CTL_K2, 0x80, L"Second key...");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 170, 241, 92, 15, ID_CTL_PAD, 0x80, L"Controller button...");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 266, 241, 54, 15, ID_CTL_CLEAR, 0x80, L"Clear");
    p = put_item(p, WS_TABSTOP | BS_PUSHBUTTON, 8, 266, 100, 15, ID_CTL_DEFAULTS, 0x80, L"Restore defaults");
    p = put_item(p, WS_TABSTOP | BS_DEFPUSHBUTTON, 338, 266, 54, 15, IDOK, 0x80, L"Close");
    g_ctlw = CreateDialogIndirectParamW(GetModuleHandleW(NULL), d, info.info.win.window, ctl_proc, 0);
}
int controls_window_request(int* action, int* what) {
    if (g_ctl_req < 0) return 0;
    *action = g_ctl_req; *what = g_ctl_req_what; g_ctl_req = -1;
    return 1;
}
void controls_window_refresh(void) { if (g_ctlw) { ctl_fill(g_ctlw); SetForegroundWindow(g_ctlw); } }

int session_window_action(void) { int a = g_ses_action; g_ses_action = 0; return a; }

void session_window_close(void) {
    if (!g_ses) return;
    DestroyWindow(g_ses);
    g_ses = NULL;
}
#else
int choice_box(SDL_Window* win, const char* title, const char* text, const char* const* buttons, int n, int def, int cancel,
               const char* check, int* checked) { (void)win; (void)title; (void)text; (void)buttons; (void)n; (void)def; (void)cancel; (void)check; (void)checked; return -1; }
char* ask_text(SDL_Window* win, const char* title, const char* prompt) { (void)win; (void)title; (void)prompt; return NULL; }
float display_mm_per_pixel(SDL_Window* win) { (void)win; return 0; }
void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells) { (void)win; (void)shell_names; (void)n_shells; }
void menu_show(int on) { (void)on; }
void menu_init_second(SDL_Window* win) { (void)win; }
void menu_show_second(int on) { (void)on; }
void menu_check(int id, int on) { (void)id; (void)on; }
void menu_enable(int id, int on) { (void)id; (void)on; }
int menu_command(const SDL_Event* e) { (void)e; return 0; }
int session_window_open(SDL_Window* win) { (void)win; return 0; }
void volume_window_open(SDL_Window* win, int* volume, int* mute) { (void)win; (void)volume; (void)mute; }
void volume_window_refresh(void) {}
void controls_window_open(SDL_Window* win) { (void)win; }
int controls_window_request(int* action, int* what) { (void)action; (void)what; return 0; }
void controls_window_refresh(void) {}
void session_window_update(const SessionWindow* w) { (void)w; }
int session_window_action(void) { return 0; }
void session_window_close(void) {}
#endif
