#include "transport.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define PROBE_MEMFD_OPCODE 0x7fffu
#define PROBE_ERROR_OPCODE 0x7ffeu
#define PROBE_FD_OPCODE 0x7ffdu

struct broker_fixture {
    int listener;
    int reply_to_hello;
};

static void send_response(
    int socket_fd,
    const struct fauxdev_message *request,
    const void *payload,
    size_t payload_size,
    const int *fds,
    size_t fd_count,
    int status
) {
    struct fauxdev_message response = {
        .opcode = request->opcode,
        .flags = FAUXDEV_MESSAGE_RESPONSE,
        .request_id = request->request_id,
        .status = status,
        .payload_size = payload_size,
        .fd_count = fd_count,
    };
    if (payload_size) memcpy(response.payload, payload, payload_size);
    if (fd_count) memcpy(response.fds, fds, sizeof(int) * fd_count);
    assert(fauxdev_message_send(socket_fd, &response) == 0);
}

static void *run_broker(void *data) {
    struct broker_fixture *fixture = data;
    int socket_fd = accept4(fixture->listener, NULL, NULL, SOCK_CLOEXEC);
    assert(socket_fd >= 0);

    struct fauxdev_message request;
    assert(fauxdev_message_receive(socket_fd, &request) == 0);
    assert(request.opcode == FAUXDEV_OPCODE_HELLO);
    assert(request.flags == FAUXDEV_MESSAGE_REQUEST);
    assert(request.payload_size == 0 && request.fd_count == 0);
    send_response(socket_fd, &request, NULL, 0, NULL, 0, 0);

    assert(fauxdev_message_receive(socket_fd, &request) == 0);
    assert(request.opcode == PROBE_FD_OPCODE);
    static const char outbound_label[] = "outbound-fd";
    assert(request.payload_size == sizeof(outbound_label));
    assert(memcmp(request.payload, outbound_label, sizeof(outbound_label)) == 0);
    assert(request.fd_count == 1);
    assert(fcntl(request.fds[0], F_GETFD) & FD_CLOEXEC);
    const uint64_t token = 1;
    assert(write(request.fds[0], &token, sizeof(token)) == sizeof(token));
    fauxdev_message_close_fds(&request);
    send_response(socket_fd, &request, NULL, 0, NULL, 0, 0);

    assert(fauxdev_message_receive(socket_fd, &request) == 0);
    assert(request.opcode == PROBE_ERROR_OPCODE);
    send_response(socket_fd, &request, NULL, 0, NULL, 0, -EBUSY);

    assert(fauxdev_message_receive(socket_fd, &request) == 0);
    assert(request.opcode == PROBE_MEMFD_OPCODE);
    assert(request.flags == FAUXDEV_MESSAGE_REQUEST);
    assert(request.payload_size == 0 && request.fd_count == 0);

    static const char contents[] = "fauxdev sealed transport buffer";
    int buffer_fd = memfd_create("fauxdev-probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(buffer_fd >= 0);
    assert(ftruncate(buffer_fd, sizeof(contents)) == 0);
    assert(pwrite(buffer_fd, contents, sizeof(contents), 0) == sizeof(contents));
    const int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    assert(fcntl(buffer_fd, F_ADD_SEALS, seals) == 0);
    static const char label[] = "probe-buffer";
    send_response(socket_fd, &request, label, sizeof(label), &buffer_fd, 1, 0);
    close(buffer_fd);
    close(socket_fd);
    return NULL;
}

static int create_listener(const char *path) {
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    assert(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    const socklen_t size = offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
    if (bind(listener, (const struct sockaddr *)&address, size) < 0) {
        perror("transport test socket bind");
        abort();
    }
    assert(chmod(path, 0600) == 0);
    assert(listen(listener, 1) == 0);
    return listener;
}

static void test_malformed_packet(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    const struct fauxdev_wire_header header = {
        .magic = htole32(FAUXDEV_PROTOCOL_MAGIC),
        .version = htole16(FAUXDEV_PROTOCOL_VERSION),
        .opcode = htole16(FAUXDEV_OPCODE_HELLO),
        .flags = htole32(FAUXDEV_MESSAGE_REQUEST),
        .request_id = htole64(1),
        .fd_count = htole32(1),
    };
    assert(send(sockets[0], &header, sizeof(header), MSG_NOSIGNAL) == sizeof(header));
    struct fauxdev_message message;
    errno = 0;
    assert(fauxdev_message_receive(sockets[1], &message) == -1);
    assert(errno == EPROTO);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_transport(int inherited) {
    char directory[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    int length = snprintf(directory, sizeof(directory), "%s/fauxdev-transport-XXXXXX", tmp);
    assert(length > 0 && (size_t)length < sizeof(directory));
    assert(mkdtemp(directory));
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    assert(snprintf(socket_path, sizeof(socket_path), "%s/broker.sock", directory) > 0);

    struct broker_fixture fixture = {.listener = create_listener(socket_path)};
    pthread_t broker;
    assert(pthread_create(&broker, NULL, run_broker, &fixture) == 0);

    int readiness_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    assert(readiness_fd >= 0);
    struct fauxdev_transport *transport;
    if (inherited) {
        int connected = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        assert(connected >= 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        strcpy(address.sun_path, socket_path);
        assert(connect(connected, (const struct sockaddr *)&address,
                offsetof(struct sockaddr_un, sun_path) + strlen(socket_path) + 1) == 0);
        transport = fauxdev_transport_from_fd(connected, readiness_fd);
        assert(fcntl(connected, F_GETFD) >= 0);
        close(connected);
    } else {
        transport = fauxdev_transport_connect(socket_path, readiness_fd);
    }
    assert(transport);

    struct fauxdev_message response;
    int outbound_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    assert(outbound_fd >= 0);
    static const char outbound_label[] = "outbound-fd";
    assert(fauxdev_transport_request(
               transport,
               PROBE_FD_OPCODE,
               outbound_label,
               sizeof(outbound_label),
               &outbound_fd,
               1,
               &response
           ) == 0);
    assert(response.payload_size == 0 && response.fd_count == 0);
    struct pollfd outbound = {.fd = outbound_fd, .events = POLLIN};
    assert(poll(&outbound, 1, 0) == 1);
    uint64_t token = 0;
    assert(read(outbound_fd, &token, sizeof(token)) == sizeof(token));
    assert(token == 1);
    close(outbound_fd);

    errno = 0;
    assert(fauxdev_transport_request(
               transport, PROBE_ERROR_OPCODE, NULL, 0, NULL, 0, &response
           ) == -1);
    assert(errno == EBUSY);
    assert(!fauxdev_transport_disconnected(transport));

    assert(fauxdev_transport_request(
               transport, PROBE_MEMFD_OPCODE, NULL, 0, NULL, 0, &response
           ) == 0);
    static const char label[] = "probe-buffer";
    assert(response.payload_size == sizeof(label));
    assert(memcmp(response.payload, label, sizeof(label)) == 0);
    assert(response.fd_count == 1);
    assert(fcntl(response.fds[0], F_GETFD) & FD_CLOEXEC);

    struct stat status;
    assert(fstat(response.fds[0], &status) == 0);
    static const char contents[] = "fauxdev sealed transport buffer";
    assert(status.st_size == sizeof(contents));
    const int expected_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    assert((fcntl(response.fds[0], F_GET_SEALS) & expected_seals) == expected_seals);
    const char *mapping = mmap(NULL, status.st_size, PROT_READ, MAP_SHARED, response.fds[0], 0);
    assert(mapping != MAP_FAILED);
    assert(memcmp(mapping, contents, sizeof(contents)) == 0);
    assert(munmap((void *)mapping, status.st_size) == 0);
    fauxdev_message_close_fds(&response);

    struct pollfd readiness = {.fd = readiness_fd, .events = POLLIN};
    assert(poll(&readiness, 1, 2000) == 1);
    assert(readiness.revents & POLLIN);
    assert(fauxdev_transport_disconnected(transport));
    errno = 0;
    assert(fauxdev_transport_request(
               transport, PROBE_MEMFD_OPCODE, NULL, 0, NULL, 0, &response
           ) == -1);
    assert(errno == EPIPE);
    readiness.revents = 0;
    assert(poll(&readiness, 1, 0) == 1);

    fauxdev_transport_destroy(transport);
    close(readiness_fd);
    assert(pthread_join(broker, NULL) == 0);
    close(fixture.listener);
    assert(unlink(socket_path) == 0);
    assert(rmdir(directory) == 0);
}

static void *run_silent_broker(void *data) {
    struct broker_fixture *fixture = data;
    int socket_fd = accept4(fixture->listener, NULL, NULL, SOCK_CLOEXEC);
    assert(socket_fd >= 0);
    struct fauxdev_message request;
    assert(fauxdev_message_receive(socket_fd, &request) == 0);
    assert(request.opcode == FAUXDEV_OPCODE_HELLO);
    if (fixture->reply_to_hello) {
        send_response(socket_fd, &request, NULL, 0, NULL, 0, 0);
        assert(fauxdev_message_receive(socket_fd, &request) == 0);
        assert(request.opcode == PROBE_MEMFD_OPCODE);
    }
    struct pollfd descriptor = {.fd = socket_fd, .events = POLLIN};
    assert(poll(&descriptor, 1, 5000) == 1);
    char byte;
    assert(recv(socket_fd, &byte, 1, 0) == 0);
    close(socket_fd);
    return NULL;
}

static void test_silent_broker_timeout(int reply_to_hello) {
    char directory[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    int length = snprintf(directory, sizeof(directory), "%s/fauxdev-timeout-XXXXXX", tmp);
    assert(length > 0 && (size_t)length < sizeof(directory));
    assert(mkdtemp(directory));
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    assert(snprintf(socket_path, sizeof(socket_path), "%s/broker.sock", directory) > 0);
    struct broker_fixture fixture = {
        .listener = create_listener(socket_path), .reply_to_hello = reply_to_hello
    };
    pthread_t broker;
    assert(pthread_create(&broker, NULL, run_silent_broker, &fixture) == 0);
    int readiness_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    assert(readiness_fd >= 0);
    if (reply_to_hello) {
        struct fauxdev_transport *transport =
            fauxdev_transport_connect(socket_path, readiness_fd);
        assert(transport);
        struct fauxdev_message response;
        errno = 0;
        assert(fauxdev_transport_request(
                   transport, PROBE_MEMFD_OPCODE, NULL, 0, NULL, 0, &response
               ) == -1);
        assert(errno == ETIMEDOUT);
        assert(fauxdev_transport_disconnected(transport));
        fauxdev_transport_destroy(transport);
    } else {
        errno = 0;
        assert(fauxdev_transport_connect(socket_path, readiness_fd) == NULL);
        assert(errno == ETIMEDOUT);
    }
    struct pollfd readiness = {.fd = readiness_fd, .events = POLLIN};
    assert(poll(&readiness, 1, 0) == 1);
    assert(readiness.revents & POLLIN);
    assert(pthread_join(broker, NULL) == 0);
    close(readiness_fd);
    close(fixture.listener);
    assert(unlink(socket_path) == 0);
    assert(rmdir(directory) == 0);
}

int main(void) {
    test_malformed_packet();
    test_transport(0);
    test_transport(1);
    test_silent_broker_timeout(0);
    test_silent_broker_timeout(1);
    puts("fauxdev transport tests passed");
    return 0;
}
