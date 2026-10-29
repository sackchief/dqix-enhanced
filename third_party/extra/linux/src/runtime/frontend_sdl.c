/* SDL2 frontend: a window with both screens, keyboard / mouse / gamepad
 * input, audio output and pacing to the DS frame rate (~59.83 fps).
 *
 * Keys (DS button <- keyboard):
 *   D-pad <- arrow keys     A <- X      B <- Z      X <- S      Y <- A
 *   L <- Q                  R <- W      START <- Enter         SELECT <- Right Shift / Backspace
 *   touch screen <- left mouse button on the bottom screen
 *   Tab = fast forward (on/off) F11 or Alt+Enter = fullscreen (mouse at the top edge shows the menu bar)
 *   F2 = layout: stacked, side by side, top screen in the corner, widescreen, widescreen dynamic (two windows)
 *   U = single screen (widescreen: hide the small one)   F9 = swap the screens (widescreen: hold to see the other)
 *   K / I / O = widescreen's small screen: corner, size, opacity    Ctrl+R = reset   - / = = volume
 *   Every key and controller button can be changed (controls.c, Game > Controls).
 *   F3 = window size 1x..4x       F4 = 3D resolution 1x..4x (Enhanced)  F5 = Classic / Enhanced
 *   F6 = DS Lite screen filter    F7 = DS Lite console around the screens   P = pause
 *   F8 = actual size (the screens as big as a real DS Lite's)
 *   F12 = screenshot (dqix-shot-N.bmp)
 *   F10 = bug report (reports/<date-time>/: screenshot, inputs, saves, log)
 *   Esc = leave fullscreen, or quit (it asks first; confirm_quit in settings.ini)
 * On Windows every setting is also in the menu bar (menu_win32 below).
 * A game controller works too (Nintendo layout by position: right face button = A). */
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32
#include <direct.h>                                         /* _mkdir */
#else
#include <sys/stat.h>                                       /* mkdir */
#endif
#include "nds.h"
#include "frontend.h"
#include "gl_render.h"
#include "discord.h"
#include "music.h"
#include "text_hd.h"
#include "menu.h"
#include "version.h"
#include "session.h"
#include "air.h"
#include "update.h"
#include "text.h"
#include "controls.h"
#ifndef _WIN32
#include "../../overlay/linux_overlay.h"
#include "../../overlay/linux_overlay_gl.h"
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define DS_FPS 59.8261

static SDL_Window* g_win;
static SDL_Renderer* g_ren;
static SDL_Texture* g_tex;
static SDL_AudioDeviceID g_audio;
static SDL_GameController* g_pad;
static int g_side_by_side, g_corner, g_scale = 2, g_fullscreen;   /* g_corner: the top screen small, beside the bottom one */
static int g_wide;                                                /* widescreen: the 3D screen wider than the DS's, the other small in its corner */
static float g_wide_mix = 1.0f;                                   /* widescreen: 1 = the field wide, 0 = beside the small screen (2D under its corner) */
static int g_small_blank;                                         /* widescreen: the small screen shows nothing (all black): not drawn */
static int g_swap;                                                /* the screens trade places (F9) */
static int g_two;                                                 /* two windows, one screen in each (two monitors) */
static SDL_Window* g_win2;                                        /* ...the second one (the top screen, or the bottom one when swapped) */
static int g_win2_full, g_win2_rect[4];                           /* ...fullscreen; where it was as a window (w = 0: not yet) */
static int g_wide_pop;                                            /* widescreen: the small screen in its own window (the field alone) */
static int g_smooth = 0;                                          /* Enhanced: smooth (filtered) textures */
static int g_discord = 1;                                         /* Modern > Show on Discord what I'm playing (discord.c) */
static char g_discord_app_id_override[64];
static int g_discord_app_id_override_set;
static int g_tex_pack = 1, g_tex_up = 0, g_tex_dump = 0;           /* Mods (gl_render.c) */
static int g_mus_pack = 1, g_mus_names = 0, g_mus_vol = 70;      /* Mods: music packs (music.c) */
static int g_hd_text = 0;                                            /* Enhanced: the game's letters redrawn at full resolution (text_hd.h) */
static int g_res_auto = 1;                                        /* 3D resolution: as sharp as the screen shows it (else g_scale3d) */
static int g_small_opacity = 50;                                  /* widescreen: how solid the small screen over the field is (O), % */
static int g_wide_dyn;                                            /* widescreen: 0 basic (the corner stays), 1 dynamic (experimental: the game's windows move right) */
static int g_small_corner;                                        /* basic widescreen: 0 top left, 1 top right, 2 bottom right, 3 bottom left */
static int g_small_size = 1;                                      /* ...0 small, 1 medium, 2 large */
static int g_small_hide;                                          /* widescreen: no small screen (left trigger / U) */
static int g_peek;                                                /* widescreen: the other screen big while the swap button is held */
static int g_single;                                              /* one screen only (the bottom one, or the top one swapped) */
static int g_hud = 0;                                             /* widescreen battles: the party's panels and the commands over the 3D (hud_update) */
static int g_hud_n, g_hud_band[8][2];                             /* ...the panels found on the commands screen: first row, end row */
static int g_hud_win[4];                                          /* ...or a window alone there (experience, level up): x, y, w, h (w 0: none) */
static int g_volume = 100, g_mute;                                /* Game > Volume */
static char g_notice[160]; static uint64_t g_notice_until;        /* a short message in the title bar */
static float g_small_alpha = 1.0f;                                /* ...as shown now (solid while the mouse is on it) */
static uint64_t g_next_frame, g_perf_freq;
static int g_fast;
static uint32_t g_fps_frames; static uint64_t g_fps_t0;
static int g_headless;          /* DQIX_NOWINDOW: run without a window (still uses SDL audio off) */
static int g_hidden;            /* another player's console in a session: no window, no sound, no pacing */
static int g_gl;                /* OpenGL renderer active (else SDL_Renderer + software 3D) */
static char g_gl_fallback_reason[320], g_sdl_window_failure[320];
static char g_sdl_renderer_accelerated_error[320], g_sdl_renderer_software_error[320], g_sdl_texture_failure[320];
static int g_gl_swap_interval_set_result = -2, g_gl_swap_interval_actual = -2;
static SDL_GLContext g_glctx;
static int g_scale3d = 2;
extern void (*g_report_capture)(uint32_t out[2][256 * 192]);
extern void (*g_report_perf)(FILE* f);
static void perf_write(FILE* f);
static int g_classic = 1;       /* Classic: the DS's own 3D renderer, exact; Enhanced: 3D on the GPU at 1-4x */
extern int g_comp_gl;
static int g_paused, g_turbo;   /* Game > Pause, Game > Fast forward (latched) */
#ifndef _WIN32
static int g_linux_overlay_open, g_linux_overlay_selection, g_linux_overlay_saved_paused;
static uint8_t *g_linux_overlay_rgba;
static SDL_Texture *g_linux_overlay_texture;
static int g_linux_overlay_error_reported;
#endif
static int g_ff_hold;           /* fast forward while Tab is held (0: Tab switches it on and off) */

/* How the screens are shown (View and Screen menus). The DS Lite parts need OpenGL. */
static struct {
    int lcd;                     /* DS Lite screen filter */
    int lcd_color, grid, ghost;  /* ... its LCD colours, pixel grid, motion blur */
    int backlight;               /* 1 (low) .. 4 (max) */
    int gap;                     /* the real distance between the screens */
    int shell;                   /* draw the console around the screens */
    int shell_color;             /* index into SHELLS */
    int integer;                 /* whole-number scaling only */
    int actual;                  /* screens as big as a real DS Lite's */
    float monitor_width_mm;      /* settings.ini override for the monitor's width (0 = ask the monitor) */
    int dlc;                     /* switch on the downloadable quests and special guests when the save loads */
} g_opt = { 1, 1, 1, 1, 4, 0, 1, 0, 0, 0, 0, 0 };   /* a new install: the DS Lite screen inside the console photo (settings.ini keeps each player's choice) */

/* Actual size: a DS Lite pixel is 0.238 mm (a 3-inch 4:3 screen, 256 pixels
 * across = 60.96 mm). Window pixels per DS pixel on the current monitor. */
#define DSLITE_MM_PER_PIXEL (60.96f / 256.0f)
static float g_actual_sc;        /* 0 = monitor size unknown */
static void update_actual(void) {
    g_actual_sc = 0;
    if (!g_win) return;
    float mm = 0;
    if (g_opt.monitor_width_mm > 0) {
        SDL_DisplayMode dm;
        if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(g_win), &dm) == 0 && dm.w > 0) mm = g_opt.monitor_width_mm / (float)dm.w;
    } else mm = display_mm_per_pixel(g_win);
    if (mm > 0) g_actual_sc = DSLITE_MM_PER_PIXEL / mm;
}

/* DS Lite colours: lid, base, buttons, hinge (sRGB). */
typedef struct { const char* name; const char* key; uint32_t lid, base, btn, hinge; } Shell;
static const Shell SHELLS[] = {
    { "Polar White", "white", 0xECECEA, 0xECECEA, 0xDCDCDA, 0xCFCFCC },
    { "Jet Black", "black", 0x222225, 0x222225, 0x2E2E32, 0x161618 },
    { "Ice Blue", "ice_blue", 0xBCD6E6, 0xBCD6E6, 0xE4EDF2, 0xA6C0D0 },
    { "Coral Pink", "coral_pink", 0xF0B7C1, 0xF0B7C1, 0xF6E1E5, 0xDBA0AB },
    { "Enamel Navy", "navy", 0x24314F, 0x24314F, 0x313E5F, 0x1A2440 },
    { "Crimson / Black", "crimson", 0xB21F2B, 0x222225, 0x2E2E32, 0x161618 },
};
#define N_DRAWN ((int)(sizeof SHELLS / sizeof SHELLS[0]))

/* Console photos: skins/<name>.png with skins/<name>.txt giving where the
 * screens are in the picture ("top x y w h", "bottom x y w h", optional
 * "name ..."). Listed after the drawn colours. */
#define MAX_SKINS 10
typedef struct { char file[200], name[64]; float top[4], bot[4]; int w, h; } Skin;
static Skin g_skins[MAX_SKINS];
static int g_nskins;
static int g_skin_loaded = -1;                 /* which photo is in the GPU */
#define N_SHELLS (N_DRAWN + g_nskins)

static void skin_add(const char* png) {
    if (g_nskins >= MAX_SKINS) return;
    Skin* k = &g_skins[g_nskins];
    memset(k, 0, sizeof *k);
    snprintf(k->file, sizeof k->file, "skins/%s", png);
    char txt[220]; snprintf(txt, sizeof txt, "%s", k->file);
    char* dot = strrchr(txt, '.'); if (!dot) return;
    strcpy(dot, ".txt");
    FILE* f = fopen(txt, "r");
    if (!f) return;
    char line[256]; int got = 0;
    snprintf(k->name, sizeof k->name, "%.*s (photo)", (int)(strrchr(png, '.') - png), png);
    while (fgets(line, sizeof line, f)) {
        float a, b, c, d;
        if (sscanf(line, " top %f %f %f %f", &a, &b, &c, &d) == 4) { k->top[0] = a; k->top[1] = b; k->top[2] = c; k->top[3] = d; got |= 1; }
        else if (sscanf(line, " bottom %f %f %f %f", &a, &b, &c, &d) == 4) { k->bot[0] = a; k->bot[1] = b; k->bot[2] = c; k->bot[3] = d; got |= 2; }
        else if (!strncmp(line, "name ", 5)) { snprintf(k->name, sizeof k->name, "%s", line + 5); k->name[strcspn(k->name, "\r\n")] = 0; }
    }
    fclose(f);
    if (got == 3 && k->top[2] > 0) g_nskins++;
}

#ifdef _WIN32
#include <io.h>
static void skins_scan(void) {
    struct _finddata_t fd; intptr_t h = _findfirst("skins/*.png", &fd);
    if (h == -1) return;
    do skin_add(fd.name); while (_findnext(h, &fd) == 0);
    _findclose(h);
}
#else
#include <dirent.h>
static void skins_scan(void) {
    DIR* d = opendir("skins");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) { size_t n = strlen(e->d_name); if (n > 4 && !strcmp(e->d_name + n - 4, ".png")) skin_add(e->d_name); }
    closedir(d);
}
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_WARNINGS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image_write.h"                                  /* public domain (Sean Barrett): the report's window.png */
#pragma GCC diagnostic pop

/* The photo for the chosen console colour (if it is one), in the GPU. */
static const Skin* current_skin(void) {
    int i = g_opt.shell_color - N_DRAWN;
    if (i < 0 || i >= g_nskins || !g_gl) return NULL;
    if (g_skin_loaded != i) {
        int w, h, n;
        uint8_t* px = stbi_load(g_skins[i].file, &w, &h, &n, 4);
        if (!px) { fprintf(stderr, "can't load %s\n", g_skins[i].file); return NULL; }
        gl_set_skin(px, w, h);
        stbi_image_free(px);
        g_skins[i].w = w; g_skins[i].h = h;
        g_skin_loaded = i;
    }
    return &g_skins[i];
}
static const float BACKLIGHT[5] = { 0, 0.36f, 0.56f, 0.78f, 1.0f };
static const char* BACKLIGHT_NAME[5] = { "", "low", "medium", "high", "max" };

/* Canvas (in DS pixels) for the current layout, and where the screens sit on it.
 * The DS Lite: 0.24 mm per pixel; the lid and base are 133 mm wide; the
 * screens are 90 pixels (21.5 mm) apart. */
/* ---------------------------------------------------------------- scenes with their own widescreen
 * Some scenes are built only for the DS's width (beyond it the set ends: the
 * new game's starry sky, the Observatory): widescreen shows them with black
 * sides. A few use both screens as one picture (the great tree's movie):
 * widescreen shows them stacked while they play. The scene is the map or the
 * movie the game is loading (card.c). Built-in list below; players add their
 * own in widescreen.txt next to the game ("bars map NAME", "stacked movie NAME"),
 * or with Modern > "Widescreen: this place with black sides". */
extern char g_scene_map[32], g_scene_movie[32];
extern uint64_t g_scene_movie_frame;
typedef struct { char what, kind; char name[30]; } SceneRule;   /* what: 'b' bars, 's' stacked; kind: 'm' map, 'v' movie */
static const SceneRule SCENE_RULES[] = {
    { 'b', 'm', "M01M12" },                                       /* the starry sky (a new game's start, the story's turns) */
    { 'b', 'm', "X01M01" },                                       /* the way up to the Observatory: a painted sky that ends */
    { 0, 0, "" }                                                  /* (end) */
};
static SceneRule g_user_rules[128]; static int g_n_user_rules = -1;
static void scene_rules_load(void) {
    g_n_user_rules = 0;
    FILE* f = fopen("widescreen.txt", "r"); if (!f) return;
    char line[200];
    while (fgets(line, sizeof line, f) && g_n_user_rules < 128) {
        char w[16], k[16], n[64];
        if (line[0] == '#' || sscanf(line, "%15s %15s %63s", w, k, n) != 3) continue;
        SceneRule r = { !strcmp(w, "stacked") ? 's' : !strcmp(w, "bars") ? 'b' : !strcmp(w, "wide") ? 'w' : 0, !strcmp(k, "movie") ? 'v' : !strcmp(k, "map") ? 'm' : 0, "" };
        if (!r.what || !r.kind) continue;
        snprintf(r.name, sizeof r.name, "%s", n);
        g_user_rules[g_n_user_rules++] = r;
    }
    fclose(f);
}
static int scene_movie_playing(void) { return g_scene_movie[0] && g_frame - g_scene_movie_frame < 90; }
static int scene_rule(void) {                                     /* 0, 'b' or 's' */
    if (g_n_user_rules < 0) scene_rules_load();
    for (int pass = 0; pass < 2; pass++) {                        /* a playing movie first, then the map */
        const char* name = pass == 0 ? (scene_movie_playing() ? g_scene_movie : NULL) : g_scene_map;
        char kind = pass == 0 ? 'v' : 'm';
        if (!name || !name[0]) continue;
        for (int i = 0; i < g_n_user_rules; i++) if (g_user_rules[i].kind == kind && !strcmp(g_user_rules[i].name, name)) return g_user_rules[i].what == 'w' ? 0 : g_user_rules[i].what;
        for (int i = 0; SCENE_RULES[i].what; i++) if (SCENE_RULES[i].kind == kind && !strcmp(SCENE_RULES[i].name, name)) return SCENE_RULES[i].what;
        /* A movie (the game's animated cutscenes) is one picture over both screens:
         * shown stacked, as on the DS, unless widescreen.txt says "wide movie NAME" */
        if (pass == 0) return 's';
    }
    return 0;
}
static int g_scene_stack;                                         /* this frame: show stacked (both screens are one picture) */
static int g_ui_plain;                                            /* dynamic widescreen: a menu screen of its own: the DS's picture as it is, 4:3 at the right */
static int g_hud_right;                                            /* the battle panels' right edge on the canvas (0: none) */
static uint64_t g_last_battle;                                    /* the frame the battle panels were last seen */
static void notice(const char* fmt, ...);
/* Modern > "Widescreen: black sides in this place": add or remove this map in widescreen.txt */
static void scene_toggle_bars(void) {
    if (g_n_user_rules < 0) scene_rules_load();
    if (!g_scene_map[0]) { notice("No place to remember here"); return; }
    int found = -1;
    for (int i = 0; i < g_n_user_rules; i++) if (g_user_rules[i].kind == 'm' && !strcmp(g_user_rules[i].name, g_scene_map)) found = i;
    if (found >= 0) { g_user_rules[found] = g_user_rules[--g_n_user_rules]; notice("Widescreen as usual here (%s)", g_scene_map); }
    else if (g_n_user_rules < 128) {
        SceneRule r = { 'b', 'm', "" }; snprintf(r.name, sizeof r.name, "%s", g_scene_map);
        g_user_rules[g_n_user_rules++] = r; notice("Black sides here from now on (%s)", g_scene_map);
    }
    FILE* f = fopen("widescreen.txt", "w"); if (!f) return;
    fprintf(f, "# Scenes with their own widescreen (Modern > Widescreen: black sides in this place).\n");
    fprintf(f, "# bars map NAME: black sides in that place; stacked movie NAME: both screens stacked while it plays\n# (movies are stacked anyway); wide movie NAME: that movie widescreen instead.\n");
    for (int i = 0; i < g_n_user_rules; i++)
        fprintf(f, "%s %s %s\n", g_user_rules[i].what == 's' ? "stacked" : "bars", g_user_rules[i].kind == 'v' ? "movie" : "map", g_user_rules[i].name);
    fclose(f);
}

/* Is the Alchemimicon (the recipe book, overlay 6) open? Its recipe list is on the
 * bottom screen: shown big instead (DQIX_NO_ALCHEMY_SWAP: tests, not). */
static int alchemimicon_open(void) {
    static int off = -1; if (off < 0) off = getenv("DQIX_NO_ALCHEMY_SWAP") != NULL;
    if (off) return 0;
    extern uint64_t overlays_loaded_mask(void); extern int gpu3d_poly_count(void);
    return ((overlays_loaded_mask() >> 6) & 1) && gpu3d_poly_count() == 0;   /* (not the Krak Pot, which has it too: its pot is 3D) */
}

/* Widescreen: DS pixels to add left and right of the 3D picture. 16:9 in a
 * window; in fullscreen, as wide as the monitor (up to 2:1 more). */
static int wide_side_target(void) {
    if (!(g_gl && g_wide && !g_side_by_side) || g_scene_stack) return 0;
    int side = 43;                                                /* (192 * 16 / 9 - 256) / 2 */
    if (g_fullscreen && g_win) {
        int w, h; SDL_GetWindowSize(g_win, &w, &h);
        if (h > 0) side = (int)lroundf(((float)w / (float)h * 192.0f - 256.0f) / 2.0f);
    }
    return side < 0 ? 0 : side > 128 ? 128 : side;
}

/* second: the layout of the second window (two windows) */
static void canvas_layout_for(GlPresent* v, int ww, int wh, int second) {
    memset(v, 0, sizeof *v);
    int two = g_gl && g_two && !g_side_by_side && !g_single;
    int wide = g_gl && g_wide && !g_side_by_side && !two && !g_scene_stack;
    int single = g_single && !wide;                               /* one screen (in widescreen the left trigger hides the small one instead) */
    int corner = g_gl && g_corner && !g_side_by_side && !wide && !two && !single;
    int shell = g_gl && g_opt.shell && !g_side_by_side && !corner && !wide && !two && !single && !(g_wide && g_scene_stack);   /* (a movie in widescreen: plain stacked, no console) */
    int side = 0, which = -1;
    if (g_gl) which = gl_wide_shown(&side);                       /* how the last picture was composed */
    int flip = alchemimicon_open();                               /* the Alchemimicon: its recipe list (the bottom screen) big */
    v->topx[0] = v->botx[0] = (float)side; v->topx[1] = v->botx[1] = 256;
    if (wide) {
        /* Modern: the screen with the 3D (the field, or the battle) fills the
         * window, wider than the DS's; the other screen sits small in a corner
         * over it, like a minimap (see-through: O).
         * Basic: in the corner the player picks (K) at the size they pick (I).
         * Dynamic (experimental): the map top left; the field never moves:
         * the game's windows move right instead (gpu3d.c, ui_shift_classify;
         * a screen without 3D sits at the right); in battles the party's
         * panels sit stacked in the bottom left (the battle HUD, always on),
         * cut to their names and HP/MP while the attacks play.
         * Left trigger / U hides the small screen; holding the swap button
         * shows it big in the middle (g_peek). */
        float S = (float)wide_side_target();
        int pop = g_wide_pop;                                     /* the small screen in the second window */
        int dyn = g_wide_dyn && !pop && !g_small_hide && !g_peek;
        float t = 1.0f;                                           /* (the field no longer slides: the game's windows move instead, gl_set_ui_shift) */
        v->cw = 256 + 2 * S; v->ch = 192;
        int topbig = (which == 0) != flip;
        float* big = topbig ? v->top : v->bot, *small = topbig ? v->bot : v->top;
        float* bigx = topbig ? v->topx : v->botx;
        float wx0 = dyn ? 2 * S : S, wtx0 = (float)side, ww = 256;   /* wide: the field (or, without 3D, the DS's picture: dynamic, at the right) */
        if (which >= 0 && !flip && !(dyn && g_ui_plain)) { wx0 = S - (float)side; wtx0 = 0; ww = 256 + 2 * (float)side; }   /* (a submenu: 4:3, at the right, black beside it) */
        float cx0 = v->cw - 256, ctx0 = (float)side, cww = 256;   /* corner: the DS's picture at the right */
        big[0] = cx0 + (wx0 - cx0) * t; big[2] = cww + (ww - cww) * t;
        bigx[0] = ctx0 + (wtx0 - ctx0) * t; bigx[1] = big[2];
        big[1] = 0; big[3] = 192;
        if (!g_wide_dyn) {
            static const float SW[3] = { 64, 256.0f / 3, 128 }, SH[3] = { 48, 64, 96 };
            int z = g_small_size < 0 || g_small_size > 2 ? 1 : g_small_size, c = g_small_corner & 3;
            small[2] = SW[z]; small[3] = SH[z];
            small[0] = c == 0 || c == 3 ? 2 : v->cw - small[2] - 2;
            small[1] = c < 2 ? 2 : 192 - small[3] - 2;
        } else if (which == 0 && !dyn) { small[2] = 128; small[3] = 96; small[0] = small[1] = 2; }   /* battles: top left, over the sky */
        else if (which == 0 && g_last_battle && g_frame - g_last_battle < 60 * 10) small[2] = small[3] = 0;   /* dynamic, battles: the panels only, no small screen */
        else { small[0] = 0; small[1] = 2; small[2] = 256.0f / 3; small[3] = 64; }   /* top left; the windows start right of it */
        if (g_small_blank && t > 0.99f) small[2] = small[3] = 0;   /* black (battle intros, fades): leave the field whole */
        v->front = small == v->bot;                               /* the small one in front */
        v->alpha = 1.0f + (g_small_alpha - 1.0f) * t;            /* see-through (O) while it is over the field */
        if (g_small_hide && !pop) small[2] = small[3] = 0;
        if (g_peek && !pop) { small[0] = (v->cw - 256) / 2; small[1] = 0; small[2] = 256; small[3] = 192; v->alpha = 1.0f; }   /* the other screen, whole */
        if (pop) {                                                /* first window: the field alone; second: the small screen, whole */
            v->alpha = 1.0f;
            if (second) { v->cw = 256; v->ch = 192; big[0] = big[1] = big[2] = big[3] = 0; small[0] = small[1] = 0; small[2] = 256; small[3] = 192; }
            else small[0] = small[1] = small[2] = small[3] = 0;
        }
        if (which == 0 && (g_hud_n > 0 || g_hud_win[2]) && !g_peek && !(g_small_hide && !pop)) {
            /* Battle HUD: the commands screen cut into its panels, over the 3D,
             * stacked in the bottom left at half size: the one whose turn it
             * is (the tall panel, with the commands) on top, the others below
             * it in the party's order.
             * The map window keeps the last map (gl_hold() in present_gl). */
            if (second) {
                if (pop) { v->held = 1; v->bot[0] = v->bot[1] = v->bot[2] = v->bot[3] = 0; v->top[0] = v->top[1] = 0; v->top[2] = 256; v->top[3] = 192; v->topx[0] = (float)side; v->topx[1] = 256; v->front = 0; }
            } else {
                small[0] = small[1] = small[2] = small[3] = 0;
                if (g_hud_win[2]) {                                   /* a window alone: big, at the top in the middle */
                    float* q = v->piece[v->npiece++], w = (float)g_hud_win[2], h = (float)g_hud_win[3], kw = fminf(1.0f, fminf(200.0f / w, 110.0f / h));
                    q[4] = 1; q[5] = (float)g_hud_win[0]; q[6] = (float)g_hud_win[1]; q[7] = w; q[8] = h;
                    q[2] = w * kw; q[3] = h * kw; q[0] = (v->cw - q[2]) / 2; q[1] = 6; q[9] = 0.7f;   /* (see-through like the panels) */
                }
                const float k = 0.5f, gap = 2, see = 0.7f;            /* half size; the fill 70% solid, white lines and letters solid */
                float total = 0;
                int tall = 0;
                for (int i = 0; i < g_hud_n; i++) { total += (float)(g_hud_band[i][1] - g_hud_band[i][0]) * k + gap; if (g_hud_band[i][1] - g_hud_band[i][0] >= 50) tall = 1; }
                float y = 192 - total;
                for (int pass = 0; pass < 2; pass++)              /* the tall one first */
                    for (int i = 0; i < g_hud_n && v->npiece + 2 <= GL_MAX_PIECES; i++) {
                        float h = (float)(g_hud_band[i][1] - g_hud_band[i][0]);
                        if ((h >= 50) == pass) continue;
                        float* q = v->piece[v->npiece++];
                        if (tall) {                                   /* choosing commands: the whole panels */
                            q[4] = 1; q[5] = 6; q[6] = (float)g_hud_band[i][0]; q[7] = 244; q[8] = h;
                            q[0] = 4; q[1] = y; q[2] = 244 * k; q[3] = h * k; q[9] = see;
                        } else {                                      /* the attacks: each panel's name and HP/MP only (its third box left out), */
                            q[4] = 1; q[5] = 6; q[6] = (float)g_hud_band[i][0]; q[7] = 113; q[8] = h;   /* with the panel's own right edge after them */
                            q[0] = 4; q[1] = y; q[2] = 113 * k; q[3] = h * k; q[9] = see;
                            float* e = v->piece[v->npiece++];
                            e[4] = 1; e[5] = 241; e[6] = q[6]; e[7] = 9; e[8] = h;
                            e[0] = q[0] + q[2]; e[1] = y; e[2] = 9 * k; e[3] = q[3]; e[9] = see;
                        }
                        y += q[3] + gap;
                    }
                if (!second) { g_hud_right = 0; for (int i = 0; i < v->npiece; i++) if (v->piece[i][1] > 96 && v->piece[i][0] + v->piece[i][2] > g_hud_right) g_hud_right = (int)(v->piece[i][0] + v->piece[i][2]); }
            }
        }
    } else if (two || single) {
        v->cw = 256; v->ch = 192;                                /* one screen; the other is in the other window, or not shown */
    } else if (corner) {
        /* Modern: the bottom screen (the field) as big as the window allows, the
         * top screen (map, party) a third of its size in the top left corner:
         * the canvas is 16:9 */
        v->cw = 256 + 256.0f / 3; v->ch = 192;
        v->top[0] = 0; v->top[1] = 0; v->bot[0] = 256.0f / 3; v->bot[1] = 0;
    } else if (g_side_by_side && !single) {
        v->cw = 512; v->ch = 192;
        v->top[0] = 0; v->bot[0] = 256; v->top[1] = v->bot[1] = 0;
    } else if (shell && current_skin()) {
        /* the photo: canvas units are DS pixels at the photo's top-screen scale */
        const Skin* k = current_skin();
        float kk = k->top[2] / 256.0f;
        v->cw = (float)k->w / kk; v->ch = (float)k->h / kk;
        v->top[0] = k->top[0] / kk; v->top[1] = k->top[1] / kk;
        v->bot[0] = k->bot[0] / kk; v->bot[1] = k->bot[1] / kk;
        v->skin = 1; v->skin_k = kk;
    } else if (shell) {
        v->cw = 560; v->ch = 630;
        v->top[0] = v->bot[0] = 152; v->top[1] = 78; v->bot[1] = 78 + 192 + 90;
    } else {
        int gap = g_gl && g_opt.gap ? 90 : 0;
        v->cw = 256; v->ch = (float)(384 + gap);
        v->top[0] = v->bot[0] = 0; v->top[1] = 0; v->bot[1] = (float)(192 + gap);
    }
    if (!wide) { v->top[2] = v->bot[2] = 256; v->top[3] = v->bot[3] = 192; }
    if (corner) { v->top[2] = 256.0f / 3; v->top[3] = 64; }
    if (two || single) {                                         /* this window's screen: the bottom one in the first, unless swapped */
        int sw = g_swap != flip;
        float* off = (second ? !sw : sw) ? v->bot : v->top;
        off[0] = off[1] = off[2] = off[3] = 0;
    } else if ((g_swap != flip) && !wide) {                                /* F9: the screens trade places */
        for (int i = 0; i < 4; i++) { float t = v->top[i]; v->top[i] = v->bot[i]; v->bot[i] = t; }
        for (int i = 0; i < 2; i++) { float t = v->topx[i]; v->topx[i] = v->botx[i]; v->botx[i] = t; }
    }
    v->win_w = ww; v->win_h = wh;
    float sc = fminf((float)ww / v->cw, (float)wh / v->ch);
    if (g_opt.actual && g_actual_sc > 0) { if (g_actual_sc < sc) sc = g_actual_sc; }   /* real size (smaller if it doesn't fit) */
    else if (g_opt.integer && sc >= 1.0f) sc = floorf(sc);
    v->sc = sc;
    v->ox = floorf(((float)ww - v->cw * sc) / 2); v->oy = floorf(((float)wh - v->ch * sc) / 2);
    v->shell = shell;
    v->lcd = g_gl && g_opt.lcd;
    v->lcd_color = g_opt.lcd_color; v->grid = g_opt.grid; v->ghost = g_opt.ghost;
    v->backlight = BACKLIGHT[g_opt.backlight];
    const Shell* sh = &SHELLS[g_opt.shell_color < N_DRAWN ? g_opt.shell_color : 1];
    uint32_t cols[4] = { sh->lid, sh->base, sh->btn, sh->hinge };
    float* dst[4] = { v->lid, v->base, v->btn, v->hinge };
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) dst[i][k] = (float)((cols[i] >> (16 - 8 * k)) & 255) / 255.0f;
}
static void canvas_layout(GlPresent* v, int ww, int wh) { canvas_layout_for(v, ww, wh, 0); }

