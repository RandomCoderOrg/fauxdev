#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static void check(int ok, const char *step) {
    if (!ok) { perror(step); exit(1); }
}

static int64_t now_ns(void) {
    struct timespec now;
    check(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "clock_gettime");
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s input width height fps frames\n", argv[0]);
        return 2;
    }
    int input = atoi(argv[1]);
    unsigned width = (unsigned)atoi(argv[2]), height = (unsigned)atoi(argv[3]);
    unsigned fps = (unsigned)atoi(argv[4]), frames = (unsigned)atoi(argv[5]);
    if (input < 0 || !width || !height || !fps || !frames || frames > 100) return 2;
    int fd = open("/dev/video0", O_RDWR);
    check(fd >= 0, "open");
    check(ioctl(fd, VIDIOC_S_INPUT, &input) == 0, "S_INPUT");
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    check(ioctl(fd, VIDIOC_S_FMT, &format) == 0 &&
          format.fmt.pix.width == width && format.fmt.pix.height == height, "S_FMT");
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    check(ioctl(fd, VIDIOC_S_PARM, &parm) == 0 &&
          parm.parm.capture.timeperframe.denominator == fps, "S_PARM");
    struct v4l2_requestbuffers req = {
        .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    check(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count >= 2, "REQBUFS");
    void *mapped[2];
    size_t length[2];
    for (unsigned i = 0; i < 2; ++i) {
        struct v4l2_buffer buffer = {.type = req.type, .memory = req.memory, .index = i};
        check(ioctl(fd, VIDIOC_QUERYBUF, &buffer) == 0, "QUERYBUF");
        length[i] = buffer.length;
        mapped[i] = mmap(NULL, buffer.length, PROT_READ, MAP_SHARED, fd, buffer.m.offset);
        check(mapped[i] != MAP_FAILED, "mmap");
        check(ioctl(fd, VIDIOC_QBUF, &buffer) == 0, "QBUF");
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int64_t start = now_ns();
    int64_t first_arrival = 0;
    check(ioctl(fd, VIDIOC_STREAMON, &type) == 0, "STREAMON");
    for (unsigned i = 0; i < frames; ++i) {
        struct v4l2_buffer buffer = {.type = type, .memory = V4L2_MEMORY_MMAP};
        check(ioctl(fd, VIDIOC_DQBUF, &buffer) == 0 && buffer.index < 2 &&
              buffer.bytesused == format.fmt.pix.sizeimage, "DQBUF");
        int64_t arrival = now_ns();
        if (!first_arrival) first_arrival = arrival;
        int64_t stamp = (int64_t)buffer.timestamp.tv_sec * 1000000000 +
                        (int64_t)buffer.timestamp.tv_usec * 1000;
        printf("frame=%u index=%u arrival_ns=%lld v4l_ns=%lld bytes=%u\n",
               i, buffer.index, (long long)arrival, (long long)stamp, buffer.bytesused);
        fflush(stdout);
        check(ioctl(fd, VIDIOC_QBUF, &buffer) == 0, "requeue");
    }
    fprintf(stderr, "startup_ns=%lld\n", (long long)(first_arrival - start));
    check(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0, "STREAMOFF");
    for (unsigned i = 0; i < 2; ++i) check(munmap(mapped[i], length[i]) == 0, "munmap");
    req.count = 0;
    check(ioctl(fd, VIDIOC_REQBUFS, &req) == 0, "release");
    check(close(fd) == 0, "close");
    return 0;
}
