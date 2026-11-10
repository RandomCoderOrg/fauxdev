#include "transport.h"

#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum { INVENTORY_LIMIT = 128 * 1024, INVENTORY_CHUNK = 60 * 1024 };

static int read_reply(int fd, uint16_t opcode, uint64_t id, struct fauxdev_message *reply) {
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    int result;
    do result = poll(&ready, 1,
                     opcode == FAUXDEV_OPCODE_CAMERA_INVENTORY ? 10000 : 2000);
    while (result < 0 && errno == EINTR);
    if (!result) { errno = ETIMEDOUT; return -1; }
    if (result < 0 || fauxdev_message_receive(fd, reply) < 0) return -1;
    if (reply->flags != FAUXDEV_MESSAGE_RESPONSE || reply->opcode != opcode ||
        reply->request_id != id || reply->fd_count || reply->status > 0) {
        fauxdev_message_close_fds(reply);
        errno = EPROTO;
        return -1;
    }
    if (reply->status < 0) {
        errno = reply->status >= -4095 ? -reply->status : EPROTO;
        return -1;
    }
    return 0;
}

static int fetch_inventory(int fd, char **output, size_t *length) {
    struct fauxdev_message request = {
        .opcode = FAUXDEV_OPCODE_HELLO, .flags = FAUXDEV_MESSAGE_REQUEST, .request_id = 1,
    };
    struct fauxdev_message reply;
    if (fauxdev_message_send(fd, &request) < 0 ||
        read_reply(fd, request.opcode, request.request_id, &reply) < 0) return -1;
    if (reply.payload_size) { errno = EPROTO; return -1; }

    char *text = NULL;
    uint32_t total = 0, offset = 0;
    do {
        request = (struct fauxdev_message){
            .opcode = FAUXDEV_OPCODE_CAMERA_INVENTORY,
            .flags = FAUXDEV_MESSAGE_REQUEST, .request_id = 2 + offset,
            .payload_size = sizeof(struct fauxdev_inventory_request),
        };
        struct fauxdev_inventory_request query = {.offset = htole32(offset)};
        memcpy(request.payload, &query, sizeof(query));
        if (fauxdev_message_send(fd, &request) < 0 ||
            read_reply(fd, request.opcode, request.request_id, &reply) < 0) goto fail;
        if (reply.payload_size < sizeof(struct fauxdev_inventory_reply)) { errno = EPROTO; goto fail; }
        struct fauxdev_inventory_reply header;
        memcpy(&header, reply.payload, sizeof(header));
        uint32_t got_total = le32toh(header.total), got_offset = le32toh(header.offset);
        size_t chunk = reply.payload_size - sizeof(header);
        if (!got_total || got_total > INVENTORY_LIMIT || got_offset != offset ||
            got_offset >= got_total ||
            (total && got_total != total) || !chunk || chunk > INVENTORY_CHUNK ||
            chunk > got_total - got_offset) {
            errno = EPROTO;
            goto fail;
        }
        if (!text) {
            text = malloc((size_t)got_total + 1);
            if (!text) goto fail;
            total = got_total;
        }
        memcpy(text + offset, reply.payload + sizeof(header), chunk);
        offset += (uint32_t)chunk;
    } while (offset < total);
    text[total] = '\0';
    *output = text;
    *length = total;
    return 0;
fail:
    free(text);
    return -1;
}

static int open_broker(void) {
    const char *inherited = getenv("FAUXDEV_VIDEO_FD");
    if (inherited && *inherited) {
        char *end;
        errno = 0;
        long value = strtol(inherited, &end, 10);
        if (errno || *end || value < 0 || value > INT_MAX) { errno = EINVAL; return -1; }
        int type;
        socklen_t size = sizeof(type);
        if (getsockopt((int)value, SOL_SOCKET, SO_TYPE, &type, &size) < 0) return -1;
        if (size != sizeof(type) || type != SOCK_SEQPACKET) { errno = EPROTOTYPE; return -1; }
        return (int)value;
    }
    const char *path = getenv("FAUXDEV_VIDEO_SOCKET");
    if (!path || path[0] != '/') { errno = ENOENT; return -1; }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t length = strlen(path);
    if (length >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    memcpy(address.sun_path, path, length + 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address,
                offsetof(struct sockaddr_un, sun_path) + length + 1) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

int main(void) {
    int fd = open_broker();
    if (fd < 0) { perror("camera inventory broker"); return 1; }
    char *report = NULL;
    size_t length = 0;
    int result = fetch_inventory(fd, &report, &length);
    int error = errno;
    close(fd);
    if (result < 0) { errno = error; perror("camera inventory"); return 1; }
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(STDOUT_FILENO, report + written, length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { perror("camera inventory output"); free(report); return 1; }
        written += (size_t)count;
    }
    free(report);
    return 0;
}
