#include "transport.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 320, HEIGHT = 240, FRAME_SIZE = WIDTH * HEIGHT * 3 };

struct broker_state {
    int listener;
    int release_fd;
    atomic_bool second_request_seen;
    atomic_bool second_late_write_done;
};

struct dequeue_attempt {
    int fd;
    atomic_bool started;
    atomic_bool finished;
    int result;
    int error;
};

static void *dequeue(void *argument) {
    struct dequeue_attempt *attempt = argument;
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
    };
    atomic_store(&attempt->started, true);
    attempt->result = ioctl(attempt->fd, VIDIOC_DQBUF, &buffer);
    attempt->error = errno;
    atomic_store(&attempt->finished, true);
    return NULL;
}

static void reply(int socket_fd, const struct fauxdev_message *request) {
    struct fauxdev_message response = {
        .opcode = request->opcode,
        .flags = FAUXDEV_MESSAGE_RESPONSE,
        .request_id = request->request_id,
    };
    assert(fauxdev_message_send(socket_fd, &response) == 0);
}

static long long elapsed_ms(struct timespec start) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
}

static void *broker(void *argument) {
    struct broker_state *state = argument;
    for (int turn = 0; turn < 3; turn++) {
        int socket_fd = accept4(state->listener, NULL, NULL, SOCK_CLOEXEC);
        assert(socket_fd >= 0);
        struct ucred peer;
        socklen_t peer_size = sizeof(peer);
        assert(getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) == 0);
        assert(peer.uid == getuid());
        struct fauxdev_message request;
        assert(fauxdev_message_receive(socket_fd, &request) == 0);
        assert(request.opcode == FAUXDEV_OPCODE_HELLO && request.fd_count == 0);
        reply(socket_fd, &request);
        assert(fauxdev_message_receive(socket_fd, &request) == 0);
        assert(request.opcode == FAUXDEV_OPCODE_CAMERA_INPUTS && !request.payload_size && !request.fd_count);
        struct fauxdev_message unsupported = {
            .opcode = request.opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
            .request_id = request.request_id, .status = -ENOTTY,
        };
        assert(fauxdev_message_send(socket_fd, &unsupported) == 0);
        assert(fauxdev_message_receive(socket_fd, &request) == 0);
        assert(request.opcode == FAUXDEV_OPCODE_VIDEO_FRAME);
        assert(request.payload_size == sizeof(struct fauxdev_video_frame_request));
        assert(request.fd_count == 1);
        struct fauxdev_video_frame_request frame;
        memcpy(&frame, request.payload, sizeof(frame));
        assert(le32toh(frame.width) == WIDTH && le32toh(frame.height) == HEIGHT);
        assert(le32toh(frame.stride) == WIDTH * 3 && le32toh(frame.size) == FRAME_SIZE);
        assert(le32toh(frame.pixel_format) == V4L2_PIX_FMT_RGB24);
        struct stat status;
        assert(fstat(request.fds[0], &status) == 0);
        assert((uint64_t)le32toh(frame.offset) + FRAME_SIZE <= (uint64_t)status.st_size);
        if (turn == 1) {
            atomic_store(&state->second_request_seen, true);
            struct pollfd peer_closed = {.fd = socket_fd, .events = POLLRDHUP};
            assert(poll(&peer_closed, 1, 5000) == 1);
            assert(peer_closed.revents & (POLLRDHUP | POLLHUP));
        }
        unsigned char *pixels = malloc(FRAME_SIZE);
        assert(pixels);
        memset(pixels, turn == 0 ? 0x5a : turn == 1 ? 0x6b : 0x7c, FRAME_SIZE);
        assert(pwrite(request.fds[0], pixels, FRAME_SIZE, le32toh(frame.offset)) == FRAME_SIZE);
        free(pixels);
        if (turn == 1) atomic_store(&state->second_late_write_done, true);
        fauxdev_message_close_fds(&request);
        if (turn != 1) {
            if (turn == 0) usleep(2200000); // Frame response may exceed control timeout.
            reply(socket_fd, &request);
        }
        if (turn != 1) {
            char release;
            assert(read(state->release_fd, &release, 1) == 1);
        }
        close(socket_fd);
    }
    return NULL;
}

