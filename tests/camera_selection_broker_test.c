#include "transport.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

enum { FRAME_SIZE = 320 * 240 * 3 };

static void respond(int fd, const struct fauxdev_message *request, int status,
                    const void *payload, size_t size) {
    struct fauxdev_message reply = {
        .opcode = request->opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
        .request_id = request->request_id, .status = status, .payload_size = size,
    };
    if (size) memcpy(reply.payload, payload, size);
    assert(fauxdev_message_send(fd, &reply) == 0);
}

static void *mock_broker(void *argument) {
    int fd = *(int *)argument;
    int active = -1, fail_once = 1, starts = 0, stops = 0, stop_errors = 0;
    struct fauxdev_message request;
    while (fauxdev_message_receive(fd, &request) == 0) {
        switch (request.opcode) {
        case FAUXDEV_OPCODE_HELLO:
            assert(!request.payload_size && !request.fd_count);
            respond(fd, &request, 0, NULL, 0);
            break;
        case FAUXDEV_OPCODE_CAMERA_INPUTS: {
            assert(!request.payload_size && !request.fd_count);
            struct fauxdev_camera_inputs_reply list = {
                .count = htole32(3), .default_index = htole32(1),
            };
            list.inputs[0].facing = htole32(0);
            strcpy(list.inputs[0].id, "front");
            list.inputs[1].facing = htole32(1);
            strcpy(list.inputs[1].id, "rear");
            list.inputs[2].facing = htole32(UINT32_MAX);
            strcpy(list.inputs[2].id, "unknown");
            respond(fd, &request, 0, &list,
                    offsetof(struct fauxdev_camera_inputs_reply, inputs) + 3 * sizeof(list.inputs[0]));
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_START: {
            assert(request.payload_size == sizeof(struct fauxdev_camera_start_request) && !request.fd_count);
            struct fauxdev_camera_start_request start;
            memcpy(&start, request.payload, sizeof(start));
            int selected = (int)le32toh(start.index);
            assert(active < 0 && selected < 2);
            if (!selected && fail_once) {
                fail_once = 0;
                respond(fd, &request, -EIO, NULL, 0);
            } else {
                active = selected;
                starts++;
                respond(fd, &request, 0, NULL, 0);
            }
            break;
        }
        case FAUXDEV_OPCODE_VIDEO_FRAME: {
            assert(active >= 0 && request.fd_count == 1);
            unsigned char pixels[4096];
            memset(pixels, active ? 0x22 : 0x11, sizeof(pixels));
            for (size_t offset = 0; offset < FRAME_SIZE; offset += sizeof(pixels)) {
                size_t count = FRAME_SIZE - offset < sizeof(pixels) ? FRAME_SIZE - offset : sizeof(pixels);
                assert(pwrite(request.fds[0], pixels, count, (off_t)offset) == (ssize_t)count);
            }
            respond(fd, &request, 0, NULL, 0);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_STOP:
            assert(active >= 0 && !request.payload_size && !request.fd_count);
            if (starts == 3) {
                stop_errors++;
                respond(fd, &request, -EIO, NULL, 0);
            } else {
                active = -1;
                stops++;
                respond(fd, &request, 0, NULL, 0);
            }
            break;
        case FAUXDEV_OPCODE_CAMERA_MODES:
        case FAUXDEV_OPCODE_CAMERA_GET_CONFIG:
            respond(fd, &request, -ENOTTY, NULL, 0);
            break;
        default:
            assert(0 && "unexpected opcode");
        }
        fauxdev_message_close_fds(&request);
    }
    assert(active == 0 && starts == 3 && stops == 2 && stop_errors == 1);
    close(fd);
    return NULL;
}

static void *ambiguous_start_broker(void *argument) {
    int fd = *(int *)argument;
    struct fauxdev_message request;
    assert(fauxdev_message_receive(fd, &request) == 0 && request.opcode == FAUXDEV_OPCODE_HELLO);
    respond(fd, &request, 0, NULL, 0);
    assert(fauxdev_message_receive(fd, &request) == 0 && request.opcode == FAUXDEV_OPCODE_CAMERA_INPUTS);
    struct fauxdev_camera_inputs_reply list = {.count = htole32(1)};
    strcpy(list.inputs[0].id, "rear");
    respond(fd, &request, 0, &list,
            offsetof(struct fauxdev_camera_inputs_reply, inputs) + sizeof(list.inputs[0]));
    assert(fauxdev_message_receive(fd, &request) == 0 && request.opcode == FAUXDEV_OPCODE_CAMERA_MODES);
    respond(fd, &request, -ENOTTY, NULL, 0);
    assert(fauxdev_message_receive(fd, &request) == 0 && request.opcode == FAUXDEV_OPCODE_CAMERA_GET_CONFIG);
    respond(fd, &request, -ENOTTY, NULL, 0);
    assert(fauxdev_message_receive(fd, &request) == 0 && request.opcode == FAUXDEV_OPCODE_CAMERA_START);
    struct fauxdev_message bad_reply = {
        .opcode = FAUXDEV_OPCODE_CAMERA_START, .flags = FAUXDEV_MESSAGE_RESPONSE,
        .request_id = request.request_id + 1,
    };
    assert(fauxdev_message_send(fd, &bad_reply) == 0);
    struct pollfd peer = {.fd = fd, .events = POLLRDHUP};
    assert(poll(&peer, 1, 3000) == 1 && (peer.revents & (POLLRDHUP | POLLHUP)));
    close(fd);
    return NULL;
}

static void capture(int fd, unsigned char expected, int fail_start, int fail_stop) {
    struct v4l2_requestbuffers req = {
        .count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count == 1);
    int same_input = expected == 0x11 ? 0 : 1;
    errno = 0;
    assert(ioctl(fd, VIDIOC_S_INPUT, &same_input) == -1 && errno == EBUSY);
    struct v4l2_buffer frame = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0,
    };
    assert(ioctl(fd, VIDIOC_QUERYBUF, &frame) == 0);
    unsigned char *pixels = mmap(NULL, FRAME_SIZE, PROT_READ, MAP_SHARED, fd, frame.m.offset);
    assert(pixels != MAP_FAILED);
    assert(ioctl(fd, VIDIOC_QBUF, &frame) == 0);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (fail_start) {
        errno = 0;
        assert(ioctl(fd, VIDIOC_STREAMON, &type) == -1 && errno == EIO);
    }
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    assert(poll(&ready, 1, 3000) == 1);
    frame = (struct v4l2_buffer){.type = type, .memory = V4L2_MEMORY_MMAP};
    assert(ioctl(fd, VIDIOC_DQBUF, &frame) == 0 && frame.bytesused == FRAME_SIZE);
    for (size_t i = 0; i < FRAME_SIZE; ++i) assert(pixels[i] == expected);
    errno = 0;
    if (fail_stop) assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == -1 && errno == EIO);
    else assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    assert(munmap(pixels, FRAME_SIZE) == 0);
    req.count = 0;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
}

