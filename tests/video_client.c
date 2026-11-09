#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 320, HEIGHT = 240, FRAME_SIZE = WIDTH * HEIGHT * 3 };

struct dequeue_attempt {
    int fd;
    atomic_bool started;
    atomic_bool finished;
    int result;
    int error;
    struct v4l2_buffer buffer;
};

static void *dequeue(void *argument) {
    struct dequeue_attempt *attempt = argument;
    attempt->buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    attempt->buffer.memory = V4L2_MEMORY_MMAP;
    atomic_store(&attempt->started, true);
    attempt->result = ioctl(attempt->fd, VIDIOC_DQBUF, &attempt->buffer);
    attempt->error = errno;
    atomic_store(&attempt->finished, true);
    return NULL;
}

int main(void) {
    int fd = open("/dev/video0", O_RDWR | O_NONBLOCK);
    assert(fd >= 0);
    int duplicate = dup(fd);
    assert(duplicate >= 0);
    assert(close(fd) == 0);
    fd = duplicate;
    struct v4l2_capability cap = {0};
    assert(ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0);
    assert((cap.device_caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING)) ==
           (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING));
    int input = -1;
    assert(ioctl(fd, VIDIOC_G_INPUT, &input) == 0 && input == 0);
    struct v4l2_input input_info = {.index = 0};
    assert(ioctl(fd, VIDIOC_ENUMINPUT, &input_info) == 0);
    assert(input_info.type == V4L2_INPUT_TYPE_CAMERA);
    input_info.index = 1;
    errno = 0;
    assert(ioctl(fd, VIDIOC_ENUMINPUT, &input_info) == -1 && errno == EINVAL);
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    assert(ioctl(fd, VIDIOC_G_FMT, &format) == 0);
    assert(format.fmt.pix.width == WIDTH && format.fmt.pix.height == HEIGHT);
    assert(format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24);
    assert(format.fmt.pix.bytesperline == WIDTH * 3 && format.fmt.pix.sizeimage == FRAME_SIZE);
    struct v4l2_requestbuffers req = {
        .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
    };
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count == 2);
    void *mapped[2];
    for (unsigned int i = 0; i < 2; i++) {
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i
        };
        assert(ioctl(fd, VIDIOC_QUERYBUF, &b) == 0);
        assert(b.length == FRAME_SIZE);
        assert(b.m.offset % sysconf(_SC_PAGESIZE) == 0);
        if (i == 1) assert(b.m.offset >= FRAME_SIZE);
        mapped[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, fd, b.m.offset);
        assert(mapped[i] != MAP_FAILED);
    }
    struct v4l2_buffer first = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 1
    };
    assert(ioctl(fd, VIDIOC_QBUF, &first) == 0);
    errno = 0;
    assert(ioctl(fd, VIDIOC_QBUF, &first) == -1 && errno == EINVAL);
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    assert(poll(&ready, 1, 0) == 0);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    assert(poll(&ready, 1, 1000) == 1);
    struct v4l2_buffer out = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
    };
    assert(ioctl(fd, VIDIOC_DQBUF, &out) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    assert(out.index == 1 && out.bytesused == FRAME_SIZE && out.sequence == 0);
    assert(out.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);
    int64_t frame_us = (int64_t)out.timestamp.tv_sec * 1000000 + out.timestamp.tv_usec;
    assert(frame_us >= (int64_t)before.tv_sec * 1000000 + before.tv_nsec / 1000);
    assert(frame_us <= (int64_t)after.tv_sec * 1000000 + after.tv_nsec / 1000);
    const unsigned char *pixels = mapped[1];
    assert(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0);
    assert(pixels[3] == 4 && pixels[4] == 0 && pixels[5] == 0);
    assert(poll(&ready, 1, 0) == 0);
    errno = 0;
    assert(ioctl(fd, VIDIOC_DQBUF, &out) == -1 && errno == EAGAIN);
    first.index = 0;
    assert(ioctl(fd, VIDIOC_QBUF, &first) == 0);
    first.index = 1;
    assert(ioctl(fd, VIDIOC_QBUF, &first) == 0);
    assert(poll(&ready, 1, 1000) == 1);
    out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    out.memory = V4L2_MEMORY_MMAP;
    out.index = UINT_MAX;
    assert(ioctl(fd, VIDIOC_DQBUF, &out) == 0);
    assert(out.index == 0 && out.sequence == 1);
    pixels = mapped[0];
    assert(pixels[2] == 1);
    assert(poll(&ready, 1, 1000) == 1);
    out.index = UINT_MAX;
    assert(ioctl(fd, VIDIOC_DQBUF, &out) == 0);
    assert(out.index == 1 && out.sequence == 2);
    pixels = mapped[1];
    assert(pixels[2] == 2);
    assert(poll(&ready, 1, 0) == 0);
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    assert(poll(&ready, 1, 0) == 0);
    req.count = 0;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count == 0);
    for (unsigned int i = 0; i < 2; i++) assert(munmap(mapped[i], FRAME_SIZE) == 0);
    assert(close(fd) == 0);

    fd = open("/dev/video0", O_RDWR);
    assert(fd >= 0 && !(fcntl(fd, F_GETFL) & O_NONBLOCK));
    req.count = 1;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count == 1);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    struct dequeue_attempt attempt = {.fd = fd};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, dequeue, &attempt) == 0);
    while (!atomic_load(&attempt.started)) usleep(1000);
    usleep(50000);
    assert(!atomic_load(&attempt.finished));
    struct v4l2_buffer queued = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0
    };
    assert(ioctl(fd, VIDIOC_QBUF, &queued) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(attempt.result == 0 && attempt.buffer.index == 0);

    struct dequeue_attempt stopped = {.fd = fd};
    assert(pthread_create(&thread, NULL, dequeue, &stopped) == 0);
    while (!atomic_load(&stopped.started)) usleep(1000);
    usleep(50000);
    assert(!atomic_load(&stopped.finished));
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    assert(ioctl(fd, VIDIOC_QBUF, &queued) == 0);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(stopped.result == -1 && stopped.error == EINVAL);
    struct v4l2_buffer restarted = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
    };
    assert(ioctl(fd, VIDIOC_DQBUF, &restarted) == 0 && restarted.index == 0);
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    assert(close(fd) == 0);
    puts("fauxdev video tests passed");
    return 0;
}