/* ---------------------------------------------------------------- settings.ini
 * Remembers the player's choices between launches (next to the game). */
static const char* SETTINGS = "settings.ini";

/* Wireless: the community servers that replace Nintendo Wi-Fi Connection.
 * The DS finds a server through its DNS setting (firmware_set_ap), exactly
 * as players set it up by hand on a real DS. */
static const struct { const char* key; const char* dns; } WFC[] = {
    { "builtin", "10.64.0.2" },          /* wfc.c answers; the DNS is the virtual network's */
    { "altwfc", "172.104.88.237" },
    { "wiimmfi", "164.132.44.106" },
};
static char g_wfc_server[64] = "builtin";       /* a key above or a DNS address */
static char g_player_name[32] = "";              /* sessions: what friends see */
static int g_session_port = SESSION_PORT;
static int g_update_auto = 1;                    /* look for a new stable version at start */
static int g_confirm_quit = 1;                   /* Esc / closing the window asks first */
static char g_update_skip[32];                   /* ...but not this one (the player said so) */
static int g_net_want = 1;                     /* the menu's "connect" check: on for a new install (settings.ini keeps each player's choice) */
static const char* wfc_dns(void) {
    for (unsigned i = 0; i < sizeof WFC / sizeof WFC[0]; i++) if (!strcmp(g_wfc_server, WFC[i].key)) return WFC[i].dns;
    return g_wfc_server;
}
static int wfc_custom(void) {
    for (unsigned i = 0; i < sizeof WFC / sizeof WFC[0]; i++) if (!strcmp(g_wfc_server, WFC[i].key)) return 0;
    return 1;
}
static void wireless_apply(int boot) {
    g_wireless.online = g_net_want;
    g_wireless.builtin = !strcmp(g_wfc_server, "builtin");
    snprintf(g_wireless.dns, sizeof g_wireless.dns, "%s", wfc_dns());
    if (!boot) {
        firmware_set_ap(g_wireless.online, g_wireless.dns); net_enable(g_wireless.online);
        wfc_enable(g_wireless.online && g_wireless.builtin, NULL);
    }
}
/* every console has its own MAC address, and the servers tell players apart
 * by it: one is made up the first time (Nintendo's prefix, random rest) */
static void wireless_mac_new(void) {
    uint32_t r = (uint32_t)time(NULL) ^ (uint32_t)SDL_GetPerformanceCounter() ^ (uint32_t)(uintptr_t)&r;
    for (int i = 0; i < 8; i++) r = r * 1103515245u + 12345u;
    g_wireless.mac[0] = 0x00; g_wireless.mac[1] = 0x09; g_wireless.mac[2] = 0xBF;
    for (int i = 3; i < 6; i++) { r = r * 1103515245u + 12345u; g_wireless.mac[i] = (uint8_t)(r >> 16); }
    g_wireless.mac_set = 1;
}
/* the console around the screens until the player picks another: the photo of a real DS Lite */
static int g_color_chosen;
static void default_console_color(void) {
    for (int i = 0; i < g_nskins; i++) if (!strcmp(g_skins[i].file + 6, "dslite_black.png")) g_opt.shell_color = N_DRAWN + i;
}
/* settings.ini's names for g_fw_language (1-5), and the menu's */
static const char* const LANGUAGE_KEY[6] = { "", "english", "french", "german", "italian", "spanish" };
static const char* const LANGUAGE_NAME[6] = { "", "English", "Fran\xc3\xa7" "ais", "Deutsch", "Italiano", "Espa\xc3\xb1ol" };
static void settings_load(void) {
    FILE* f = fopen(SETTINGS, "r");
    if (!f) return;
    char line[200], color[64] = "";
    int seen_rs = 0;                                              /* render_scale (v29) wins over the older resolution_3d */
    int opacity_chosen = 0, opacity_saved = 0;                    /* v31: the small screen's opacity counts once picked (before, 100 was saved either way) */
    while (fgets(line, sizeof line, f)) {
        char key[64]; char val[64];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
        if (!strcmp(key, "player_name")) {                       /* the whole rest of the line (may have spaces) */
            const char* v = strchr(line, '=') + 1; while (*v == ' ') v++;
            snprintf(g_player_name, sizeof g_player_name, "%s", v);
            g_player_name[strcspn(g_player_name, "\r\n")] = 0;
            continue;
        }
        if (!strcmp(key, "update_check")) { g_update_auto = atoi(val) != 0; continue; }
        if (!strcmp(key, "confirm_quit")) { g_confirm_quit = atoi(val) != 0; continue; }
        if (!strcmp(key, "fast_forward")) { g_ff_hold = !strcmp(val, "hold"); continue; }
        if (!strcmp(key, "update_skip")) { snprintf(g_update_skip, sizeof g_update_skip, "%s", val); continue; }
        if (!strcmp(key, "session_port")) { int v = atoi(val); if (v > 0 && v < 65536) g_session_port = v; continue; }
        if (!strcmp(key, "session_consoles")) { if (!strcmp(val, "processes")) { static char e[] = "DQIX_PROCESSES=1"; putenv(e); } continue; }
        if (!strcmp(key, "mode")) g_classic = strcmp(val, "enhanced") != 0;
        else if (!strcmp(key, "resolution_3d")) { int v = atoi(val); if (v >= 1 && v <= 4) { g_scale3d = v; if (!seen_rs) g_res_auto = v == 4; } }   /* before v29: 4 was the most (now Auto) */
        else if (!strcmp(key, "render_scale")) { seen_rs = 1; int v = atoi(val); g_res_auto = !strcmp(val, "auto") || v < 1; if (!g_res_auto) g_scale3d = v > GL_MAX_SCALE ? GL_MAX_SCALE : v; }
        else if (!strcmp(key, "window_scale")) { int v = atoi(val); if (v >= 1 && v <= 8) g_scale = v; }
        else if (!strcmp(key, "layout")) { g_side_by_side = !strcmp(val, "side_by_side"); g_corner = !strcmp(val, "corner"); g_wide = !strcmp(val, "wide") || !strcmp(val, "wide_dynamic"); g_wide_dyn = !strcmp(val, "wide_dynamic"); g_two = !strcmp(val, "two_windows"); }
        else if (!strcmp(key, "single_screen")) g_single = atoi(val) != 0;
        else if (!strcmp(key, "small_screen_corner")) g_small_corner = atoi(val) & 3;
        else if (!strcmp(key, "small_screen_size")) { int v = atoi(val); if (v >= 0 && v <= 2) g_small_size = v; }
        else if (!strcmp(key, "small_screen_hidden")) g_small_hide = atoi(val) != 0;
        else if (!strcmp(key, "small_screen_opacity_chosen")) opacity_chosen = atoi(val) != 0;
        else if (!strcmp(key, "volume")) { int v = atoi(val); if (v >= 0 && v <= 100) g_volume = v; }
        else if (!strcmp(key, "mute")) g_mute = atoi(val) != 0;
        else if (!strcmp(key, "swap_screens")) g_swap = atoi(val) != 0;
        else if (!strcmp(key, "widescreen_popout")) g_wide_pop = atoi(val) != 0;
        else if (!strcmp(key, "widescreen_battle_hud")) g_hud = atoi(val) != 0;
        else if (!strcmp(key, "smooth_textures")) g_smooth = atoi(val) != 0;
        else if (!strcmp(key, "outlines")) g_gl_outline = !strcmp(val, "medium") ? 2 : !strcmp(val, "ds") ? 0 : 1;
        else if (!strcmp(key, "soft_shadows")) g_gl_soft_alpha = atoi(val) != 0;
        else if (!strcmp(key, "smooth_shading")) g_gl_soft_shade = atoi(val) != 0;
        else if (!strcmp(key, "hd_text")) g_hd_text = atoi(val) != 0;
        else if (!strcmp(key, "texture_packs")) g_tex_pack = atoi(val) != 0;
        else if (!strcmp(key, "music_packs")) g_mus_pack = atoi(val) != 0;
        else if (!strcmp(key, "music_song_names")) g_mus_names = atoi(val) != 0;
        else if (!strcmp(key, "music_pack_volume")) { int v = atoi(val); if (v >= 0 && v <= 200) g_mus_vol = v; }
        else if (!strcmp(key, "discord")) g_discord = atoi(val) != 0;
        else if (!strcmp(key, "discord_app_id")) {
            snprintf(g_discord_app_id_override, sizeof g_discord_app_id_override, "%s", val);
            g_discord_app_id_override_set = 1;
            discord_set_app_id(val);
        }
        else if (!strcmp(key, "upscale_textures")) g_tex_up = atoi(val) != 0;
        else if (!strcmp(key, "hd_text_shadows")) g_tx_shadows = atoi(val) != 0;
        else if (!strcmp(key, "small_screen_opacity")) { int v = atoi(val); if (v >= 10 && v <= 100) opacity_saved = v; }
        else if (!strcmp(key, "second_window")) { int r[4]; if (sscanf(val, "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) == 4 && r[2] > 0 && r[3] > 0) memcpy(g_win2_rect, r, sizeof r); }
        else if (!strcmp(key, "second_window_fullscreen")) g_win2_full = atoi(val) != 0;
        else if (!strcmp(key, "walk_any_direction")) g_analog_walk = atoi(val) != 0;   /* (the older analog_walk, off by default then, is ignored) */
        else if (!strcmp(key, "fullscreen")) g_fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "dslite_filter")) g_opt.lcd = atoi(val) != 0;
        else if (!strcmp(key, "lcd_colors")) g_opt.lcd_color = atoi(val) != 0;
        else if (!strcmp(key, "pixel_grid")) g_opt.grid = atoi(val) != 0;
        else if (!strcmp(key, "motion_blur")) g_opt.ghost = atoi(val) != 0;
        else if (!strcmp(key, "backlight")) { for (int i = 1; i <= 4; i++) if (!strcmp(val, BACKLIGHT_NAME[i])) g_opt.backlight = i; }
        else if (!strcmp(key, "screen_gap")) g_opt.gap = atoi(val) != 0;
        else if (!strcmp(key, "console")) g_opt.shell = atoi(val) != 0;
        else if (!strcmp(key, "console_color")) snprintf(color, sizeof color, "%s", val);
        else if (!strcmp(key, "console_color_chosen")) g_color_chosen = atoi(val) != 0;
        else if (!strcmp(key, "integer_scaling")) g_opt.integer = atoi(val) != 0;
        else if (!strcmp(key, "actual_size")) g_opt.actual = atoi(val) != 0;
        else if (!strcmp(key, "monitor_width_mm")) g_opt.monitor_width_mm = (float)atof(val);
        else if (!strcmp(key, "downloadable_content")) g_opt.dlc = atoi(val) != 0;
        else if (!strcmp(key, "language")) { for (int i = 1; i <= 5; i++) if (!strcmp(val, LANGUAGE_KEY[i])) g_fw_language = i; }
        else if (!strcmp(key, "wireless")) g_net_want = atoi(val) != 0;
        else if (!strcmp(key, "wfc_server")) snprintf(g_wfc_server, sizeof g_wfc_server, "%s", val);
        else if (!strcmp(key, "wireless_mac")) {
            unsigned b[6];
            if (sscanf(val, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
                for (int i = 0; i < 6; i++) g_wireless.mac[i] = (uint8_t)b[i];
                g_wireless.mac_set = 1;
            }
        }
    }
    fclose(f);
    if (opacity_chosen && opacity_saved) g_small_opacity = opacity_saved;
    /* the colour counts once the player picked one (before nightly5 it was saved either way) */
    if (g_color_chosen) {
        for (int i = 0; i < N_DRAWN; i++) if (!strcmp(color, SHELLS[i].key)) g_opt.shell_color = i;
        for (int i = 0; i < g_nskins; i++) if (!strcmp(color, g_skins[i].file + 6)) g_opt.shell_color = N_DRAWN + i;   /* a photo, by file name */
    }
}
static void settings_save(void) {
    if (g_headless || getenv("DQIX_NO_SETTINGS")) return;
    FILE* f = fopen(SETTINGS, "w");
    if (!f) return;
    fprintf(f, "# Dragon Quest IX (PC) settings. The game saves this when you change something.\n");
    fprintf(f, "# mode: classic (exactly like the DS) or enhanced (sharper 3D on the graphics card). F5 switches.\n");
    fprintf(f, "mode = %s\n", g_classic ? "classic" : "enhanced");
    fprintf(f, "# 3D resolution in enhanced mode: 1 to 12, or auto (as sharp as the window shows it; F4)\n");
    if (g_res_auto) fprintf(f, "render_scale = auto\n"); else fprintf(f, "render_scale = %d\n", g_scale3d);
    fprintf(f, "resolution_3d = %d\n", g_res_auto || g_scale3d > 4 ? 4 : g_scale3d);   /* for versions before v29 */
    fprintf(f, "# enhanced: smooth textures (filtered) instead of the DS's blocky texels: 0 or 1\n");
    fprintf(f, "smooth_textures = %d\n", g_smooth);
    fprintf(f, "# enhanced above 1x: character outlines thin, medium or ds (the DS's own rule, thicker)\n");
    fprintf(f, "outlines = %s\n", g_gl_outline == 2 ? "medium" : g_gl_outline == 0 ? "ds" : "thin");
    fprintf(f, "# enhanced above 1x: shadows, glows and light with soft edges as a smooth gradient: 0 or 1\n");
    fprintf(f, "soft_shadows = %d\n", g_gl_soft_alpha != 0);
    fprintf(f, "# enhanced above 1x: the shading on characters as a smooth ramp instead of the DS's steps: 0 or 1\n");
    fprintf(f, "smooth_shading = %d\n", g_gl_soft_shade != 0);
    fprintf(f, "# enhanced: HD text, the game's letters drawn again at full resolution from fonts/main.ttf and fonts/small.ttf: 0 or 1 (F1)\n");
    fprintf(f, "hd_text = %d\n", g_hd_text);
    fprintf(f, "# show on Discord what you're playing (when the Discord app is running): 0 or 1\n");
    fprintf(f, "discord = %d\n", g_discord);
    /* Preserve an explicit Discord application ID when runtime settings are saved. */
    if (g_discord_app_id_override_set)
        fprintf(f, "discord_app_id = %s\n", g_discord_app_id_override);
    fprintf(f, "# Mods: texture packs (PNGs in the textures folder drawn instead of the game's): 0 or 1\n");
    fprintf(f, "texture_packs = %d\n", g_tex_pack);
    fprintf(f, "# Mods: music packs (OGG / FLAC recordings in the music folder played instead of the game's songs): 0 or 1\n");
    fprintf(f, "music_packs = %d\n", g_mus_pack);
    fprintf(f, "# Mods: a notice with each song's name as it starts (to find which file replaces it): 0 or 1\n");
    fprintf(f, "music_song_names = %d\n", g_mus_names);
    fprintf(f, "# Mods: how loud the recordings are against the game's sounds, in %% (0 to 200)\n");
    fprintf(f, "music_pack_volume = %d\n", g_mus_vol);
    fprintf(f, "# Mods: the 3D textures upscaled 4x with xBR: 0 or 1\n");
    fprintf(f, "upscale_textures = %d\n", g_tex_up);
    fprintf(f, "# HD text: keep the letters' shadows and outlines (0: plain letters)\n");
    fprintf(f, "hd_text_shadows = %d\n", g_tx_shadows);
    fprintf(f, "# widescreen: the small screen (map, battle commands) in a window of its own: 0 or 1\n");
    fprintf(f, "widescreen_popout = %d\n", g_wide_pop);
    fprintf(f, "# widescreen battles (experimental): the party's panels and the commands stacked in the bottom left, over the 3D: 0 or 1\n");
    fprintf(f, "widescreen_battle_hud = %d\n", g_hud);
    fprintf(f, "# window size: 1 to 8 times the DS screens (F3)\n");
    fprintf(f, "window_scale = %d\n", g_scale);
    fprintf(f, "# stacked, side_by_side, corner (the top screen small beside the field), wide (Basic Widescreen),\n");
    fprintf(f, "# wide_dynamic (Dynamic Widescreen, experimental: the game's windows moved right of the map) or two_windows (one screen in each); F2\n");
    fprintf(f, "layout = %s\n", g_side_by_side ? "side_by_side" : g_wide ? (g_wide_dyn ? "wide_dynamic" : "wide") : g_corner ? "corner" : g_two ? "two_windows" : "stacked");
    fprintf(f, "# one screen only (U / left trigger): 0 or 1\n");
    fprintf(f, "single_screen = %d\n", g_single);
    fprintf(f, "# widescreen's small screen: corner 0 top left, 1 top right, 2 bottom right, 3 bottom left (K / right stick click);\n");
    fprintf(f, "# size 0 small, 1 medium, 2 large (I); hidden 0 or 1 (U / left trigger)\n");
    fprintf(f, "small_screen_corner = %d\n", g_small_corner);
    fprintf(f, "small_screen_size = %d\n", g_small_size);
    fprintf(f, "small_screen_hidden = %d\n", g_small_hide);
    fprintf(f, "# volume 0 to 100 (- and =), and mute\n");
    fprintf(f, "volume = %d\n", g_volume);
    fprintf(f, "mute = %d\n", g_mute);
    fprintf(f, "# the screens trade places (F9): 0 or 1\n");
    fprintf(f, "swap_screens = %d\n", g_swap);
    fprintf(f, "# widescreen: how solid the small screen over the field is, in %% (O / left stick click): 100, 75, 50 or 25\n");
    fprintf(f, "small_screen_opacity = %d\n", g_small_opacity);
    fprintf(f, "small_screen_opacity_chosen = 1\n");
    fprintf(f, "# two windows: where the second one goes (x,y,width,height) and whether it is fullscreen\n");
    fprintf(f, "second_window = %d,%d,%d,%d\n", g_win2_rect[0], g_win2_rect[1], g_win2_rect[2], g_win2_rect[3]);
    fprintf(f, "second_window_fullscreen = %d\n", g_win2_full);
    fprintf(f, "# Modern: the left stick walks in any direction, like the stylus (0 or 1)\n");
    fprintf(f, "walk_any_direction = %d\n", g_analog_walk);
    fprintf(f, "# 0 or 1 (F11)\n");
    fprintf(f, "fullscreen = %d\n", g_fullscreen);
    fprintf(f, "# DS Lite screen filter (F6) and its parts: 0 or 1; backlight: low, medium, high, max\n");
    fprintf(f, "dslite_filter = %d\n", g_opt.lcd);
    fprintf(f, "lcd_colors = %d\n", g_opt.lcd_color);
    fprintf(f, "pixel_grid = %d\n", g_opt.grid);
    fprintf(f, "motion_blur = %d\n", g_opt.ghost);
    fprintf(f, "backlight = %s\n", BACKLIGHT_NAME[g_opt.backlight]);
    fprintf(f, "# the real gap between the screens; the DS Lite drawn around them (F7) and its colour:\n");
    fprintf(f, "# white, black, ice_blue, coral_pink, navy, crimson, or a photo in the skins folder by file name\n");
    fprintf(f, "screen_gap = %d\n", g_opt.gap);
    fprintf(f, "console = %d\n", g_opt.shell);
    fprintf(f, "console_color_chosen = %d\n", g_color_chosen);
    fprintf(f, "console_color = %s\n", g_opt.shell_color < N_DRAWN ? SHELLS[g_opt.shell_color].key : g_skins[g_opt.shell_color - N_DRAWN].file + 6);
    fprintf(f, "# whole-number scaling only (every DS pixel the same size): 0 or 1\n");
    fprintf(f, "integer_scaling = %d\n", g_opt.integer);
    fprintf(f, "# screens as big as a real DS Lite's (F8): 0 or 1. The size of your monitor is read from the\n");
    fprintf(f, "# monitor itself; if it comes out wrong, put its visible width in millimetres here (0 = automatic)\n");
    fprintf(f, "actual_size = %d\n", g_opt.actual);
    fprintf(f, "monitor_width_mm = %g\n", g_opt.monitor_width_mm);
    fprintf(f, "# switch on every downloadable quest and special guest at start, without connecting: 0 or 1\n");
    fprintf(f, "downloadable_content = %d\n", g_opt.dlc);
    fprintf(f, "# the game's language (European version; the USA version is in English): english, french, german, italian or spanish\n");
    fprintf(f, "language = %s\n", LANGUAGE_KEY[g_fw_language >= 1 && g_fw_language <= 5 ? g_fw_language : 1]);
    fprintf(f, "# Wireless menu: connect to a community Nintendo Wi-Fi Connection server: 0 or 1\n");
    fprintf(f, "wireless = %d\n", g_net_want);
    fprintf(f, "# which server: builtin (offline, the files in the wfc folder), altwfc, wiimmfi, or another's DNS address\n");
    fprintf(f, "wfc_server = %s\n", g_wfc_server);
    fprintf(f, "# playing with friends far away: your name, and the port friends connect to when you host\n");
    fprintf(f, "player_name = %s\n", g_player_name);
    fprintf(f, "# updates: look for a new stable version when the game starts (Help menu); a version not to offer again\n");
    fprintf(f, "update_check = %d\n", g_update_auto);
    fprintf(f, "# Esc or closing the window asks before quitting (0: quits at once)\n");
    fprintf(f, "confirm_quit = %d\n", g_confirm_quit);
    fprintf(f, "# Tab (fast forward): toggle (press once on, once off) or hold\n");
    fprintf(f, "fast_forward = %s\n", g_ff_hold ? "hold" : "toggle");
    fprintf(f, "update_skip = %s\n", g_update_skip[0] ? g_update_skip : "-");
    fprintf(f, "session_port = %d\n", g_session_port);
    fprintf(f, "# how your friends' DSes run on this PC: thread (one thread, the usual) or processes (if thread misbehaves)\n");
    fprintf(f, "session_consoles = %s\n", getenv("DQIX_PROCESSES") ? "processes" : "thread");
    fprintf(f, "# this copy's own MAC address (made up the first time; the servers know you by it)\n");
    if (g_wireless.mac_set)
        fprintf(f, "wireless_mac = %02x:%02x:%02x:%02x:%02x:%02x\n", g_wireless.mac[0], g_wireless.mac[1],
                g_wireless.mac[2], g_wireless.mac[3], g_wireless.mac[4], g_wireless.mac[5]);
    fclose(f);
}

/* Classic or Enhanced: which 3D renderer runs. Without OpenGL 3.3 only Classic exists. */
static void apply_mode(void) {
    if (!g_gl) g_classic = 1;
    if (g_classic) {
        g_renderer3d = NULL; g_comp_gl = 0; g_report_capture = NULL;
    } else {
        g_renderer3d = gl_renderer3d(); g_comp_gl = 1; g_report_capture = gl_read_screens;
    }
}

/* Audio drives the pacing: the game runs ~100 ms ahead of what is being
 * heard, so a slow frame only eats into that cushion instead of causing a
 * dropout. If the buffer still runs dry, output pauses until it refills
 * (a clean gap rather than stutter). */
#define AUDIO_TARGET (32768u / 10)        /* stereo frames queued ahead (~100 ms) */
static volatile int g_audio_priming = 1;

static void audio_cb(void* u, Uint8* stream, int len) {
    (void)u;
    int16_t* out = (int16_t*)stream;
    uint32_t frames = (uint32_t)len / 4;
    if (g_audio_priming) {
        if (sound_buffered() < AUDIO_TARGET / 2) { memset(stream, 0, (size_t)len); return; }
        g_audio_priming = 0;
    }
    uint32_t got = sound_read(out, frames);
    if (got < frames) {                                   /* ran dry: fade the tail, then wait to refill */
        int16_t l = got ? out[(got - 1) * 2] : 0, r = got ? out[(got - 1) * 2 + 1] : 0;
        for (uint32_t i = got; i < frames; i++) {
            int k = (int)(frames - i);
            out[i * 2] = (int16_t)(l * k / (int)(frames - got + 1)); out[i * 2 + 1] = (int16_t)(r * k / (int)(frames - got + 1));
        }
        g_audio_priming = 1;
    }
    int v = g_mute ? 0 : g_volume;                        /* Game > Volume: by ear (loudness follows the square) */
    if (v < 100) {
        int32_t gain = v * v * 65536 / 10000;               /* 16.16 */
        for (uint32_t i = 0; i < frames * 2; i++) out[i] = (int16_t)(((int32_t)out[i] * gain) >> 16);
    }
}

/* Window size = canvas x scale, made smaller if it would not fit on the screen. */
static void menu_sync(void);
/* keep: the layout changed (swap, single screen, another layout): keep the
 * size the player gave the window - the same if the picture keeps its shape,
 * else the new shape at the same zoom. Otherwise (window size, actual size,
 * the start) the chosen size. */
static float g_last_cw, g_last_ch;                              /* the canvas the window was last sized for */
static void layout_window_keep(int keep) {
    GlPresent v; canvas_layout(&v, 1, 1);
    int w = (int)v.cw, h = (int)v.ch;
    if (!g_gl) SDL_RenderSetLogicalSize(g_ren, w, h);
    float lw = g_last_cw, lh = g_last_ch;
    g_last_cw = v.cw; g_last_ch = v.ch;
    if (g_fullscreen) return;
    float s = g_opt.actual && g_actual_sc > 0 ? g_actual_sc : (float)g_scale;
    if (keep && lw > 0 && lh > 0) {
        if (fabsf(lw - v.cw) < 0.5f && fabsf(lh - v.ch) < 0.5f) return;   /* same shape: nothing to do */
        int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
        float cur = fminf((float)ww / lw, (float)wh / lh);       /* the zoom the window has now */
        if (cur > 0.25f) s = cur;
    }
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(g_win), &r) == 0) {
        float fit = fminf((float)(r.w - 16) / (float)w, (float)(r.h - 64) / (float)h);   /* room for the title and menu bars */
        if (s > fit) s = fit;
    }
    SDL_SetWindowSize(g_win, (int)ceilf((float)w * s), (int)ceilf((float)h * s));
}
static void layout_window(void) { layout_window_keep(0); }

