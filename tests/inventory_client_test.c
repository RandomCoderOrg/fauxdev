#include "transport.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum { FIRST_CHUNK = 60 * 1024, REPORT_SIZE = FIRST_CHUNK + 100 };

static void respond(int fd, const struct fauxdev_message *request,
                    uint32_t total, uint32_t offset, size_t chunk) {
    struct fauxdev_message reply = {
        .opcode = request->opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
        .request_id = request->request_id,
        .payload_size = sizeof(struct fauxdev_inventory_reply) + chunk,
    };
    struct fauxdev_inventory_reply header = {
        .total = htole32(total), .offset = htole32(offset),
    };
    memcpy(reply.payload, &header, sizeof(header));
    memset(reply.payload + sizeof(header), 'x', chunk);
    assert(fauxdev_message_send(fd, &reply) == 0);
}

static void run_case(const char *tool, int mode) {
    int sockets[2], output[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
    assert(pipe(output) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(sockets[0]);
        close(output[0]);
        assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
        close(output[1]);
        char fd_text[32];
        snprintf(fd_text, sizeof(fd_text), "%d", sockets[1]);
        assert(setenv("FAUXDEV_VIDEO_FD", fd_text, 1) == 0);
        unsetenv("FAUXDEV_VIDEO_SOCKET");
        execl(tool, tool, (char *)NULL);
        _exit(127);
    }
    close(sockets[1]);
    close(output[1]);
    struct fauxdev_message request;
    assert(fauxdev_message_receive(sockets[0], &request) == 0);
    assert(request.opcode == FAUXDEV_OPCODE_HELLO && !request.payload_size && !request.fd_count);
    struct fauxdev_message hello = {
        .opcode = FAUXDEV_OPCODE_HELLO, .flags = FAUXDEV_MESSAGE_RESPONSE,
        .request_id = request.request_id,
    };
    assert(fauxdev_message_send(sockets[0], &hello) == 0);

    assert(fauxdev_message_receive(sockets[0], &request) == 0);
    assert(request.opcode == FAUXDEV_OPCODE_CAMERA_INVENTORY);
    assert(request.payload_size == sizeof(struct fauxdev_inventory_request) && !request.fd_count);
    struct fauxdev_inventory_request query;
    memcpy(&query, request.payload, sizeof(query));
    assert(le32toh(query.offset) == 0);
    if (mode == 2) {
        struct fauxdev_message error = {
            .opcode = request.opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
            .request_id = request.request_id, .status = -ENOTTY,
        };
        assert(fauxdev_message_send(sockets[0], &error) == 0);
    } else {
        respond(sockets[0], &request, REPORT_SIZE, mode == 1 ? 1 : 0, FIRST_CHUNK);
    }
    if (mode == 0) {
        assert(fauxdev_message_receive(sockets[0], &request) == 0);
        assert(request.opcode == FAUXDEV_OPCODE_CAMERA_INVENTORY);
        memcpy(&query, request.payload, sizeof(query));
        assert(le32toh(query.offset) == FIRST_CHUNK);
        respond(sockets[0], &request, REPORT_SIZE, FIRST_CHUNK, 100);
    }
    close(sockets[0]);
    size_t received = 0;
    char buffer[4096];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < count; ++i) assert(buffer[i] == 'x');
        received += (size_t)count;
    }
    assert(count == 0);
    close(output[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == (mode ? 1 : 0));
    assert(received == (mode ? 0 : REPORT_SIZE));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    run_case(argv[1], 0);
    run_case(argv[1], 1);
    run_case(argv[1], 2);
    puts("inventory client tests passed");
    return 0;
}
