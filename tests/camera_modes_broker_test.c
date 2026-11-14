#include "transport.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

enum { LARGE_W = 4032, LARGE_H = 3024, LARGE_SIZE = LARGE_W * LARGE_H * 3 };

struct broker_args { int fd; int malformed; int no_start_v2; };

static void reply(int fd, const struct fauxdev_message *request, int status,
                  const void *payload, size_t length) {
    struct fauxdev_message response = {
        .opcode = request->opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
        .request_id = request->request_id, .status = status, .payload_size = length,
    };
    if (length) memcpy(response.payload, payload, length);
    assert(fauxdev_message_send(fd, &response) == 0);
}

static void *broker(void *context) {
    const struct broker_args *args = context;
    int active = -1;
    unsigned int width = 0, height = 0;
    struct fauxdev_message request;
    while (fauxdev_message_receive(args->fd, &request) == 0) {
        switch (request.opcode) {
        case FAUXDEV_OPCODE_HELLO:
            reply(args->fd, &request, 0, NULL, 0);
            break;
        case FAUXDEV_OPCODE_CAMERA_INPUTS: {
            struct fauxdev_camera_inputs_reply list = {.count = htole32(2)};
            strcpy(list.inputs[0].id, "rear");
            strcpy(list.inputs[1].id, "front");
            reply(args->fd, &request, 0, &list,
                  offsetof(struct fauxdev_camera_inputs_reply, inputs) + 2 * sizeof(list.inputs[0]));
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_MODES: {
            struct fauxdev_camera_modes_request query;
            assert(request.payload_size == sizeof(query) && !request.fd_count);
            memcpy(&query, request.payload, sizeof(query));
            unsigned int index = le32toh(query.index), offset = le32toh(query.offset);
            assert(index < 2);
            unsigned int total = index ? 2 : 65;
            assert(offset < total);
            struct fauxdev_camera_modes_reply modes = {
                .total = htole32(args->malformed == 1 ? 0 : total),
                .offset = htole32(args->malformed == 2 ? offset + 1 : offset),
            };
            unsigned int count = total - offset;
            if (count > FAUXDEV_CAMERA_MODE_PAGE) count = FAUXDEV_CAMERA_MODE_PAGE;
            if (args->malformed == 3) count = 2;
            for (unsigned int i = 0; i < count; ++i) {
                unsigned int n = offset + i;
                unsigned int w = index ? (n ? 800 : 320) :
                                 n == 64 ? LARGE_W : n == 63 ? 320 : 100 + n;
                unsigned int h = index ? (n ? 600 : 240) :
                                 n == 64 ? LARGE_H : n == 63 ? 240 : 100;
                if (args->malformed == 3) { w = 320; h = 240; }
                modes.modes[i] = (struct fauxdev_camera_mode){htole32(w), htole32(h)};
            }
            reply(args->fd, &request, 0, &modes,
                  offsetof(struct fauxdev_camera_modes_reply, modes) +
                  count * sizeof(modes.modes[0]));
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_GET_CONFIG:
            reply(args->fd, &request, -ENOTTY, NULL, 0);
            break;
        case FAUXDEV_OPCODE_CAMERA_START_V2: {
            if (args->no_start_v2) {
                reply(args->fd, &request, -ENOTTY, NULL, 0);
                break;
            }
            struct fauxdev_camera_start_v2_request start;
            assert(request.payload_size == sizeof(start) && !request.fd_count && active < 0);
            memcpy(&start, request.payload, sizeof(start));
            active = (int)le32toh(start.index);
            width = le32toh(start.width);
            height = le32toh(start.height);
            assert((active == 0 && width == LARGE_W && height == LARGE_H) ||
                   (active == 1 && width == 800 && height == 600));
            reply(args->fd, &request, 0, NULL, 0);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_START: {
            struct fauxdev_camera_start_request start;
            assert(args->no_start_v2 && request.payload_size == sizeof(start) && !request.fd_count);
            memcpy(&start, request.payload, sizeof(start));
            active = (int)le32toh(start.index);
            width = 320;
            height = 240;
            reply(args->fd, &request, 0, NULL, 0);
            break;
        }
        case FAUXDEV_OPCODE_VIDEO_FRAME: {
            struct fauxdev_video_frame_request frame;
            assert(active >= 0 && request.payload_size == sizeof(frame) && request.fd_count == 1);
            memcpy(&frame, request.payload, sizeof(frame));
            size_t size = (size_t)width * height * 3;
            assert(le32toh(frame.width) == width && le32toh(frame.height) == height &&
                   le32toh(frame.size) == size);
            struct stat st;
            assert(fstat(request.fds[0], &st) == 0 && st.st_size >= (off_t)size);
            unsigned char pixel = active ? 0x33 : 0x55;
            assert(pwrite(request.fds[0], &pixel, 1, 0) == 1);
            assert(pwrite(request.fds[0], &pixel, 1, size - 1) == 1);
            reply(args->fd, &request, 0, NULL, 0);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_STOP:
            assert(active >= 0);
            active = -1;
            reply(args->fd, &request, 0, NULL, 0);
            break;
        default:
            assert(0 && "unexpected mode broker opcode");
        }
        fauxdev_message_close_fds(&request);
    }
    close(args->fd);
    return NULL;
}

static int open_with_broker(int malformed, int no_start_v2, pthread_t *thread, struct broker_args *args,
                            int *source_fd) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    args->fd = pair[0];
    args->malformed = malformed;
    args->no_start_v2 = no_start_v2;
    assert(pthread_create(thread, NULL, broker, args) == 0);
    char text[32];
    snprintf(text, sizeof(text), "%d", pair[1]);
    assert(setenv("FAUXDEV_VIDEO_FD", text, 1) == 0);
    *source_fd = pair[1];
    return open("/dev/video0", O_RDWR);
}

int main(void) {
    for (int malformed = 1; malformed <= 3; ++malformed) {
        pthread_t thread;
        struct broker_args args;
        int source;
        errno = 0;
        assert(open_with_broker(malformed, 0, &thread, &args, &source) == -1 && errno == EPROTO);
        close(source);
        assert(pthread_join(thread, NULL) == 0);
    }
    pthread_t thread;
    struct broker_args args;
    int source;
    int fd = open_with_broker(0, 0, &thread, &args, &source);
    assert(fd >= 0);
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    errno = 0;
    assert(ioctl(fd, VIDIOC_G_PARM, &parm) == -1 && errno == ENOTTY);
    errno = 0;
    assert(ioctl(fd, VIDIOC_S_PARM, &parm) == -1 && errno == ENOTTY);
    struct v4l2_frmivalenum interval = {
        .pixel_format = V4L2_PIX_FMT_RGB24, .width = 320, .height = 240,
    };
    errno = 0;
    assert(ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1 && errno == ENOTTY);
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    assert(ioctl(fd, VIDIOC_G_FMT, &format) == 0 && format.fmt.pix.width == 320);
    struct v4l2_frmsizeenum size = {.pixel_format = V4L2_PIX_FMT_RGB24, .index = 64};
    assert(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0 &&
           size.discrete.width == LARGE_W && size.discrete.height == LARGE_H);
    size.index = 65;
    errno = 0;
    assert(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1 && errno == EINVAL);
    format.fmt.pix.width = LARGE_W - 1;
    format.fmt.pix.height = LARGE_H;
    assert(ioctl(fd, VIDIOC_TRY_FMT, &format) == 0 && format.fmt.pix.width == LARGE_W);
    format = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    assert(ioctl(fd, VIDIOC_G_FMT, &format) == 0 && format.fmt.pix.width == 320);
    format.fmt.pix.width = LARGE_W;
    format.fmt.pix.height = LARGE_H;
    assert(ioctl(fd, VIDIOC_S_FMT, &format) == 0 && format.fmt.pix.sizeimage == LARGE_SIZE);
    struct v4l2_requestbuffers req = {
        .count = 64, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count >= 2 && req.count <= 3);
    struct v4l2_buffer frame = {.type = req.type, .memory = req.memory, .index = 0};
    assert(ioctl(fd, VIDIOC_QUERYBUF, &frame) == 0 && frame.length == LARGE_SIZE);
    unsigned char *pixels = mmap(NULL, LARGE_SIZE, PROT_READ, MAP_SHARED, fd, frame.m.offset);
    assert(pixels != MAP_FAILED);
    assert(ioctl(fd, VIDIOC_QBUF, &frame) == 0);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    frame = (struct v4l2_buffer){.type = type, .memory = V4L2_MEMORY_MMAP};
    assert(ioctl(fd, VIDIOC_DQBUF, &frame) == 0 && frame.bytesused == LARGE_SIZE);
    assert(pixels[0] == 0x55 && pixels[LARGE_SIZE - 1] == 0x55);
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    req.count = 0;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
    int selected = 1;
    assert(ioctl(fd, VIDIOC_S_INPUT, &selected) == 0);
    assert(ioctl(fd, VIDIOC_G_FMT, &format) == 0 && format.fmt.pix.width == 320);
    size = (struct v4l2_frmsizeenum){.pixel_format = V4L2_PIX_FMT_RGB24, .index = 1};
    assert(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0 && size.discrete.width == 800);
    format.fmt.pix.width = 800;
    format.fmt.pix.height = 600;
    assert(ioctl(fd, VIDIOC_S_FMT, &format) == 0 && format.fmt.pix.sizeimage == 800 * 600 * 3);
    req.count = 2;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count == 2);
    frame = (struct v4l2_buffer){.type = req.type, .memory = req.memory, .index = 0};
    assert(ioctl(fd, VIDIOC_QBUF, &frame) == 0);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    frame = (struct v4l2_buffer){.type = type, .memory = V4L2_MEMORY_MMAP};
    assert(ioctl(fd, VIDIOC_DQBUF, &frame) == 0 && frame.bytesused == 800 * 600 * 3);
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    assert(pixels[0] == 0x55 && pixels[LARGE_SIZE - 1] == 0x55);
    assert(munmap(pixels, LARGE_SIZE) == 0);
    assert(close(fd) == 0);
    assert(shutdown(source, SHUT_RDWR) == 0);
    close(source);
    assert(pthread_join(thread, NULL) == 0);

    fd = open_with_broker(0, 1, &thread, &args, &source);
    assert(fd >= 0);
    format = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    format.fmt.pix.width = LARGE_W;
    format.fmt.pix.height = LARGE_H;
    assert(ioctl(fd, VIDIOC_S_FMT, &format) == 0);
    req.count = 2;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
    errno = 0;
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == -1 && errno == ENOTTY);
    req.count = 0;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
    format.fmt.pix.width = 320;
    format.fmt.pix.height = 240;
    assert(ioctl(fd, VIDIOC_S_FMT, &format) == 0);
    req.count = 2;
    assert(ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
    frame = (struct v4l2_buffer){.type = req.type, .memory = req.memory, .index = 0};
    assert(ioctl(fd, VIDIOC_QBUF, &frame) == 0);
    assert(ioctl(fd, VIDIOC_STREAMON, &type) == 0);
    frame = (struct v4l2_buffer){.type = type, .memory = V4L2_MEMORY_MMAP};
    assert(ioctl(fd, VIDIOC_DQBUF, &frame) == 0 && frame.bytesused == 320 * 240 * 3);
    assert(ioctl(fd, VIDIOC_STREAMOFF, &type) == 0);
    assert(close(fd) == 0);
    assert(shutdown(source, SHUT_RDWR) == 0);
    close(source);
    assert(pthread_join(thread, NULL) == 0);
    puts("fauxdev camera modes tests passed");
    return 0;
}