/* OpenGL: a 3.3 core context; on any failure fall back to SDL_Renderer + software 3D. */
static int try_gl(void) {
    g_gl_fallback_reason[0]=g_sdl_window_failure[0]=0;
    g_sdl_renderer_accelerated_error[0]=g_sdl_renderer_software_error[0]=g_sdl_texture_failure[0]=0;
    g_gl_swap_interval_set_result=g_gl_swap_interval_actual=-2;
    if(getenv("DQIX_SOFTWARE")) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"DQIX_SOFTWARE is set; OpenGL was deliberately skipped");
        return 0;
    }
    SDL_ClearError();
    if(SDL_GL_LoadLibrary(NULL)!=0) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"SDL_GL_LoadLibrary(default) failed: %.220s",SDL_GetError());
        fprintf(stderr,"renderer-diagnostics: OpenGL library load failed: %s\n",g_gl_fallback_reason);
        return 0;
    }
    SDL_ClearError();
    if(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3)!=0) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"SDL_GL_SetAttribute(major version) failed: %.220s",SDL_GetError()); SDL_GL_UnloadLibrary(); return 0;
    }
    if(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3)!=0) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"SDL_GL_SetAttribute(minor version) failed: %.220s",SDL_GetError()); SDL_GL_UnloadLibrary(); return 0;
    }
    if(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE)!=0) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"SDL_GL_SetAttribute(core profile) failed: %.220s",SDL_GetError()); SDL_GL_UnloadLibrary(); return 0;
    }
    if(SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1)!=0) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"SDL_GL_SetAttribute(double buffer) failed: %.220s",SDL_GetError()); SDL_GL_UnloadLibrary(); return 0;
    }
    SDL_ClearError();
    g_win=SDL_CreateWindow("Dragon Quest IX",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,256*g_scale,384*g_scale,
                           SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if(!g_win) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"SDL_CreateWindow(SDL_WINDOW_OPENGL) failed: %.220s",SDL_GetError());
        fprintf(stderr,"renderer-diagnostics: OpenGL window creation failed: %s\n",g_gl_fallback_reason);
        SDL_GL_UnloadLibrary();
        return 0;
    }
    SDL_ClearError(); g_glctx=SDL_GL_CreateContext(g_win);
    if(!g_glctx) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"SDL_GL_CreateContext(OpenGL 3.3 core) failed: %.220s",SDL_GetError());
        fprintf(stderr,"renderer-diagnostics: OpenGL context creation failed: %s\n",g_gl_fallback_reason);
        SDL_DestroyWindow(g_win);g_win=NULL;return 0;
    }
    g_gl_swap_interval_set_result=SDL_GL_SetSwapInterval(0);
    g_gl_swap_interval_actual=SDL_GL_GetSwapInterval();
    const char* e=getenv("DQIX_SCALE3D"); if(e){g_scale3d=atoi(e);g_res_auto=0;}
    if(getenv("DQIX_SMOOTH"))g_smooth=atoi(getenv("DQIX_SMOOTH"))!=0;
    if(getenv("DQIX_OUTLINE"))g_gl_outline=atoi(getenv("DQIX_OUTLINE"));
    if(getenv("DQIX_SOFTALPHA"))g_gl_soft_alpha=atoi(getenv("DQIX_SOFTALPHA"));
    if(getenv("DQIX_SOFTSHADE"))g_gl_soft_shade=atoi(getenv("DQIX_SOFTSHADE"));
    gl_set_smooth(g_smooth);
    if(getenv("DQIX_HDTEXT"))g_hd_text=atoi(getenv("DQIX_HDTEXT"))!=0;
    gl_hd_fonts(getenv("DQIX_TXFONTS")?getenv("DQIX_TXFONTS"):"fonts");
    gl_set_hd_text(g_hd_text);gl_set_texture_pack(g_tex_pack);gl_set_tex_upscale(g_tex_up);
    SDL_ClearError();
    if(!gl_render_init(g_res_auto?(getenv("DQIX_AUTO_FROM")?2:4):g_scale3d)) {
        const char* error=SDL_GetError();
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"gl_render_init failed (SDL_GetError=%s)",error&&error[0]?error:"no SDL error reported");
        fprintf(stderr,"renderer-diagnostics: OpenGL renderer initialization failed: %s\n",g_gl_fallback_reason);
        SDL_GL_DeleteContext(g_glctx);g_glctx=NULL;SDL_DestroyWindow(g_win);g_win=NULL;return 0;
    }
    g_gl=1;g_renderer3d_verify=getenv("DQIX_GL_VERIFY")!=NULL;return 1;
}
static int try_gl_with_x11_retry(void) {
    const char* selected_driver=SDL_GetCurrentVideoDriver();
    const char* requested_driver=getenv("SDL_VIDEODRIVER");
    const char* x_display=getenv("DISPLAY");
    char original_driver[64], wayland_reason[320], x11_reason[320];
    static const char window_failure[]="SDL_CreateWindow(SDL_WINDOW_OPENGL) failed";

    if(try_gl()) return 1;
    if(strncmp(g_gl_fallback_reason,window_failure,sizeof window_failure-1)!=0 ||
       !selected_driver || strcmp(selected_driver,"wayland")!=0 ||
       (requested_driver&&requested_driver[0]) || !x_display || !x_display[0]) return 0;

    snprintf(original_driver,sizeof original_driver,"%s",selected_driver);
    snprintf(wayland_reason,sizeof wayland_reason,"%s",g_gl_fallback_reason);
    fprintf(stderr,"renderer-diagnostics: retrying OpenGL with X11/XWayland after Wayland window failure: %s\n",wayland_reason);

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    (void)SDL_setenv("SDL_VIDEODRIVER","x11",1);
    SDL_ClearError();
    if(SDL_InitSubSystem(SDL_INIT_VIDEO)==0) {
        if(try_gl()) {
            fprintf(stderr,"renderer-diagnostics: OpenGL retry via X11 succeeded after Wayland failure: %s\n",wayland_reason);
            return 1;
        }
        snprintf(x11_reason,sizeof x11_reason,"%s",g_gl_fallback_reason[0]?g_gl_fallback_reason:"OpenGL retry failed without a reported reason");
    } else {
        snprintf(x11_reason,sizeof x11_reason,"SDL_InitSubSystem(video, x11) failed: %.220s",SDL_GetError());
    }

    fprintf(stderr,"renderer-diagnostics: OpenGL retry via X11 failed: %s\n",x11_reason);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if(SDL_setenv("SDL_VIDEODRIVER",original_driver,1)!=0) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,
                 "Wayland OpenGL failed (%.90s); X11 retry failed (%.90s); could not restore SDL_VIDEODRIVER",
                 wayland_reason,x11_reason);
        return 0;
    }
    SDL_ClearError();
    if(SDL_InitSubSystem(SDL_INIT_VIDEO)!=0) {
        snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,
                 "Wayland OpenGL failed (%.75s); X11 retry failed (%.75s); restoring Wayland failed: %.70s",
                 wayland_reason,x11_reason,SDL_GetError());
        fprintf(stderr,"renderer-diagnostics: video-driver restore failed: %s\n",SDL_GetError());
        return 0;
    }
    snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,
             "Wayland OpenGL failed (%.115s); X11 retry failed (%.115s)",wayland_reason,x11_reason);
    fprintf(stderr,"renderer-diagnostics: restored video driver=%s; continuing with SDL_Renderer fallback\n",original_driver);
    return 0;
}

static const char* renderer_or_unknown(const char* s){return s&&s[0]?s:"(unavailable)";}
static void renderer_log_gl_attribute(const char* name,SDL_GLattr attr) {
    int v=0; if(SDL_GL_GetAttribute(attr,&v)==0)fprintf(stderr,"renderer-diagnostics: gl.attribute.%s=%d\n",name,v);
    else fprintf(stderr,"renderer-diagnostics: gl.attribute.%s=unavailable error=%s\n",name,renderer_or_unknown(SDL_GetError()));
}
/* stderr is captured by the launcher into the workspace-root devlog.log. */
static void renderer_diagnostics(void) {
    SDL_version cv,rv;SDL_VERSION(&cv);SDL_GetVersion(&rv);
    const char* vd=SDL_GetCurrentVideoDriver();
    fprintf(stderr,"renderer-diagnostics: begin\n");
    fprintf(stderr,"renderer-diagnostics: ready=%s\n",
        g_headless?"headless":((g_gl||(g_ren&&g_tex))?"yes":"no"));
    fprintf(stderr,"renderer-diagnostics: game_renderer_fallback=%s\n",
        g_headless?"not-applicable":(g_gl?"false":"true"));
    fprintf(stderr,"renderer-diagnostics: render_path=%s\n",
        g_headless?"headless/no-window":(g_gl?"OpenGL + gl_render":(g_ren?"SDL_Renderer + software 3D":"initialization failed")));
    fprintf(stderr,"renderer-diagnostics: fallback_reason=%s\n",
        g_headless?"not-applicable (DQIX_NOWINDOW)":(g_gl?"none":renderer_or_unknown(g_gl_fallback_reason)));
    fprintf(stderr,"renderer-diagnostics: platform=%s sdl.compile=%u.%u.%u sdl.runtime=%u.%u.%u video_driver=%s SDL_VIDEODRIVER=%s SDL_RENDER_DRIVER=%s\n",
        renderer_or_unknown(SDL_GetPlatform()),cv.major,cv.minor,cv.patch,rv.major,rv.minor,rv.patch,
        renderer_or_unknown(vd),renderer_or_unknown(getenv("SDL_VIDEODRIVER")),renderer_or_unknown(getenv("SDL_RENDER_DRIVER")));
    fprintf(stderr,"renderer-diagnostics: env.DQIX_SOFTWARE=%s env.DQIX_MODE=%s env.DQIX_SCALE=%s env.DQIX_SCALE3D=%s\n",
        renderer_or_unknown(getenv("DQIX_SOFTWARE")),renderer_or_unknown(getenv("DQIX_MODE")),
        renderer_or_unknown(getenv("DQIX_SCALE")),renderer_or_unknown(getenv("DQIX_SCALE3D")));
    fprintf(stderr,"renderer-diagnostics: game_mode=%s scale=%d scale3d=%d scale3d_auto=%s fullscreen=%s\n",
        g_classic?"classic":"enhanced",g_scale,g_scale3d,g_res_auto?"true":"false",g_fullscreen?"true":"false");
    if(g_headless)fprintf(stderr,"renderer-diagnostics: window=not-created; no video renderer requested\n");
    else if(g_win) {
        int w=0,h=0,dw=0,dh=0;Uint32 flags=SDL_GetWindowFlags(g_win);int display=SDL_GetWindowDisplayIndex(g_win);
        SDL_GetWindowSize(g_win,&w,&h);SDL_GL_GetDrawableSize(g_win,&dw,&dh);
        fprintf(stderr,"renderer-diagnostics: window.id=%u flags=0x%08x size=%dx%d drawable=%dx%d display=%d opengl=%s resizable=%s high_dpi=%s fullscreen=%s\n",
            SDL_GetWindowID(g_win),flags,w,h,dw,dh,display,(flags&SDL_WINDOW_OPENGL)?"true":"false",
            (flags&SDL_WINDOW_RESIZABLE)?"true":"false",(flags&SDL_WINDOW_ALLOW_HIGHDPI)?"true":"false",
            (flags&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_FULLSCREEN_DESKTOP))?"true":"false");
        if(display>=0){SDL_DisplayMode dm;if(SDL_GetCurrentDisplayMode(display,&dm)==0)
            fprintf(stderr,"renderer-diagnostics: display.mode=%dx%d refresh_hz=%d format=%s\n",dm.w,dm.h,dm.refresh_rate,SDL_GetPixelFormatName(dm.format));
            else fprintf(stderr,"renderer-diagnostics: display.mode=unavailable error=%s\n",renderer_or_unknown(SDL_GetError()));}
    } else if(g_sdl_window_failure[0])fprintf(stderr,"renderer-diagnostics: window.creation_error=%s\n",g_sdl_window_failure);
    if(g_gl&&g_glctx) {
        typedef const unsigned char* (*GetStringFn)(unsigned int);
        GetStringFn gs=(GetStringFn)SDL_GL_GetProcAddress("glGetString");
        renderer_log_gl_attribute("context_major",SDL_GL_CONTEXT_MAJOR_VERSION);
        renderer_log_gl_attribute("context_minor",SDL_GL_CONTEXT_MINOR_VERSION);
        renderer_log_gl_attribute("context_profile",SDL_GL_CONTEXT_PROFILE_MASK);
        renderer_log_gl_attribute("context_flags",SDL_GL_CONTEXT_FLAGS);
        renderer_log_gl_attribute("double_buffer",SDL_GL_DOUBLEBUFFER);
        renderer_log_gl_attribute("depth_size",SDL_GL_DEPTH_SIZE);renderer_log_gl_attribute("stencil_size",SDL_GL_STENCIL_SIZE);
        renderer_log_gl_attribute("red_size",SDL_GL_RED_SIZE);renderer_log_gl_attribute("green_size",SDL_GL_GREEN_SIZE);
        renderer_log_gl_attribute("blue_size",SDL_GL_BLUE_SIZE);renderer_log_gl_attribute("alpha_size",SDL_GL_ALPHA_SIZE);
        renderer_log_gl_attribute("multisample_buffers",SDL_GL_MULTISAMPLEBUFFERS);renderer_log_gl_attribute("multisample_samples",SDL_GL_MULTISAMPLESAMPLES);
        fprintf(stderr,"renderer-diagnostics: gl.request=3.3-core swap_interval_set_result=%d swap_interval_actual=%d\n",g_gl_swap_interval_set_result,g_gl_swap_interval_actual);
        fprintf(stderr,"renderer-diagnostics: gl.vendor=%s\n",gs?renderer_or_unknown((const char*)gs(0x1F00u)):"(unavailable)");
        fprintf(stderr,"renderer-diagnostics: gl.renderer=%s\n",gs?renderer_or_unknown((const char*)gs(0x1F01u)):"(unavailable)");
        fprintf(stderr,"renderer-diagnostics: gl.version=%s\n",gs?renderer_or_unknown((const char*)gs(0x1F02u)):"(unavailable)");
        fprintf(stderr,"renderer-diagnostics: glsl.version=%s\n",gs?renderer_or_unknown((const char*)gs(0x8B8Cu)):"(unavailable)");
    } else {
        fprintf(stderr,"renderer-diagnostics: gl.context_present=false requested=3.3-core\n");
        fprintf(stderr,"renderer-diagnostics: fallback.window_error=%s accelerated_renderer_error=%s software_renderer_error=%s texture_error=%s\n",
            renderer_or_unknown(g_sdl_window_failure),renderer_or_unknown(g_sdl_renderer_accelerated_error),
            renderer_or_unknown(g_sdl_renderer_software_error),renderer_or_unknown(g_sdl_texture_failure));
        if(g_ren){
            SDL_RendererInfo i;if(SDL_GetRendererInfo(g_ren,&i)==0){
                fprintf(stderr,"renderer-diagnostics: sdl_renderer.name=%s flags=0x%08x accelerated=%s software=%s vsync=%s target_texture=%s max_texture=%dx%d format_count=%u\n",
                    renderer_or_unknown(i.name),i.flags,(i.flags&SDL_RENDERER_ACCELERATED)?"true":"false",
                    (i.flags&SDL_RENDERER_SOFTWARE)?"true":"false",(i.flags&SDL_RENDERER_PRESENTVSYNC)?"true":"false",
                    (i.flags&SDL_RENDERER_TARGETTEXTURE)?"true":"false",i.max_texture_width,i.max_texture_height,i.num_texture_formats);
                for(Uint32 n=0;n<i.num_texture_formats&&n<16;n++)
                    fprintf(stderr,"renderer-diagnostics: sdl_renderer.texture_format[%u]=%s\n",n,SDL_GetPixelFormatName(i.texture_formats[n]));
            }else fprintf(stderr,"renderer-diagnostics: sdl_renderer.info_error=%s\n",renderer_or_unknown(SDL_GetError()));
            int ow=0,oh=0;if(SDL_GetRendererOutputSize(g_ren,&ow,&oh)==0)fprintf(stderr,"renderer-diagnostics: sdl_renderer.output_size=%dx%d\n",ow,oh);
            else fprintf(stderr,"renderer-diagnostics: sdl_renderer.output_size_error=%s\n",renderer_or_unknown(SDL_GetError()));
        }else fprintf(stderr,"renderer-diagnostics: sdl_renderer=not-created\n");
        if(g_tex){Uint32 fmt=SDL_PIXELFORMAT_UNKNOWN;int access=-1,w=0,h=0;
            if(SDL_QueryTexture(g_tex,&fmt,&access,&w,&h)==0)fprintf(stderr,"renderer-diagnostics: sdl_texture.format=%s access=%d size=%dx%d\n",SDL_GetPixelFormatName(fmt),access,w,h);
            else fprintf(stderr,"renderer-diagnostics: sdl_texture.query_error=%s\n",renderer_or_unknown(SDL_GetError()));
        }else fprintf(stderr,"renderer-diagnostics: sdl_texture=not-created\n");
    }
    fprintf(stderr,"renderer-diagnostics: end\n");
}

/* The window's icon: the game's own, the 32x32 picture the DS menu shows for
 * the cartridge (in the ROM's banner). On Windows, dqix.exe normally carries
 * it already (made from the ROM by the build), and Windows takes it from there
 * at every size; this is for other systems and builds without it. */
static void window_icon_for(SDL_Window* win) {
#ifdef _WIN32
    if (FindResourceA(NULL, MAKEINTRESOURCEA(1), (LPCSTR)RT_GROUP_ICON)) return;
#endif
    extern size_t g_rom_size;
    if (!g_rom || g_rom_size < 0x200 || !win) return;
    uint32_t off; memcpy(&off, g_rom + 0x68, 4);
    if (off == 0 || off + 0x240 > g_rom_size) return;
    const uint8_t* bits = g_rom + off + 0x20;                  /* 4x4 tiles of 8x8, 4 bits a pixel */
    uint32_t pal[16];
    for (int i = 0; i < 16; i++) {                               /* BGR555; colour 0 is see-through */
        uint16_t c; memcpy(&c, g_rom + off + 0x220 + i * 2, 2);
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        pal[i] = (i ? 0xFF000000u : 0) | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
    }
    enum { N = 64 };                                             /* doubled, sharp pixels */
    static uint32_t px[N * N];
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int sx = x / 2, sy = y / 2, t = (sy / 8) * 4 + sx / 8;
            uint8_t v = bits[t * 32 + (sy & 7) * 4 + (sx & 7) / 2];
            px[y * N + x] = pal[(sx & 1) ? v >> 4 : v & 15];
        }
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px, N, N, 32, N * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) { SDL_SetWindowIcon(win, s); SDL_FreeSurface(s); }
}
static void window_icon(void) { window_icon_for(g_win); }

/* ---------------------------------------------------------------- two windows
 * One screen in each, for two monitors: the first window (with the menu bar)
 * shows the bottom screen, the second the top one; F9 swaps them. The second
 * window shares the first one's OpenGL context and opens on another monitor
 * if there is one (then where it was left). */
