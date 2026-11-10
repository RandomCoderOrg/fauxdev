#include "transport.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

struct fauxdev_transport {
    int socket_fd;
    int readiness_fd;
    int stop_fd;
    pthread_t monitor;
    pthread_mutex_t request_lock;
    atomic_bool disconnected;
    bool monitor_started;
    uint64_t next_request_id;
};

static void close_fds(int *fds, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (fds[index] >= 0) syscall(SYS_close, fds[index]);
    }
}

void fauxdev_message_close_fds(struct fauxdev_message *message) {
    if (!message) return;
    close_fds(message->fds, message->fd_count);
    message->fd_count = 0;
}

int fauxdev_message_send(int socket_fd, const struct fauxdev_message *message) {
    if (!message || message->payload_size > FAUXDEV_PROTOCOL_MAX_PAYLOAD ||
        message->fd_count > FAUXDEV_PROTOCOL_MAX_FDS ||
        (message->flags != FAUXDEV_MESSAGE_REQUEST &&
         message->flags != FAUXDEV_MESSAGE_RESPONSE) ||
        !message->request_id) {
        errno = EINVAL;
        return -1;
    }

    const struct fauxdev_wire_header header = {
        .magic = htole32(FAUXDEV_PROTOCOL_MAGIC),
        .version = htole16(FAUXDEV_PROTOCOL_VERSION),
        .opcode = htole16(message->opcode),
        .flags = htole32(message->flags),
        .payload_size = htole32((uint32_t)message->payload_size),
        .request_id = htole64(message->request_id),
        .status = (int32_t)htole32((uint32_t)message->status),
        .fd_count = htole32((uint32_t)message->fd_count),
    };
    struct iovec vectors[2] = {
        {.iov_base = (void *)&header, .iov_len = sizeof(header)},
        {.iov_base = (void *)message->payload, .iov_len = message->payload_size},
    };
    unsigned char control[CMSG_SPACE(sizeof(int) * FAUXDEV_PROTOCOL_MAX_FDS)] = {0};
    struct msghdr outgoing = {
        .msg_iov = vectors,
        .msg_iovlen = message->payload_size ? 2 : 1,
    };
    if (message->fd_count) {
        outgoing.msg_control = control;
        outgoing.msg_controllen = CMSG_SPACE(sizeof(int) * message->fd_count);
        struct cmsghdr *rights = CMSG_FIRSTHDR(&outgoing);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int) * message->fd_count);
        memcpy(CMSG_DATA(rights), message->fds, sizeof(int) * message->fd_count);
    }

    const size_t expected = sizeof(header) + message->payload_size;
    const ssize_t sent = sendmsg(socket_fd, &outgoing, MSG_NOSIGNAL);
    if (sent < 0) return -1;
    if ((size_t)sent != expected) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int fauxdev_message_receive(int socket_fd, struct fauxdev_message *message) {
    if (!message) {
        errno = EINVAL;
        return -1;
    }
    memset(message, 0, sizeof(*message));
    for (size_t index = 0; index < FAUXDEV_PROTOCOL_MAX_FDS; index++) message->fds[index] = -1;

    unsigned char packet[sizeof(struct fauxdev_wire_header) + FAUXDEV_PROTOCOL_MAX_PAYLOAD];
    unsigned char control[CMSG_SPACE(sizeof(int) * FAUXDEV_PROTOCOL_MAX_FDS)] = {0};
    struct iovec vector = {.iov_base = packet, .iov_len = sizeof(packet)};
    struct msghdr incoming = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    const ssize_t received = recvmsg(socket_fd, &incoming, MSG_CMSG_CLOEXEC);
    if (received == 0) {
        errno = ECONNRESET;
        return -1;
    }
    if (received < 0) return -1;

    bool invalid_control = false;
    for (struct cmsghdr *item = CMSG_FIRSTHDR(&incoming); item;
         item = CMSG_NXTHDR(&incoming, item)) {
        if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS ||
            item->cmsg_len < CMSG_LEN(0)) {
            invalid_control = true;
            continue;
        }
        const size_t bytes = item->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int)) {
            invalid_control = true;
            continue;
        }
        const size_t count = bytes / sizeof(int);
        if (message->fd_count + count > FAUXDEV_PROTOCOL_MAX_FDS) {
            invalid_control = true;
            continue;
        }
        memcpy(message->fds + message->fd_count, CMSG_DATA(item), bytes);
        message->fd_count += count;
    }
    if (invalid_control || (incoming.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        (size_t)received < sizeof(struct fauxdev_wire_header)) {
        fauxdev_message_close_fds(message);
        errno = EMSGSIZE;
        return -1;
    }

    struct fauxdev_wire_header header;
    memcpy(&header, packet, sizeof(header));
    const uint32_t payload_size = le32toh(header.payload_size);
    const uint32_t fd_count = le32toh(header.fd_count);
    if (le32toh(header.magic) != FAUXDEV_PROTOCOL_MAGIC ||
        le16toh(header.version) != FAUXDEV_PROTOCOL_VERSION ||
        payload_size > FAUXDEV_PROTOCOL_MAX_PAYLOAD ||
        (size_t)received != sizeof(header) + payload_size || fd_count != message->fd_count) {
        fauxdev_message_close_fds(message);
        errno = EPROTO;
        return -1;
    }

    message->opcode = le16toh(header.opcode);
    message->flags = le32toh(header.flags);
    message->request_id = le64toh(header.request_id);
    message->status = (int32_t)le32toh((uint32_t)header.status);
    message->payload_size = payload_size;
    if ((message->flags != FAUXDEV_MESSAGE_REQUEST &&
         message->flags != FAUXDEV_MESSAGE_RESPONSE) ||
        !message->request_id ||
        (message->flags == FAUXDEV_MESSAGE_REQUEST && message->status != 0)) {
        fauxdev_message_close_fds(message);
        errno = EPROTO;
        return -1;
    }
    memcpy(message->payload, packet + sizeof(header), payload_size);
    return 0;
}

