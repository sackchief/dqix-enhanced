#pragma once
#include <stdint.h>

/* Draw a top-down straight-alpha RGBA image over the current OpenGL window.
 * Requires the game's current OpenGL context to remain current. */
int dqix_linux_overlay_gl_draw(const uint8_t *rgba, int width, int height,
    int viewport_width, int viewport_height);
void dqix_linux_overlay_gl_shutdown(void);