static void second_window_title(void) {
    if (g_win2) SDL_SetWindowTitle(g_win2, g_wide ? "Dragon Quest IX - map and commands" : g_swap ? "Dragon Quest IX - bottom screen" : "Dragon Quest IX - top screen");
}
static void second_window_keep_rect(void) {                     /* remember where it is (as a window) */
    if (!g_win2 || g_win2_full) return;
    SDL_GetWindowPosition(g_win2, &g_win2_rect[0], &g_win2_rect[1]);
    SDL_GetWindowSize(g_win2, &g_win2_rect[2], &g_win2_rect[3]);
}
static void second_window_sync(void) {
    int want = g_gl && ((g_two && !g_single) || (g_wide && g_wide_pop)) && !g_side_by_side && g_win && !g_headless;
    if (!want && g_win2) {
        second_window_keep_rect();
        SDL_GL_MakeCurrent(g_win, g_glctx);
        SDL_DestroyWindow(g_win2); g_win2 = NULL;
        return;
    }
    if (!want || g_win2) { second_window_title(); return; }
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED, w = 256 * g_scale, h = 192 * g_scale, placed = 0;
    if (g_win2_rect[2] > 0) {                                    /* where it was, if that is still on a monitor */
        for (int d = 0; d < SDL_GetNumVideoDisplays(); d++) {
            SDL_Rect r;
            if (SDL_GetDisplayBounds(d, &r) == 0 && g_win2_rect[0] + 40 > r.x && g_win2_rect[0] + 40 < r.x + r.w && g_win2_rect[1] + 10 >= r.y && g_win2_rect[1] + 10 < r.y + r.h) placed = 1;
        }
        if (placed) { x = g_win2_rect[0]; y = g_win2_rect[1]; w = g_win2_rect[2]; h = g_win2_rect[3]; }
    }
    if (!placed && SDL_GetNumVideoDisplays() > 1) {              /* another monitor than the game's */
        int d = SDL_GetWindowDisplayIndex(g_win) == 0 ? 1 : 0;
        x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(d); placed = 1;
    }
    if (!placed) {                                                /* one monitor: beside the game's window */
        int mx, my, mw, mh; SDL_GetWindowPosition(g_win, &mx, &my); SDL_GetWindowSize(g_win, &mw, &mh);
        SDL_Rect r;
        if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(g_win), &r) == 0 && mx + mw + 16 + w <= r.x + r.w) { x = mx + mw + 16; y = my; }
    }
    g_win2 = SDL_CreateWindow("Dragon Quest IX", x, y, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win2) { fprintf(stderr, "second window: %s\n", SDL_GetError()); return; }
    window_icon_for(g_win2);
    menu_init_second(g_win2);                                    /* its own bar: Fullscreen */
    second_window_title();
    if (SDL_GL_MakeCurrent(g_win2, g_glctx) == 0) SDL_GL_SetSwapInterval(0);
    SDL_GL_MakeCurrent(g_win, g_glctx);
    if (g_win2_full) { menu_show_second(0); SDL_SetWindowFullscreen(g_win2, SDL_WINDOW_FULLSCREEN_DESKTOP); }
    SDL_RaiseWindow(g_win);                                      /* the keyboard stays with the game's window */
}
static void second_window_fullscreen(void) {
    if (!g_win2) return;
    if (!g_win2_full) second_window_keep_rect();
    g_win2_full = !g_win2_full;
    menu_show_second(!g_win2_full);
    SDL_SetWindowFullscreen(g_win2, g_win2_full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void lobby_hook(void);
static void idle_pump(void);
static void handle_events(void);
static int g_cap_action;
static void capture_key(SDL_Scancode k, Uint16 mod);
static void capture_pad(int pad);
void frontend_init(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetMainReady();
    g_report_perf = perf_write;
    g_headless = getenv("DQIX_NOWINDOW") != NULL;
    if (getenv("DQIX_HIDDEN")) { g_hidden = g_headless = 1; return; }
    g_analog_walk = 1;                                          /* Modern: walking with the stick is on unless turned off */
    skins_scan();
    default_console_color();
    if (getenv("DQIX_NO_SETTINGS")) controls_defaults(); else controls_load();
    if (!getenv("DQIX_NO_SETTINGS")) {
        settings_load();
        if (!g_wireless.mac_set && !g_headless) { wireless_mac_new(); settings_save(); }
    }
    wireless_apply(1);                                          /* read by firmware_init */
    if (!g_headless) { music_set_volume(g_mus_vol); music_enable(g_mus_pack); }   /* music packs (music.c; off in replays and sessions) */
    if (g_opt.dlc) backup_unlock_dlc();                        /* before the game reads the save */
    if (getenv("DQIX_MODE")) g_classic = strcmp(getenv("DQIX_MODE"), "enhanced") != 0;   /* tests */
    /* real pixels on high-DPI Windows setups (not stretched by the system), so
     * the pixel grid and actual size are drawn on the monitor's own pixels */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    /* Controllers: the buttons by position (right = A, bottom = B, like the DS)
     * for every brand. SDL otherwise reports Nintendo controllers (Switch Pro,
     * Joy-Cons) by their labels, which would swap A/B and X/Y on them. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1");   /* two Joy-Cons are one controller */
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");      /* fullscreen: the menu strip takes the focus for a moment */
    if (SDL_Init((g_headless ? 0 : SDL_INIT_VIDEO) | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(1);
    }
    atexit(SDL_Quit);
    if (!g_headless) {
        const char* sc = getenv("DQIX_SCALE");
        if (sc) { g_scale = atoi(sc); if (g_scale < 1) g_scale = 1; if (g_scale > 8) g_scale = 8; }
        if (!try_gl_with_x11_retry()) {
            if(!g_gl_fallback_reason[0])snprintf(g_gl_fallback_reason,sizeof g_gl_fallback_reason,"OpenGL initialization failed without a specific reason");
            SDL_ClearError();
            g_win=SDL_CreateWindow("Dragon Quest IX",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,256*g_scale,384*g_scale,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
            if(!g_win){snprintf(g_sdl_window_failure,sizeof g_sdl_window_failure,"SDL_CreateWindow for SDL_Renderer failed: %.220s",SDL_GetError());renderer_diagnostics();exit(1);}
            SDL_ClearError();g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED);
            if(!g_ren){snprintf(g_sdl_renderer_accelerated_error,sizeof g_sdl_renderer_accelerated_error,"%.300s",SDL_GetError());SDL_ClearError();
                g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_SOFTWARE);
                if(!g_ren)snprintf(g_sdl_renderer_software_error,sizeof g_sdl_renderer_software_error,"%.300s",SDL_GetError());}
            if(!g_ren){renderer_diagnostics();exit(1);}
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"nearest");SDL_ClearError();
            g_tex=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,256,384);
            if(!g_tex){snprintf(g_sdl_texture_failure,sizeof g_sdl_texture_failure,"SDL_CreateTexture(ARGB8888 streaming 256x384) failed: %.220s",SDL_GetError());renderer_diagnostics();exit(1);}
        }
        window_icon();
        {   /* started again by the program itself (reset, sessions): the window where it was */
            const char* wp = getenv("DQIX_WINPOS"); int x, y;
            if (wp && sscanf(wp, "%d,%d", &x, &y) == 2) SDL_SetWindowPosition(g_win, x, y);
        }
        apply_mode();
        const char* names[N_DRAWN + MAX_SKINS];
        for (int i = 0; i < N_DRAWN; i++) names[i] = SHELLS[i].name;
        for (int i = 0; i < g_nskins; i++) names[N_DRAWN + i] = g_skins[i].name;
        menu_init(g_win, names, N_SHELLS);
        if (g_update_auto && !session_requested() && update_project_dir()) update_check_async();
        update_actual();
        menu_sync();
        layout_window();
        if (g_fullscreen) { menu_show(0); SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP); }
        second_window_sync();
        atexit(settings_save);
        session_lobby_hook = lobby_hook;
        air_idle_hook = idle_pump;
    }
    renderer_diagnostics();
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 32768; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);           /* SDL converts to the device's format */
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else fprintf(stderr, "no audio: %s\n", SDL_GetError());
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; }
    g_perf_freq = SDL_GetPerformanceFrequency();
    g_next_frame = SDL_GetPerformanceCounter();
    g_fps_t0 = g_next_frame;
}

static uint16_t key_bits(const Uint8* ks) { return controls_ds_keys(ks); }   /* Game > Controls (controls.c) */

static uint16_t pad_bits(void) {
    if (!g_pad) { g_fe_stick_x = g_fe_stick_y = g_fe_rstick_x = 0; g_fe_stick_keys = 0; return 0; }
    SDL_GameController* p = g_pad;
    uint16_t k = controls_ds_pad(p);                           /* the buttons: Game > Controls (by position by default, like a DS) */
    int lx = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
    uint16_t s = 0;                                              /* the stick as a D-pad (input.c decides) */
    if (lx > 16000) s |= 1 << 4; if (lx < -16000) s |= 1 << 5;
    if (ly < -16000) s |= 1 << 6; if (ly > 16000) s |= 1 << 7;
    g_fe_stick_x = lx; g_fe_stick_y = ly; g_fe_stick_keys = s;
    g_fe_rstick_x = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTX);
    return k;
}

/* Window pixel -> bottom-screen pixel; returns 0 when outside it, unless
 * clamp is set (a drag that started on the screen stays on its edge, as a
 * stylus would at the rim). */
static int to_touch_in(SDL_Window* win, int wx, int wy, int* tx, int* ty, int clamp) {
    float lx, ly;
    GlPresent v;
    if (!win || (win != g_win && win != g_win2)) win = g_win;
    if (g_gl) {                                                  /* same layout as gl_present() */
        int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
        canvas_layout_for(&v, ww, wh, win == g_win2);
        lx = ((float)wx + 0.5f - v.ox) / v.sc; ly = ((float)wy + 0.5f - v.oy) / v.sc;
    } else { canvas_layout(&v, 1, 1); SDL_RenderWindowToLogical(g_ren, wx, wy, &lx, &ly); }
    if (v.npiece) {                                               /* the battle HUD's panels: back to where they are on the touch screen */
        static int last;                                          /* the panel last touched: a drag off it stays on its edge */
        int hit = -1;
        for (int i = v.npiece - 1; i >= 0 && hit < 0; i--) {
            const float* q = v.piece[i];
            if (lx >= q[0] && lx < q[0] + q[2] && ly >= q[1] && ly < q[1] + q[3]) hit = i;
        }
        if (hit < 0 && !(clamp && last < v.npiece)) return 0;   /* the rest of the commands screen isn't shown */
        if (hit >= 0) last = hit;
        const float* q = v.piece[last];
        float px = fminf(fmaxf(lx, q[0]), q[0] + q[2] - 0.01f), py = fminf(fmaxf(ly, q[1]), q[1] + q[3] - 0.01f);
        int x = (int)floorf(q[5] + (px - q[0]) * q[7] / q[2]), y = (int)floorf(q[6] + (py - q[1]) * q[8] / q[3]);
        if (x < 0 || x > 255 || y < 0 || y > 191) return 0;
        *tx = x; *ty = y;
        return 1;
    }
    if (v.front == 0 && v.top[2] < 256 && lx >= v.top[0] && lx < v.top[0] + v.top[2] && ly >= v.top[1] && ly < v.top[1] + v.top[3])
        return 0;                                                 /* on the small top screen in front of it */
    int side = 0; if (g_gl) gl_wide_shown(&side);
    if (g_gl && v.bot[2] <= 0) return 0;                         /* not shown */
    float bw = v.bot[2] > 0 ? v.bot[2] : 256, bh = v.bot[3] > 0 ? v.bot[3] : 192, bx = v.botx[1] > 0 ? v.botx[1] : 256;
    int x = (int)floorf((lx - v.bot[0]) * bx / bw + (g_gl ? v.botx[0] - (float)side : 0)), y = (int)floorf((ly - v.bot[1]) * 192.0f / bh);
    if (g_gl && gl_wide_shown(&side) == 1 && y >= 0 && y < 192) {   /* dynamic widescreen: a window moved right is touched where it really is */
        extern int gpu3d_ui_unshift(int*, int);
        if (!gpu3d_ui_unshift(&x, y)) gl_ui_unshift(&x, y);
    }
    if (clamp) { x = x < 0 ? 0 : x > 255 ? 255 : x; y = y < 0 ? 0 : y > 191 ? 191 : y; }
    if (x < 0 || x > 255 || y < 0 || y > 191) return 0;
    *tx = x; *ty = y;
    return 1;
}
static int to_touch(int wx, int wy, int* tx, int* ty, int clamp) { return to_touch_in(g_win, wx, wy, tx, ty, clamp); }

static int g_shot_req;
static void save_bmp_screens(const uint32_t scr[2][256 * 192], const char* path) {
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, 256, 384, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return;
    for (int y = 0; y < 384; y++) memcpy((uint8_t*)sf->pixels + y * sf->pitch, &scr[y / 192][(y % 192) * 256], 256 * 4);
    SDL_SaveBMP(sf, path);
    SDL_FreeSurface(sf);
}

static void save_screenshot(void) {
    if (g_gl) {
        static uint32_t scr[2][256 * 192];
        static int n;
        char path[64];
        gl_read_screens(scr);
        snprintf(path, sizeof path, "dqix-shot-%d.bmp", n++);
        save_bmp_screens(scr, path);
        fprintf(stderr, "screenshot: %s\n", path);
        return;
    }
    int w, h;
    SDL_GetRendererOutputSize(g_ren, &w, &h);
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return;
    if (SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch) == 0) {
        static int n;
        char path[64];
        snprintf(path, sizeof path, "dqix-shot-%d.bmp", n++);
        SDL_SaveBMP(sf, path);
        fprintf(stderr, "screenshot: %s\n", path);
    }
    SDL_FreeSurface(sf);
}

/* tests: DQIX_MEMFIND="frame,hex,hex...": at that frame, the main RAM / DTCM
 * addresses holding each 32-bit value, and the projection matrix */
static void memfind_hook(void) {
    static long long at = -2; static unsigned vals[8]; static int nv;
    if (at == -2) {
        at = -1; const char* e = getenv("DQIX_MEMFIND");
        if (e) { at = atoll(e); const char* c = e; while ((c = strchr(c, ',')) && nv < 8) { c++; vals[nv++] = (unsigned)strtoul(c, NULL, 16); } }
    }
    {   /* DQIX_RAMDUMP=frame[,frame...]: the main RAM to ram<frame>.bin (tests) */
        static char rl[400]; static int ri;
        if (!ri) { ri = 1; const char* e = getenv("DQIX_RAMDUMP"); if (e) snprintf(rl, sizeof rl, ",%s,", e); }
        char key[32]; snprintf(key, sizeof key, ",%llu,", (unsigned long long)g_frame);
        if (rl[0] && strstr(rl, key)) {
            char path[64]; snprintf(path, sizeof path, "ram%llu.bin", (unsigned long long)g_frame);
            FILE* f = fopen(path, "wb"); if (f) { fwrite(g_main_ram, 1, MAIN_RAM_SIZE, f); fclose(f); }
        }
    }
    if (at < 0 || g_frame != (uint64_t)at) return;
    extern void gpu3d_debug_proj(void);
    gpu3d_debug_proj();
    {   const char* d = getenv("DQIX_MEMDUMP");                  /* "addr,addr,...": 0x20 bytes before and after each */
        while (d && *d) { uint32_t a = (uint32_t)strtoul(d, NULL, 16) & 0x3FFFFC;
            fprintf(stderr, "dump %08x:", 0x02000000u + a - 0x20); for (int i = -8; i < 8; i++) { uint32_t v; memcpy(&v, g_main_ram + a + i * 4, 4); fprintf(stderr, " %08x", v); } fprintf(stderr, "\n");
            d = strchr(d, ','); if (d) d++; } }
    for (int k = 0; k < nv; k++) {
        int n = 0;
        for (uint32_t a = 0; a + 4 <= MAIN_RAM_SIZE && n < 200; a += 2) { uint32_t v; memcpy(&v, g_main_ram + a, 4); if (v == vals[k]) { fprintf(stderr, "memfind %08x at %08x\n", vals[k], 0x02000000u + a); n++; } }
        for (uint32_t a = 0; a + 4 <= DTCM_SIZE; a += 2) { uint32_t v; memcpy(&v, g_dtcm + a, 4); if (v == vals[k]) fprintf(stderr, "memfind %08x at dtcm+%04x\n", vals[k], a); }
    }
}

static void do_action(int id);
/* tests: DQIX_TEST_ACTIONS="frame:menu id;frame:resize:w:h;...": menu actions and window resizes at given frames */
static void test_actions_hook(void) {
    static const char* e = (const char*)-1; if (e == (const char*)-1) e = getenv("DQIX_TEST_ACTIONS");
    if (!e) return;
    for (const char* c = e; c && *c; ) {
        unsigned long long f = strtoull(c, NULL, 10); const char* a = strchr(c, ':');
        if (a && f == g_frame) {
            int w, h;
            if (!strncmp(a + 1, "resize:", 7) && sscanf(a + 8, "%d:%d", &w, &h) == 2) SDL_SetWindowSize(g_win, w, h);
            else do_action(atoi(a + 1));
        }
        c = strchr(c, ';'); if (c) c++;
    }
}

static void test_hooks(void) {
    memfind_hook();
    test_actions_hook();
    static long long report_at = -2;
    if (report_at == -2) { const char* e = getenv("DQIX_REPORT_AT"); report_at = e ? atoll(e) : -1; }
    if (report_at >= 0 && g_frame == (uint64_t)report_at) report_write("DQIX_REPORT_AT (test)");
    static long long shot_at = -2;
    if (shot_at == -2) { const char* e = getenv("DQIX_SHOT_AT"); shot_at = e ? atoll(e) : -1; }
    if (g_shot_req || (shot_at >= 0 && g_frame == (uint64_t)shot_at)) { save_screenshot(); g_shot_req = 0; }
}

/* DQIX_GL_DUMP="dir,every": write the GL-composed screens (256x384) like DQIX_DUMP (tests) */
static void gl_dump_hook(void) {
    static int every = -1; static char dir[400];
    if (every < 0) {
        const char* e = getenv("DQIX_GL_DUMP");
        every = 0;
        if (e) { const char* c = strchr(e, ','); size_t n = c ? (size_t)(c - e) : strlen(e); if (n >= sizeof dir) n = sizeof dir - 1; memcpy(dir, e, n); dir[n] = 0; every = c ? atoi(c + 1) : 60; }
    }
    if (every <= 0 || g_frame % (uint64_t)every) return;
    char path[600]; snprintf(path, sizeof path, "%s/f%06llu.ppm", dir, (unsigned long long)g_frame);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    if (getenv("DQIX_GL_DUMP_FULL")) {                       /* at the rendering resolution */
        int w, h; uint8_t* rgb = gl_read_full(&w, &h);
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        fwrite(rgb, 1, (size_t)w * h * 3, f);
        free(rgb);
    } else {                                                 /* native 256x384, for comparisons */
        static uint32_t scr[2][256 * 192];
        gl_read_screens(scr);
        fprintf(f, "P6\n256 384\n255\n");
        for (int sc = 0; sc < 2; sc++) for (int i = 0; i < 256 * 192; i++) { uint32_t c = scr[sc][i]; uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c }; fwrite(rgb, 1, 3, f); }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- frame timings (for bug reports) */
#define PERF_N 1800                                          /* the last 30 seconds */
typedef struct { uint64_t frame; float busy, gl, upload, present, wait, d2; int uploads, polys, audio_ms; uint8_t fast; } PerfRec;
static PerfRec g_perf[PERF_N];
static unsigned g_perf_pos;
static uint64_t g_perf_end;                                  /* end of the previous frame (after pacing) */
static double ticks_ms(uint64_t t) { return (double)t * 1000.0 / (double)g_perf_freq; }

static void perf_write(FILE* f) {
    fprintf(f, "mode: %s\n", g_classic ? "classic (DS 3D renderer)" : "enhanced (3D on the GPU)");
    fprintf(f, "renderer: %s\n", g_gl ? gl_renderer_name() : "software (no OpenGL 3.3)");
    if (g_gl && !g_classic) fprintf(f, "3D resolution: %dx\n", gl_render_scale());
    fprintf(f, "audio: %s\n\n", g_audio ? "on" : "off");
    fprintf(f, "Per frame, in ms. A frame must take under 16.7 ms in total (busy + present) to keep full speed.\n");
    fprintf(f, "busy = all work for the frame: game = game code, 2d = 2D layers, gl = 3D on the GPU, upload = new textures (inside gl),\n");
    fprintf(f, "present = showing the frame, wait = idle time to keep the pace, audio = sound queued.\n\n");
    unsigned n = g_perf_pos < PERF_N ? g_perf_pos : PERF_N - 1;     /* the slot at g_perf_pos is the frame in progress */
    unsigned slow = 0; double worst = 0; uint64_t worst_f = 0;
    for (unsigned k = n; k > 0; k--) {
        const PerfRec* r = &g_perf[(g_perf_pos - k) % PERF_N];
        double t = r->busy + r->present;
        if (t > 16.7) slow++;
        if (t > worst) { worst = t; worst_f = r->frame; }
    }
    fprintf(f, "frames: %u, over 16.7 ms: %u, worst %.1f ms at frame %llu\n\n", n, slow, worst, (unsigned long long)worst_f);
    fprintf(f, "  frame    busy    game      2d      gl  upload (n)   polys  present    wait  audio\n");
    for (unsigned k = n; k > 0; k--) {
        const PerfRec* r = &g_perf[(g_perf_pos - k) % PERF_N];
        fprintf(f, "%7llu %7.1f %7.1f %7.1f %7.1f %7.1f (%3d) %6d %8.1f %7.1f %6d%s%s\n", (unsigned long long)r->frame, r->busy,
                r->busy - r->d2 - r->gl, r->d2, r->gl, r->upload, r->uploads,
                r->polys, r->present, r->wait, r->audio_ms, r->fast ? " fast" : "", r->busy + r->present > 16.7 ? "  <-- slow" : "");
    }
}

/* A bug report also keeps the window as it looked (window.png, at most 1920
 * pixels wide): the DS screens alone (screenshot.bmp) don't show the layout,
 * widescreen's sides or the scaling (GitHub #18). */
static char g_report_shot_dir[300];
static void report_window_shot(int w, int h) {
    if (!g_report_shot_dir[0] || w <= 0 || h <= 0) return;
    uint8_t* rgb = gl_read_window(w, h);
    int k = (w + 1919) / 1920; if (k < 1) k = 1;
    int ow = w / k, oh = h / k;
    uint8_t* out = malloc((size_t)ow * oh * 3);
    if (out) {
        for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) for (int c = 0; c < 3; c++) {
            unsigned sum = 0;
            for (int j = 0; j < k; j++) for (int i = 0; i < k; i++) sum += rgb[((size_t)(y * k + j) * w + (x * k + i)) * 3 + c];
            out[((size_t)y * ow + x) * 3 + c] = (uint8_t)(sum / (unsigned)(k * k));
        }
        char path[400]; snprintf(path, sizeof path, "%s/window.png", g_report_shot_dir);
        stbi_write_png(path, ow, oh, 3, out, ow * 3);
        free(out);
    }
    free(rgb);
    g_report_shot_dir[0] = 0;
}

/* ... and the display settings (display.txt): the settings that change what
 * is on screen, nothing personal (no names, addresses or Wi-Fi ID). */
static void settings_save(void);
static void report_display(const char* dir) {
    static const char* KEEP[] = { "mode", "render_scale", "smooth_textures", "outlines", "soft_shadows", "smooth_shading", "hd_text", "widescreen_popout", "window_scale", "layout",
        "single_screen", "small_screen_corner", "small_screen_size", "small_screen_hidden", "small_screen_opacity", "swap_screens",
        "second_window", "second_window_fullscreen", "walk_any_direction", "fullscreen", "dslite_filter", "lcd_colors", "pixel_grid",
        "motion_blur", "backlight", "screen_gap", "console", "integer_scaling", "actual_size" };
    char path[400]; snprintf(path, sizeof path, "%s/display.txt", dir);
    FILE* out = fopen(path, "w");
    if (!out) return;
    if (g_win) {
        int w, h, dw, dh; SDL_GetWindowSize(g_win, &w, &h); SDL_GL_GetDrawableSize(g_win, &dw, &dh);
        SDL_DisplayMode m; int disp = SDL_GetWindowDisplayIndex(g_win);
        if (disp < 0 || SDL_GetCurrentDisplayMode(disp, &m) != 0) m.w = m.h = 0;
        fprintf(out, "window: %dx%d (drawn at %dx%d)%s, display %dx%d, renderer %s\n", w, h, dw, dh, g_fullscreen ? " fullscreen" : "",
                m.w, m.h, g_gl ? "OpenGL" : "software");
    }
    settings_save();
    FILE* in = fopen("settings.ini", "r");
    char line[400];
    while (in && fgets(line, sizeof line, in)) {
        if (line[0] == '#') continue;
        char key[64]; if (sscanf(line, " %63[a-z_0-9]", key) != 1) continue;
        for (size_t i = 0; i < sizeof KEEP / sizeof KEEP[0]; i++) if (!strcmp(key, KEEP[i])) { fputs(line, out); break; }
    }
    if (in) fclose(in);
    fclose(out);
}

