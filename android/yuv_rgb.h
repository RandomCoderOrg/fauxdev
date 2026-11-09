#ifndef FAUXDEV_YUV_RGB_H
#define FAUXDEV_YUV_RGB_H

#include <stddef.h>
#include <stdint.h>

struct fauxdev_plane {
    const uint8_t *data;
    size_t length;
    int row_stride;
    int pixel_stride;
};

int fauxdev_yuv420_to_rgb24(
    uint8_t *output, int output_width, int output_height,
    int input_width, int input_height, const struct fauxdev_plane planes[3]
);

#endif
