/* The in-game menu (menu_ingame.c): every option over the game, for a
 * controller, keyboard, mouse or touch, on every system. */
#pragma once
#include "SDL.h"
#include <stdint.h>
#include <stddef.h>

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
/* the values changed with left / right (MF_VALUE rows: the volume): get writes the shown text */
void ingame_menu_set_values(int (*get)(int id, char* out, size_t n), void (*adjust)(int id, int dir));
/* A message or question inside the menu (instead of a window): begin it, then
 * keep the game's window going until ingame_dialog_result() isn't -1 (the
 * chosen button; Esc / B: cancel). check: a check box's text (NULL: none). */
void ingame_dialog_begin(const char* title, const char* text, const char* const* buttons, int n, int def, int cancel, const char* check, int checked);
int ingame_dialog_result(int* checked);
void ingame_dialog_update(const char* title, const char* text);   /* a live message: new words, same buttons */
int ingame_dialog_is_on(void);
void ingame_dialog_cancel(void);
/* A list to pick from (files, controls): rights: text at the right of each row
 * (NULL: none). Result: -2 still open, -1 back, else the row chosen. */
void ingame_list_begin(const char* title, const char* const* items, const char* const* rights, int n, int sel, const char* foot);
int ingame_list_result(void);
/* Typing: the keyboard types (pass SDL_TEXTINPUT to ingame_text_input), a
 * controller or the mouse uses the keyboard drawn on the screen. Result: -2
 * still typing, 0 cancelled, 1 done (the text in out). */
void ingame_text_begin(const char* title, const char* prompt, const char* initial, int max);
void ingame_text_input(const char* utf8);
int ingame_text_result(char* out, size_t n);