int main(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    char fd_text[32];
    snprintf(fd_text, sizeof(fd_text), "%d", pair[1]);
    assert(setenv("FAUXDEV_VIDEO_FD", fd_text, 1) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, mock_broker, &pair[0]) == 0);
    int fd = open("/dev/video0", O_RDWR);
    assert(fd >= 0);
    struct v4l2_input input = {.index = 0};
    assert(ioctl(fd, VIDIOC_ENUMINPUT, &input) == 0);
    assert(strstr((char *)input.name, "Front"));
    input.index = 1;
    assert(ioctl(fd, VIDIOC_ENUMINPUT, &input) == 0);
    assert(strstr((char *)input.name, "Rear"));
    input.index = 2;
    assert(ioctl(fd, VIDIOC_ENUMINPUT, &input) == 0);
    assert(strstr((char *)input.name, "Unknown"));
    input.index = 3;
    errno = 0;
    assert(ioctl(fd, VIDIOC_ENUMINPUT, &input) == -1 && errno == EINVAL);
    int selected = -1;
    assert(ioctl(fd, VIDIOC_G_INPUT, &selected) == 0 && selected == 1);
    struct v4l2_frmsizeenum size = {.pixel_format = V4L2_PIX_FMT_RGB24};
    assert(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0);
    assert(size.type == V4L2_FRMSIZE_TYPE_DISCRETE &&
           size.discrete.width == 320 && size.discrete.height == 240);
    size.index = 1;
    errno = 0;
    assert(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1 && errno == EINVAL);
    size.index = 0;
    size.pixel_format = V4L2_PIX_FMT_NV12;
    errno = 0;
    assert(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1 && errno == EINVAL);
    selected = 3;
    errno = 0;
    assert(ioctl(fd, VIDIOC_S_INPUT, &selected) == -1 && errno == EINVAL);
    selected = 0;
    assert(ioctl(fd, VIDIOC_S_INPUT, &selected) == 0);
    capture(fd, 0x11, 1, 0);
    selected = 1;
    assert(ioctl(fd, VIDIOC_S_INPUT, &selected) == 0);
    capture(fd, 0x22, 0, 0);
    selected = 0;
    assert(ioctl(fd, VIDIOC_S_INPUT, &selected) == 0);
    capture(fd, 0x11, 0, 1);
    errno = 0;
    assert(ioctl(fd, VIDIOC_S_INPUT, &selected) == -1 && errno == EPIPE);
    assert(close(fd) == 0);
    assert(pthread_join(thread, NULL) == 0);

    // A malformed START reply is ambiguous: the broker may have opened the
    // camera, so the guest must disconnect rather than keep a restartable FD.
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    snprintf(fd_text, sizeof(fd_text), "%d", pair[1]);
    assert(setenv("FAUXDEV_VIDEO_FD", fd_text, 1) == 0);
    assert(pthread_create(&thread, NULL, ambiguous_start_broker, &pair[0]) == 0);
    fd = open("/dev/video0", O_RDWR);
    assert(fd >= 0);
    struct v4l2_requestbuffers req = {
        .count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
    struct v4l2_buffer frame = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0,
    };
    assert(ioctl(fd, VIDIOC_QBUF, &frame) == 0);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    errno = 0;
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == -1 && errno == EPROTO);
    req.count = 0;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
    selected = 0;
    errno = 0;
    assert(ioctl(fd, VIDIOC_S_INPUT, &selected) == -1 && errno == EPIPE);
    assert(close(fd) == 0);
    assert(pthread_join(thread, NULL) == 0);
    puts("camera selection broker tests passed");
    return 0;
}
