#include "yuv_rgb.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const uint8_t y[] = {0, 16, 0, 0, 64, 0, 0, 0, 128};
    const uint8_t uv[] = {128, 128, 128, 128, 128, 128, 128, 128};
    const struct fauxdev_plane planes[3] = {
        {.data = y, .length = sizeof(y), .row_stride = 3, .pixel_stride = 1},
        {.data = uv, .length = sizeof(uv), .row_stride = 4, .pixel_stride = 2},
        {.data = uv + 1, .length = sizeof(uv) - 1, .row_stride = 4, .pixel_stride = 2},
    };
    uint8_t rgb[3 * 3 * 3] = {0};
    assert(fauxdev_yuv420_to_rgb24(rgb, 3, 3, 3, 3, planes) == 0);
    assert(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);
    assert(rgb[3] == 16 && rgb[4] == 16 && rgb[5] == 16);
    assert(rgb[(1 * 3 + 1) * 3] == 64);
    assert(rgb[(2 * 3 + 2) * 3] == 128);
    struct fauxdev_plane too_short[3];
    memcpy(too_short, planes, sizeof(too_short));
    too_short[0].length = 2;
    assert(fauxdev_yuv420_to_rgb24(rgb, 3, 3, 3, 3, too_short) == -EINVAL);
    puts("fauxdev YUV conversion tests passed");
    return 0;
}