int main(void) {
    char directory[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    assert(snprintf(directory, sizeof(directory), "%s/fauxdev-video-broker-XXXXXX", tmp) > 0);
    assert(mkdtemp(directory));
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    assert(snprintf(path, sizeof(path), "%s/broker.sock", directory) > 0);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path);
    assert(bind(listener, (const struct sockaddr *)&address,
                offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1) == 0);
    assert(chmod(path, 0600) == 0);
    assert(listen(listener, 1) == 0);
    int release_pipe[2];
    assert(pipe(release_pipe) == 0);
    struct broker_state state = {.listener = listener, .release_fd = release_pipe[0]};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, broker, &state) == 0);
    assert(setenv("FAUXDEV_VIDEO_SOCKET", path, 1) == 0);

    int fd = open("/dev/video0", O_RDWR);
    assert(fd >= 0);
    struct v4l2_requestbuffers buffers = {
        .count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
    };
    assert(ioctl(fd, VIDIOC_REQBUFS, &buffers) == 0 && buffers.count == 1);
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
    };
    assert(ioctl(fd, VIDIOC_QUERYBUF, &buffer) == 0);
    unsigned char *pixels = mmap(NULL, buffer.length, PROT_READ, MAP_SHARED, fd, buffer.m.offset);
    assert(pixels != MAP_FAILED);
    struct timespec started;
    assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    assert(ioctl(fd, VIDIOC_QBUF, &buffer) == 0);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    assert(elapsed_ms(started) < 1000);
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    assert(poll(&ready, 1, 5000) == 1);
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    assert(ioctl(fd, VIDIOC_DQBUF, &buffer) == 0);
    assert(buffer.bytesused == FRAME_SIZE);
    for (size_t i = 0; i < FRAME_SIZE; i++) assert(pixels[i] == 0x5a);
    struct dequeue_attempt attempt = {.fd = fd};
    pthread_t waiter;
    assert(pthread_create(&waiter, NULL, dequeue, &attempt) == 0);
    while (!atomic_load(&attempt.started)) usleep(1000);
    usleep(50000);
    assert(!atomic_load(&attempt.finished));
    assert(write(release_pipe[1], "x", 1) == 1);
    assert(pthread_join(waiter, NULL) == 0);
    assert(attempt.result == -1 && attempt.error == EPIPE);
    assert(poll(&ready, 1, 2000) == 1);

    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    assert(poll(&ready, 1, 0) == 0);
    buffer = (struct v4l2_buffer){
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0
    };
    assert(ioctl(fd, VIDIOC_QBUF, &buffer) == 0);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    for (int wait = 0; wait < 2000 && !atomic_load(&state.second_request_seen); wait++) usleep(1000);
    assert(atomic_load(&state.second_request_seen));
    assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    assert(elapsed_ms(started) < 1000);
    assert(poll(&ready, 1, 0) == 0);
    for (int wait = 0; wait < 2000 && !atomic_load(&state.second_late_write_done); wait++) usleep(1000);
    assert(atomic_load(&state.second_late_write_done));
    for (size_t i = 0; i < FRAME_SIZE; i++) assert(pixels[i] == 0x5a);
    buffer = (struct v4l2_buffer){
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0
    };
    assert(ioctl(fd, VIDIOC_QBUF, &buffer) == 0);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    assert(poll(&ready, 1, 5000) == 1);
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    assert(ioctl(fd, VIDIOC_DQBUF, &buffer) == 0);
    for (size_t i = 0; i < FRAME_SIZE; i++) assert(pixels[i] == 0x7c);
    assert(write(release_pipe[1], "x", 1) == 1);
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);

    assert(munmap(pixels, FRAME_SIZE) == 0);
    assert(close(fd) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(close(release_pipe[0]) == 0);
    assert(close(release_pipe[1]) == 0);
    assert(close(listener) == 0);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
    errno = 0;
    assert(open("/dev/video0", O_RDWR) == -1 && errno == ENOENT);
    puts("fauxdev video broker tests passed");
    return 0;
}