/* DQIX_WINSHOT_AT=frame[,frame...]: save the whole window as shown (tests) */
static void window_shot_hook(int w, int h, int second) {
    static char list[20000]; static int init;
    if (!init) { init = 1; const char* e = getenv("DQIX_WINSHOT_AT"); if (e) snprintf(list, sizeof list, ",%s,", e); }
    if (!list[0]) return;
    char key[32]; snprintf(key, sizeof key, ",%llu,", (unsigned long long)g_frame);
    if (!strstr(list, key)) return;
    uint8_t* rgb = gl_read_window(w, h);
    char path[64]; snprintf(path, sizeof path, second ? "win%06llu_2.ppm" : "win%06llu.ppm", (unsigned long long)g_frame);
    FILE* f = fopen(path, "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, (size_t)w * h * 3, f); fclose(f); }
    free(rgb);
}

/* Widescreen: is there 2D (a menu, the title) where the small screen sits on
 * the big one? Then slide to the corner layout, and back when it's only the
 * field again. From the 3D screen's last picture: pixels where no 2D layer
 * covers the 3D are the field. */
extern uint8_t g_comp_info[256 * 192 * 4];
extern int g_comp_has3d;
extern uint32_t g_engine_out[2][256 * 192];
static void wide_update(int a_on_top) {
    if (!g_wide || g_wide_pop) { g_wide_mix = 1.0f; if (!g_wide) return; }
    int side = 0, which = gl_wide_shown(&side);
    {   /* the small screen: the one without the 3D (engine B), or the top one */
        int small_top = which != 0;
        int eng = small_top == a_on_top ? 0 : 1;
        const uint32_t* px = g_engine_out[eng];
        int blank = 1;
        for (int i = 0; i < 256 * 192 && blank; i += 7) if (px[i] & 0xFFFFFF) blank = 0;
        g_small_blank = blank;
    }
    int covered = 1;
    if (which == 0) covered = 0;                                  /* 3D on the top screen: no sliding */
    else if (which == 1 && g_comp_has3d) {
        int n2d = 0, n = 0;
        for (int y = 2; y < 66; y += 2)
            for (int x = 0; x < 46; x += 2) { n++; if ((g_comp_info[(y * 256 + x) * 4] & 3) == 0) n2d++; }
        covered = n2d * 20 > n;                                   /* more than 5% */
    }
    static int frames;                                            /* how long it has been like this */
    static int last = -1;
    if (covered != last) { last = covered; frames = 0; } else frames++;
    float target = g_wide_mix;
    if (covered && frames >= 1) target = 0.0f;                   /* make room quickly */
    if (!covered && frames >= 20) target = 1.0f;                  /* back to wide once it stays clear */
    float step = 1.0f / 12.0f;
    if (g_wide_mix < target) g_wide_mix = fminf(target, g_wide_mix + step);
    else if (g_wide_mix > target) g_wide_mix = fmaxf(target, g_wide_mix - step);
}

/* Widescreen battles (3D on the top screen): find the party's panels on the
 * commands screen. They are stacked across it, each framed in white with a
 * grey line (0x515151) between them; the one whose turn it is is taller and
 * holds the commands. Anything else there (no such lines) keeps the usual
 * small screen. In the field, keep the map for the map window. */
static void hud_update(int a_on_top) {
    g_hud_n = 0; g_hud_win[2] = 0;
    if (!g_gl || !(g_hud || g_wide_dyn) || !g_wide || g_side_by_side || (g_two && !g_single)) return;   /* (dynamic widescreen: always) */
    int side = 0, which = gl_wide_shown(&side);
    if (which == 1 && g_wide_pop) {                               /* the field: the map the window keeps during battles */
        uint16_t mb; memcpy(&mb, &g_io[(a_on_top ? 0 : 0x1000) + 0x6C], 2);   /* the top screen's engine (the map) */
        static unsigned n;                                        /* a copy of the whole picture: a few times a second is enough */
        if (n++ % 10 == 0 && !g_small_blank && (!(mb >> 14) || !(mb & 0x1F))) gl_hold();   /* not while it fades out (into a battle) */
        return;
    }
    if (which != 0) return;
    const uint32_t* px = g_engine_out[a_on_top ? 1 : 0];          /* the bottom screen's engine */
    uint32_t c_sep = 0x515151, c_frame = 0xFBFBFB;                /* dynamic widescreen: also while they fade in from black */
    if (g_wide_dyn) {                                             /* (master brightness: those colours dimmed the same way) */
        uint16_t mb; memcpy(&mb, &g_io[(a_on_top ? 0x1000 : 0) + 0x6C], 2);
        uint32_t f = mb & 31; if (f > 16) f = 16;
        if ((mb >> 14) == 2 && f) {
            uint32_t v6[2] = { 0x51 >> 2, 0xFB >> 2 }, out[2];
            for (int k = 0; k < 2; k++) { uint32_t c = v6[k] - ((v6[k] * f + 15) >> 4); c = c << 2 | c >> 4; out[k] = c * 0x010101u; }
            c_sep = out[0]; c_frame = out[1];
        }
    }
    int sep[192], nsep = 0;
    for (int y = 0; y < 192; y++) {
        int n = 0;
        for (int x = 10; x < 246; x++) if ((px[y * 256 + x] & 0xFFFFFF) == c_sep) n++;
        sep[y] = n > 200; nsep += sep[y];
    }
    if (nsep < 1) {                                               /* no panels: one window alone (experience, level up)? */
        if (!g_last_battle || g_frame - g_last_battle > 60 * 30) return;   /* only just after a battle (not the field's own windows) */
        int x0 = 256, y0 = 192, x1 = -1, y1 = -1, n = 0;              /* its inside: dark grey 0x303030, in both styles */
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++)
                if ((px[y * 256 + x] & 0xFFFFFF) == 0x303030) { n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        if (x1 - x0 < 100 || y1 - y0 < 40 || n * 10 < (x1 - x0 + 1) * (y1 - y0 + 1) * 7) return;   /* (mostly that grey: one window) */
        x0 = x0 < 3 ? 0 : x0 - 3; y0 = y0 < 3 ? 0 : y0 - 3; x1 = x1 > 252 ? 255 : x1 + 3; y1 = y1 > 188 ? 191 : y1 + 3;   /* its frame */
        g_hud_win[0] = x0; g_hud_win[1] = y0; g_hud_win[2] = x1 - x0 + 1; g_hud_win[3] = y1 - y0 + 1;
        return;
    }
    int n = 0, tall = 0;
    for (int y = 0; y < 192;) {
        if (sep[y]) { y++; continue; }
        int y0 = y;
        while (y < 192 && !sep[y]) y++;
        if (y - y0 < 8) continue;                                 /* a border, not a panel */
        /* a member's panel: 30-45 rows (the one whose turn it is: up to 80), framed in white */
        int h = y - y0, mid = (y0 + y) / 2, framed = 0;
        for (int x = 0; x < 16; x++) if ((px[mid * 256 + x] & 0xFFFFFF) == c_frame) framed = 1;
        if (!framed) continue;                                    /* the wood around the panels (above them in the attacks, below a party of 3) */
        if (n == 8 || h < 30 || h > 80 || (h >= 50 && tall++)) return;
        g_hud_band[n][0] = y0; g_hud_band[n][1] = y; n++;
    }
    if (n >= 2) { g_hud_n = n; g_last_battle = g_frame; }
}

/* 3D resolution Auto: render at 1.5 times the size the 3D is shown at (at
 * least 2x, at most what the GPU holds), so it is never stretched and the
 * extra detail smooths the edges. Changes after the size has stayed the same
 * for half a second (resizing a window doesn't rebuild it every frame). */
static void auto_scale(void) {
    if (!g_res_auto || g_classic || getenv("DQIX_SCALE3D")) return;
    { static long long from = -2; if (from == -2) { const char* e = getenv("DQIX_AUTO_FROM"); from = e ? atoll(e) : -1; }   /* tests: 2x until then */
      if (from >= 0 && g_frame < (uint64_t)from) return; }
    float need = 0;
    SDL_Window* ws[2] = { g_win, g_win2 };
    for (int k = 0; k < 2; k++) {
        if (!ws[k]) continue;
        int w, h; SDL_GL_GetDrawableSize(ws[k], &w, &h);
        GlPresent v; canvas_layout_for(&v, w, h, k);
        for (int i = 0; i < 2; i++) {
            const float* r = i ? v.bot : v.top, *x = i ? v.botx : v.topx;
            if (r[2] > 0 && x[1] > 0) { float p = v.sc * r[2] / x[1]; if (p > need) need = p; }
        }
    }
    int want = (int)ceilf(need * 1.5f - 0.05f), top = gl_render_max_scale();
    if (want < 2) want = 2;
    if (want > top) want = top;
    static int pending, frames;
    if (want == gl_render_scale()) { pending = 0; frames = 0; return; }
    if (want != pending) { pending = want; frames = 0; return; }
    if (++frames >= 30) { gl_render_set_scale(want); pending = frames = 0; }
}

/* What the game itself depends on, worked out each frame even when nothing is
 * shown (DQIX_PRESENT_FROM): the widescreen width, and so what counts as in view. */
static void present_gl_state(void) {
    {   int r = g_wide && !g_classic ? scene_rule() : 0;
        g_gl_bars = r == 'b';
        g_scene_stack = r == 's';
    }
    g_wide_side = g_classic ? 0 : wide_side_target();            /* the next 3D frames (only the picture changes) */
    {   static long long off_from = -2;                           /* DQIX_WIDE_OFF_FROM=frame: the DS's width from then (tests) */
        if (off_from == -2) { const char* e = getenv("DQIX_WIDE_OFF_FROM"); off_from = e ? atoll(e) : -1; }
        if (off_from >= 0 && g_frame >= (uint64_t)off_from) g_wide_side = 0;
    }
    {   /* ...and what the game counts as in view (not in a session: every PC must decide alike) */
        static int off = -1; if (off < 0) off = getenv("DQIX_NO_WIDE_CULL") != NULL;
        g_wide_cull = off || session_requested() ? 0 : g_wide_side;
        extern unsigned g_box_tests, g_box_in, g_box_widened;
        static int lg = -1; if (lg < 0) lg = getenv("DQIX_BOXLOG") != NULL;
        if (lg && g_frame % 60 == 0) fprintf(stderr, "box: frame %llu tests %u in %u widened %u\n", (unsigned long long)g_frame, g_box_tests, g_box_in, g_box_widened);
        if (lg && g_frame % 60 == 0) g_box_tests = g_box_in = g_box_widened = 0;
    }
}

#ifndef _WIN32
static const char *linux_overlay_renderer_name(void) {
    SDL_RendererInfo info;
    if (g_gl) return "OpenGL";
    if (g_ren && SDL_GetRendererInfo(g_ren, &info) == 0 && info.name && info.name[0]) return info.name;
    return "SDL_Renderer";
}

static const uint8_t *linux_overlay_pixels(void) {
    const size_t bytes = (size_t)DQIX_LINUX_OVERLAY_WIDTH * DQIX_LINUX_OVERLAY_HEIGHT * 4;
    if (!g_linux_overlay_open) return NULL;
    if (!g_linux_overlay_rgba) {
        g_linux_overlay_rgba = (uint8_t *)malloc(bytes);
        if (!g_linux_overlay_rgba) {
            if (!g_linux_overlay_error_reported) fprintf(stderr, "linux-overlay: cannot allocate the RGBA surface\n");
            g_linux_overlay_error_reported = 1;
            return NULL;
        }
    }
    dqix_linux_overlay_render_rgba(g_linux_overlay_rgba, bytes, g_linux_overlay_selection,
        g_classic, g_fullscreen, g_gl, linux_overlay_renderer_name());
    return g_linux_overlay_rgba;
}

static void linux_overlay_draw_gl(SDL_Window *window) {
    const uint8_t *pixels;
    int width = 0, height = 0;
    if (!g_linux_overlay_open || !g_gl || !window) return;
    pixels = linux_overlay_pixels();
    if (!pixels) return;
    SDL_GL_GetDrawableSize(window, &width, &height);
    if (!dqix_linux_overlay_gl_draw(pixels, DQIX_LINUX_OVERLAY_WIDTH, DQIX_LINUX_OVERLAY_HEIGHT,
                                    width, height) && !g_linux_overlay_error_reported) {
        fprintf(stderr, "linux-overlay: OpenGL draw failed; see earlier overlay diagnostics\n");
        g_linux_overlay_error_reported = 1;
    }
}

static void linux_overlay_draw_sdl(void) {
    const uint8_t *pixels;
    SDL_Rect destination;
    int width = 0, height = 0, logical_width = 0, logical_height = 0;
    if (!g_linux_overlay_open || g_gl || !g_ren) return;
    pixels = linux_overlay_pixels();
    if (!pixels) return;
    if (!g_linux_overlay_texture) {
        g_linux_overlay_texture = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING, DQIX_LINUX_OVERLAY_WIDTH, DQIX_LINUX_OVERLAY_HEIGHT);
        if (!g_linux_overlay_texture) {
            if (!g_linux_overlay_error_reported) fprintf(stderr, "linux-overlay: SDL texture creation failed: %s\n", SDL_GetError());
            g_linux_overlay_error_reported = 1;
            return;
        }
        if (SDL_SetTextureBlendMode(g_linux_overlay_texture, SDL_BLENDMODE_BLEND) != 0) {
            if (!g_linux_overlay_error_reported) fprintf(stderr, "linux-overlay: SDL texture blend mode failed: %s\n", SDL_GetError());
            g_linux_overlay_error_reported = 1;
        }
    }
    if (SDL_UpdateTexture(g_linux_overlay_texture, NULL, pixels, DQIX_LINUX_OVERLAY_WIDTH * 4) != 0) {
        if (!g_linux_overlay_error_reported) fprintf(stderr, "linux-overlay: SDL texture update failed: %s\n", SDL_GetError());
        g_linux_overlay_error_reported = 1;
        return;
    }
    SDL_RenderGetLogicalSize(g_ren, &logical_width, &logical_height);
    if (logical_width > 0 && logical_height > 0) { width = logical_width; height = logical_height; }
    else if (SDL_GetRendererOutputSize(g_ren, &width, &height) != 0) return;

    {
        Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 255;
        SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
        int have_color = SDL_GetRenderDrawColor(g_ren, &old_r, &old_g, &old_b, &old_a) == 0;
        int have_blend = SDL_GetRenderDrawBlendMode(g_ren, &old_blend) == 0;
        SDL_Rect dim = {0, 0, width, height};
        if (SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND) == 0 &&
            SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 118) == 0) {
            if (SDL_RenderFillRect(g_ren, &dim) != 0 && !g_linux_overlay_error_reported) {
                fprintf(stderr, "linux-overlay: background dim failed: %s\n", SDL_GetError());
                g_linux_overlay_error_reported = 1;
            }
        } else if (!g_linux_overlay_error_reported) {
            fprintf(stderr, "linux-overlay: background dim setup failed: %s\n", SDL_GetError());
            g_linux_overlay_error_reported = 1;
        }
        if (have_color) SDL_SetRenderDrawColor(g_ren, old_r, old_g, old_b, old_a);
        if (have_blend) SDL_SetRenderDrawBlendMode(g_ren, old_blend);
    }

    if (!dqix_linux_overlay_fit_rect(width, height,
                                     &destination.x, &destination.y,
                                     &destination.w, &destination.h)) return;
    if (SDL_RenderCopy(g_ren, g_linux_overlay_texture, NULL, &destination) != 0 &&
        !g_linux_overlay_error_reported) {
        fprintf(stderr, "linux-overlay: SDL_RenderCopy failed: %s\n", SDL_GetError());
        g_linux_overlay_error_reported = 1;
    }
}
#endif

static void present_gl(void) {
    auto_scale();
    present_gl_state();
    uint16_t pow; memcpy(&pow, &g_io[0x304], 2);
    {   /* dynamic widescreen: the game's windows move right (the field doesn't); in battles only the messages (gpu3d.c) */
        int side = 0; gl_wide_shown(&side);
        int on = g_gl && g_wide && g_wide_dyn && !g_wide_pop && !g_side_by_side && !(g_two && !g_single);
        extern int g_ui_shift3d, g_ui_battle3d, g_ui_hold, g_ui_area3d, g_ui_battle_left;
        g_ui_shift3d = on ? side : 0; g_ui_battle3d = g_hud_n > 0;                                            /* (gpu3d.c) */
        g_ui_battle_left = g_hud_right > 0 ? g_hud_right - side + 4 : 0;   /* the battle messages start right of the panels */
        gl_set_ui_shift(on ? side : 0, g_hud_n > 0);                                                          /* (2D ones) */
        /* a menu screen of its own (Stella's, Equipment: its own background over the whole picture):
         * the DS's picture as it is, 4:3 at the right, nothing moved; back once it's the field again.
         * Windows over the field (the menu, its lists) keep moving right instead. */
        static int over, under;
        extern int g_ui_scene, g_ui_windows3d;
        extern int g_ui_bg2d;
        int menu_screen = on && !g_ui_battle3d && (g_ui_area3d >= 1000 ||                     /* a 3D picture over the whole screen (Stella's) */
                          (g_ui_scene < 24 * 32 * 4 / 10 && g_ui_bg2d >= 160));             /* hardly any scene, and a 2D picture of its own
                                                                                               * around it (the quest list), not black */
        if (menu_screen) { over++; under = 0; } else { under++; over = 0; }
        if (!on) g_ui_plain = 0;
        else if (!g_ui_plain && over >= 2) g_ui_plain = 1;
        else if (g_ui_plain && (under >= 6 || (under >= 1 && g_ui_scene >= 24 * 32 * 3 / 4))) g_ui_plain = 0;   /* (the field back: at once) */
        g_ui_hold = g_ui_plain; gl_set_ui_hold(g_ui_plain);
        {   static int lg = -1; if (lg < 0) lg = getenv("DQIX_UILOG") != NULL;     /* tests: what the menu-screen test sees */
            extern int g_ui_npoly;
            if (lg && on) fprintf(stderr, "ui f%llu scene %d win3d %d area3d %d area2d %d battle %d plain %d n %d bg2d %d\n", (unsigned long long)g_frame,
                                  g_ui_scene, g_ui_windows3d, g_ui_area3d, gl_ui_area(), g_ui_battle3d, g_ui_plain, g_ui_npoly, g_ui_bg2d); }
    }
    {   static int lg = -1; static uint64_t last = ~0ull;                   /* tests: the overlays loaded, as they change */
        if (lg < 0) lg = getenv("DQIX_OVLLOG") != NULL;
        if (lg) { extern uint64_t overlays_loaded_mask(void); uint64_t m = overlays_loaded_mask();
            if (m != last) { last = m; fprintf(stderr, "ovl f%llu:", (unsigned long long)g_frame); for (int i = 0; i < 64; i++) if (m >> i & 1) fprintf(stderr, " %d", i); fprintf(stderr, "\n"); } } }
    gl_compose((pow >> 15) & 1, pow & 1);
    wide_update((pow >> 15) & 1);
    hud_update((pow >> 15) & 1);
    gl_dump_hook();
    test_hooks();
    {   /* widescreen: the small screen see-through (O), but solid while the mouse is on it */
        float target = (float)g_small_opacity / 100.0f;
        int ww, wh, mx, my; SDL_GetWindowSize(g_win, &ww, &wh);
        if (target < 1.0f && SDL_GetMouseFocus() == g_win) {
            GlPresent m; canvas_layout(&m, ww, wh);
            SDL_GetMouseState(&mx, &my);
            float lx = ((float)mx + 0.5f - m.ox) / m.sc, ly = ((float)my + 0.5f - m.oy) / m.sc;
            const float* r = m.front ? m.bot : m.top;
            if (lx >= r[0] && lx < r[0] + r[2] && ly >= r[1] && ly < r[1] + r[3]) target = 1.0f;
        }
        g_small_alpha += (target - g_small_alpha) * 0.2f;
        if (fabsf(target - g_small_alpha) < 0.01f) g_small_alpha = target;
    }
    int w, h; SDL_GL_GetDrawableSize(g_win, &w, &h);
    GlPresent v; canvas_layout(&v, w, h);
    {   /* fast forward on: three arrows in the picture's bottom left corner */
        static int tests = -1; if (tests < 0) tests = getenv("DQIX_FAST") != NULL;
        if (g_fast && !tests) {
            float pw = v.cw * v.sc, ph = v.ch * v.sc;
            float bh = fmaxf(16.0f, ph * 0.04f), m = fmaxf(8.0f, ph * 0.02f);
            v.badge[0] = v.ox + m; v.badge[1] = v.oy + ph - m - bh; v.badge[2] = bh * 1.8f; v.badge[3] = bh;
            (void)pw;
        }
    }
    gl_present(&v);
#ifndef _WIN32
    linux_overlay_draw_gl(g_win);
#endif
    window_shot_hook(w, h, 0);
    SDL_GL_SwapWindow(g_win);
    if (g_win2 && SDL_GL_MakeCurrent(g_win2, g_glctx) == 0) {   /* two windows: the other screen */
        int w2, h2; SDL_GL_GetDrawableSize(g_win2, &w2, &h2);
        GlPresent v2; canvas_layout_for(&v2, w2, h2, 1); v2.again = 1;
        gl_present(&v2);
#ifndef _WIN32
        linux_overlay_draw_gl(g_win2);
#endif
        window_shot_hook(w2, h2, 1);
        SDL_GL_SwapWindow(g_win2);
        SDL_GL_MakeCurrent(g_win, g_glctx);
    }
}

static void present(void) {
    static long long skip = -2;                               /* DQIX_PRESENT_FROM=frame: don't draw before it (tests) */
    if (skip == -2) { const char* e = getenv("DQIX_PRESENT_FROM"); skip = e ? atoll(e) : -1; }
    if (skip >= 0 && g_frame < (uint64_t)skip) { if (g_gl) present_gl_state(); return; }
    if (g_gl) { present_gl(); return; }
    void* pix; int pitch;
    if (SDL_LockTexture(g_tex, NULL, &pix, &pitch) == 0) {
        for (int s = 0; s < 2; s++)
            for (int y = 0; y < 192; y++)
                memcpy((uint8_t*)pix + (size_t)(s * 192 + y) * pitch, &g_screen[s][y * 256], 256 * 4);
        SDL_UnlockTexture(g_tex);
    }
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    {   /* each screen where the layout puts it (stacked or side by side, maybe swapped) */
        GlPresent v; canvas_layout(&v, 1, 1);
        SDL_Rect top = { 0, 0, 256, 192 }, bot = { 0, 192, 256, 192 };
        SDL_Rect dt = { (int)v.top[0], (int)v.top[1], (int)v.top[2], (int)v.top[3] }, db = { (int)v.bot[0], (int)v.bot[1], (int)v.bot[2], (int)v.bot[3] };
        if (dt.w > 0) SDL_RenderCopy(g_ren, g_tex, &top, &dt);
        if (db.w > 0) SDL_RenderCopy(g_ren, g_tex, &bot, &db);
    }
    test_hooks();
#ifndef _WIN32
    linux_overlay_draw_sdl();
#endif
    SDL_RenderPresent(g_ren);
}

/* ---------------------------------------------------------------- settings changes (keys and menu) */
static void update_offer(const UpdateInfo* u);
static double g_last_fps;
static void update_title(double fps);
/* a short message in the title bar for two seconds ("Volume 70%") */
static void notice(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(g_notice, sizeof g_notice, fmt, ap); va_end(ap);
    g_notice_until = SDL_GetPerformanceCounter() + g_perf_freq * 2;
    if (g_win) update_title(g_last_fps);
}
static void update_title(double fps) {
    if (fps > 0) g_last_fps = fps;
    { UpdateInfo u; if (update_poll(&u)) update_offer(&u); }
    session_window_close();                                     /* the lobby is over */
    char title[400], ses[300] = "";
    if (session_running()) {
        SessionInfo si; session_info(&si);
        int here = 0; for (int k = 0; k < si.n; k++) here += !si.gone[k];
        if (si.desync) snprintf(ses, sizeof ses, " - OUT OF SYNC: leave the session (Wireless menu)");
        else if (si.state == 4) snprintf(ses, sizeof ses, " - session over: %s", si.message);
        else snprintf(ses, sizeof ses, " - together: %d of %d players, delay %d", here, si.n, si.delay);
        static char last[256]; static uint64_t shown;
        if (strcmp(last, si.message)) { snprintf(last, sizeof last, "%s", si.message); shown = SDL_GetPerformanceCounter(); }
        if (!si.desync && si.state == 3 && SDL_GetPerformanceCounter() - shown < g_perf_freq * 8) snprintf(ses + strlen(ses), sizeof ses - strlen(ses), " - %s", si.message);
    }
    if (g_notice[0] && SDL_GetPerformanceCounter() < g_notice_until) snprintf(title, sizeof title, "Dragon Quest IX " DQIX_VERSION " - %s", g_notice);
    else if (g_paused) snprintf(title, sizeof title, "Dragon Quest IX " DQIX_VERSION " - paused (P to continue)%s", ses);
    else snprintf(title, sizeof title, "Dragon Quest IX " DQIX_VERSION " - %s%s - %.1f fps%s%s", g_classic ? "Classic" : "Enhanced",
                  g_gl && g_opt.lcd ? " - DS Lite screen" : "", fps, g_fast ? " (fast)" : "", ses);
    SDL_SetWindowTitle(g_win, title);
    if (!g_headless && !getenv("DQIX_INPUT_FILE")) {               /* Discord (not in replays) */
        static int was = -1; if (was != g_discord) { was = g_discord; discord_enable(g_discord); }
        discord_activity(session_running() ? "Playing with friends" : "", "DQIX Enhanced " DQIX_VERSION);
    }
}

static void menu_sync(void) {
    {
        SessionInfo si; session_info(&si);
        int in = session_requested();
        menu_enable(MI_SES_START, in && si.host && si.state == 2 && si.n >= 2);
        menu_enable(MI_SES_LEAVE, in);
        menu_enable(MI_SES_HOST, !in); menu_enable(MI_SES_JOIN, !in);
        menu_enable(MI_NET, !in);                            /* a session uses the built-in Wi-Fi only (the PCs must agree) */
    }
    int stacked = !g_side_by_side && !g_single && !((g_corner || g_wide || g_two) && g_gl);
    int wide = g_wide && g_gl && !g_side_by_side;
    menu_check(MI_PAUSE, g_paused);
    menu_check(MI_UPDATE_AUTO, g_update_auto);
    menu_check(MI_TURBO, g_turbo);
    menu_check(MI_DLC, g_opt.dlc);
    menu_check(MI_NET, g_net_want);
    menu_check(MI_SRV_BUILTIN, !strcmp(g_wfc_server, "builtin"));
    menu_check(MI_SRV_ALTWFC, !strcmp(g_wfc_server, "altwfc"));
    menu_check(MI_SRV_WIIMMFI, !strcmp(g_wfc_server, "wiimmfi"));
    menu_check(MI_SRV_CUSTOM, wfc_custom()); menu_enable(MI_SRV_CUSTOM, wfc_custom());
    menu_check(MI_CLASSIC, g_classic); menu_check(MI_ENHANCED, !g_classic);
    menu_enable(MI_ENHANCED, g_gl);
    {
        static const int ids[7] = { MI_RES1, MI_RES2, MI_RES3, MI_RES4, MI_RES6, MI_RES8, MI_RES_AUTO }, sc[7] = { 1, 2, 3, 4, 6, 8, 0 };
        for (int i = 0; i < 7; i++) { menu_check(ids[i], sc[i] ? !g_res_auto && g_scale3d == sc[i] : g_res_auto); menu_enable(ids[i], g_gl && !g_classic); }
    }
    menu_check(MI_POPOUT, g_wide_pop && wide); menu_enable(MI_POPOUT, g_gl);
    menu_enable(MI_SCENE_BARS, wide);
    menu_check(MI_HUD, g_hud && wide); menu_enable(MI_HUD, g_gl);
    menu_check(MI_SMOOTH, g_smooth); menu_enable(MI_SMOOTH, g_gl && !g_classic);
    menu_check(MI_HD_TEXT, g_hd_text != 0); menu_enable(MI_HD_TEXT, g_gl != 0);
    menu_check(MI_DISCORD, g_discord && discord_available()); menu_enable(MI_DISCORD, discord_available());
    menu_check(MI_TEXPACK, g_tex_pack); menu_check(MI_TEXUP, g_tex_up); menu_check(MI_TEXDUMP, g_tex_dump);
    for (int i = MI_TEXPACK; i <= MI_TEXFOLDER; i++) menu_enable(i, g_gl != 0);
    menu_check(MI_MUSPACK, g_mus_pack); menu_check(MI_MUSNAMES, g_mus_names);
    menu_check(MI_SOFT_SHADOWS, g_gl_soft_alpha != 0); menu_enable(MI_SOFT_SHADOWS, g_gl && !g_classic);
    menu_check(MI_SMOOTH_SHADE, g_gl_soft_shade != 0); menu_enable(MI_SMOOTH_SHADE, g_gl && !g_classic);
    for (int i = 0; i < 3; i++) { menu_check(MI_OUTLINE0 + i, g_gl_outline == (i == 0 ? 1 : i == 1 ? 2 : 0)); menu_enable(MI_OUTLINE0 + i, g_gl && !g_classic); }
    menu_check(MI_LCD, g_opt.lcd); menu_enable(MI_LCD, g_gl);
    int lcd = g_gl && g_opt.lcd;
    menu_check(MI_LCDCOL, g_opt.lcd_color); menu_enable(MI_LCDCOL, lcd);
    menu_check(MI_GRID, g_opt.grid); menu_enable(MI_GRID, lcd);
    menu_check(MI_GHOST, g_opt.ghost); menu_enable(MI_GHOST, lcd);
    for (int i = 0; i < 4; i++) { menu_check(MI_BL1 + i, g_opt.backlight == i + 1); menu_enable(MI_BL1 + i, lcd); }
    menu_check(MI_SHELL, g_opt.shell); menu_enable(MI_SHELL, g_gl && stacked);
    for (int i = 0; i < N_SHELLS; i++) { menu_check(MI_SHELLC0 + i, g_opt.shell_color == i); menu_enable(MI_SHELLC0 + i, g_gl && stacked && g_opt.shell); }
    menu_check(MI_GAP, g_opt.gap || g_opt.shell); menu_enable(MI_GAP, g_gl && stacked && !g_opt.shell);
    menu_check(MI_STACKED, stacked); menu_check(MI_SIDE, g_side_by_side);
    menu_check(MI_CORNER, g_corner && !g_wide && !g_side_by_side); menu_enable(MI_CORNER, g_gl);
    menu_check(MI_WIDE, g_wide && !g_wide_dyn && !g_side_by_side); menu_enable(MI_WIDE, g_gl);
    menu_check(MI_WIDE_DYN, g_wide && g_wide_dyn && !g_side_by_side); menu_enable(MI_WIDE_DYN, g_gl);
    menu_check(MI_SINGLE, wide ? g_small_hide : g_single);
    for (int i = 0; i < 4; i++) { menu_check(MI_CORNER0 + i, g_small_corner == i); menu_enable(MI_CORNER0 + i, g_gl && !g_wide_dyn); }
    for (int i = 0; i < 3; i++) { menu_check(MI_SSIZE0 + i, g_small_size == i); menu_enable(MI_SSIZE0 + i, g_gl && !g_wide_dyn); }
    menu_check(MI_MUTE, g_mute);
    menu_enable(MI_RESET, !session_requested());
    menu_check(MI_TWO, g_two && !g_side_by_side && g_gl); menu_enable(MI_TWO, g_gl);
    menu_check(MI_SWAP, g_swap && !wide); menu_enable(MI_SWAP, !wide);
    for (int i = 0; i < 4; i++) { menu_check(MI_OPACITY1 + i, g_small_opacity == 100 - 25 * i); menu_enable(MI_OPACITY1 + i, g_gl); }
    menu_check(MI_ANALOG, g_analog_walk);
    for (int i = 0; i < 5; i++) { menu_check(MI_LANG1 + i, g_fw_language == i + 1); menu_enable(MI_LANG1 + i, rom_is_eur()); }
    for (int i = 0; i < 4; i++) { menu_check(MI_SIZE1 + i, g_scale == i + 1); menu_enable(MI_SIZE1 + i, !g_opt.actual); }
    menu_check(MI_INTEGER, g_opt.integer); menu_enable(MI_INTEGER, !g_opt.actual);
    menu_check(MI_ACTUAL, g_opt.actual); menu_enable(MI_ACTUAL, g_gl);
    menu_check(MI_FULL, g_fullscreen);
}

