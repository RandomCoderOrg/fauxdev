#include "internal.h"

#include <errno.h>
#include <fauxdev/probe.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

__attribute__((visibility("hidden"))) extern const struct fauxdev_backend fauxdev_video_backend;

struct probe_session {
    uint32_t version;
};

static int probe_open(int readiness_fd, int flags, void **data) {
    (void)readiness_fd;
    (void)flags;
    struct probe_session *session = malloc(sizeof(*session));
    if (!session) return -1;
    session->version = FAUXDEV_PROBE_VERSION;
    *data = session;
    return 0;
}

static int probe_ioctl(
    void *data, int fd, int status_flags, unsigned long request, void *argument
) {
    (void)status_flags;
    struct probe_session *session = data;
    switch (request) {
    case FAUXDEV_PROBE_GET_VERSION:
        if (!argument) {
            errno = EFAULT;
            return -1;
        }
        *(uint32_t *)argument = session->version;
        return 0;
    case FAUXDEV_PROBE_SIGNAL: {
        if (!argument) {
            errno = EFAULT;
            return -1;
        }
        const uint64_t count = *(const uint64_t *)argument;
        if (count == 0 || count == UINT64_MAX) {
            errno = EINVAL;
            return -1;
        }
        return syscall(SYS_write, fd, &count, sizeof(count)) == sizeof(count) ? 0 : -1;
    }
    case FAUXDEV_PROBE_CONSUME:
        if (!argument) {
            errno = EFAULT;
            return -1;
        }
        return syscall(SYS_read, fd, argument, sizeof(uint64_t)) == sizeof(uint64_t) ? 0 : -1;
    default:
        errno = ENOTTY;
        return -1;
    }
}

static void probe_release(void *data) { free(data); }

static const struct fauxdev_backend probe_backend = {
    .path = FAUXDEV_PROBE_PATH,
    .mode = 0660,
    .major = 240,
    .minor = 0,
    .open = probe_open,
    .ioctl = probe_ioctl,
    .release = probe_release,
};

const struct fauxdev_backend *fauxdev_find_backend(const char *path) {
    if (!path) return NULL;
    if (strcmp(path, probe_backend.path) == 0) return &probe_backend;
    return strcmp(path, fauxdev_video_backend.path) == 0 ? &fauxdev_video_backend : NULL;
}
