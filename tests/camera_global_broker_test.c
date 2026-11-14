#include "transport.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

struct mock {
    int fd;
    uint64_t generation;
    uint32_t index, width, height, fps;
    int leased;
    int force_stale;
};

static void *broker(void *arg) {
    struct mock *m = arg;
    struct fauxdev_message request;
    while (fauxdev_message_receive(m->fd, &request) == 0) {
        struct fauxdev_message reply = {
            .opcode = request.opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
            .request_id = request.request_id,
        };
        switch (request.opcode) {
        case FAUXDEV_OPCODE_HELLO: break;
        case FAUXDEV_OPCODE_CAMERA_INPUTS: {
            struct fauxdev_camera_inputs_reply inputs = {.count = htole32(2)};
            strcpy(inputs.inputs[0].id, "rear");
            strcpy(inputs.inputs[1].id, "front");
            reply.payload_size = 8 + 2 * sizeof(inputs.inputs[0]);
            memcpy(reply.payload, &inputs, reply.payload_size);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_MODES: {
            struct fauxdev_camera_modes_reply modes = {
                .total = htole32(2), .offset = htole32(0),
                .modes = {{htole32(320), htole32(240)}, {htole32(640), htole32(480)}},
            };
            reply.payload_size = 8 + 2 * sizeof(modes.modes[0]);
            memcpy(reply.payload, &modes, reply.payload_size);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_GET_CONFIG:
        case FAUXDEV_OPCODE_CAMERA_SET_CONFIG: {
            if (request.opcode == FAUXDEV_OPCODE_CAMERA_SET_CONFIG) {
                struct fauxdev_camera_config wanted;
                assert(request.payload_size == sizeof(wanted));
                memcpy(&wanted, request.payload, sizeof(wanted));
                if (le64toh(wanted.generation) != m->generation) reply.status = -EAGAIN;
                else if (m->leased) reply.status = -EBUSY;
                else {
                    m->index = le32toh(wanted.index);
                    m->width = le32toh(wanted.width);
                    m->height = le32toh(wanted.height);
                    m->fps = 30;
                    ++m->generation;
                }
            }
            struct fauxdev_camera_config current = {
                .generation = htole64(m->generation), .index = htole32(m->index),
                .width = htole32(m->width),
                .height = htole32(m->height),
            };
            memcpy(reply.payload, &current, sizeof(current));
            reply.payload_size = sizeof(current);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_ENUM_RATES: {
            struct fauxdev_camera_rates_request query;
            assert(request.payload_size == sizeof(query));
            memcpy(&query, request.payload, sizeof(query));
            assert(le32toh(query.offset) == 0);
            assert(le32toh(query.index) < 2);
            assert(le32toh(query.width) == 320 || le32toh(query.width) == 640);
            struct fauxdev_camera_rates_reply rates = {
                .total = htole32(le32toh(query.width) == 640 && !le32toh(query.index) ? 3 : 2),
                .fps = {htole32(15), htole32(30), htole32(60)},
            };
            reply.payload_size = 8 + (le32toh(query.width) == 640 && !le32toh(query.index) ? 3 : 2) * sizeof(uint32_t);
            memcpy(reply.payload, &rates, reply.payload_size);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_GET_RATE:
        case FAUXDEV_OPCODE_CAMERA_SET_RATE: {
            if (request.opcode == FAUXDEV_OPCODE_CAMERA_SET_RATE) {
                struct fauxdev_camera_rate wanted;
                assert(request.payload_size == sizeof(wanted));
                memcpy(&wanted, request.payload, sizeof(wanted));
                uint32_t fps = le32toh(wanted.fps);
                if (le64toh(wanted.generation) != m->generation) reply.status = -EAGAIN;
                else if (m->leased) reply.status = -EBUSY;
                else if (fps != 15 && fps != 30 &&
                         (fps != 60 || m->width != 640 || m->index != 0))
                    reply.status = -EINVAL;
                else if (fps != m->fps) { m->fps = fps; ++m->generation; }
            }
            struct fauxdev_camera_rate current = {
                .generation = htole64(m->generation), .fps = htole32(m->fps),
            };
            memcpy(reply.payload, &current, sizeof(current));
            reply.payload_size = sizeof(current);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_ACQUIRE_QUEUE: {
            struct fauxdev_camera_queue_request wanted;
            assert(request.payload_size == sizeof(wanted));
            memcpy(&wanted, request.payload, sizeof(wanted));
            if (m->force_stale) { ++m->generation; m->force_stale = 0; }
            if (le64toh(wanted.generation) != m->generation) reply.status = -EAGAIN;
            else if (m->leased) reply.status = -EBUSY;
            else m->leased = 1;
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_RELEASE_QUEUE:
            if (!m->leased) reply.status = -EINVAL;
            else m->leased = 0;
            break;
        case FAUXDEV_OPCODE_CAMERA_GET_PRIORITY:
        case FAUXDEV_OPCODE_CAMERA_SET_PRIORITY: {
            struct fauxdev_camera_priority priority = {.value = htole32(V4L2_PRIORITY_DEFAULT)};
            memcpy(reply.payload, &priority, sizeof(priority));
            reply.payload_size = sizeof(priority);
            break;
        }
        case FAUXDEV_OPCODE_CAMERA_STOP: break;
        default: assert(0 && "unexpected camera opcode");
        }
        fauxdev_message_close_fds(&request);
        assert(fauxdev_message_send(m->fd, &reply) == 0);
    }
    close(m->fd);
    return NULL;
}

static struct v4l2_format format(uint32_t width, uint32_t height) {
    struct v4l2_format value = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    value.fmt.pix.width = width;
    value.fmt.pix.height = height;
    value.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    return value;
}

static void *empty_broker(void *arg) {
    int fd = *(int *)arg;
    struct fauxdev_message request;
    while (fauxdev_message_receive(fd, &request) == 0) {
        struct fauxdev_message reply = {
            .opcode = request.opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
            .request_id = request.request_id,
        };
        if (request.opcode == FAUXDEV_OPCODE_CAMERA_INPUTS) {
            struct fauxdev_camera_inputs_reply empty = {0};
            reply.payload_size = 8;
            memcpy(reply.payload, &empty, reply.payload_size);
        } else assert(request.opcode == FAUXDEV_OPCODE_HELLO);
        assert(fauxdev_message_send(fd, &reply) == 0);
    }
    close(fd);
    return NULL;
}

int main(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    char number[32];
    snprintf(number, sizeof(number), "%d", pair[1]);
    assert(setenv("FAUXDEV_VIDEO_FD", number, 1) == 0);
    struct mock mock = {.fd = pair[0], .generation = 1, .width = 320,
                        .height = 240, .fps = 30};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, broker, &mock) == 0);
    int first = open("/dev/video0", O_RDWR), second = open("/dev/video0", O_RDWR);
    assert(first >= 0 && second >= 0);

    struct v4l2_format f = format(640, 480);
    assert(ioctl(second, VIDIOC_S_FMT, &f) == 0);
    struct v4l2_frmivalenum interval = {
        .pixel_format = V4L2_PIX_FMT_RGB24, .width = 640, .height = 480, .index = 2,
    };
    assert(ioctl(first, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0 &&
           interval.discrete.numerator == 1 && interval.discrete.denominator == 60);
    interval.index = 3;
    errno = 0;
    assert(ioctl(first, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1 && errno == EINVAL);
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    assert(ioctl(first, VIDIOC_G_PARM, &parm) == 0 &&
           parm.parm.capture.timeperframe.denominator == 30);
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 60;
    assert(ioctl(first, VIDIOC_S_PARM, &parm) == 0 &&
           parm.parm.capture.timeperframe.denominator == 60);
    assert(ioctl(second, VIDIOC_G_PARM, &parm) == 0 &&
           parm.parm.capture.timeperframe.denominator == 60);
    f = format(0, 0);
    assert(ioctl(first, VIDIOC_G_FMT, &f) == 0 && f.fmt.pix.width == 640);
    struct v4l2_requestbuffers req = {
        .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    assert(ioctl(first, VIDIOC_REQBUFS, &req) == 0 && req.count == 2);
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 15;
    errno = 0;
    assert(ioctl(second, VIDIOC_S_PARM, &parm) == -1 && errno == EBUSY);
    f = format(320, 240);
    errno = 0;
    assert(ioctl(second, VIDIOC_S_FMT, &f) == -1 && errno == EBUSY);
    req.count = 0;
    assert(ioctl(first, VIDIOC_REQBUFS, &req) == 0);
    assert(ioctl(second, VIDIOC_S_FMT, &f) == 0);
    assert(ioctl(first, VIDIOC_G_PARM, &parm) == 0 &&
           parm.parm.capture.timeperframe.denominator == 30);
    const struct v4l2_fract resets[] = {{0, 0}, {0, 1}, {1, 0}, {1, 24}};
    for (unsigned int i = 0; i < sizeof(resets) / sizeof(resets[0]); ++i) {
        parm.parm.capture.timeperframe = resets[i];
        assert(ioctl(first, VIDIOC_S_PARM, &parm) == 0 &&
               parm.parm.capture.timeperframe.denominator == 30);
    }
    parm.parm.capture.timeperframe = (struct v4l2_fract){1, 15};
    assert(ioctl(first, VIDIOC_S_PARM, &parm) == 0);
    int selected = 1;
    assert(ioctl(first, VIDIOC_S_INPUT, &selected) == 0);
    assert(ioctl(second, VIDIOC_G_INPUT, &selected) == 0 && selected == 1);
    assert(ioctl(second, VIDIOC_G_PARM, &parm) == 0 &&
           parm.parm.capture.timeperframe.denominator == 30);
    selected = 0;
    assert(ioctl(second, VIDIOC_S_INPUT, &selected) == 0);
    interval.width = 320;
    interval.height = 240;
    interval.index = 2;
    errno = 0;
    assert(ioctl(first, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1 && errno == EINVAL);
    f = format(0, 0);
    assert(ioctl(first, VIDIOC_G_FMT, &f) == 0 && f.fmt.pix.width == 320);

    mock.force_stale = 1;
    req.count = 1;
    errno = 0;
    assert(ioctl(first, VIDIOC_REQBUFS, &req) == -1 && errno == EAGAIN);
    assert(ioctl(first, VIDIOC_REQBUFS, &req) == 0);
    assert(close(first) == 0);
    req.count = 1;
    assert(ioctl(second, VIDIOC_REQBUFS, &req) == 0);
    req.count = 0;
    assert(ioctl(second, VIDIOC_REQBUFS, &req) == 0);
    assert(close(second) == 0);
    assert(shutdown(pair[1], SHUT_RDWR) == 0);
    close(pair[1]);
    assert(pthread_join(thread, NULL) == 0);
    int empty[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, empty) == 0);
    snprintf(number, sizeof(number), "%d", empty[1]);
    assert(setenv("FAUXDEV_VIDEO_FD", number, 1) == 0);
    assert(pthread_create(&thread, NULL, empty_broker, &empty[0]) == 0);
    int no_camera = open("/dev/video0", O_RDWR);
    assert(no_camera >= 0);
    struct v4l2_input input = {.index = 0};
    errno = 0;
    assert(ioctl(no_camera, VIDIOC_ENUMINPUT, &input) == -1 && errno == EINVAL);
    req.count = 2;
    errno = 0;
    assert(ioctl(no_camera, VIDIOC_REQBUFS, &req) == -1 && errno == ENODEV);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    errno = 0;
    assert(ioctl(no_camera, VIDIOC_STREAMON, &type) == -1 && errno == ENODEV);
    assert(close(no_camera) == 0);
    assert(shutdown(empty[1], SHUT_RDWR) == 0);
    close(empty[1]);
    assert(pthread_join(thread, NULL) == 0);
    puts("fauxdev global camera config tests passed");
    return 0;
}