static const char* CONTROLS_TEXT =
    "These are the usual keys; Game > Controls changes any of them.\n\n"
    "DS button      Keyboard              Controller\n"
    "D-pad          arrow keys            D-pad / left stick\n"
    "A / B          X / Z                 right / bottom button\n"
    "X / Y          S / A                 top / left button\n"
    "L / R          Q / W                 shoulder buttons\n"
    "               (turn the camera)     or the right stick, while walking\n"
    "START          Enter                 Start\n"
    "SELECT         Right Shift           Back\n"
    "Touch screen   left mouse button on the bottom screen\n\n"
    "Screens: F2 layout   U single screen (left trigger)   F9 swap (right trigger; widescreen: hold to see the other)\n"
    "Widescreen's small screen: K corner (right stick click)   I size   O opacity (left stick click)\n"
    "Tab fast forward (on/off)   P pause   Ctrl+R reset (controller: hold Start+Select+L+R)   - / = volume\n"
    "F5 Classic / Enhanced   F4 3D resolution   F6 DS Lite screen   F7 DS Lite console   F8 actual size   F3 window size\n"
    "F11 or Alt+Enter fullscreen (mouse at the top: menu bar)   F12 screenshot   F10 bug report   Esc leave fullscreen / quit (asks first)\n"
    "In fullscreen, double-click outside the touch screen to go back to the window.";

/* A message box whose long lines are wrapped (the system boxes size themselves
 * to the longest line, which made the help as wide as the screen). */
static int info_box(Uint32 flags, const char* title, const char* text, SDL_Window* win) {
    size_t n = strlen(text);
    char* out = malloc(n * 2 + 1);
    if (!out) return SDL_ShowSimpleMessageBox(flags, title, text, win);
    const int W = 76;
    size_t o = 0; int col = 0, wrapped = 0;
    for (size_t i = 0; i < n;) {
        if (text[i] == '\n') { out[o++] = '\n'; col = 0; i++; continue; }
        size_t j = i; while (j < n && text[j] != ' ' && text[j] != '\n') j++;    /* a word */
        int len = (int)(j - i);
        if (col > 0 && col + len > W) { while (o > 0 && out[o - 1] == ' ') o--; out[o++] = '\n'; col = 0; wrapped = 1; }
        memcpy(out + o, text + i, (size_t)len); o += (size_t)len; col += len; i = j;
        if (len) wrapped = 0;
        while (i < n && text[i] == ' ') { if (!(wrapped && col == 0)) { out[o++] = ' '; col++; } i++; }
    }
    out[o] = 0;
    int r = SDL_ShowSimpleMessageBox(flags, title, out, win);
    free(out);
    return r;
}

static int g_menu_peek;          /* fullscreen: the menu bar is showing because the mouse is at the top */
static void settings_save(void);

/* ---------------------------------------------------------------- playing with friends far away */
static const char SESSION_HELP[] =
    "Tag mode, visiting other worlds and inviting friends (Pavo) need DS systems close to each other. "
    "This lets friends anywhere play those parts together, up to 4 players.\n\n"
    "How: one of you hosts (Wireless > Play with friends far away > Host), the others join with the code "
    "the host's game shows. When everyone is in, the host presses Start the game. All the games restart together, "
    "each from its own save, and from then on every PC runs every player's DS: yours in the window, your "
    "friends' unseen. Only the buttons travel between the PCs, so the DSes really are side by side - "
    "on every PC at once.\n\n"
    "What to expect:\n"
    "- Save your adventure before hosting or joining: the game restarts.\n"
    "- Everyone needs the same version of this game.\n"
    "- Buttons react a little later than usual (the input delay, a few frames, depending on the distance).\n"
    "- If someone's PC is slow, everyone waits for it while your DSes are talking wirelessly.\n"
    "- The built-in Wi-Fi works as usual (DQVC, the downloads), with the host's files for everyone. Other online servers are off during a session.\n"
    "- Your game saves to your own save as always. Leaving (or closing the game) switches your DS off for the others.\n\n"
    "Getting in: the code works on any internet - it connects through a small relay on the internet when the "
    "host's router doesn't let friends in directly (apartment and mobile internet, for example). Where the router "
    "does, the host's game also shows an address that connects directly, with a little less delay. "
    "Windows may ask whether to allow the game on networks: allow it.";

/* start this program again, in a session or not (the save is written first) */
static void restart_with(const char* session) {
    backup_flush();
    settings_save();
    char pos[48] = "";                                           /* the new window where this one is */
    if (g_win && !g_fullscreen) { int x, y; SDL_GetWindowPosition(g_win, &x, &y); snprintf(pos, sizeof pos, "%d,%d", x, y); }
#ifdef _WIN32
    SetEnvironmentVariableA("DQIX_WINPOS", pos[0] ? pos : NULL);
    SetEnvironmentVariableA("DQIX_SESSION", session);
    SetEnvironmentVariableA("DQIX_PLAYER_NAME", g_player_name[0] ? g_player_name : NULL);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    wchar_t* cmd = _wcsdup(GetCommandLineW());
    DWORD flags = GetConsoleWindow() ? 0 : CREATE_NO_WINDOW;   /* started without a console (dqix.exe double-clicked): stay without */
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        exit(0);
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dragon Quest IX", "The game couldn't start again by itself. Close it and start it again.", g_win);
#else
    if (pos[0]) setenv("DQIX_WINPOS", pos, 1); else unsetenv("DQIX_WINPOS");
    if (session) setenv("DQIX_SESSION", session, 1); else unsetenv("DQIX_SESSION");
    if (g_player_name[0]) setenv("DQIX_PLAYER_NAME", g_player_name, 1);
    extern char** g_argv;
    execv("/proc/self/exe", g_argv);
#endif
}

static int choose(const char* title, const char* text, const char* const* buttons, int n, int def, int cancel);
static int ask_yes(const char* title, const char* text, const char* yes) {
    const char* b[2] = { yes, "Cancel" };
    return choose(title, text, b, 2, 0, 1) == 0;
}

static int ask_name(void) {
    char* t = g_win ? ask_text(g_win, "Your name", "The name your friends see in the session (your hero's name, say):") : NULL;
    if (!t) return g_player_name[0] != 0;
    t[strcspn(t, "\r\n")] = 0;
    if (t[0]) snprintf(g_player_name, sizeof g_player_name, "%s", t);
    free(t);
    settings_save();
    return g_player_name[0] != 0;
}

static void session_action(int id) {
    static const char RESTART[] = "\n\nThe game restarts for this: save your adventure first (in the game) if you haven't.";
    switch (id) {
    case MI_SES_HOST: {
        if (!g_player_name[0] && !ask_name()) return;
        char text[400]; snprintf(text, sizeof text, "Host a session: your friends join you (up to 3 of them).%s", RESTART);
        if (!ask_yes("Host a session", text, "Host")) return;
        char ses[64]; snprintf(ses, sizeof ses, "host:%d", g_session_port);
        restart_with(ses);
        break;
    }
    case MI_SES_JOIN: {
        if (!g_player_name[0] && !ask_name()) return;
        char* t = g_win ? ask_text(g_win, "Join a friend",
            "The code your friend's game shows (like DQ-7KF2-QM4P), or their address (like 203.0.113.7:27900). "
            "The game restarts and connects: save your adventure first if you haven't.") : NULL;
        if (!t) return;
        char* a = t; while (*a == ' ') a++;
        a[strcspn(a, " \r\n")] = 0;
        if (!a[0]) { free(t); return; }
        char ses[200]; snprintf(ses, sizeof ses, "join:%s", a);
        free(t);
        restart_with(ses);
        break;
    }
    case MI_SES_LEAVE:
        if (!ask_yes("Leave the session", "Leave and play alone? Your DS is switched off for the others; your game restarts "
                     "(it was saved whenever you saved in the game).", "Leave")) return;
        restart_with(NULL);
        break;
    case MI_SES_START: session_start_now(); break;
    case MI_SES_NAME: ask_name(); break;
    case MI_SES_HELP: info_box(SDL_MESSAGEBOX_INFORMATION, "Playing with friends far away", SESSION_HELP, g_win); break;
    }
}

/* ---------------------------------------------------------------- updates (update.c) */
/* ---------------------------------------------------------------- sending a bug report (GitHub)
 * Nothing is uploaded by the game: it packs the report into one .zip, opens a
 * new GitHub issue in the browser with the details filled in, and opens the
 * folder with the zip so the player can drag it in. Only after a clear
 * warning (GitHub is public), with the save files optional. */
static void url_add(char* out, size_t n, const char* s) {
    size_t k = strlen(out);
    for (; *s && k + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[k++] = (char)c;
        else { snprintf(out + k, n - k, "%%%02X", c); k += 3; }
    }
    out[k] = 0;
}

/* Mods > Export the game's songs: on a thread of its own (it reads only the ROM), the progress in the title */
static volatile int g_exp_done, g_exp_total, g_exp_running, g_exp_finished;
static void exp_progress(int d, int t) { g_exp_done = d; g_exp_total = t; }
static int exp_thread(void* u) {
    (void)u;
    int n = music_export("music/export", exp_progress);
    g_exp_done = n; g_exp_finished = 1; g_exp_running = 0;
    return 0;
}
static void open_folder(const char* path);
static void music_export_start(void) {
    if (g_exp_running) { notice("Already exporting the songs (%d of %d)", g_exp_done, g_exp_total); return; }
#ifdef _WIN32
    _mkdir("music"); _mkdir("music/export");
#else
    mkdir("music", 0777); mkdir("music/export", 0777);
#endif
    g_exp_running = 1; g_exp_finished = 0; g_exp_done = 0; g_exp_total = 0;
    SDL_Thread* t = SDL_CreateThread(exp_thread, "music export", NULL);
    if (t) { SDL_DetachThread(t); notice("Exporting the game's songs to music\\export..."); }
    else g_exp_running = 0;
}
static void music_export_poll(void) {
    static uint64_t last;
    if (g_exp_running && g_exp_total && SDL_GetPerformanceCounter() - last > g_perf_freq / 2) {
        last = SDL_GetPerformanceCounter(); notice("Exporting the game's songs: %d of %d", g_exp_done, g_exp_total);
    }
    if (g_exp_finished) {
        g_exp_finished = 0;
        notice(g_exp_done ? "Exported %d songs to music\\export (loops.txt has their loop points)" : "Couldn't export the songs (%d)", g_exp_done);
        if (g_exp_done) open_folder("music/export");
    }
}

static void open_folder(const char* path) {
#ifdef _WIN32
    char full[MAX_PATH]; if (!GetFullPathNameA(path, sizeof full, full, NULL)) return;
    char cmd[MAX_PATH + 40]; snprintf(cmd, sizeof cmd, "explorer.exe \"%s\"", full);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
#else
    char url[700]; snprintf(url, sizeof url, "file://%s", path); SDL_OpenURL(url);
#endif
}
static void show_in_folder(const char* path) {
#ifdef _WIN32
    char full[MAX_PATH]; if (!GetFullPathNameA(path, sizeof full, full, NULL)) return;
    for (char* p = full; *p; p++) if (*p == '/') *p = '\\';
    char cmd[MAX_PATH + 40]; snprintf(cmd, sizeof cmd, "explorer.exe /select,\"%s\"", full);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
#else
    char dir[600]; snprintf(dir, sizeof dir, "%s", path);
    char* s = strrchr(dir, '/'); if (s) *s = 0;
    char url[700]; snprintf(url, sizeof url, "file://%s", dir);
    SDL_OpenURL(url);
#endif
}

static int choose(const char* title, const char* text, const char* const* buttons, int n, int def, int cancel);
static void report_send(const char* dir, const char* description) {
    static const char* WARN =
        "Send this report to the people working on the game?\n\n"
        "Your browser opens a new bug report on the project's GitHub page, already filled in, and the report's file is copied "
        "for you: in the page you only press Ctrl+V, then Create. You need a free GitHub account (no account: paste it "
        "on the Discord instead).\n\n"
        "Everything on GitHub is public: anyone can see the report and download its file. The file contains:\n"
        "  - a screenshot of the game, and of the window with your display settings\n"
        "  - every button you pressed since the game started\n"
        "  - your save file (your characters' names and progress), at the start and now, if ticked below\n"
        "  - your description\n"
        "  - the game's version, log and settings: your graphics card, the Wi-Fi settings and the console's made-up ID; "
        "after playing with friends also their names and the host's internet address\n"
        "  - timing measurements\n\n"
        "Nothing is sent until you press Submit on GitHub.";
    int with_saves = 1;
    const char* b[2] = { "Open GitHub", "Don't send" };
    int r = choice_box(g_win, "Send the bug report", WARN, b, 2, 0, 1, "Include my save file (it makes the problem much easier to find)", &with_saves);
    if (r < 0) {                                                  /* no Windows dialog: three buttons instead of the tick */
        const char* b3[3] = { "Send with my save", "Send without my save", "Don't send" };
        int r3 = choose("Send the bug report", WARN, b3, 3, 0, 2);
        if (r3 == 2) return;
        with_saves = r3 == 0; r = 0;
    }
    if (r != 0) return;
    char zip[400];
    if (!report_zip(dir, with_saves, zip, sizeof zip)) { info_box(SDL_MESSAGEBOX_ERROR, "Send the bug report", "The report couldn't be packed. It is still in the reports folder.", g_win); return; }
    const char* zname = strrchr(zip, '/'); zname = zname ? zname + 1 : zip;
    char title[160] = "Bug report (" DQIX_VERSION ")";
    if (description && description[0]) {
        char first[90]; snprintf(first, sizeof first, "%s", description);
        first[strcspn(first, "\r\n")] = 0;
        if (strlen(first) > 70) strcpy(first + 67, "...");
        snprintf(title, sizeof title, "%s (" DQIX_VERSION ")", first);
    }
    int copied = clipboard_set_file(zip);                       /* Ctrl+V in the page attaches it */
    static char body[6000];
    snprintf(body, sizeof body,
             "%s"
             "**Version:** %s (%s)\n**Mode:** %s\n**Graphics:** %s\n\n**What happened:**\n%s\n\n"
             "**Report file:** `%s`%s%s\n",
             copied ? "## \U0001F449 Click here and press Ctrl+V to attach your report, then press Create\n"
                      "_(Delete this line if you like. The report is attached once you see a link like `[v32-....zip](...)` below.)_\n\n"
                    : "## \U0001F449 Drag the report file from the folder that just opened into this box, then press Create\n\n",
             DQIX_VERSION,
#ifdef _WIN32
             "Windows",
#else
             "Linux",
#endif
             g_classic ? "Classic" : "Enhanced", g_gl ? gl_renderer_name() : "software",
             description && description[0] ? description : "(please describe what you did and what went wrong)",
             zname, with_saves ? "" : " (without the save file)", copied ? "" : " (in the folder that opened)");
    static char url[16000];
    snprintf(url, sizeof url, "https://github.com/" UPDATE_REPO "/issues/new?labels=bug&title=");
    url_add(url, sizeof url, title);
    strncat(url, "&body=", sizeof url - strlen(url) - 1);
    url_add(url, sizeof url, body);
    if (!copied) show_in_folder(zip);
    if (SDL_OpenURL(url) != 0) {
        char m[600]; snprintf(m, sizeof m, "The browser didn't open. Go to https://github.com/%s/issues/new and attach %s.", UPDATE_REPO, zip);
        info_box(SDL_MESSAGEBOX_INFORMATION, "Send the bug report", m, g_win);
    }
    /* the two steps left, until the player says it's done */
    static const char STEPS[] =
        "Almost done - 2 steps in the page that just opened:\n\n"
        "  1.  Click in the big text box and press  Ctrl+V\n"
        "        (this attaches your report: a link to the file appears)\n\n"
        "  2.  Press  Create  (the green button)\n\n"
        "No GitHub account? Press Ctrl+V in a message on the Discord instead.\n\n"
        "Ctrl+V did nothing? Press Copy again, or Show the file and drag it into the box.";
    static const char STEPS_DRAG[] =
        "Almost done - 2 steps:\n\n"
        "  1.  Drag the report file from the folder that opened into the big text box on the page\n\n"
        "  2.  Press  Create  (the green button)\n\n"
        "No GitHub account? Drop the file in a message on the Discord instead.";
    const char* sb[3] = { "Done", copied ? "Copy again" : "Show the file", "Show the file" };
    for (;;) {
        int r = choose("Send the bug report", copied ? STEPS : STEPS_DRAG, sb, copied ? 3 : 2, 0, 0);
        if (r == 1 && copied) { clipboard_set_file(zip); notice("The report is copied: press Ctrl+V in the page"); }
        else if (r == 1 || r == 2) show_in_folder(zip);
        else break;
    }
    g_next_frame = SDL_GetPerformanceCounter();
}

/* A message with buttons: Windows' own dialog (buttons sized to their words),
 * SDL's elsewhere. Returns the chosen button's index, or `cancel`. */
static int choose(const char* title, const char* text, const char* const* buttons, int n, int def, int cancel) {
    int r = choice_box(g_win, title, text, buttons, n, def, cancel, NULL, NULL);
    if (r >= 0) return r;
    SDL_MessageBoxButtonData b[8];
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        b[i].flags = (i == def ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0) | (i == cancel ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0);
        b[i].buttonid = i; b[i].text = buttons[i];
    }
    const SDL_MessageBoxData d = { SDL_MESSAGEBOX_INFORMATION, g_win, title, text, n, b, NULL };
    int out = cancel;
    if (SDL_ShowMessageBox(&d, &out) != 0 || out < 0) return cancel;
    return out;
}

/* Esc, the window's X or Alt+F4: ask first (an accidental Esc lost hours of
 * play). "Keep playing" is the default, so Enter doesn't quit either. */
static void quit_ask(void) {
    if (!g_confirm_quit || getenv("DQIX_NO_CONFIRM")) exit(0);
    static int asking;
    if (asking) return;
    asking = 1;
    static const char* b[2] = { "Quit", "Keep playing" };
    static const char TEXT[] = "Quit Dragon Quest IX?\n\nAnything since you last saved in the game will be lost.";
    int dont = 0;
    int r = choice_box(g_win, "Quit", TEXT, b, 2, 1, 1, "Don't ask again", &dont);
    if (r < 0) r = choose("Quit", TEXT, b, 2, 1, 1);
    asking = 0;
    g_next_frame = SDL_GetPerformanceCounter();                 /* the game waited: don't race to catch up */
    if (r != 0) return;
    if (dont) { g_confirm_quit = 0; settings_save(); }
    exit(0);
}

/* Game > Save file: a copy of your save anywhere, or a save from another copy
 * of the game or an emulator (raw .sav like melonDS's, DeSmuME's .dsv with its
 * footer). The save in use is kept in save-backups/ first; the game then
 * starts again with the new one. */
