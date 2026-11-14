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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { FRAME_SIZE = 320 * 240 * 3 };

struct broker_args { int fd; unsigned int input_count; };

static void *mock_broker(void *argument) {
    const struct broker_args *args = argument;
    int fd = args->fd;
    struct fauxdev_message request;
    while (fauxdev_message_receive(fd, &request) == 0) {
        struct fauxdev_message reply = {
            .opcode = request.opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
            .request_id = request.request_id,
        };
        if (request.opcode == FAUXDEV_OPCODE_CAMERA_INPUTS) {
            struct fauxdev_camera_inputs_reply inputs = {.count = htole32(args->input_count)};
            strcpy(inputs.inputs[0].id, "test-camera");
            if (args->input_count == 2) strcpy(inputs.inputs[1].id, "new-camera");
            reply.payload_size = sizeof(inputs.count) + sizeof(inputs.default_index) +
                                 args->input_count * sizeof(inputs.inputs[0]);
            memcpy(reply.payload, &inputs, reply.payload_size);
        } else if (request.opcode == FAUXDEV_OPCODE_CAMERA_MODES ||
                   request.opcode == FAUXDEV_OPCODE_CAMERA_GET_CONFIG) {
            reply.status = -ENOTTY;
        } else if (request.opcode == FAUXDEV_OPCODE_VIDEO_FRAME) {
            assert(request.fd_count == 1);
            unsigned char pixels[4096];
            memset(pixels, 0x42, sizeof(pixels));
            for (size_t offset = 0; offset < FRAME_SIZE; offset += sizeof(pixels)) {
                size_t count = FRAME_SIZE - offset < sizeof(pixels) ? FRAME_SIZE - offset : sizeof(pixels);
                assert(pwrite(request.fds[0], pixels, count, offset) == (ssize_t)count);
            }
        } else {
            assert(request.opcode == FAUXDEV_OPCODE_HELLO ||
                   request.opcode == FAUXDEV_OPCODE_CAMERA_START ||
                   request.opcode == FAUXDEV_OPCODE_CAMERA_STOP);
        }
        fauxdev_message_close_fds(&request);
        assert(fauxdev_message_send(fd, &reply) == 0);
    }
    close(fd);
    return NULL;
}

int main(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    char text[32];
    snprintf(text, sizeof(text), "%d", pair[1]);
    assert(setenv("FAUXDEV_VIDEO_FD", text, 1) == 0);
    pthread_t broker;
    struct broker_args first_broker = {.fd = pair[0], .input_count = 1};
    assert(pthread_create(&broker, NULL, mock_broker, &first_broker) == 0);

    int handles[100];
    for (size_t i = 0; i < 100; ++i) {
        handles[i] = open("/dev/video0", O_RDWR);
        assert(handles[i] >= 0);
    }
    assert(fcntl(pair[1], F_GETFD) & FD_CLOEXEC);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        errno = 0;
        if (open("/dev/video0", O_RDWR) != -1 || errno != EPIPE) _exit(1);
        _exit(0);
    }
    int child_status;
    assert(waitpid(child, &child_status, 0) == child &&
           WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    struct v4l2_requestbuffers req = {
        .count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    assert(ioctl(handles[0], VIDIOC_REQBUFS, &req) == 0 && req.count == 1);
    errno = 0;
    assert(ioctl(handles[1], VIDIOC_REQBUFS, &req) == -1 && errno == EBUSY);
    req.count = 0;
    assert(ioctl(handles[0], VIDIOC_REQBUFS, &req) == 0);
    req.count = 1;
    assert(ioctl(handles[1], VIDIOC_REQBUFS, &req) == 0);

    for (size_t i = 0; i < 100; ++i) {
        if (i != 0 && i != 1) assert(close(handles[i]) == 0);
    }
    struct v4l2_buffer frame = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0,
    };
    assert(ioctl(handles[1], VIDIOC_QUERYBUF, &frame) == 0);
    unsigned char *pixels = mmap(NULL, FRAME_SIZE, PROT_READ, MAP_SHARED,
                                 handles[1], frame.m.offset);
    assert(pixels != MAP_FAILED);
    assert(ioctl(handles[1], VIDIOC_QBUF, &frame) == 0);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    assert(ioctl(handles[1], VIDIOC_STREAMON, &type) == 0);
    assert(close(handles[0]) == 0);
    frame = (struct v4l2_buffer){.type = type, .memory = V4L2_MEMORY_MMAP};
    assert(ioctl(handles[1], VIDIOC_DQBUF, &frame) == 0 && frame.bytesused == FRAME_SIZE);
    assert(pixels[0] == 0x42 && pixels[FRAME_SIZE - 1] == 0x42);
    assert(ioctl(handles[1], VIDIOC_STREAMOFF, &type) == 0);
    assert(munmap(pixels, FRAME_SIZE) == 0);
    assert(close(handles[1]) == 0);

    int reopened = open("/dev/video0", O_RDWR);
    assert(reopened >= 0);
    struct v4l2_capability cap;
    assert(ioctl(reopened, VIDIOC_QUERYCAP, &cap) == 0);
    req.count = 1;
    assert(ioctl(reopened, VIDIOC_REQBUFS, &req) == 0);
    req.count = 0;
    assert(ioctl(reopened, VIDIOC_REQBUFS, &req) == 0);
    assert(close(reopened) == 0);

    int active = open("/dev/video0", O_RDWR);
    assert(active >= 0);
    struct stat first_identity, second_identity;
    assert(fstat(pair[1], &first_identity) == 0);
    int replacement[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, replacement) == 0);
    struct broker_args second_broker = {.fd = replacement[0], .input_count = 2};
    pthread_t replacement_thread;
    assert(pthread_create(&replacement_thread, NULL, mock_broker, &second_broker) == 0);
    int inherited_number = pair[1];
    close(pair[1]);
    assert(dup2(replacement[1], inherited_number) == inherited_number);
    close(replacement[1]);
    assert(fstat(inherited_number, &second_identity) == 0);
    assert(first_identity.st_dev != second_identity.st_dev ||
           first_identity.st_ino != second_identity.st_ino);
    errno = 0;
    assert(open("/dev/video0", O_RDWR) == -1 && errno == EBUSY);
    assert(close(active) == 0);
    assert(setenv("FAUXDEV_VIDEO_FD", "999999", 1) == 0);
    errno = 0;
    assert(open("/dev/video0", O_RDWR) == -1 && errno == EBADF);
    assert(setenv("FAUXDEV_VIDEO_FD", text, 1) == 0);

    reopened = open("/dev/video0", O_RDWR);
    assert(reopened >= 0);
    struct v4l2_input second_input = {.index = 1};
    assert(ioctl(reopened, VIDIOC_ENUMINPUT, &second_input) == 0);
    assert(strstr((char *)second_input.name, "new-camera"));
    assert(close(reopened) == 0);
    assert(pthread_join(broker, NULL) == 0);
    assert(shutdown(inherited_number, SHUT_RDWR) == 0);
    close(inherited_number);
    assert(pthread_join(replacement_thread, NULL) == 0);
    puts("fauxdev multi-open tests passed");
    return 0;
}