static void mark_disconnected(struct fauxdev_transport *transport) {
    if (atomic_exchange_explicit(&transport->disconnected, true, memory_order_acq_rel)) return;
    const uint64_t token = 1;
    while (syscall(SYS_write, transport->readiness_fd, &token, sizeof(token)) < 0 &&
           errno == EINTR) {}
}

static void *monitor_connection(void *data) {
    struct fauxdev_transport *transport = data;
    struct pollfd descriptors[2] = {
        {.fd = transport->socket_fd, .events = POLLRDHUP},
        {.fd = transport->stop_fd, .events = POLLIN},
    };
    for (;;) {
        const int result = poll(descriptors, 2, -1);
        if (result < 0) {
            if (errno == EINTR) continue;
            mark_disconnected(transport);
            return NULL;
        }
        if (descriptors[1].revents & POLLIN) return NULL;
        if (descriptors[0].revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) {
            mark_disconnected(transport);
            return NULL;
        }
    }
}

int fauxdev_transport_request(
    struct fauxdev_transport *transport,
    uint16_t opcode,
    const void *payload,
    size_t payload_size,
    const int *fds,
    size_t fd_count,
    struct fauxdev_message *response
) {
    if (!transport || !response || payload_size > FAUXDEV_PROTOCOL_MAX_PAYLOAD ||
        fd_count > FAUXDEV_PROTOCOL_MAX_FDS || (payload_size && !payload) ||
        (fd_count && !fds)) {
        errno = EINVAL;
        return -1;
    }
    if (atomic_load_explicit(&transport->disconnected, memory_order_acquire)) {
        errno = EPIPE;
        return -1;
    }

    pthread_mutex_lock(&transport->request_lock);
    if (atomic_load_explicit(&transport->disconnected, memory_order_acquire)) {
        pthread_mutex_unlock(&transport->request_lock);
        errno = EPIPE;
        return -1;
    }
    struct fauxdev_message request = {
        .opcode = opcode,
        .flags = FAUXDEV_MESSAGE_REQUEST,
        .request_id = ++transport->next_request_id,
        .payload_size = payload_size,
        .fd_count = fd_count,
    };
    if (!request.request_id) request.request_id = ++transport->next_request_id;
    if (payload_size) memcpy(request.payload, payload, payload_size);
    if (fd_count) memcpy(request.fds, fds, sizeof(int) * fd_count);

    int result = fauxdev_message_send(transport->socket_fd, &request);
    if (result == 0) {
        /* Android may forbid changing options on a socket passed from another app. */
        struct pollfd ready = {.fd = transport->socket_fd, .events = POLLIN};
        do {
            result = poll(&ready, 1, opcode == FAUXDEV_OPCODE_VIDEO_FRAME ? 20000 : 2000);
        } while (result < 0 && errno == EINTR);
        if (result == 0) { errno = ETIMEDOUT; result = -1; }
        else if (result > 0) result = 0;
    }
    if (result == 0) result = fauxdev_message_receive(transport->socket_fd, response);
    if (result < 0) {
        const int saved_errno = errno;
        mark_disconnected(transport);
        pthread_mutex_unlock(&transport->request_lock);
        errno = saved_errno == ETIMEDOUT || saved_errno == EAGAIN || saved_errno == EWOULDBLOCK
                    ? ETIMEDOUT
                    : saved_errno == EPROTO || saved_errno == EMSGSIZE ? saved_errno : EPIPE;
        return -1;
    }
    if (response->flags != FAUXDEV_MESSAGE_RESPONSE || response->opcode != opcode ||
        response->request_id != request.request_id || response->status > 0) {
        fauxdev_message_close_fds(response);
        mark_disconnected(transport);
        pthread_mutex_unlock(&transport->request_lock);
        errno = EPROTO;
        return -1;
    }
    if (response->status < 0) {
        const int status = response->status;
        fauxdev_message_close_fds(response);
        pthread_mutex_unlock(&transport->request_lock);
        errno = status >= -4095 ? -status : EPROTO;
        return -1;
    }
    pthread_mutex_unlock(&transport->request_lock);
    return 0;
}

