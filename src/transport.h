#ifndef FAUXDEV_TRANSPORT_H
#define FAUXDEV_TRANSPORT_H

#include <fauxdev/protocol.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fauxdev_message {
    uint16_t opcode;
    uint32_t flags;
    uint64_t request_id;
    int32_t status;
    size_t payload_size;
    unsigned char payload[FAUXDEV_PROTOCOL_MAX_PAYLOAD];
    size_t fd_count;
    int fds[FAUXDEV_PROTOCOL_MAX_FDS];
};

#define FAUXDEV_INTERNAL __attribute__((visibility("hidden")))

FAUXDEV_INTERNAL int fauxdev_message_send(
    int socket_fd, const struct fauxdev_message *message
);
FAUXDEV_INTERNAL int fauxdev_message_receive(int socket_fd, struct fauxdev_message *message);
FAUXDEV_INTERNAL void fauxdev_message_close_fds(struct fauxdev_message *message);

struct fauxdev_transport;

FAUXDEV_INTERNAL struct fauxdev_transport *
fauxdev_transport_connect(const char *socket_path, int readiness_fd);
/* Borrows connected_fd; the transport duplicates it and owns only the duplicate. */
FAUXDEV_INTERNAL struct fauxdev_transport *
fauxdev_transport_from_fd(int connected_fd, int readiness_fd);
FAUXDEV_INTERNAL int fauxdev_transport_request(
    struct fauxdev_transport *transport,
    uint16_t opcode,
    const void *payload,
    size_t payload_size,
    const int *fds,
    size_t fd_count,
    struct fauxdev_message *response
);
FAUXDEV_INTERNAL bool fauxdev_transport_disconnected(
    const struct fauxdev_transport *transport
);
FAUXDEV_INTERNAL void fauxdev_transport_cancel(struct fauxdev_transport *transport);
FAUXDEV_INTERNAL void fauxdev_transport_destroy(struct fauxdev_transport *transport);

#endif
