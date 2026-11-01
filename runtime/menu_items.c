/* Every option of the menus, once: the Windows menu bar (menu_win32.c) and the
 * in-game menu (menu_ingame.c) are both built from this list, so a new option
 * shows up in both. Which items are ticked or greyed out is kept here too
 * (the frontend calls menu_check / menu_enable), on every system. */
#include "menu.h"
#include <string.h>

#define SUB(t)          { MK_SUB, 0, t, NULL, 0 }
#define END             { MK_END, 0, NULL, NULL, 0 }
#define SEP             { MK_SEP, 0, NULL, NULL, 0 }
#define IT(id, t, k)    { MK_ITEM, id, t, k, 0 }
#define WIN(id, t, k)   { MK_ITEM, id, t, k, MF_WINDOWS }   /* opens a Windows window (not yet in the in-game menu elsewhere) */

const MenuItem g_menu_items[] = {
    SUB("&Game"),
        IT(MI_PAUSE, "&Pause", "P"),
        IT(MI_TURBO, "&Fast forward", "Tab"),
        IT(MI_RESET, "&Reset...", "Ctrl+R"),
        SEP,
        WIN(MI_VOLUME, "&Volume...", "- / ="),
        IT(MI_MUTE, "&Mute", NULL),
        WIN(MI_CONTROLS_EDIT, "&Controls...", NULL),
        SUB("&Language (European version)"),
            IT(MI_LANG1, "&English", NULL),
            IT(MI_LANG1 + 1, "&Fran\xc3\xa7" "ais", NULL),
            IT(MI_LANG1 + 2, "&Deutsch", NULL),
            IT(MI_LANG1 + 3, "&Italiano", NULL),
            IT(MI_LANG1 + 4, "E&spa\xc3\xb1ol", NULL),
        END,
        SEP,
        SUB("Save &file"),
            WIN(MI_SAVE_EXPORT, "&Export your save (a copy)...", NULL),
            WIN(MI_SAVE_IMPORT, "&Import a save (from another copy or an emulator)...", NULL),
        END,
        SEP,
        IT(MI_SHOT, "Save &screenshot", "F12"),
        IT(MI_REPORT, "Save &bug report", "F10"),
        SEP,
        IT(MI_EXIT, "E&xit", NULL),
    END,
    SUB("G&raphics"),
        IT(MI_CLASSIC, "&Classic: exactly like the DS", "F5"),
        IT(MI_ENHANCED, "&Enhanced: sharper 3D", "F5"),
        SEP,
        SUB("3D &resolution (Enhanced)"),
            IT(MI_RES1, "1x (DS)", NULL),
            IT(MI_RES2, "2x", NULL),
            IT(MI_RES3, "3x", NULL),
            IT(MI_RES4, "4x", NULL),
            IT(MI_RES6, "6x", NULL),
            IT(MI_RES8, "8x", NULL),
            SEP,
            IT(MI_RES_AUTO, "&Auto: as sharp as your screen (up to 12x)", NULL),
        END,
        IT(MI_SMOOTH, "&Smooth textures (Enhanced)", NULL),
        SUB("Character &outlines (Enhanced)"),
            IT(MI_OUTLINE0, "&Thin (clean, like emulators at a high resolution)", NULL),
            IT(MI_OUTLINE0 + 1, "&Medium", NULL),
            IT(MI_OUTLINE0 + 2, "Like the &DS (thicker, follows the polygons)", NULL),
        END,
        IT(MI_HD_TEXT, "&HD text: the letters at full resolution (Enhanced, new)", "F1"),
        IT(MI_SOFT_SHADOWS, "Soft s&hadows and glows (Enhanced)", NULL),
    END,
    SUB("&Screen"),
        IT(MI_LCD, "&DS Lite screen", "F6"),
        SEP,
        IT(MI_LCDCOL, "LCD &colours", NULL),
        IT(MI_GRID, "&Pixel grid (at 3x and larger)", NULL),
        IT(MI_GHOST, "&Motion blur", NULL),
        SUB("&Backlight"),
            IT(MI_BL1, "&Low", NULL),
            IT(MI_BL2, "&Medium", NULL),
            IT(MI_BL3, "&High", NULL),
            IT(MI_BL4, "Ma&x", NULL),
        END,
    END,
    SUB("&View"),
        IT(MI_SHELL, "Show the DS &Lite", "F7"),
        SUB("Console &colour"),
            { MK_SHELLS, MI_SHELLC0, NULL, NULL, 0 },
        END,
        IT(MI_GAP, "&Gap between the screens", NULL),
        IT(MI_ACTUAL, "&Actual size (as big as a real DS Lite)", "F8"),
        SEP,
        IT(MI_STACKED, "S&tacked", "F2"),
        IT(MI_SIDE, "&Side by side", "F2"),
        IT(MI_CORNER, "Top screen in the &corner", "F2"),
        IT(MI_WIDE, "&Basic Widescreen (the action wide, the other screen in a corner)", "F2"),
        IT(MI_WIDE_DYN, "D&ynamic Widescreen (Experimental): the map stays in the corner, the game's windows move right", "F2"),
        IT(MI_TWO, "Two wi&ndows (one screen in each, for two monitors)", "F2"),
        SEP,
        IT(MI_SINGLE, "Single scr&een (widescreen: hide the small screen)", "U"),
        IT(MI_SWAP, "Swa&p the screens (widescreen: hold to see the other)", "F9"),
        SUB("Widescreen: the small scr&een"),
            IT(MI_CORNER0, "Top left", "K"),
            IT(MI_CORNER0 + 1, "Top right", "K"),
            IT(MI_CORNER0 + 2, "Bottom right", "K"),
            IT(MI_CORNER0 + 3, "Bottom left", "K"),
            SEP,
            IT(MI_SSIZE0, "Small", "I"),
            IT(MI_SSIZE0 + 1, "Medium", "I"),
            IT(MI_SSIZE0 + 2, "Large", "I"),
            SEP,
            IT(MI_OPACITY1, "100% (solid)", "O"),
            IT(MI_OPACITY1 + 1, "75%", "O"),
            IT(MI_OPACITY1 + 2, "50%", "O"),
            IT(MI_OPACITY1 + 3, "25%", "O"),
            SEP,
            IT(MI_POPOUT, "In its own &window (a clean field)", NULL),
        END,
        SEP,
        SUB("&Window size"),
            IT(MI_SIZE1, "1x", NULL),
            IT(MI_SIZE2, "2x", NULL),
            IT(MI_SIZE3, "3x", NULL),
            IT(MI_SIZE4, "4x", NULL),
        END,
        IT(MI_INTEGER, "&Integer scaling", NULL),
        IT(MI_FULL, "&Fullscreen", "F11"),
    END,
    SUB("&Wireless"),
        IT(MI_NET, "&Connect (Nintendo Wi-Fi Connection replacement)", NULL),
        SUB("&Server"),
            IT(MI_SRV_BUILTIN, "&Built-in (offline: DQIX's preserved downloads)", NULL),
            SEP,
            IT(MI_SRV_ALTWFC, "&AltWFC (172.104.88.237)", NULL),
            IT(MI_SRV_WIIMMFI, "&Wiimmfi (164.132.44.106): not for DQIX", NULL),
            IT(MI_SRV_CUSTOM, "&Custom (wfc_server in settings.ini)", NULL),
        END,
        SEP,
        SUB("&Play with friends far away (tag mode, visiting)"),
            IT(MI_SES_HOST, "&Host: your friends join you...", NULL),
            IT(MI_SES_JOIN, "&Join a friend...", NULL),
            IT(MI_SES_START, "&Start now (everyone is here)", "Enter"),
            IT(MI_SES_LEAVE, "&Leave and play alone", NULL),
            SEP,
            WIN(MI_SES_NAME, "Your &name...", NULL),
            IT(MI_SES_HELP, "&How does this work?", NULL),
        END,
        SEP,
        IT(MI_DLC, "&Unlock all downloadable content now (without Sellma)", NULL),
        SEP,
        IT(MI_NET_HELP, "&How does this work?", NULL),
    END,
    SUB("&Modern"),
        IT(MI_ANALOG, "Walk in any &direction with the left stick", NULL),
        IT(MI_WIDE, "&Basic Widescreen: the action wide, the other screen in a corner", NULL),
        IT(MI_WIDE_DYN, "D&ynamic Widescreen (Experimental)", NULL),
        IT(MI_CORNER, "Top screen in the &corner (the field as on the DS)", NULL),
        IT(MI_POPOUT, "Widescreen: the small screen in its own &window (a clean field)", NULL),
        IT(MI_SCENE_BARS, "Widescreen: &black sides in this place (remembered)", NULL),
        IT(MI_DISCORD, "Show on &Discord what I'm playing", NULL),
        IT(MI_HUD, "Widescreen: battle &HUD over the 3D (experimental: the party's panels in the bottom left)", NULL),
        SEP,
        IT(MI_MODERN_HELP, "&What is this?", NULL),
    END,
    SUB("M&ods"),
        IT(MI_TEXPACK, "&Texture packs (the pictures in the textures folder)", NULL),
        IT(MI_TEXUP, "&Upscale the 3D textures (xBR: smooth slopes and curves)", NULL),
        IT(MI_TEXRELOAD, "&Reload the texture packs", NULL),
        IT(MI_TEXFOLDER, "&Open the textures folder", NULL),
        SEP,
        IT(MI_TEXDUMP, "&Dump textures (to make a pack: every texture shown goes to textures\\dump)", NULL),
        SEP,
        IT(MI_MUSPACK, "&Music packs (the recordings in the music folder)", NULL),
        IT(MI_MUSRELOAD, "R&eload the music", NULL),
        IT(MI_MUSFOLDER, "O&pen the music folder (and its list of the songs)", NULL),
        IT(MI_MUSNAMES, "&Show each song's name as it starts", NULL),
        IT(MI_MUSEXPORT, "E&xport the game's songs (WAV files with their loops, to music\\export: about 650 MB)", NULL),
        SEP,
        IT(MI_MODS_HELP, "&What is this?", NULL),
    END,
    SUB("&Help"),
        IT(MI_CONTROLS, "&Controls", NULL),
        IT(MI_UPDATE, "Check for &updates...", NULL),
        IT(MI_UPDATE_AUTO, "Check for updates at &start", NULL),
        SEP,
        IT(MI_SEND_REPORT, "&Send a bug report...", NULL),
        IT(MI_ABOUT, "&About", NULL),
    END,
    { MK_STOP, 0, NULL, NULL, 0 },
};

