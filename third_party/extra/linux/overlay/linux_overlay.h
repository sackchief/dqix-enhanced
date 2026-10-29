#pragma once
#include <stddef.h>
#include <stdint.h>

#define DQIX_LINUX_OVERLAY_WIDTH 512
#define DQIX_LINUX_OVERLAY_HEIGHT 768
#define DQIX_LINUX_OVERLAY_ROW0 280
#define DQIX_LINUX_OVERLAY_ROW_STEP 62
#define DQIX_LINUX_OVERLAY_ROW_X 58
#define DQIX_LINUX_OVERLAY_ROW_WIDTH 396
#define DQIX_LINUX_OVERLAY_ROW_HEIGHT 44

/* Builds a top-down straight-alpha RGBA image. No SDL or graphics API dependency. */
void dqix_linux_overlay_render_rgba(uint8_t *rgba, size_t capacity,
    int selected, int classic, int fullscreen, int gl_active, const char *renderer_name);
/* Fit the overlay canvas in a viewport while keeping its native aspect ratio. */
int dqix_linux_overlay_fit_rect(int viewport_width, int viewport_height,
    int *x, int *y, int *width, int *height);
