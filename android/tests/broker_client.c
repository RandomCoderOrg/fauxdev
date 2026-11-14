#include "transport.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct fauxdev_message exchange(int fd, uint16_t opcode, const void *payload, size_t size) {
    static uint64_t request_id;
    struct fauxdev_message request = {
        .opcode = opcode, .flags = FAUXDEV_MESSAGE_REQUEST,
        .request_id = ++request_id, .payload_size = size,
    };
    if (size) memcpy(request.payload, payload, size);
    assert(fauxdev_message_send(fd, &request) == 0);
    struct fauxdev_message response;
    assert(fauxdev_message_receive(fd, &response) == 0);
    assert(response.opcode == opcode && response.request_id == request.request_id);
    assert(response.flags == FAUXDEV_MESSAGE_RESPONSE && response.fd_count == 0);
    return response;
}

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    if (!strcmp(argv[1], "idle") || !strcmp(argv[1], "active")) assert(argc == 3);
    const char *raw_fd = getenv("FAUXDEV_VIDEO_FD");
    assert(raw_fd);
    int fd = atoi(raw_fd);
    assert(fd >= 0);
    assert(exchange(fd, FAUXDEV_OPCODE_HELLO, NULL, 0).status == 0);
    struct fauxdev_message inputs = exchange(fd, FAUXDEV_OPCODE_CAMERA_INPUTS, NULL, 0);
    assert(inputs.status == 0 && inputs.payload_size >=
           offsetof(struct fauxdev_camera_inputs_reply, inputs));
    struct fauxdev_camera_inputs_reply *list = (void *)inputs.payload;
    assert(le32toh(list->count) > 0);
    if (!strcmp(argv[1], "query")) {
        puts("QUERY_OK");
        return 0;
    }
    if (!strcmp(argv[1], "idle")) {
        puts("IDLE");
        fflush(stdout);
        usleep((useconds_t)atoi(argv[2]) * 1000);
        return 0;
    }
    struct fauxdev_camera_start_request start = {
        .index = htole32(le32toh(list->default_index)),
    };
    struct fauxdev_message reply = exchange(fd, FAUXDEV_OPCODE_CAMERA_START, &start, sizeof(start));
    if (!strcmp(argv[1], "busy")) {
        assert(reply.status == -EBUSY);
        puts("BUSY_OK");
        return 0;
    }
    assert(reply.status == 0);
    puts("ACTIVE");
    fflush(stdout);
    if (!strcmp(argv[1], "abrupt")) return 0;
    if (!strcmp(argv[1], "wait-stop")) {
        struct fauxdev_message closed;
        assert(fauxdev_message_receive(fd, &closed) == -1);
        puts("DISCONNECTED");
        return 0;
    }
    assert(!strcmp(argv[1], "active"));
    usleep((useconds_t)atoi(argv[2]) * 1000);
    assert(exchange(fd, FAUXDEV_OPCODE_CAMERA_STOP, NULL, 0).status == 0);
    puts("STOPPED");
    return 0;
}
