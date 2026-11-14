#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what) { perror(what); return 1; }

int main(int argc, char **argv) {
    if ((argc != 2 && argc != 3) || (strcmp(argv[1], "hold") &&
                      strcmp(argv[1], "priority") && strcmp(argv[1], "set") &&
                      strcmp(argv[1], "query") && strcmp(argv[1], "getrate") &&
                      strcmp(argv[1], "setrate"))) {
        fprintf(stderr, "usage: %s hold|priority|set [320|640]|query|getrate|setrate fps\n", argv[0]);
        return 2;
    }
    int fd = open("/dev/video0", O_RDWR);
    if (fd < 0) return fail("open");
    if (!strcmp(argv[1], "hold") || !strcmp(argv[1], "priority")) {
        struct v4l2_requestbuffers req = {
            .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (!strcmp(argv[1], "hold")) {
            if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) return fail("REQBUFS 2");
            puts("LEASED");
        } else {
            enum v4l2_priority priority = V4L2_PRIORITY_RECORD;
            if (ioctl(fd, VIDIOC_S_PRIORITY, &priority) < 0) return fail("S_PRIORITY");
            puts("PRIORITY");
        }
        fflush(stdout);
        struct timespec wait = {.tv_sec = 30};
        while (nanosleep(&wait, &wait) < 0 && errno == EINTR) {}
        if (!strcmp(argv[1], "hold")) {
            req.count = 0;
            if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) return fail("REQBUFS 0");
            puts("RELEASED");
        }
    } else if (!strcmp(argv[1], "getrate") || !strcmp(argv[1], "setrate")) {
        if (!strcmp(argv[1], "setrate") && argc != 3) return 2;
        struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
        if (!strcmp(argv[1], "setrate")) {
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = (uint32_t)atoi(argv[2]);
            if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) return fail("S_PARM");
        } else if (ioctl(fd, VIDIOC_G_PARM, &parm) < 0) return fail("G_PARM");
        printf("%u/%u\n", parm.parm.capture.timeperframe.numerator,
               parm.parm.capture.timeperframe.denominator);
    } else {
        struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
        if (!strcmp(argv[1], "set")) {
            f.fmt.pix.width = argc == 3 && !strcmp(argv[2], "320") ? 320 : 640;
            f.fmt.pix.height = f.fmt.pix.width == 320 ? 240 : 480;
            f.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
            if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) return fail("S_FMT");
        } else if (ioctl(fd, VIDIOC_G_FMT, &f) < 0) return fail("G_FMT");
        printf("%ux%u\n", f.fmt.pix.width, f.fmt.pix.height);
    }
    if (close(fd) < 0) return fail("close");
    return 0;
}
