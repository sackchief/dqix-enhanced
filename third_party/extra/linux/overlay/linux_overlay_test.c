#include "linux_overlay.h"
#include <stdio.h>
#include <stdlib.h>

static int pixel_is(const uint8_t *p, int x, int y, unsigned r, unsigned g, unsigned b, unsigned a) {
    const uint8_t *q = p + ((size_t)y * DQIX_LINUX_OVERLAY_WIDTH + (size_t)x) * 4;
    return q[0] == r && q[1] == g && q[2] == b && q[3] == a;
}
int main(void) {
    const size_t bytes = (size_t)DQIX_LINUX_OVERLAY_WIDTH * DQIX_LINUX_OVERLAY_HEIGHT * 4;
    uint8_t *a = (uint8_t*)malloc(bytes), *b = (uint8_t*)malloc(bytes);
    if (!a || !b) { free(a); free(b); return 2; }
    dqix_linux_overlay_render_rgba(a, bytes, 0, 1, 0, 0, "software");
    dqix_linux_overlay_render_rgba(b, bytes, 1, 1, 0, 0, "software");
    int ok = 1;
    ok &= pixel_is(a, 4, 4, 0, 0, 0, 0);
    {
        int x = -1, y = -1, w = -1, h = -1;
        ok &= dqix_linux_overlay_fit_rect(512, 768, &x, &y, &w, &h);
        ok &= x == 0 && y == 0 && w == 512 && h == 768;
        ok &= dqix_linux_overlay_fit_rect(1920, 1080, &x, &y, &w, &h);
        ok &= x == 600 && y == 0 && w == 720 && h == 1080;
        ok &= dqix_linux_overlay_fit_rect(1024, 768, &x, &y, &w, &h);
        ok &= x == 256 && y == 0 && w == 512 && h == 768;
        ok &= !dqix_linux_overlay_fit_rect(0, 768, &x, &y, &w, &h);
    }
    ok &= pixel_is(a, 32, 130, 245, 190, 83, 255);
    const int x = DQIX_LINUX_OVERLAY_ROW_X + 6;
    const int y = DQIX_LINUX_OVERLAY_ROW0;
    const uint8_t *pa = a + ((size_t)y * DQIX_LINUX_OVERLAY_WIDTH + (size_t)x) * 4;
    const uint8_t *pb = b + ((size_t)y * DQIX_LINUX_OVERLAY_WIDTH + (size_t)x) * 4;
    ok &= pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2];
    if (!ok) fprintf(stderr, "overlay RGBA rendering assertions failed\n");
    free(a); free(b);
    return ok ? 0 : 1;
}