/* ---------------------------------------------------------------- ticks and greyed-out items */
#define MI_MAX 512
static unsigned char g_checked[MI_MAX], g_disabled[MI_MAX];
static unsigned g_menu_gen;                              /* changes whenever a tick or a grey changes */

void menu_check_os(int id, int on);                      /* the Windows menu bar (menu_win32.c) */
void menu_enable_os(int id, int on);

void menu_check(int id, int on) {
    if (id > 0 && id < MI_MAX && g_checked[id] != (on != 0)) { g_checked[id] = on != 0; g_menu_gen++; }
    menu_check_os(id, on);
}
void menu_enable(int id, int on) {
    if (id > 0 && id < MI_MAX && g_disabled[id] != (on == 0)) { g_disabled[id] = on == 0; g_menu_gen++; }
    menu_enable_os(id, on);
}
int menu_is_checked(int id) { return id > 0 && id < MI_MAX && g_checked[id]; }
int menu_is_enabled(int id) { return !(id > 0 && id < MI_MAX && g_disabled[id]); }
unsigned menu_generation(void) { return g_menu_gen; }

/* "&Game" -> "Game" ("&&" -> "&") */
void menu_plain_label(const char* in, char* out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < n; i++) {
        if (in[i] == '&') { if (in[i + 1] == '&') out[j++] = '&', i++; continue; }
        out[j++] = in[i];
    }
    if (n) out[j] = 0;
}
