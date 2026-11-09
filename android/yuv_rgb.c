#include "yuv_rgb.h"

#include <errno.h>
#include <stdint.h>

static uint8_t clamp(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

int fauxdev_yuv420_to_rgb24(
    uint8_t *output, int output_width, int output_height,
    int input_width, int input_height, const struct fauxdev_plane planes[3]
) {
    if (!output || !planes || output_width <= 0 || output_height <= 0 ||
        input_width <= 0 || input_height <= 0) return -EINVAL;
    for (int p = 0; p < 3; p++) {
        if (!planes[p].data || planes[p].row_stride <= 0 || planes[p].pixel_stride <= 0)
            return -EINVAL;
    }
    for (int y = 0; y < output_height; y++) {
        const int source_y = (int)((int64_t)y * input_height / output_height);
        for (int x = 0; x < output_width; x++) {
            const int source_x = (int)((int64_t)x * input_width / output_width);
            const int px[3] = {source_x, source_x / 2, source_x / 2};
            const int py[3] = {source_y, source_y / 2, source_y / 2};
            uint8_t sample[3];
            for (int p = 0; p < 3; p++) {
                const size_t index = (size_t)py[p] * planes[p].row_stride +
                                     (size_t)px[p] * planes[p].pixel_stride;
                if (index >= planes[p].length) return -EINVAL;
                sample[p] = planes[p].data[index];
            }
            const int yy = sample[0];
            const int u = (int)sample[1] - 128;
            const int v = (int)sample[2] - 128;
            const size_t output_index = ((size_t)y * output_width + x) * 3;
            // shortcut: BT.601 full-range conversion; use image color metadata for fidelity.
            output[output_index] = clamp(yy + (359 * v) / 256);
            output[output_index + 1] = clamp(yy - (88 * u + 183 * v) / 256);
            output[output_index + 2] = clamp(yy + (454 * u) / 256);
        }
    }
    return 0;
}
