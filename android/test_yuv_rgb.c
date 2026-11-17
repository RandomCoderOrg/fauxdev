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
    const uint8_t padded_y[] = {1, 2, 3, 4, 0, 5, 6, 7, 8, 0};
    const uint8_t padded_uv[] = {10, 20, 11, 21, 0, 0};
    const struct fauxdev_plane nv_planes[3] = {
        {.data = padded_y, .length = sizeof(padded_y), .row_stride = 5, .pixel_stride = 1},
        {.data = padded_uv, .length = sizeof(padded_uv), .row_stride = 6, .pixel_stride = 2},
        {.data = padded_uv + 1, .length = sizeof(padded_uv) - 1, .row_stride = 6, .pixel_stride = 2},
    };
    uint8_t nv12[12] = {0};
    const uint8_t expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 11, 21};
    assert(fauxdev_yuv420_to_nv12(nv12, 4, 2, 4, 2, nv_planes) == 0);
    assert(memcmp(nv12, expected, sizeof(expected)) == 0);
    const uint8_t planar_u[] = {10, 11};
    const uint8_t planar_v[] = {20, 21};
    struct fauxdev_plane planar[3];
    memcpy(planar, nv_planes, sizeof(planar));
    planar[1] = (struct fauxdev_plane){.data = planar_u, .length = sizeof(planar_u), .row_stride = 2, .pixel_stride = 1};
    planar[2] = (struct fauxdev_plane){.data = planar_v, .length = sizeof(planar_v), .row_stride = 2, .pixel_stride = 1};
    assert(fauxdev_yuv420_to_nv12(nv12, 4, 2, 4, 2, planar) == 0);
    assert(memcmp(nv12, expected, sizeof(expected)) == 0);
    assert(fauxdev_yuv420_to_nv12(nv12, 3, 2, 4, 2, nv_planes) == -EINVAL);
    struct fauxdev_plane short_uv[3];
    memcpy(short_uv, nv_planes, sizeof(short_uv));
    short_uv[2].length = 2;
    assert(fauxdev_yuv420_to_nv12(nv12, 4, 2, 4, 2, short_uv) == -EINVAL);
    memcpy(short_uv, nv_planes, sizeof(short_uv));
    short_uv[0].length = 8;
    assert(fauxdev_yuv420_to_nv12(nv12, 4, 2, 4, 2, short_uv) == -EINVAL);
    puts("fauxdev YUV conversion tests passed");
    return 0;
}
