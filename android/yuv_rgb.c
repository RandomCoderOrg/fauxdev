#include "yuv_rgb.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

int fauxdev_yuv420_to_nv12(
    uint8_t *output, int output_width, int output_height,
    int input_width, int input_height, const struct fauxdev_plane planes[3]
) {
    if (!output || !planes || output_width <= 0 || output_height <= 0 ||
        input_width <= 0 || input_height <= 0 || (output_width & 1) || (output_height & 1))
        return -EINVAL;
    for (int p = 0; p < 3; ++p)
        if (!planes[p].data || planes[p].row_stride <= 0 || planes[p].pixel_stride <= 0)
            return -EINVAL;
    const int native_size = input_width == output_width && input_height == output_height;
    for (int y = 0; y < output_height; ++y) {
        if (native_size && planes[0].pixel_stride == 1) {
            size_t at = (size_t)y * planes[0].row_stride;
            if (at > planes[0].length || (size_t)output_width > planes[0].length - at)
                return -EINVAL;
            memcpy(output + (size_t)y * output_width, planes[0].data + at, output_width);
        } else {
            int sy = (int)((int64_t)y * input_height / output_height);
            for (int x = 0; x < output_width; ++x) {
                int sx = (int)((int64_t)x * input_width / output_width);
                size_t at = (size_t)sy * planes[0].row_stride + (size_t)sx * planes[0].pixel_stride;
                if (at >= planes[0].length) return -EINVAL;
                output[(size_t)y * output_width + x] = planes[0].data[at];
            }
        }
    }
    uint8_t *uv = output + (size_t)output_width * output_height;
    for (int y = 0; y < output_height / 2; ++y) {
        int sy = native_size ? y : (int)((int64_t)(2 * y) * input_height / output_height) / 2;
        for (int x = 0; x < output_width / 2; ++x) {
            int sx = native_size ? x : (int)((int64_t)(2 * x) * input_width / output_width) / 2;
            for (int p = 1; p < 3; ++p) {
                size_t at = (size_t)sy * planes[p].row_stride + (size_t)sx * planes[p].pixel_stride;
                if (at >= planes[p].length) return -EINVAL;
                uv[(size_t)y * output_width + 2 * x + p - 1] = planes[p].data[at];
            }
        }
    }
    return 0;
}

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