static struct fauxdev_transport *open_transport(int connected_fd, int readiness_fd) {
    if (connected_fd < 0 || readiness_fd < 0) {
        errno = EINVAL;
        return NULL;
    }
    struct fauxdev_transport *transport = calloc(1, sizeof(*transport));
    if (!transport) { syscall(SYS_close, connected_fd); return NULL; }
    transport->socket_fd = connected_fd;
    transport->readiness_fd = -1;
    transport->stop_fd = -1;
    atomic_init(&transport->disconnected, false);
    const int mutex_error = pthread_mutex_init(&transport->request_lock, NULL);
    if (mutex_error) {
        syscall(SYS_close, connected_fd);
        free(transport);
        errno = mutex_error;
        return NULL;
    }
    transport->readiness_fd = syscall(SYS_fcntl, readiness_fd, F_DUPFD_CLOEXEC, 0);
    if (transport->readiness_fd < 0) goto fail;
    transport->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (transport->stop_fd < 0) goto fail;

    struct fauxdev_message hello;
    if (fauxdev_transport_request(
            transport, FAUXDEV_OPCODE_HELLO, NULL, 0, NULL, 0, &hello
        ) < 0) {
        goto fail;
    }
    if (hello.payload_size || hello.fd_count) {
        fauxdev_message_close_fds(&hello);
        errno = EPROTO;
        goto fail;
    }
    const int thread_error = pthread_create(&transport->monitor, NULL, monitor_connection, transport);
    if (thread_error) {
        errno = thread_error;
        goto fail;
    }
    transport->monitor_started = true;
    return transport;

fail: {
        const int saved_errno = errno;
        fauxdev_transport_destroy(transport);
        errno = saved_errno;
        return NULL;
    }
}

struct fauxdev_transport *fauxdev_transport_connect(const char *socket_path, int readiness_fd) {
    if (!socket_path || !*socket_path || readiness_fd < 0) {
        errno = EINVAL;
        return NULL;
    }
    const size_t path_length = strlen(socket_path);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (path_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(address.sun_path, socket_path, path_length + 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    const socklen_t address_size = offsetof(struct sockaddr_un, sun_path) + path_length + 1;
    if (connect(fd, (const struct sockaddr *)&address, address_size) < 0) {
        int saved_errno = errno;
        syscall(SYS_close, fd);
        errno = saved_errno;
        return NULL;
    }
    return open_transport(fd, readiness_fd);
}

struct fauxdev_transport *fauxdev_transport_from_fd(int connected_fd, int readiness_fd) {
    if (connected_fd < 0 || readiness_fd < 0) {
        errno = EINVAL;
        return NULL;
    }
    int type = 0;
    socklen_t type_size = sizeof(type);
    if (getsockopt(connected_fd, SOL_SOCKET, SO_TYPE, &type, &type_size) < 0) {
        return NULL;
    }
    if (type_size != sizeof(type) || type != SOCK_SEQPACKET) {
        errno = EPROTOTYPE;
        return NULL;
    }
    int copy = syscall(SYS_fcntl, connected_fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) return NULL;
    return open_transport(copy, readiness_fd);
}

bool fauxdev_transport_disconnected(const struct fauxdev_transport *transport) {
    return !transport ||
           atomic_load_explicit(&transport->disconnected, memory_order_acquire);
}

void fauxdev_transport_cancel(struct fauxdev_transport *transport) {
    if (!transport) return;
    mark_disconnected(transport);
    if (transport->socket_fd >= 0) shutdown(transport->socket_fd, SHUT_RDWR);
}

void fauxdev_transport_destroy(struct fauxdev_transport *transport) {
    if (!transport) return;
    if (transport->monitor_started) {
        const uint64_t token = 1;
        while (syscall(SYS_write, transport->stop_fd, &token, sizeof(token)) < 0 &&
               errno == EINTR) {}
        pthread_join(transport->monitor, NULL);
    }
    if (transport->socket_fd >= 0) syscall(SYS_close, transport->socket_fd);
    if (transport->readiness_fd >= 0) syscall(SYS_close, transport->readiness_fd);
    if (transport->stop_fd >= 0) syscall(SYS_close, transport->stop_fd);
    pthread_mutex_destroy(&transport->request_lock);
    free(transport);
}
