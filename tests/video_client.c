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
    unsigned char *anonymous = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, fd, 0);
    assert(anonymous != MAP_FAILED);
    anonymous[0] = 0x5a;
    assert(anonymous[0] == 0x5a);
    assert(munmap(anonymous, 4096) == 0);
    struct v4l2_capability cap = {0};
    assert(ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0);
    assert((cap.device_caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING)) ==
           (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING));
    assert(cap.version == ((3u << 16) | (1u << 8)));
    assert(cap.device_caps & V4L2_CAP_EXT_PIX_FORMAT);
    errno = 0;
    assert(ioctl(fd, _IO('V', 0xff), NULL) == -1 && errno == ENOTTY);
    assert(ioctl(fd, VIDIOC_LOG_STATUS) == 0);
    int other = open("/dev/video0", O_RDWR);
    assert(other >= 0);
    enum v4l2_priority priority;
    assert(ioctl(fd, VIDIOC_G_PRIORITY, &priority) == 0 && priority == V4L2_PRIORITY_DEFAULT);
    priority = V4L2_PRIORITY_RECORD;
    assert(ioctl(fd, VIDIOC_S_PRIORITY, &priority) == 0);
    assert(ioctl(other, VIDIOC_G_PRIORITY, &priority) == 0 && priority == V4L2_PRIORITY_RECORD);
    int selected = 0;
    errno = 0;
    assert(ioctl(other, VIDIOC_S_INPUT, &selected) == -1 && errno == EBUSY);
    struct v4l2_format other_format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    errno = 0;
    assert(ioctl(other, VIDIOC_S_FMT, &other_format) == -1 && errno == EBUSY);
    priority = V4L2_PRIORITY_INTERACTIVE;
    errno = 0;
    assert(ioctl(other, VIDIOC_S_PRIORITY, &priority) == -1 && errno == EBUSY);
    assert(ioctl(fd, VIDIOC_S_PRIORITY, &priority) == 0);
    assert(close(other) == 0);
    struct v4l2_fmtdesc description = {
        .index = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .reserved = {1, 1, 1},
    };
    assert(ioctl(fd, VIDIOC_ENUM_FMT, &description) == 0);
    for (size_t i = 0; i < 3; ++i) assert(description.reserved[i] == 0);
    assert(strcmp((char *)description.description, "24-bit RGB 8-8-8") == 0);
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
    assert(format.fmt.pix.priv == V4L2_PIX_FMT_PRIV_MAGIC);
    assert(format.fmt.pix.bytesperline == WIDTH * 3 && format.fmt.pix.sizeimage == FRAME_SIZE);
    struct v4l2_requestbuffers req = {
        .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
    };
    req.flags = V4L2_MEMORY_FLAG_NON_COHERENT;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count == 2);
    assert(req.flags == 0);
    void *mapped[2];
    for (unsigned int i = 0; i < 2; i++) {
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i
        };
        assert(ioctl(fd, VIDIOC_QUERYBUF, &b) == 0);
        assert(b.memory == V4L2_MEMORY_MMAP);
        assert(b.field == V4L2_FIELD_NONE);
        assert(b.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);
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
    assert(out.field == V4L2_FIELD_NONE);
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
    struct v4l2_create_buffers created = {
        .count = 0, .memory = V4L2_MEMORY_MMAP, .format = format,
    };
    assert(ioctl(fd, VIDIOC_CREATE_BUFS, &created) == 0 && created.index == 2);
    assert(created.count == 0);
    created.count = 1;
    created.format.fmt.pix.sizeimage = FRAME_SIZE - 1;
    errno = 0;
    assert(ioctl(fd, VIDIOC_CREATE_BUFS, &created) == -1 && errno == EINVAL);
    struct v4l2_buffer old_buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = 1,
    };
    assert(ioctl(fd, VIDIOC_QUERYBUF, &old_buffer) == 0);
    assert(old_buffer.length == FRAME_SIZE);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    created.count = 1;
    created.format.fmt.pix.sizeimage = FRAME_SIZE * 2;
    assert(ioctl(fd, VIDIOC_CREATE_BUFS, &created) == 0);
    assert(created.index == 2 && created.count == 1);
    struct v4l2_buffer appended = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = 2,
    };
    assert(ioctl(fd, VIDIOC_QUERYBUF, &appended) == 0);
    assert(appended.length == FRAME_SIZE * 2);
    assert(appended.m.offset % sysconf(_SC_PAGESIZE) == 0);
    assert(appended.m.offset >= old_buffer.m.offset + old_buffer.length);
    void *larger = mmap(NULL, appended.length, PROT_READ, MAP_SHARED, fd, appended.m.offset);
    assert(larger != MAP_FAILED);
    appended.memory = V4L2_MEMORY_MMAP;
    assert(ioctl(fd, VIDIOC_QBUF, &appended) == 0);
    assert(poll(&ready, 1, 1000) == 1);
    out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    out.memory = V4L2_MEMORY_MMAP;
    assert(ioctl(fd, VIDIOC_DQBUF, &out) == 0 && out.index == 2);
    assert(out.bytesused == FRAME_SIZE && out.length == FRAME_SIZE * 2);
    assert(((unsigned char *)larger)[2] == 3);
    assert(((unsigned char *)mapped[1])[2] == 2);
    req.count = 0;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count == 0);
    assert(((unsigned char *)larger)[2] == 3);
    assert(((unsigned char *)mapped[1])[2] == 2);
    req.count = 1;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count == 1);
    struct v4l2_buffer replacement = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = 0,
    };
    assert(ioctl(fd, VIDIOC_QUERYBUF, &replacement) == 0);
    assert(replacement.length == FRAME_SIZE && replacement.m.offset == 0);
    void *new_mapping = mmap(NULL, replacement.length, PROT_READ, MAP_SHARED, fd,
                             replacement.m.offset);
    assert(new_mapping != MAP_FAILED);
    replacement.memory = V4L2_MEMORY_MMAP;
    assert(ioctl(fd, VIDIOC_QBUF, &replacement) == 0);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    assert(poll(&ready, 1, 1000) == 1);
    out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    out.memory = V4L2_MEMORY_MMAP;
    assert(ioctl(fd, VIDIOC_DQBUF, &out) == 0 && out.index == 0);
    assert(((unsigned char *)new_mapping)[3] == 4);
    assert(((unsigned char *)larger)[2] == 3);
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    req.count = 0;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
    assert(munmap(new_mapping, FRAME_SIZE) == 0);
    assert(munmap(larger, FRAME_SIZE * 2) == 0);
    created.count = 100;
    created.format.fmt.pix.sizeimage = FRAME_SIZE;
    assert(ioctl(fd, VIDIOC_CREATE_BUFS, &created) == 0);
    assert(created.index == 0 && created.count == VIDEO_MAX_FRAME);
    struct v4l2_buffer last = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = VIDEO_MAX_FRAME - 1,
    };
    assert(ioctl(fd, VIDIOC_QUERYBUF, &last) == 0);
    assert(last.length == FRAME_SIZE && last.m.offset % sysconf(_SC_PAGESIZE) == 0);
    created.count = 1;
    errno = 0;
    assert(ioctl(fd, VIDIOC_CREATE_BUFS, &created) == -1 && errno == ENOBUFS);
    req.count = 0;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
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
