#ifndef FAUXDEV_INTERNAL_H
#define FAUXDEV_INTERNAL_H

#include <sys/stat.h>
#include <sys/types.h>
#include <stddef.h>

struct fauxdev_backend {
    const char *path;
    mode_t mode;
    unsigned int major;
    unsigned int minor;
    int (*open)(int readiness_fd, int flags, void **data);
    int (*ioctl)(
        void *data,
        int readiness_fd,
        int status_flags,
        unsigned long request,
        void *argument
    );
    void *(*mmap)(void *data, void *addr, size_t length, int prot, int flags, off_t offset);
    void (*release)(void *data);
};

__attribute__((visibility("hidden"))) const struct fauxdev_backend *
fauxdev_find_backend(const char *path);

#endif
