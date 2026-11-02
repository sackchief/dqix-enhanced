/* Text on the DS screens (the session lobby), with a font from the system. */
#pragma once
#include <stdint.h>

int text_ready(void);                      /* a font was found */
/* Draws UTF-8 text into a 0x00RRGGBB buffer of width w, height h. Returns the
 * width drawn. */
int text_draw(uint32_t* buf, int w, int h, int x, int y, float px, uint32_t color, const char* s);
int text_width(float px, const char* s);   /* how wide text_draw would draw it */
/* Word-wrapped within max_w pixels; returns the y below the last line. */
int text_wrap(uint32_t* buf, int w, int h, int x, int y, int max_w, float px, uint32_t color, const char* s);
