#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

enum { FRAME_SIZE = 320 * 240 * 3 };

static void check(int ok, const char *step) {
    if (!ok) { perror(step); exit(1); }
}

static void save_ppm(const char *prefix, unsigned index, const uint8_t *pixels) {
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s-%u.ppm", prefix, index);
    check(length > 0 && (size_t)length < sizeof(path), "output path");
    FILE *file = fopen(path, "wb");
    check(file != NULL, "open image");
    check(fprintf(file, "P6\n320 240\n255\n") > 0, "write PPM header");
    check(fwrite(pixels, 1, FRAME_SIZE, file) == FRAME_SIZE, "write PPM pixels");
    check(fclose(file) == 0, "close image");
    printf("saved %s\n", path);
}

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "usage: %s [output-prefix]\n", argv[0]);
        return 2;
    }
    const char *output_prefix = argc == 2 ? argv[1] : NULL;
    int fd = open("/dev/video0", O_RDWR);
    check(fd >= 0, "open video0");
    struct v4l2_input inputs[64];
    unsigned count = 0;
    for (; count < 64; ++count) {
        inputs[count] = (struct v4l2_input){.index = count};
        if (ioctl(fd, VIDIOC_ENUMINPUT, &inputs[count]) < 0) {
            check(errno == EINVAL, "enumerate input");
            break;
        }
        printf("input %u: %s\n", count, inputs[count].name);
    }
    check(count > 0, "no camera inputs");
    for (unsigned i = 0; i < count; ++i) {
        int selected = (int)i, current = -1;
        check(ioctl(fd, VIDIOC_S_INPUT, &selected) == 0, "select input");
        check(ioctl(fd, VIDIOC_G_INPUT, &current) == 0 && current == selected, "current input");
        struct v4l2_requestbuffers buffers = {
            .count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
        };
        check(ioctl(fd, VIDIOC_REQBUFS, &buffers) == 0 && buffers.count == 1, "allocate buffer");
        struct v4l2_buffer frame = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0,
        };
        check(ioctl(fd, VIDIOC_QUERYBUF, &frame) == 0 && frame.length == FRAME_SIZE, "query buffer");
        uint8_t *pixels = mmap(NULL, frame.length, PROT_READ, MAP_SHARED, fd, frame.m.offset);
        check(pixels != MAP_FAILED, "map buffer");
        check(ioctl(fd, VIDIOC_QBUF, &frame) == 0, "queue buffer");
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        check(ioctl(fd, VIDIOC_STREAMON, &type) == 0, "start stream");
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        check(poll(&ready, 1, 20000) > 0 && (ready.revents & POLLIN), "wait frame");
        frame = (struct v4l2_buffer){.type = type, .memory = V4L2_MEMORY_MMAP};
        check(ioctl(fd, VIDIOC_DQBUF, &frame) == 0 && frame.bytesused == FRAME_SIZE, "dequeue frame");
        uint64_t hash = UINT64_C(14695981039346656037);
        for (unsigned j = 0; j < FRAME_SIZE; ++j)
            hash = (hash ^ pixels[j]) * UINT64_C(1099511628211);
        printf("captured input %u (%s): %u bytes, hash %016llx\n", i, inputs[i].name,
               frame.bytesused, (unsigned long long)hash);
        if (output_prefix) save_ppm(output_prefix, i, pixels);
        check(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0, "stop stream");
        check(munmap(pixels, FRAME_SIZE) == 0, "unmap buffer");
        buffers.count = 0;
        check(ioctl(fd, VIDIOC_REQBUFS, &buffers) == 0, "release buffers");
    }
    check(close(fd) == 0, "close video0");
    return 0;
}
