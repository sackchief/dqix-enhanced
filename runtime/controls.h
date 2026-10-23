/* Which keys and controller buttons do what (Game > Controls, controls.ini).
 * Every action has up to two keyboard keys and one controller button. The
 * DS's buttons are actions too; Esc (leave fullscreen / quit) and Alt+Enter
 * (fullscreen) stay fixed, and so do the sticks (walking, the camera). */
#pragma once
#include "SDL.h"
#include <stdint.h>
#include <stddef.h>

enum {
    /* the DS's buttons (in the order of the DS's key bits, see ds_bit) */
    CA_A, CA_B, CA_X, CA_Y, CA_L, CA_R, CA_START, CA_SELECT, CA_UP, CA_DOWN, CA_LEFT, CA_RIGHT,
    CA_N_DS,
    /* the program's own */
    CA_FAST = CA_N_DS, CA_PAUSE, CA_FULL, CA_LAYOUT, CA_SINGLE, CA_SWAP, CA_CORNER, CA_SMALLSIZE, CA_OPACITY,
    CA_WINSIZE, CA_RES3D, CA_MODE, CA_LCD, CA_SHELL, CA_ACTUAL,
    CA_VOLDOWN, CA_VOLUP, CA_RESET, CA_SHOT, CA_REPORT, CA_HDTEXT,
    CA_N
};

#define CTL_MOD_CTRL  (1 << 16)                /* a key: SDL scancode | these */
#define CTL_MOD_ALT   (2 << 16)
#define CTL_MOD_SHIFT (4 << 16)
#define CTL_PAD_LT 100                         /* controller "buttons" beyond SDL's: the triggers */
#define CTL_PAD_RT 101
#define CTL_NONE (-1)

typedef struct {
    const char* id;                            /* in controls.ini */
    const char* name;                          /* in the Controls window */
    int ds_bit;                                /* the DS key bit, or -1 */
} ControlAction;
extern const ControlAction g_ctl[CA_N];
extern int g_ctl_key[CA_N][2];                 /* 0 = none */
extern int g_ctl_pad[CA_N];                    /* CTL_NONE = none */

void controls_defaults(void);
void controls_load(void);                      /* controls.ini, or the defaults */
void controls_save(void);

int controls_key_action(SDL_Scancode sc, Uint16 mod);   /* the action a key press starts, or -1 */
int controls_key_held(int action, const Uint8* ks);     /* one of its keys is down */
int controls_pad_held(int action, SDL_GameController* p);
int controls_pad_down(SDL_GameController* p, int pad);  /* a controller button (or trigger) is down */
uint16_t controls_ds_keys(const Uint8* ks);             /* the DS's buttons held on the keyboard */
uint16_t controls_ds_pad(SDL_GameController* p);        /* ... on the controller */

void controls_key_name(int key, char* out, size_t n);   /* "Ctrl+R", "Left Shift", "-" */
void controls_pad_name(int pad, char* out, size_t n);   /* "Right face button", "Left trigger" */
int controls_key_owner(int key, int* slot);             /* the action that has this key, or -1 */
int controls_pad_owner(int pad);
/* Give `action` this key (slot 0/1) or controller button; whoever had it gets
 * this action's old one (a swap). */
void controls_set_key(int action, int slot, int key);
void controls_set_pad(int action, int pad);
