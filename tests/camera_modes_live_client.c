#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void check(int ok, const char *step) {
    if (!ok) { perror(step); exit(1); }
}

static void capture(int fd, unsigned input, unsigned index, struct v4l2_frmsizeenum *mode) {
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    fmt.fmt.pix.width = mode->discrete.width;
    fmt.fmt.pix.height = mode->discrete.height;
    check(ioctl(fd, VIDIOC_S_FMT, &fmt) == 0 &&
          fmt.fmt.pix.width == mode->discrete.width &&
          fmt.fmt.pix.height == mode->discrete.height, "S_FMT");
    struct v4l2_requestbuffers req = {
        .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    check(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count >= 2, "REQBUFS");
    struct v4l2_buffer frame = {.type = req.type, .memory = req.memory, .index = 0};
    check(ioctl(fd, VIDIOC_QUERYBUF, &frame) == 0 && frame.length >= fmt.fmt.pix.sizeimage,
          "QUERYBUF");
    size_t length = frame.length;
    uint8_t *pixels = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, frame.m.offset);
    check(pixels != MAP_FAILED, "mmap");
    check(ioctl(fd, VIDIOC_QBUF, &frame) == 0, "QBUF");
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    check(ioctl(fd, VIDIOC_STREAMON, &type) == 0, "STREAMON");
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    check(poll(&ready, 1, 20000) > 0 && (ready.revents & POLLIN), "poll");
    frame = (struct v4l2_buffer){.type = type, .memory = V4L2_MEMORY_MMAP};
    check(ioctl(fd, VIDIOC_DQBUF, &frame) == 0 && frame.bytesused == fmt.fmt.pix.sizeimage,
          "DQBUF");
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i += 4096) hash = (hash ^ pixels[i]) * UINT64_C(1099511628211);
    hash = (hash ^ pixels[length - 1]) * UINT64_C(1099511628211);
    printf("input=%u mode=%u %ux%u bytes=%u sample-hash=%016llx\n", input, index,
           fmt.fmt.pix.width, fmt.fmt.pix.height, frame.bytesused, (unsigned long long)hash);
    fflush(stdout);
    check(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0, "STREAMOFF");
    check(munmap(pixels, length) == 0, "munmap");
    req.count = 0;
    check(ioctl(fd, VIDIOC_REQBUFS, &req) == 0, "release");
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    char *end;
    unsigned long input = strtoul(argv[1], &end, 10);
    if (*end || input > 63) return 2;
    unsigned long selected_mode = 0;
    if (argc == 3) {
        selected_mode = strtoul(argv[2], &end, 10);
        if (*end || selected_mode > 1024) return 2;
    }
    int fd = open("/dev/video0", O_RDWR);
    check(fd >= 0, "open");
    int selected = (int)input;
    check(ioctl(fd, VIDIOC_S_INPUT, &selected) == 0, "S_INPUT");
    unsigned count = 0;
    for (;;) {
        struct v4l2_frmsizeenum mode = {.index = count, .pixel_format = V4L2_PIX_FMT_RGB24};
        if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &mode) < 0) {
            check(errno == EINVAL, "ENUM_FRAMESIZES");
            break;
        }
        if (argc == 2 || count == selected_mode) capture(fd, input, count, &mode);
        ++count;
    }
    if (argc == 3) check(selected_mode < count, "mode index");
    printf("input=%lu enumerated=%u modes\n", input, count);
    check(close(fd) == 0, "close");
    return 0;
}