static void save_export(void) {
    char path[1024];
    if (!file_dialog(g_win, 1, "Export your save", "DS save (*.sav)|*.sav|All files (*.*)|*.*", "dqix.sav", path, sizeof path)) {
#ifndef _WIN32
        notice("Your save is %s, next to the game: copy that file", backup_path());
#endif
        return;
    }
    backup_flush();
    uint32_t n; const uint8_t* img = backup_image(&n);
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(img, 1, n, f) != n) { if (f) fclose(f); info_box(SDL_MESSAGEBOX_ERROR, "Export your save", "The file couldn't be written there.", g_win); return; }
    fclose(f);
    notice("Save exported");
}
static void save_backup_copy(char* out, size_t n) {           /* the save in use -> save-backups/dqix-<date>.sav */
    out[0] = 0;
#ifdef _WIN32
    _mkdir("save-backups");
#else
    mkdir("save-backups", 0755);
#endif
    time_t now = time(NULL); struct tm* t = localtime(&now);
    snprintf(out, n, "save-backups/dqix-%04d%02d%02d-%02d%02d%02d.sav", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    uint32_t sz; const uint8_t* img = backup_image(&sz);
    FILE* f = fopen(out, "wb");
    if (!f) { out[0] = 0; return; }
    fwrite(img, 1, sz, f); fclose(f);
}
static void restart_with(const char* session);
static void save_import(void) {
    if (session_requested()) { info_box(SDL_MESSAGEBOX_INFORMATION, "Import a save", "Leave the session first (Wireless menu).", g_win); return; }
    char path[1024];
    if (!file_dialog(g_win, 0, "Import a save", "DS saves (*.sav;*.dsv;*.bin)|*.sav;*.dsv;*.bin|All files (*.*)|*.*", NULL, path, sizeof path)) {
#ifndef _WIN32
        notice("Put the save next to the game as %s (the game not running)", backup_path());
#endif
        return;
    }
    static uint8_t buf[1 << 20];
    FILE* f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    /* DQIX's save is 64 KB; emulators add a footer (DeSmuME) or keep a bigger chip's size */
    uint32_t size = n >= 0x10000 ? 0x10000 : (uint32_t)n;
    if (!size || backup_check_image(buf, size) == 0) {
        info_box(SDL_MESSAGEBOX_ERROR, "Import a save", "That file isn't a Dragon Quest IX save (or it is damaged). Nothing was changed.", g_win);
        return;
    }
    char text[1400], kept[300];
    snprintf(text, sizeof text, "Use this save?\n\n%s\n\nThe game starts again with it. Your current save is kept as a copy in the save-backups folder, next to the game.", path);
    if (!ask_yes("Import a save", text, "Import and restart")) return;
    save_backup_copy(kept, sizeof kept);
    if (!kept[0]) { info_box(SDL_MESSAGEBOX_ERROR, "Import a save", "Your current save couldn't be copied to save-backups, so nothing was changed.", g_win); return; }
    backup_replace(buf, size);
    restart_with(NULL);
}

static void update_go(const char* branch, const char* version) {
    if (session_requested()) { info_box(SDL_MESSAGEBOX_INFORMATION, "Update", "Leave the session first (Wireless menu), then update.", g_win); return; }
    if (!update_project_dir()) {
        info_box(SDL_MESSAGEBOX_INFORMATION, "Update", "This copy of the game wasn't made with BUILD.bat, so it can't update itself. "
                 "Download the new version from https://github.com/" UPDATE_REPO " instead.", g_win);
        return;
    }
    char text[700];
    snprintf(text, sizeof text, "Update to %s?\n\nThe game closes, downloads %s, builds it from your ROM again (a few minutes; "
             "a window shows how far it is) and starts again. Your saves, settings and extras are kept, and the current "
             "version is kept in the update folder in case you want to go back (update\\rollback.bat).", version, version);
    { const char* b[2] = { "Update", "Cancel" }; if (choose("Update", text, b, 2, 0, 1) != 0) return; }
    backup_flush();
    settings_save();
    if (g_win) SDL_SetWindowTitle(g_win, "Dragon Quest IX - downloading the update...");
    char err[200] = "";
    if (update_install(branch, version, err, sizeof err)) exit(0);
    char m[400]; snprintf(m, sizeof m, "The update couldn't be downloaded: %s.", err);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Update", m, g_win);
}

/* at start: a newer stable version */
static void update_offer(const UpdateInfo* u) {
    if (!u->ok || !update_newer(u->stable, DQIX_VERSION) || !strcmp(u->stable, g_update_skip) || session_requested()) return;
    /* the nightly too, when it's newer still: an optional second choice */
    int nightly = u->nightly[0] && strcmp(u->nightly, u->stable) && update_newer(u->nightly, u->stable);
    char text[500];
    snprintf(text, sizeof text, "Dragon Quest IX %s is available (you have %s).\n\nUpdate now?%s", u->stable, DQIX_VERSION,
             nightly ? "\n\nOr try the nightly version: the newest work, but less tested." : "");
    char upd[64], ngt[64];
    snprintf(upd, sizeof upd, "Update to %s", u->stable);
    snprintf(ngt, sizeof ngt, "Install nightly %s", u->nightly);
    const char* b[4]; int id[4], nb = 0;
    id[nb] = 1; b[nb++] = upd;
    if (nightly) { id[nb] = 2; b[nb++] = ngt; }
    id[nb] = 3; b[nb++] = "Skip this version";
    id[nb] = 0; b[nb++] = "Later";
    int r = choose("Update available", text, b, nb, 0, nb - 1);
    if (r < 0 || r >= nb) return;
    if (id[r] == 1) update_go("stable", u->stable);
    else if (id[r] == 2) update_go("main", u->nightly);
    else if (id[r] == 3) { snprintf(g_update_skip, sizeof g_update_skip, "%s", u->stable); settings_save(); }
}

/* Help > Check for updates */
static void update_menu(void) {
    if (g_win) SDL_SetWindowTitle(g_win, "Dragon Quest IX - checking for updates...");
    UpdateInfo u; update_check_now(&u);
    if (!u.ok) {
        char m[300]; snprintf(m, sizeof m, "Couldn't check for updates: %s.", u.error[0] ? u.error : "no answer");
        info_box(SDL_MESSAGEBOX_INFORMATION, "Updates", m, g_win);
        return;
    }
    const char* tag_s = update_newer(u.stable, DQIX_VERSION) ? " (newer)" : !strcmp(u.stable, DQIX_VERSION) ? " (this one)" : " (older)";
    const char* tag_n = update_newer(u.nightly, DQIX_VERSION) ? " (newer)" : !strcmp(u.nightly, DQIX_VERSION) ? " (this one)" : " (older)";
    char back[200] = "";
    if (update_newer(DQIX_VERSION, u.stable))
        snprintf(back, sizeof back, "\n\nInstalling stable goes back to %s, an older version than this one. Your saves and settings are kept.", u.stable);
    char text[900];
    snprintf(text, sizeof text,
             "This copy: %s\n\nStable: %s%s\nTested; recommended for everyone.\n\nNightly: %s%s\nThe newest work: new features "
             "and fixes first, but less tested.%s\n\nFriends playing together need the same version.",
             DQIX_VERSION, u.stable, tag_s, u.nightly, tag_n, back);
    char bs[64], bn[64];
    snprintf(bs, sizeof bs, "Install stable %s", u.stable);
    snprintf(bn, sizeof bn, "Install nightly %s", u.nightly);
    const char* b[3]; int id[3], nb = 0;
    if (strcmp(u.stable, DQIX_VERSION)) { id[nb] = 1; b[nb++] = bs; }
    if (strcmp(u.nightly, DQIX_VERSION) && strcmp(u.nightly, u.stable)) { id[nb] = 2; b[nb++] = bn; }
    id[nb] = 0; b[nb++] = "Close";
    int r = choose("Updates", text, b, nb, nb - 1, nb - 1);
    if (r < 0 || r >= nb) return;
    if (id[r] == 1) update_go("stable", u.stable);
    else if (id[r] == 2) update_go("main", u.nightly);
}

/* the lobby: until everyone is in */
static void present(void);
static void do_action(int id);
static void lobby_draw(void) {
    SessionInfo si; session_info(&si);
    static int copied;
    if (si.host && si.ready && !copied) { if (si.reach != 3) SDL_SetClipboardText(si.address); copied = 1; }
    for (int s = 0; s < 2; s++) for (int i = 0; i < 256 * 192; i++) g_screen[s][i] = s ? 0x10203Au : 0x0B1830u;
    uint32_t* top = g_screen[0]; uint32_t* bot = g_screen[1];
    const uint32_t W = 0xFFFFFFu, Y = 0xFFD860u, G = 0xA8B8D8u, R = 0xFF8080u;
    if (!text_ready()) {                                     /* no font: the title bar says it */
        char t[300]; snprintf(t, sizeof t, "Session: %s (Enter = start, Esc = cancel)", si.state == 2 ? (si.host ? si.address : "waiting for the host") : si.message);
        if (g_win) SDL_SetWindowTitle(g_win, t);
        present(); return;
    }
    int y = 6;
    text_draw(top, 256, 192, 8, y, 15, Y, "Playing with friends far away"); y += 22;
    if (si.state == 5) { text_wrap(top, 256, 192, 8, y, 240, 12, R, si.message); text_wrap(bot, 256, 192, 8, 8, 240, 12, G, "Esc: back (the game restarts on its own)."); present(); return; }
    if (si.host) {
        if (!si.ready) { text_draw(top, 256, 192, 8, y, 12, W, "Getting ready for friends..."); y += 18; }
        else {
            text_draw(top, 256, 192, 8, y, 12, G, si.reach == 2 ? "Give your friends this code:" :
                                                  si.reach == 3 ? "Same house only:" : "Give your friends this address:"); y += 15;
            text_draw(top, 256, 192, 12, y, 15, W, si.address); y += 19;
            text_draw(top, 256, 192, 12, y, 10, G, "(copied - paste it into a message)"); y += 15;
            y = text_wrap(top, 256, 192, 8, y, 240, 10, G, si.hint);
        }
    } else {
        text_draw(top, 256, 192, 8, y, 12, G, si.state == 1 ? "Connecting to" : "Connected to"); y += 15;
        text_draw(top, 256, 192, 12, y, 14, W, si.address); y += 20;
        if (si.state == 2) text_wrap(top, 256, 192, 8, y, 240, 12, G, "You're in. Waiting for the host to start the session.");
    }
    int by = 6;
    text_draw(bot, 256, 192, 8, by, 13, Y, "Players"); by += 18;
    for (int k = 0; k < si.n && si.state >= 2; k++) {
        char line[96];
        if (k == si.me) snprintf(line, sizeof line, "%d.  %s  (you)", k + 1, si.names[k]);
        else if (si.host && k > 0) snprintf(line, sizeof line, "%d.  %s  (%d ms)", k + 1, si.names[k], si.ping_ms[k]);
        else snprintf(line, sizeof line, "%d.  %s", k + 1, si.names[k]);
        text_draw(bot, 256, 192, 14, by, 13, W, line); by += 17;
    }
    by = 128;
    if (si.message[0]) by = text_wrap(bot, 256, 192, 8, by, 240, 10, G, si.message);
    text_wrap(bot, 256, 192, 8, 164, 240, 11, Y, si.host ? (si.n >= 2 ? "Enter: start (everyone is here)    Esc: cancel" : "Waiting for friends...    Esc: cancel")
                                                         : "Esc: cancel");
    { static int calls; const char* shot = getenv("DQIX_LOBBY_SHOT");     /* tests: the lobby after a few seconds */
      if (shot && ++calls == 120) save_bmp_screens((const uint32_t(*)[256 * 192])g_screen, shot); }
    present();
}

/* Windows: the same in a window of its own, with buttons */
static void lobby_window(void) {
    SessionInfo si; session_info(&si);
    if (!g_win || si.state == 3 || si.state == 4 || !session_window_open(g_win)) { session_window_close(); return; }
    SessionWindow w; memset(&w, 0, sizeof w);
    w.host = si.host; w.failed = si.state == 5; w.can_start = si.host && si.state == 2 && si.n >= 2;
    if (si.host) {
        snprintf(w.heading, sizeof w.heading, "%s", !si.ready ? "Getting ready for friends..." :
                 si.reach == 2 ? "Give your friends this code (they choose Join a friend and type it):" :
                 si.reach == 3 ? "Only friends in the same house can join (this PC's home address):" :
                                 "Give your friends this address (they choose Join a friend and paste it):");
        if (si.ready) snprintf(w.address, sizeof w.address, "%s", si.address);
        snprintf(w.hint, sizeof w.hint, "%s", si.ready ? si.hint : "");
    } else {
        snprintf(w.heading, sizeof w.heading, "%s", si.state == 1 ? "Connecting to:" : "Connected to:");
        snprintf(w.address, sizeof w.address, "%s", si.address);
        snprintf(w.hint, sizeof w.hint, "%s", si.state == 2 ? "You're in. The game starts when the host presses Start." : "");
    }
    for (int k = 0; k < si.n && si.state >= 2 && k < 4; k++) {
        if (k == si.me) snprintf(w.players[k], sizeof w.players[k], "%d.   %s   (you)", k + 1, si.names[k]);
        else if (si.host && k > 0) snprintf(w.players[k], sizeof w.players[k], "%d.   %s   (ping %d ms)", k + 1, si.names[k], si.ping_ms[k]);
        else snprintf(w.players[k], sizeof w.players[k], "%d.   %s%s", k + 1, si.names[k], k == 0 ? "   (host)" : "");
        w.n_players = k + 1;
    }
    if (si.state == 5) snprintf(w.status, sizeof w.status, "%s", si.message);
    else if (si.host) snprintf(w.status, sizeof w.status, "%s%s%s", si.message, si.message[0] ? "\n" : "",
                               si.n >= 2 ? "When everyone is here, press Start the game." : "Waiting for friends to join (up to 3)...");
    else snprintf(w.status, sizeof w.status, "%s", si.message);
    session_window_update(&w);
    switch (session_window_action()) {
    case 1: if (w.can_start) session_start_now(); break;
    case 2: if (si.state == 5) restart_with(NULL); else if (ask_yes("Cancel", "Stop and play alone? The game restarts.", "Stop")) restart_with(NULL); break;
    }
}

static void lobby_hook(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        int mi = menu_command(&e);
        if (mi) { do_action(mi); continue; }
        if (e.type == SDL_QUIT) exit(0);
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            SDL_Scancode k = e.key.keysym.scancode;
            SessionInfo si; session_info(&si);
            if (k == SDL_SCANCODE_ESCAPE) { if (si.state == 5) restart_with(NULL); else if (ask_yes("Cancel", "Stop and play alone? The game restarts.", "Stop")) restart_with(NULL); }
            if ((k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) && si.host && si.n >= 2) session_start_now();
        }
    }
    menu_sync();
    lobby_draw();
    lobby_window();
    SDL_Delay(30);
}

/* waiting for a friend's DS: keep the window alive */
/* while this console waits for another (sessions): keep the window alive and say why it stands still */
static void idle_pump(void) {
    if (!g_win) return;
    SDL_PumpEvents();
    static uint64_t last;
    uint64_t now = SDL_GetPerformanceCounter();
    if (now - last < g_perf_freq / 2) return;
    last = now;
    double w = air_waiting_ms();
    if (w < 1500) return;
    char d[300], t[400]; int k;
    if (!air_wait_describe(d, sizeof d, &k)) return;
    char* paren = strstr(d, " ("); if (paren) *paren = 0;                /* the numbers are for the log */
    snprintf(t, sizeof t, "Dragon Quest IX - %s (%.0f s) - details in session\\front.log", d, w / 1000.0);
    SDL_SetWindowTitle(g_win, t);
}

static void do_action(int id) {
    int relayout = 0;
    switch (id) {
    case MI_PAUSE: g_paused = !g_paused; break;
    case MI_TURBO: g_turbo = !g_turbo; notice(g_turbo ? "Fast forward on (Tab)" : "Fast forward off"); break;
    case MI_DLC:
        g_opt.dlc = !g_opt.dlc;
        info_box(SDL_MESSAGEBOX_INFORMATION, "Downloadable content", g_opt.dlc ?
            "The 64 downloadable quests and the 23 special guests will be switched on in your save the next "
            "time the game starts (they come with the save, as if downloaded from Nintendo Wi-Fi Connection).\n\n"
            "Close the game and start it again, load your adventure log, and save once in the game to keep them.\n\n"
            "The quests are offered by their quest givers once you reach them in the story; the guests visit "
            "Quester's Rest." :
            "Off. A save that already has them keeps them; this only stops switching them on at start.", g_win);
        break;
    case MI_NET:
        g_net_want = !g_net_want;
        wireless_apply(0);
        if (g_net_want) info_box(SDL_MESSAGEBOX_INFORMATION, "Wireless",
            "Connected mode is on. In the game, talk to Sellma at the DQVC counter in Stornway's Quester's Rest "
            "and the DS will connect through this PC "
            "to the server chosen under Wireless > Server.\n\n"
            "If the connection fails, press F10 right after: the bug report records how far it got.", g_win);
        break;
    case MI_SRV_BUILTIN: case MI_SRV_ALTWFC: case MI_SRV_WIIMMFI:
        snprintf(g_wfc_server, sizeof g_wfc_server, "%s", WFC[id == MI_SRV_BUILTIN ? 0 : id == MI_SRV_ALTWFC ? 1 : 2].key);
        wireless_apply(0);
        break;
    case MI_SRV_CUSTOM: break;
    case MI_NET_HELP:
        info_box(SDL_MESSAGEBOX_INFORMATION, "Wireless",
            "Nintendo Wi-Fi Connection closed in 2014. Fans run replacement servers (AltWFC, Wiimmfi) that a "
            "real DS reaches by changing its DNS setting; this game does the same.\n\n"
            "The PC plays the part of a wireless access point called DQIX-PC and passes the DS's traffic to the "
            "internet. Nothing else on your PC is changed.\n\n"
            "Server > Built-in: nothing goes to the internet. The PC answers the game itself with DQIX's "
            "downloads as Nintendo served them (preserved by fans, in the wfc folder).\n\n"
            "Connect to the internet: the DS gets online when the game asks for it, e.g. when Sellma downloads "
            "DQVC items and new quests. Turn it off and the game behaves like a DS with no access point.\n\n"
            "Unlock all downloadable content now: switches on every downloadable quest and special guest in "
            "your save when the game starts, without visiting Sellma.", g_win);
        break;
    case MI_SES_HOST: case MI_SES_JOIN: case MI_SES_LEAVE: case MI_SES_NAME: case MI_SES_START: case MI_SES_HELP:
        session_action(id);
        break;
    case MI_SHOT: g_shot_req = 1; break;
    case MI_SEND_REPORT:                                          /* Help > Send a bug report: the same as F10 */
    case MI_REPORT: {
        extern char g_report_last_dir[300];
        report_write("F10 pressed");                               /* captured first: the moment you pressed it */
        if (g_report_last_dir[0]) {
            report_display(g_report_last_dir);
            if (g_gl && g_win) {                                   /* the window now: drawn again (not shown), read back */
                snprintf(g_report_shot_dir, sizeof g_report_shot_dir, "%s", g_report_last_dir);
                int w, h; SDL_GL_GetDrawableSize(g_win, &w, &h);
                GlPresent v; canvas_layout(&v, w, h); v.again = 1;
                gl_present(&v);
                report_window_shot(w, h);
            }
        }
        char* text = g_win ? ask_text(g_win, "Bug report " DQIX_VERSION,
            "The report is saved. Describe what went wrong (what you did, what you expected, what happened) - "
            "as much as you like. Save adds it to the report; Skip keeps the report without a description.") : NULL;
        if (text && g_report_last_dir[0]) {
            char path[400]; snprintf(path, sizeof path, "%s/description.txt", g_report_last_dir);
            FILE* f = fopen(path, "wb");
            if (f) { fputs(text, f); fputs("\n", f); fclose(f); }
        }
        if (g_win && g_report_last_dir[0]) report_send(g_report_last_dir, text);   /* send it? (asks first) */
        free(text);
        if (g_win) SDL_SetWindowTitle(g_win, "Dragon Quest IX - report saved (see the reports folder)");
        g_fps_t0 = SDL_GetPerformanceCounter();                    /* keep that title up for a second */
        g_next_frame = SDL_GetPerformanceCounter();                /* the game waited while you typed */
        break;
    }
    case MI_EXIT: quit_ask(); break;
    case MI_CLASSIC: case MI_ENHANCED:
        if (!g_gl) break;
        g_classic = id == MI_CLASSIC; apply_mode(); g_fps_t0 = 0;
        break;
    case MI_RES1: case MI_RES2: case MI_RES3: case MI_RES4: case MI_RES6: case MI_RES8:
        if (!g_gl || g_classic) break;
        g_res_auto = 0;
        g_scale3d = id == MI_RES6 ? 6 : id == MI_RES8 ? 8 : id - MI_RES1 + 1; gl_render_set_scale(g_scale3d);
        break;
    case MI_RES_AUTO: if (g_gl && !g_classic) g_res_auto = 1; break;          /* auto_scale() picks it */
    case MI_SMOOTH: if (g_gl) { g_smooth = !g_smooth; gl_set_smooth(g_smooth); } break;
    case MI_SCENE_BARS: scene_toggle_bars(); break;
    case MI_DISCORD: g_discord = !g_discord; discord_enable(g_discord && !g_headless); break;
    case MI_TEXPACK:
        g_tex_pack = !g_tex_pack; gl_set_texture_pack(g_tex_pack);
        if (g_tex_pack) { int n = gl_reload_texture_pack(); notice(n ? "Texture packs: %d pictures" : "Texture packs on (the textures folder has none yet)", n); }
        else notice("Texture packs off: the game's own textures");
        break;
    case MI_TEXUP: g_tex_up = !g_tex_up; gl_set_tex_upscale(g_tex_up); notice("Upscaled 3D textures: %s", g_tex_up ? "on (hold ` to compare)" : "off"); break;
    case MI_TEXDUMP: g_tex_dump = !g_tex_dump; gl_set_texture_dump(g_tex_dump); notice(g_tex_dump ? "Dumping textures to textures\\dump" : "Dumping off"); break;
    case MI_TEXRELOAD: { int n = gl_reload_texture_pack(); notice("Texture packs: %d pictures", n); break; }
    case MI_TEXFOLDER: {
#ifdef _WIN32
        _mkdir("textures");
#else
        mkdir("textures", 0777);
#endif
        open_folder("textures");
        break;
    }
    case MI_MUSPACK: {
        g_mus_pack = !g_mus_pack;
        int n = music_enable(g_mus_pack && !session_running());
        if (!g_mus_pack) notice("Music packs off: the game's own music (from the next song)");
        else notice(n ? "Music packs: %d songs replaced (from the next time each starts)" : "Music packs on (the music folder has no songs yet)", n);
        break;
    }
    case MI_MUSRELOAD: { int n = music_reload(); notice("Music packs: %d songs replaced (from the next time each starts)", n); break; }
    case MI_MUSNAMES: g_mus_names = !g_mus_names; notice(g_mus_names ? "Each song's name shows as it starts" : "Song names off"); break;
    case MI_MUSFOLDER: {
#ifdef _WIN32
        _mkdir("music");
#else
        mkdir("music", 0777);
#endif
        music_write_list("music/songs.txt");
        open_folder("music");
        break;
    }
    case MI_MUSEXPORT: music_export_start(); break;
    case MI_MODS_HELP:
        info_box(SDL_MESSAGEBOX_INFORMATION, "Mods",
            "Texture packs: PNG pictures in the textures folder (next to the game) are drawn instead of the game's own textures, "
            "in Enhanced mode. Each is named after the texture it replaces.\n\n"
            "To make one: turn on Dump textures and play. Every texture shown is saved in textures\\dump. Redraw or upscale those "
            "PNGs (keep their names and see-through parts) and put them anywhere in the textures folder outside dump, "
            "then Reload the texture packs.\n\n"
            "Upscale the 3D textures: the game's textures enlarged 4x with xBR, so slopes and curves stay smooth (a pack's pictures come first).\n\n"
            "Hold ` (the key left of 1) to see the game's own textures, to compare.\n\n"
            "Music packs: OGG or FLAC recordings in the music folder play instead of the game's songs. Each is named after the "
            "song it replaces (BG_012.ogg); Open the music folder lists them all in songs.txt, and Show each song's name tells you "
            "which one is playing. Songs loop between the LOOPSTART and LOOPLENGTH tags (in samples) if the file has them, "
            "else the whole file; jingles (ME_) play once. The game's fades and pauses carry over. "
            "Export the game's songs writes each as a WAV file (with its loop) to music\\export, to hear which is which; "
            "a WAV with a loop works in the music folder too. "
            "music_pack_volume in settings.ini sets how loud they are. While playing together (Wireless) the game's own music plays.", g_win);
        break;
    case MI_HD_TEXT:
        if (g_gl) {
            g_hd_text = !g_hd_text; gl_set_hd_text(g_hd_text);
            notice(g_classic ? "HD text %s (shows in Enhanced, F5)" : "HD text %s", g_hd_text ? "on" : "off");
        }
        break;
    case MI_SOFT_SHADOWS:
        g_gl_soft_alpha = !g_gl_soft_alpha;
        notice("Soft shadows: %s%s", g_gl_soft_alpha ? "on" : "off (the DS's steps)", g_scale3d < 2 && !g_res_auto ? " - shows at 2x and above" : "");
        break;
    case MI_SMOOTH_SHADE:
        g_gl_soft_shade = !g_gl_soft_shade;
        notice("Smooth shading: %s%s", g_gl_soft_shade ? "on" : "off (the DS's steps)", g_scale3d < 2 && !g_res_auto ? " - shows at 2x and above" : "");
        break;
    case MI_SAVE_EXPORT: save_export(); break;
    case MI_SAVE_IMPORT: save_import(); break;
    case MI_OUTLINE0: case MI_OUTLINE0 + 1: case MI_OUTLINE0 + 2:
        g_gl_outline = id == MI_OUTLINE0 ? 1 : id == MI_OUTLINE0 + 1 ? 2 : 0;
        notice("Outlines: %s", g_gl_outline == 1 ? "thin" : g_gl_outline == 2 ? "medium" : "like the DS");
        break;
    case MI_POPOUT:
        if (!g_gl) break;
        if (g_wide && !g_side_by_side) g_wide_pop = !g_wide_pop;
        else { g_wide_pop = 1; g_wide = 1; g_corner = g_side_by_side = g_two = 0; }   /* turns widescreen on too */
        relayout = 1;
        break;
    case MI_HUD: g_hud = !g_hud; break;
    case MI_FULL2: second_window_fullscreen(); break;
    case MI_LCD: if (g_gl) g_opt.lcd = !g_opt.lcd; g_fps_t0 = 0; break;
    case MI_LCDCOL: g_opt.lcd_color = !g_opt.lcd_color; break;
    case MI_GRID: g_opt.grid = !g_opt.grid; break;
    case MI_GHOST: g_opt.ghost = !g_opt.ghost; break;
    case MI_BL1: case MI_BL2: case MI_BL3: case MI_BL4: g_opt.backlight = id - MI_BL1 + 1; break;
    case MI_SHELL: if (g_gl && !g_side_by_side) { g_opt.shell = !g_opt.shell; relayout = 1; } break;
    case MI_GAP: if (g_gl) { g_opt.gap = !g_opt.gap; relayout = 1; } break;
    case MI_STACKED: case MI_SIDE: g_side_by_side = id == MI_SIDE; g_corner = g_wide = g_two = g_single = 0; relayout = 1; break;
    case MI_CORNER: if (g_gl) { g_corner = !(g_corner && !g_wide && !g_side_by_side && !g_single); g_wide = g_side_by_side = g_two = g_single = 0; relayout = 1; } break;
    case MI_WIDE: case MI_WIDE_DYN:                               /* basic or dynamic widescreen (again: off) */
        if (g_gl) {
            int dyn = id == MI_WIDE_DYN, on = g_wide && !g_side_by_side && g_wide_dyn == dyn;
            g_wide = !on; g_wide_dyn = dyn; g_corner = g_side_by_side = g_two = g_single = 0; relayout = 1;
        }
        break;
    case MI_TWO: if (g_gl) { g_two = !(g_two && !g_side_by_side && !g_single); g_corner = g_wide = g_side_by_side = g_single = 0; relayout = 1; } break;
    case MI_SINGLE:                                               /* U / left trigger: one screen; in widescreen the small one hides */
        if (g_wide && g_gl && !g_side_by_side) { g_small_hide = !g_small_hide; notice(g_small_hide ? "Small screen hidden" : "Small screen shown"); }
        else { g_single = !g_single; relayout = 1; notice(g_single ? "Single screen (%s)" : "Both screens", g_swap ? "top" : "bottom"); }
        break;
    case MI_SWAP: if (!(g_wide && g_gl && !g_side_by_side)) { g_swap = !g_swap; relayout = 1; } break;
    case MI_OPACITY1: case MI_OPACITY1 + 1: case MI_OPACITY1 + 2: case MI_OPACITY1 + 3:
        g_small_opacity = 100 - 25 * (id - MI_OPACITY1); notice("Small screen: %d%%", g_small_opacity); break;
    case MI_CORNER0: case MI_CORNER0 + 1: case MI_CORNER0 + 2: case MI_CORNER0 + 3: {
        static const char* const N[4] = { "top left", "top right", "bottom right", "bottom left" };
        g_small_corner = id - MI_CORNER0; notice("Small screen: %s", N[g_small_corner]); break;
    }
    case MI_SSIZE0: case MI_SSIZE0 + 1: case MI_SSIZE0 + 2: {
        static const char* const N[3] = { "small", "medium", "large" };
        g_small_size = id - MI_SSIZE0; notice("Small screen: %s", N[g_small_size]); break;
    }
    case MI_RESET:
        if (session_requested()) { info_box(SDL_MESSAGEBOX_INFORMATION, "Reset", "Not while playing with friends: it would leave them out of step. Leave the session first (Wireless menu).", g_win); break; }
        if (ask_yes("Reset", "Start the game again from the beginning, like switching the DS off and on?\n\nAnything since you last saved in the game is lost.", "Reset")) restart_with(NULL);
        break;
    case MI_VOLUME:
        if (g_win) volume_window_open(g_win, &g_volume, &g_mute);
#ifndef _WIN32
        notice("Volume %d%% (- and = change it)", g_volume);
#endif
        break;
    case MI_MUTE: g_mute = !g_mute; volume_window_refresh(); notice(g_mute ? "Sound off" : "Volume %d%%", g_volume); break;
    case MI_CONTROLS_EDIT:
#ifdef _WIN32
        if (g_win) controls_window_open(g_win);
#else
        info_box(SDL_MESSAGEBOX_INFORMATION, "Controls", "Change the keys and controller buttons in controls.ini, next to the game (the game writes it with every action listed), then start the game again.", g_win);
#endif
        break;
    case MI_ANALOG: g_analog_walk = !g_analog_walk; break;
    case MI_LANG1: case MI_LANG1 + 1: case MI_LANG1 + 2: case MI_LANG1 + 3: case MI_LANG1 + 4: {
        int lang = id - MI_LANG1 + 1;
        if (lang == g_fw_language) break;
        g_fw_language = lang;
        char text[300];
        snprintf(text, sizeof text, "The game will be in %s the next time it starts: close it and start it again.\n\n"
                 "Your adventure log is the same in every language.", LANGUAGE_NAME[lang]);
        info_box(SDL_MESSAGEBOX_INFORMATION, "Language", text, g_win);
        break;
    }
    case MI_MODERN_HELP:
        info_box(SDL_MESSAGEBOX_INFORMATION, "Modern",
            "Optional ways to play that the DS didn't have. Walking with the stick is on at first; the others are off. With them all off the game is exactly the original.\n\n"
            "Walk in any direction with the left stick (on at first): on the DS the D-pad walks in 8 directions and only the stylus walks in any. With this on, "
            "a controller's left stick steers like the stylus while you walk around (the game's own stylus walking). In menus, dialogue and "
            "battles the stick stays a D-pad. Friends in a session don't need it on.\n\n"
            "Basic Widescreen: the action (the field, or a battle) is drawn wider than the DS's screen, so you see more of the world left and right, "
            "and the other screen (the map, or the battle commands) sits small in a corner over it. K (or the right stick click) moves it to "
            "the next corner, I changes its size, O (or the left stick click) how see-through it is; it turns solid while the mouse is on it. "
            "U (or the left trigger) hides it; holding F9 (or the right trigger) shows it big until you let go. In a window it is 16:9; in "
            "fullscreen it fills the monitor. Text, menus and the touch screen's buttons stay in the middle, as on the DS. People and things "
            "at the sides stay in view (except while playing with friends far away). Enhanced mode only (the 3D on the graphics card).\n\n"
            "Dynamic Widescreen (Experimental): the field never moves; the map stays top left, and the game's windows over the field move "
            "right, clear of it (dialogue stays in the middle); screens of their own (Stella's lists, Equipment) show 4:3 at the right; "
            "in battles the party's panels sit stacked in the bottom left.\n\n"
            "Top screen in the corner: the field as on the DS (4:3), the top screen beside it.\n\n"
            "Two windows: one screen in each, to put them on two monitors (each can be fullscreen).\n\n"
            "Single screen (U or the left trigger): only one screen, as big as it gets; F9 or the right trigger shows the other one.\n\n"
            "All are also under View; F2 goes through the layouts.", g_win);
        break;
    case MI_SIZE1: case MI_SIZE2: case MI_SIZE3: case MI_SIZE4: g_scale = id - MI_SIZE1 + 1; relayout = 1; break;
    case MI_INTEGER: g_opt.integer = !g_opt.integer; break;
    case MI_ACTUAL:
        if (!g_gl) break;
        g_opt.actual = !g_opt.actual; relayout = 1;
        update_actual();
        if (g_opt.actual && g_actual_sc <= 0)
            info_box(SDL_MESSAGEBOX_INFORMATION, "Actual size",
                "Your monitor didn't say how big it is, so the real size can't be worked out.\n\n"
                "Measure the width of its picture area in millimetres and put it in settings.ini\n"
                "(next to the game) as monitor_width_mm, e.g. monitor_width_mm = 597", g_win);
        break;
    case MI_FULL:
        g_fullscreen = !g_fullscreen;
        g_menu_peek = 0;
        menu_strip(0);
        if (g_fullscreen) { menu_show(0); SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP); }
        else {
            /* back to a window: its frame (title bar, borders) first, then the
             * menu bar, then its size and a place on the screen (GitHub #11:
             * it came back frameless, stuck in the corner) */
            SDL_SetWindowFullscreen(g_win, 0);
            SDL_SetWindowBordered(g_win, SDL_TRUE);
            SDL_SetWindowResizable(g_win, SDL_TRUE);
            window_fix_frame(g_win);
            menu_show(1);
            {   int x, y; SDL_GetWindowPosition(g_win, &x, &y);
                int d = SDL_GetWindowDisplayIndex(g_win); SDL_Rect r;
                if (d >= 0 && SDL_GetDisplayUsableBounds(d, &r) == 0 && x <= r.x + 1 && y <= r.y + 1)
                    SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED_DISPLAY(d), SDL_WINDOWPOS_CENTERED_DISPLAY(d));
            }
        }
        relayout = !g_fullscreen;
        break;
    case MI_CONTROLS: info_box(SDL_MESSAGEBOX_INFORMATION, "Controls", CONTROLS_TEXT, g_win); break;
    case MI_UPDATE: update_menu(); break;
    case MI_UPDATE_AUTO: g_update_auto = !g_update_auto; settings_save(); break;
    case MI_ABOUT:
        info_box(SDL_MESSAGEBOX_INFORMATION, "About",
            "Dragon Quest IX for PC, " DQIX_VERSION " (" DQIX_VERSION_DATE "), built " __DATE__ "\n\n"
            "Classic draws the game exactly like the DS; Enhanced draws its 3D\n"
            "on the graphics card at up to 4x.\n\n"
            "DS Lite screen colours: Pokefan531's measured DS Lite profile.", g_win);
        break;
    default:
        if (id >= MI_SHELLC0 && id < MI_SHELLC0 + N_SHELLS) { g_opt.shell_color = id - MI_SHELLC0; g_color_chosen = 1; relayout = 1; }
        else return;
    }
    if (relayout && g_win) {                                   /* a new window size only when one was asked for */
        int sized = (id >= MI_SIZE1 && id <= MI_SIZE4) || id == MI_ACTUAL;
        layout_window_keep(!sized);
        second_window_sync();
    }
    menu_sync();
    if (id != MI_SHOT && id != MI_REPORT && id != MI_SEND_REPORT && id != MI_PAUSE && id != MI_TURBO) settings_save();
}

