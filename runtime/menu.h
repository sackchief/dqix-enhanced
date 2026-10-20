/* The window's menu bar (Windows): every setting in one place. The frontend
 * owns the settings; this file only builds the menu, reports which item was
 * chosen, and shows check marks. Elsewhere the functions do nothing. */
#pragma once
#include "SDL.h"

enum {
    MI_PAUSE = 100, MI_TURBO, MI_SHOT, MI_REPORT, MI_EXIT, MI_DLC,
    MI_CLASSIC = 110, MI_ENHANCED, MI_RES1, MI_RES2, MI_RES3, MI_RES4,
    MI_LCD = 120, MI_LCDCOL, MI_GRID, MI_GHOST, MI_BL1, MI_BL2, MI_BL3, MI_BL4,
    MI_SHELL = 130, MI_GAP, MI_ACTUAL, MI_SHELLC0,                      /* MI_SHELLC0 + colour index */
    MI_STACKED = 150, MI_SIDE, MI_SIZE1, MI_SIZE2, MI_SIZE3, MI_SIZE4, MI_INTEGER, MI_FULL,
    MI_CONTROLS = 170, MI_ABOUT, MI_UPDATE, MI_UPDATE_AUTO,
    MI_NET = 180, MI_SRV_ALTWFC, MI_SRV_WIIMMFI, MI_SRV_CUSTOM, MI_NET_HELP, MI_SRV_BUILTIN,
    MI_SES_HOST = 190, MI_SES_JOIN, MI_SES_START, MI_SES_LEAVE, MI_SES_NAME, MI_SES_HELP,
    MI_ANALOG = 200, MI_CORNER, MI_MODERN_HELP, MI_WIDE, MI_SWAP, MI_TWO,
    MI_FULL2 = 230, MI_POPOUT, MI_RES6, MI_RES8, MI_RES_AUTO, MI_SMOOTH,
    MI_OPACITY1 = 220,                                                  /* 100, 75, 50, 25% (+0..3) */
    MI_WIDE_DYN = 240, MI_SINGLE, MI_RESET, MI_VOLUME, MI_MUTE, MI_CONTROLS_EDIT,
    MI_CORNER0 = 250,                                                   /* + 0..3: top left, top right, bottom right, bottom left */
    MI_SSIZE0 = 254,                                                    /* + 0..2: small, medium, large */
    MI_SEND_REPORT = 210,
    MI_LANG1 = 270,                                                     /* English, French, German, Italian, Spanish (+0..4) */
    MI_TEXPACK = 280, MI_TEXDUMP, MI_TEXRELOAD, MI_HDTEXT, MI_SMOOTH2D, MI_TEXUP,
    MI_FONT0 = 290,                                                     /* HD text fonts (+0..5, see g_fonts in frontend_sdl.c) */
};

/* shell_names: the console colours, in order (MI_SHELLC0 + i) */
void menu_init(SDL_Window* win, const char* const* shell_names, int n_shells);
void menu_show(int on);                       /* hidden in fullscreen */
/* the second window (two windows): a bar with only Fullscreen (MI_FULL2) */
void menu_init_second(SDL_Window* win);
void menu_show_second(int on);
void menu_check(int id, int on);
void menu_enable(int id, int on);
int menu_command(const SDL_Event* e);         /* the chosen item, or 0 */

/* Size of one pixel of the monitor the window is on, in millimetres, from
 * the monitor's own description of itself (EDID). 0 if unknown. */
float display_mm_per_pixel(SDL_Window* win);

/* A box where the player types a description (bug reports). Returns the text
 * (UTF-8, caller frees) or NULL if the box was closed without saving or isn't
 * available on this system. */
char* ask_text(SDL_Window* win, const char* title, const char* prompt);

/* A message with buttons sized to their words (Windows), and optionally a
 * check box (check = its text, *checked = its state in and out). Returns the
 * chosen button (0..n-1), `cancel` when closed, or -1 when not available here
 * (the caller then shows an SDL message box). */
int choice_box(SDL_Window* win, const char* title, const char* text, const char* const* buttons, int n, int def, int cancel,
               const char* check, int* checked);

/* The session window (Windows): while players gather, a window of its own
 * with the address to give out (and a Copy button), who has joined, and
 * Start / Cancel. Elsewhere the lobby on the DS screens does it all. */
typedef struct {
    int host;                     /* this PC hosts */
    int can_start;                /* host: friends are in */
    int failed;                   /* the session couldn't start: only Back */
    char heading[96];             /* "Give your friends this address:" / "Connected to:" */
    char address[128];
    char hint[1000];
    char players[4][96];
    int n_players;
    char status[300];
} SessionWindow;
int session_window_open(SDL_Window* win);          /* 1 if it is showing */
void session_window_update(const SessionWindow* w);
int session_window_action(void);

/* Game > Volume...: a slider (0-100) and Mute, changing *volume / *mute live (Windows). */
void volume_window_open(SDL_Window* win, int* volume, int* mute);
void volume_window_refresh(void);                   /* the values changed elsewhere (keys) */
/* Game > Controls...: the list of actions and their keys (controls.h). When the
 * player asks to change one, controls_window_request() reports it once
 * (what: 0 key, 1 second key, 2 controller button); the game then waits for
 * the key in its own window and calls controls_window_refresh(). */
void controls_window_open(SDL_Window* win);
int controls_window_request(int* action, int* what);
void controls_window_refresh(void);                    /* 0 nothing, 1 start, 2 cancel (each reported once) */
void session_window_close(void);
