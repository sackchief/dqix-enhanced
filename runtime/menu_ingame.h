/* The in-game menu (menu_ingame.c): every option over the game, for a
 * controller, keyboard, mouse or touch, on every system. */
#pragma once
#include "SDL.h"
#include <stdint.h>

void ingame_menu_set_shells(const char* const* names, int n);   /* the console colours (View > Console colour) */
void ingame_menu_open(void);
void ingame_menu_close(void);
int ingame_menu_is_open(void);
/* Input while it is open. Each returns 1 if it used the input; *action gets a
 * menu command to carry out (MI_..., 0 none). */
int ingame_menu_key(SDL_Scancode k, int* action);
int ingame_menu_mouse(const SDL_Event* e, int* action);
void ingame_menu_pad(SDL_GameController* p, int* action, uint32_t now_ms);   /* every frame: the D-pad and stick repeat */
/* Its layer for a window of w x h pixels (RGBA, rows top-down), or NULL if
 * unchanged since the last call (force: draw anyway). */
const uint8_t* ingame_menu_draw(int w, int h, int force);