/* ---------------------------------------------------------------- hotkeys (Game > Controls) */
static int wide_now(void) { return g_wide && g_gl && !g_side_by_side; }
static void hotkey(int a) {
    switch (a) {
    case CA_FAST: if (!g_ff_hold) do_action(MI_TURBO); break;    /* Tab: on, then off (hold: see g_fast) */
    case CA_PAUSE: do_action(MI_PAUSE); break;
    case CA_FULL: do_action(MI_FULL); break;
    case CA_LAYOUT: {                                            /* stacked -> side by side -> corner -> widescreen -> dynamic (-> two windows, with two monitors) */
        int two_ok = SDL_GetNumVideoDisplays() > 1, gl = g_gl;
        int id = g_single ? MI_STACKED : g_side_by_side ? (gl ? MI_CORNER : MI_STACKED) : (g_corner && gl) ? MI_WIDE
               : (g_wide && gl && !g_wide_dyn) ? MI_WIDE_DYN : (g_wide && gl) ? (two_ok ? MI_TWO : MI_STACKED) : (g_two && gl) ? MI_STACKED : MI_SIDE;
        do_action(id);
        break;
    }
    case CA_SINGLE: do_action(MI_SINGLE); break;
    case CA_SWAP: if (wide_now()) g_peek = 1; else do_action(MI_SWAP); break;   /* widescreen: held (released in frontend_frame) */
    case CA_CORNER: if (!g_wide_dyn) { do_action(MI_CORNER0 + (g_small_corner + 1) % 4); settings_save(); } break;
    case CA_SMALLSIZE: if (!g_wide_dyn) { do_action(MI_SSIZE0 + (g_small_size + 1) % 3); settings_save(); } break;
    case CA_OPACITY: do_action(MI_OPACITY1 + ((100 - g_small_opacity) / 25 + 1) % 4); break;
    case CA_WINSIZE: do_action(MI_SIZE1 + g_scale % 4); break;
    case CA_RES3D:
        if (g_gl && !g_classic) {                                /* 1x 2x 3x 4x 6x 8x Auto */
            static const int ids[7] = { MI_RES1, MI_RES2, MI_RES3, MI_RES4, MI_RES6, MI_RES8, MI_RES_AUTO }, sc[7] = { 1, 2, 3, 4, 6, 8, 0 };
            int cur = 6; for (int i = 0; i < 6; i++) if (!g_res_auto && g_scale3d == sc[i]) cur = i;
            do_action(ids[(cur + 1) % 7]);
        }
        break;
    case CA_MODE: if (g_gl) do_action(g_classic ? MI_ENHANCED : MI_CLASSIC); break;
    case CA_LCD: do_action(MI_LCD); break;
    case CA_SHELL: do_action(MI_SHELL); break;
    case CA_ACTUAL: do_action(MI_ACTUAL); break;
    case CA_VOLDOWN: case CA_VOLUP:
        g_volume += a == CA_VOLUP ? 10 : -10;
        g_volume = g_volume < 0 ? 0 : g_volume > 100 ? 100 : g_volume;
        g_mute = 0;
        volume_window_refresh(); notice("Volume %d%%", g_volume); settings_save(); menu_sync();
        break;
    case CA_RESET: do_action(MI_RESET); break;
    case CA_SHOT: do_action(MI_SHOT); break;
    case CA_REPORT: do_action(MI_REPORT); break;
    case CA_HDTEXT: do_action(MI_HD_TEXT); break;
    }
}

/* Game > Controls: waiting for the key or button the player presses for an action */
static int g_cap_action = -1, g_cap_what;                    /* what: 0 key, 1 second key, 2 controller button */
static int g_pad_resync;                                     /* a button just given out: don't fire it while still held */
static void capture_done(const char* what) {
    if (what) notice("%s: %s", g_ctl[g_cap_action].name, what); else notice("Not changed");
    g_cap_action = -1;
    controls_save();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    controls_window_capture(0, NULL);
    controls_window_refresh();
}
static void capture_key(SDL_Scancode k, Uint16 mod) {
    if (k == SDL_SCANCODE_ESCAPE) { capture_done(NULL); return; }
    if (g_cap_what == 2) return;                                 /* a controller button is wanted */
    int is_mod = k == SDL_SCANCODE_LCTRL || k == SDL_SCANCODE_RCTRL || k == SDL_SCANCODE_LALT || k == SDL_SCANCODE_RALT ||
                 k == SDL_SCANCODE_LSHIFT || k == SDL_SCANCODE_RSHIFT;
    int key = (int)k;
    if (!is_mod && g_cap_action >= CA_N_DS) key |= (mod & KMOD_CTRL ? CTL_MOD_CTRL : 0) | (mod & KMOD_ALT ? CTL_MOD_ALT : 0) | (mod & KMOD_SHIFT ? CTL_MOD_SHIFT : 0);
    controls_set_key(g_cap_action, g_cap_what, key);
    char n[64]; controls_key_name(key, n, sizeof n);
    capture_done(n);
}
static void capture_pad(int pad) {
    if (g_cap_what != 2) return;
    controls_set_pad(g_cap_action, pad);
    char n[64]; controls_pad_name(pad, n, sizeof n);
    capture_done(n);
}
static void capture_wait(void) {                                /* the game waits meanwhile */
    {   char pr[160];
        snprintf(pr, sizeof pr, g_cap_what == 2 ? "Press a controller button for: %s   (Esc cancels)" : "Press a key for: %s   (Esc cancels)", g_ctl[g_cap_action].name);
        /* the controller's buttons come through even if the dialog keeps the focus */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        controls_window_capture(1, pr);
    }
    SDL_RaiseWindow(g_win);
    while (g_cap_action >= 0) {
        snprintf(g_notice, sizeof g_notice, g_cap_what == 2 ? "Press a controller button for: %s (Esc cancels)" : "Press a key for: %s (Esc cancels)", g_ctl[g_cap_action].name);
        g_notice_until = SDL_GetPerformanceCounter() + g_perf_freq;
        update_title(g_last_fps);
        handle_events(); present(); SDL_Delay(15);
    }
    g_next_frame = SDL_GetPerformanceCounter();
    g_pad_resync = 1;
}

#ifndef _WIN32
static void linux_overlay_move_selection(int delta) {
    g_linux_overlay_selection = (g_linux_overlay_selection + delta + 4) % 4;
}

static void linux_overlay_close(int resume_game) {
    g_linux_overlay_open = 0;
    g_paused = resume_game ? 0 : g_linux_overlay_saved_paused;
    g_next_frame = SDL_GetPerformanceCounter();
    g_fps_t0 = 0;
    g_pad_resync = 1;
}

static void linux_overlay_activate(void) {
    switch (g_linux_overlay_selection) {
    case 0:
        linux_overlay_close(1);
        break;
    case 1:
        if (g_gl) do_action(g_classic ? MI_ENHANCED : MI_CLASSIC);
        else notice("Enhanced mode requires OpenGL");
        break;
    case 2:
        do_action(MI_FULL);
        break;
    case 3:
        quit_ask();
        break;
    }
}

static int linux_overlay_handle_event(const SDL_Event *event) {
    static int axis_y_latched;
    if (event->type == SDL_KEYDOWN && !event->key.repeat &&
        event->key.keysym.scancode == SDL_SCANCODE_ESCAPE && g_cap_action < 0 &&
        g_win && !g_headless) {
        if (g_linux_overlay_open) linux_overlay_close(0);
        else {
            g_linux_overlay_open = 1;
            g_linux_overlay_selection = 0;
            g_linux_overlay_saved_paused = g_paused;
            g_paused = 1;
            g_next_frame = SDL_GetPerformanceCounter();
            g_pad_resync = 1;
        }
        return 1;
    }
    if (!g_linux_overlay_open) return 0;

    switch (event->type) {
    case SDL_KEYUP:
        return 1;
    case SDL_KEYDOWN:
        if (event->key.repeat) return 1;
        switch (event->key.keysym.sym) {
        case SDLK_UP: linux_overlay_move_selection(-1); break;
        case SDLK_DOWN: linux_overlay_move_selection(1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            linux_overlay_activate(); break;
        default: break;
        }
        return 1;
    case SDL_CONTROLLERBUTTONUP:
        return 1;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (event->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: linux_overlay_move_selection(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: linux_overlay_move_selection(1); break;
        case SDL_CONTROLLER_BUTTON_A: linux_overlay_activate(); break;
        case SDL_CONTROLLER_BUTTON_B: linux_overlay_close(0); break;
        default: break;
        }
        return 1;
    case SDL_CONTROLLERAXISMOTION:
        if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            if (!axis_y_latched && event->caxis.value < -16000) {
                linux_overlay_move_selection(-1); axis_y_latched = 1;
            } else if (!axis_y_latched && event->caxis.value > 16000) {
                linux_overlay_move_selection(1); axis_y_latched = 1;
            } else if (event->caxis.value > -7000 && event->caxis.value < 7000) axis_y_latched = 0;
        }
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (event->type == SDL_MOUSEBUTTONUP && event->button.button == SDL_BUTTON_LEFT) {
            int ww = 0, wh = 0, ox = event->button.x, oy = event->button.y;
            int fit_x = 0, fit_y = 0, fit_w = 0, fit_h = 0;
            SDL_GetWindowSize(g_win, &ww, &wh);
            if (dqix_linux_overlay_fit_rect(ww, wh, &fit_x, &fit_y, &fit_w, &fit_h) &&
                ox >= fit_x && ox < fit_x + fit_w && oy >= fit_y && oy < fit_y + fit_h) {
                ox = (ox - fit_x) * DQIX_LINUX_OVERLAY_WIDTH / fit_w;
                oy = (oy - fit_y) * DQIX_LINUX_OVERLAY_HEIGHT / fit_h;
                if (ox >= DQIX_LINUX_OVERLAY_ROW_X &&
                    ox < DQIX_LINUX_OVERLAY_ROW_X + DQIX_LINUX_OVERLAY_ROW_WIDTH) {
                    for (int row = 0; row < 4; ++row) {
                        int y = DQIX_LINUX_OVERLAY_ROW0 + row * DQIX_LINUX_OVERLAY_ROW_STEP;
                        if (oy >= y - 12 && oy < y + DQIX_LINUX_OVERLAY_ROW_HEIGHT - 12) {
                            g_linux_overlay_selection = row;
                            linux_overlay_activate();
                            break;
                        }
                    }
                }
            }
        }
        return 1;
    default:
        return 0;
    }
}
#endif

static void handle_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
#ifndef _WIN32
        if (linux_overlay_handle_event(&e)) continue;
#endif
        int mi = menu_command(&e);
        if (mi) { do_action(mi); continue; }
        switch (e.type) {
        case SDL_QUIT: quit_ask(); break;
        case SDL_KEYDOWN: {
            if (e.key.repeat || !g_win) break;
            SDL_Scancode k = e.key.keysym.scancode;
            if (g_cap_action >= 0) { capture_key(k, e.key.keysym.mod); break; }   /* Game > Controls: the new key */
            int in2 = g_win2 && e.key.windowID == SDL_GetWindowID(g_win2);   /* typed in the second window */
            if (k == SDL_SCANCODE_ESCAPE) { if (in2 && g_win2_full) second_window_fullscreen(); else if (g_fullscreen) do_action(MI_FULL); else quit_ask(); break; }
            if (k == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)) { if (in2) { second_window_fullscreen(); settings_save(); } else do_action(MI_FULL); break; }
            int a = controls_key_action(k, e.key.keysym.mod);
            if (a == CA_FULL && in2) { second_window_fullscreen(); settings_save(); break; }
            if (a >= 0) hotkey(a);
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
            if (g_cap_action >= 0) capture_pad(e.cbutton.button);
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (g_cap_action >= 0 && e.caxis.value > 16000 && (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT))
                capture_pad(e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? CTL_PAD_LT : CTL_PAD_RT);
            break;
        case SDL_MOUSEBUTTONDOWN:
            /* fullscreen: a double click outside the touch screen (the top screen, the
             * console, the edges) goes back to the window; on the touch screen
             * clicks are the stylus */
            if (e.button.button == SDL_BUTTON_LEFT && e.button.clicks == 2) {
                int tx, ty, in2 = g_win2 && e.button.windowID == SDL_GetWindowID(g_win2);
                if (in2 && g_win2_full && !to_touch_in(g_win2, e.button.x, e.button.y, &tx, &ty, 0)) { second_window_fullscreen(); settings_save(); }
                else if (!in2 && g_fullscreen && !to_touch(e.button.x, e.button.y, &tx, &ty, 0)) do_action(MI_FULL);
            }
            break;
        case SDL_MOUSEMOTION:
            /* fullscreen: the menu bar appears while the mouse is at the top edge */
            if (g_win2 && g_win2_full && e.motion.windowID == SDL_GetWindowID(g_win2)) {   /* the second window's bar, the same way */
                static int peek2;
                if (!peek2 && e.motion.y <= 1) { peek2 = 1; menu_show_second(1); }
                else if (peek2 && e.motion.y > 80) { peek2 = 0; menu_show_second(0); }
            }
            if (g_fullscreen && e.motion.windowID == SDL_GetWindowID(g_win)) {
                g_menu_peek = menu_strip_shown();                 /* (it also hides itself after a menu closes) */
                if (!g_menu_peek && e.motion.y <= 1) { g_menu_peek = 1; menu_strip(1); }   /* over the picture, which keeps its size */
                else if (g_menu_peek && e.motion.y > 80) { menu_strip(0); g_menu_peek = menu_strip_shown(); }
            }
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) {           /* two windows: closing the second one goes back to one */
                if (g_win2 && e.window.windowID == SDL_GetWindowID(g_win2)) do_action(MI_STACKED);
                else quit_ask();
                break;
            }
            if (g_win2 && e.window.windowID == SDL_GetWindowID(g_win2)) {
                if ((e.window.event == SDL_WINDOWEVENT_MOVED || e.window.event == SDL_WINDOWEVENT_RESIZED) && !g_win2_full) second_window_keep_rect();
                break;
            }
            if (e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED && g_opt.actual) { update_actual(); if (!g_fullscreen) layout_window(); }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && SDL_GameControllerGetJoystick(g_pad) == SDL_JoystickFromInstanceID(e.cdevice.which)) { SDL_GameControllerClose(g_pad); g_pad = NULL; }
            break;
        }
    }
}

void frontend_frame(void) {
    if (g_hidden) { sound_discard(); return; }
    uint64_t t_start = SDL_GetPerformanceCounter();
    if (!g_perf_end) g_perf_end = t_start;
    PerfRec* pr = &g_perf[g_perf_pos % PERF_N];
    pr->frame = g_frame; pr->busy = (float)ticks_ms(t_start - g_perf_end);
    pr->gl = (float)g_gl_stats.render_ms; pr->upload = (float)g_gl_stats.upload_ms; pr->uploads = g_gl_stats.uploads; pr->polys = g_gl_stats.polys;
    memset(&g_gl_stats, 0, sizeof g_gl_stats);
    { extern double g_t2d_ms; static double last; pr->d2 = (float)(g_t2d_ms - last); last = g_t2d_ms; }
    pr->audio_ms = (int)(sound_buffered() * 1000 / 32768);
    handle_events();
    if (!g_headless) {                                          /* music packs: not while playing together (each DS's memory must match) */
        static int was = -1; int want = g_mus_pack && !session_running();
        if (want != was) { if (was >= 0 || !want) music_enable(want); was = want; }
        char nm[64]; if (music_now(nm, sizeof nm) && g_mus_names && g_win) notice("Music: %s", nm);   /* Mods > Show each song's name */
        music_export_poll();
    }
    if (g_win) {
        const Uint8* ks = SDL_GetKeyboardState(NULL);
        g_fe_keys = key_bits(ks) | pad_bits();
        static int always_fast = -1; if (always_fast < 0) always_fast = getenv("DQIX_FAST") != NULL;   /* tests */
        g_fast = (g_ff_hold && (controls_key_held(CA_FAST, ks) || controls_pad_held(CA_FAST, g_pad))) || always_fast || g_turbo;
        {   /* the controller's own shortcuts (stick clicks, triggers...): when pressed */
            static uint8_t was[CA_N];
            for (int a = CA_N_DS; a < CA_N; a++) {
                int now = (a != CA_FAST || !g_ff_hold) && controls_pad_held(a, g_pad);
                if (now && !was[a] && !g_pad_resync) hotkey(a);
                was[a] = (uint8_t)now;
            }
            g_pad_resync = 0;
            /* widescreen: the other screen shows while the swap button is held */
            if (g_peek && !controls_key_held(CA_SWAP, ks) && !controls_pad_held(CA_SWAP, g_pad)) g_peek = 0;
            g_tex_compare = controls_key_held(CA_COMPARE, ks) || controls_pad_held(CA_COMPARE, g_pad);
            if (!wide_now()) g_peek = 0;
            { static int tp = -1; if (tp < 0) tp = getenv("DQIX_TEST_PEEK") != NULL; if (tp && wide_now()) g_peek = 1; }   /* tests */
            /* reset: Start + Select + L + R held for a second on the controller (wherever they are mapped) */
            static int held_for;
            int combo = g_pad && controls_pad_held(CA_START, g_pad) && controls_pad_held(CA_SELECT, g_pad) &&
                        controls_pad_held(CA_L, g_pad) && controls_pad_held(CA_R, g_pad) && !session_requested();
            if (combo) {
                if (++held_for == 1) notice("Resetting... (keep holding Start + Select + L + R)");
                if (held_for >= 60) { held_for = 0; restart_with(NULL); }
            } else if (held_for) { if (held_for < 60) notice("Reset cancelled"); held_for = 0; }
            int ca, cw;                                           /* Game > Controls asked for a new key */
            if (controls_window_request(&ca, &cw) && ca >= 0 && ca < CA_N) {
                if (cw == 2 && !g_pad) notice("No controller found: connect one, then try again");
                else { g_cap_action = ca; g_cap_what = cw; capture_wait(); }
            }
        }
        int mx, my;
        Uint32 mb = SDL_GetMouseState(&mx, &my);
        int tx, ty;
        static int dragging;                                  /* the press began on the touch screen */
        int held = (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        g_fe_touch = held && to_touch_in(SDL_GetMouseFocus(), mx, my, &tx, &ty, dragging);
        dragging = g_fe_touch;
        if (g_fe_touch) { g_fe_tx = tx; g_fe_ty = ty; }
        uint64_t tp = SDL_GetPerformanceCounter();
        present();
        pr->present = (float)ticks_ms(SDL_GetPerformanceCounter() - tp);
        if (g_paused) {                                       /* Game > Pause: keep the window alive, the game waits */
            update_title(0);
            while (g_paused) { handle_events(); present(); SDL_Delay(15); }
            g_next_frame = SDL_GetPerformanceCounter();
            g_fps_t0 = 0;
        }
    } else g_fe_keys = pad_bits();
    uint64_t t_wait = SDL_GetPerformanceCounter();

    /* pacing */
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t period = (uint64_t)(g_perf_freq / DS_FPS);
    if (g_fast) {
        sound_discard();
        g_next_frame = now;
    } else {
        /* An even beat of one frame per 1/59.83 s from the PC's precise clock.
         * The sound card's clock drifts slightly from it, so the amount of sound
         * queued (averaged, since the card takes it in ~31 ms chunks) nudges the
         * beat by at most 0.5%: too much queued -> slightly slower, too little ->
         * slightly faster. Pacing by the queue directly released frames in pairs. */
        double adjust = 1.0;
        if (g_audio) {
            static double avg = -1;
            double q = (double)sound_buffered();
            avg = avg < 0 ? q : avg + (q - avg) * 0.02;
            double err = (avg - (double)AUDIO_TARGET) / (double)AUDIO_TARGET;
            adjust = 1.0 + fmax(-0.005, fmin(0.005, err * 0.02));
        }
        g_next_frame += (uint64_t)((double)period * adjust);
        if (now > g_next_frame + period * 6) g_next_frame = now;          /* fell far behind: don't try to catch up */
        else {
            while ((now = SDL_GetPerformanceCounter()) < g_next_frame) {
                uint64_t left_us = (g_next_frame - now) * 1000000 / g_perf_freq;
                if (left_us > 2500) SDL_Delay((Uint32)(left_us / 1000 - 2));   /* sleep, then wait out the last ~2 ms precisely */
            }
        }
    }
    if (!g_audio) sound_discard();
    g_perf_end = SDL_GetPerformanceCounter();
    pr->wait = (float)ticks_ms(g_perf_end - t_wait); pr->fast = (uint8_t)g_fast;
    g_perf_pos++;
    g_fps_frames++;
    if (g_win && now - g_fps_t0 > g_perf_freq) {
        update_title(g_fps_frames * (double)g_perf_freq / (double)(now - g_fps_t0));
        g_fps_frames = 0; g_fps_t0 = now;
    }
}

void frontend_shutdown(void) {
#ifndef _WIN32
    if (g_gl && g_win && g_glctx) (void)SDL_GL_MakeCurrent(g_win, g_glctx);
    dqix_linux_overlay_gl_shutdown();
    if (g_linux_overlay_texture) SDL_DestroyTexture(g_linux_overlay_texture);
    g_linux_overlay_texture = NULL;
    free(g_linux_overlay_rgba);
    g_linux_overlay_rgba = NULL;
#endif
}
