#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/close_range.h>
#include <linux/stat.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

struct fauxdev_session {
    const struct fauxdev_backend *backend;
    void *backend_data;
    int status_flags;
    unsigned int aliases;
    unsigned int active_operations;
    bool destroying;
    struct fauxdev_session *destroy_next;
};

struct fauxdev_binding {
    int fd;
    struct fauxdev_session *session;
    struct fauxdev_binding *next;
};

struct fauxdev_active_fd {
    int fd;
    struct fauxdev_active_fd *next;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fauxdev_binding *bindings;
static struct fauxdev_active_fd *active_fds;

static struct fauxdev_binding *find_binding_locked(int fd) {
    for (struct fauxdev_binding *entry = bindings; entry; entry = entry->next) {
        if (entry->fd == fd) return entry;
    }
    return NULL;
}

static void destroy_session(struct fauxdev_session *session) {
    if (!session) return;
    if (session->backend->release) session->backend->release(session->backend_data);
    free(session);
}

static struct fauxdev_session *maybe_destroy_locked(struct fauxdev_session *session) {
    if (!session || session->destroying || session->aliases || session->active_operations) return NULL;
    session->destroying = true;
    return session;
}

static int add_binding_locked(int fd, struct fauxdev_session *session) {
    struct fauxdev_binding *entry = malloc(sizeof(*entry));
    if (!entry) return -1;
    entry->fd = fd;
    entry->session = session;
    entry->next = bindings;
    bindings = entry;
    session->aliases++;
    return 0;
}

static struct fauxdev_session *remove_binding_locked(int fd) {
    struct fauxdev_binding **cursor = &bindings;
    while (*cursor) {
        if ((*cursor)->fd == fd) {
            struct fauxdev_binding *removed = *cursor;
            struct fauxdev_session *session = removed->session;
            *cursor = removed->next;
            free(removed);
            session->aliases--;
            return maybe_destroy_locked(session);
        }
        cursor = &(*cursor)->next;
    }
    return NULL;
}

static int normalize_path(const char *input, char output[PATH_MAX]) {
    char copy[PATH_MAX];
    if (!input || strlen(input) >= sizeof(copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(copy, input);

    char *parts[PATH_MAX / 2];
    size_t count = 0;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || *part == '\0') continue;
        if (strcmp(part, "..") == 0) {
            if (count) count--;
            continue;
        }
        parts[count++] = part;
    }

    size_t used = 0;
    output[used++] = '/';
    for (size_t index = 0; index < count; index++) {
        const size_t length = strlen(parts[index]);
        if (used + length + (index + 1 < count ? 1 : 0) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(output + used, parts[index], length);
        used += length;
        if (index + 1 < count) output[used++] = '/';
    }
    output[used] = '\0';
    return 0;
}

static int absolute_path_at(int dirfd, const char *path, char output[PATH_MAX]) {
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (*path == '/') return normalize_path(path, output);

    char base[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!getcwd(base, sizeof(base))) return -1;
    } else {
        char proc_path[64];
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", dirfd);
        const ssize_t length = syscall(SYS_readlinkat, AT_FDCWD, proc_path, base, sizeof(base) - 1);
        if (length < 0) return -1;
        base[length] = '\0';
    }

    char combined[PATH_MAX];
    const int length = snprintf(combined, sizeof(combined), "%s/%s", base, path);
    if (length < 0 || (size_t)length >= sizeof(combined)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return normalize_path(combined, output);
}

static const struct fauxdev_backend *backend_at(int dirfd, const char *path) {
    char absolute[PATH_MAX];
    return absolute_path_at(dirfd, path, absolute) == 0 ? fauxdev_find_backend(absolute) : NULL;
}

static int open_fake(const struct fauxdev_backend *backend, int flags) {
    if (flags & (O_CREAT | O_EXCL | O_TRUNC)) {
        errno = EINVAL;
        return -1;
    }

    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    if (fd < 0) return -1;

    struct fauxdev_session *session = calloc(1, sizeof(*session));
    if (!session) {
        syscall(SYS_close, fd);
        return -1;
    }
    session->backend = backend;
    session->status_flags = flags & (O_ACCMODE | O_NONBLOCK);
    if (backend->open && backend->open(fd, flags, &session->backend_data) < 0) {
        const int saved_errno = errno;
        syscall(SYS_close, fd);
        free(session);
        errno = saved_errno;
        return -1;
    }

    pthread_mutex_lock(&registry_lock);
    const int result = add_binding_locked(fd, session);
    pthread_mutex_unlock(&registry_lock);
    if (result) {
        syscall(SYS_close, fd);
        destroy_session(session);
        errno = ENOMEM;
        return -1;
    }
    return fd;
}

static int open_at_impl(int dirfd, const char *path, int flags, mode_t mode) {
    const struct fauxdev_backend *backend = backend_at(dirfd, path);
    if (backend) return open_fake(backend, flags);
    return syscall(SYS_openat, dirfd, path, flags, mode);
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, mode_t);
        va_end(arguments);
    }
    return open_at_impl(AT_FDCWD, path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, mode_t);
        va_end(arguments);
    }
    return open_at_impl(AT_FDCWD, path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, mode_t);
        va_end(arguments);
    }
    return open_at_impl(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, mode_t);
        va_end(arguments);
    }
    return open_at_impl(dirfd, path, flags, mode);
}

int __open_2(const char *path, int flags) { return open_at_impl(AT_FDCWD, path, flags, 0); }
int __open64_2(const char *path, int flags) { return open_at_impl(AT_FDCWD, path, flags, 0); }
int __openat_2(int dirfd, const char *path, int flags) { return open_at_impl(dirfd, path, flags, 0); }
int __openat64_2(int dirfd, const char *path, int flags) { return open_at_impl(dirfd, path, flags, 0); }

static void fill_stat(const struct fauxdev_backend *backend, struct stat *status) {
    memset(status, 0, sizeof(*status));
    status->st_mode = S_IFCHR | backend->mode;
    status->st_nlink = 1;
    status->st_uid = getuid();
    status->st_gid = getgid();
    status->st_rdev = makedev(backend->major, backend->minor);
    status->st_ino = 0xf0000000u + backend->minor;
    status->st_blksize = 4096;
}

static const struct fauxdev_backend *backend_for_fd_locked(int fd) {
    struct fauxdev_binding *binding = find_binding_locked(fd);
    return binding ? binding->session->backend : NULL;
}

int stat(const char *path, struct stat *status) {
    const struct fauxdev_backend *backend = backend_at(AT_FDCWD, path);
    if (backend) {
        fill_stat(backend, status);
        return 0;
    }
    return syscall(SYS_newfstatat, AT_FDCWD, path, status, 0);
}

int lstat(const char *path, struct stat *status) {
    const struct fauxdev_backend *backend = backend_at(AT_FDCWD, path);
    if (backend) {
        fill_stat(backend, status);
        return 0;
    }
    return syscall(SYS_newfstatat, AT_FDCWD, path, status, AT_SYMLINK_NOFOLLOW);
}

int fstat(int fd, struct stat *status) {
    pthread_mutex_lock(&registry_lock);
    const struct fauxdev_backend *backend = backend_for_fd_locked(fd);
    if (backend) fill_stat(backend, status);
    pthread_mutex_unlock(&registry_lock);
    return backend ? 0 : syscall(SYS_fstat, fd, status);
}

int fstatat(int dirfd, const char *path, struct stat *status, int flags) {
    const struct fauxdev_backend *backend = NULL;
    if ((flags & AT_EMPTY_PATH) && *path == '\0') {
        pthread_mutex_lock(&registry_lock);
        backend = backend_for_fd_locked(dirfd);
        if (backend) fill_stat(backend, status);
        pthread_mutex_unlock(&registry_lock);
    } else {
        backend = backend_at(dirfd, path);
        if (backend) fill_stat(backend, status);
    }
    return backend ? 0 : syscall(SYS_newfstatat, dirfd, path, status, flags);
}

static void fill_statx(const struct fauxdev_backend *backend, struct statx *status) {
    memset(status, 0, sizeof(*status));
    status->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_UID | STATX_GID |
                       STATX_INO | STATX_SIZE;
    status->stx_mode = S_IFCHR | backend->mode;
    status->stx_nlink = 1;
    status->stx_uid = getuid();
    status->stx_gid = getgid();
    status->stx_ino = 0xf0000000u + backend->minor;
    status->stx_blksize = 4096;
    status->stx_rdev_major = backend->major;
    status->stx_rdev_minor = backend->minor;
}

int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *status) {
    (void)mask;
    const struct fauxdev_backend *backend = NULL;
    if ((flags & AT_EMPTY_PATH) && *path == '\0') {
        pthread_mutex_lock(&registry_lock);
        backend = backend_for_fd_locked(dirfd);
        if (backend) fill_statx(backend, status);
        pthread_mutex_unlock(&registry_lock);
    } else {
        backend = backend_at(dirfd, path);
        if (backend) fill_statx(backend, status);
    }
    return backend ? 0 : syscall(SYS_statx, dirfd, path, flags, mask, status);
}

int __xstat(int version, const char *path, struct stat *status) {
    (void)version;
    return stat(path, status);
}

int __lxstat(int version, const char *path, struct stat *status) {
    (void)version;
    return lstat(path, status);
}

int __fxstat(int version, int fd, struct stat *status) {
    (void)version;
    return fstat(fd, status);
}

int __fxstatat(int version, int dirfd, const char *path, struct stat *status, int flags) {
    (void)version;
    return fstatat(dirfd, path, status, flags);
}

int dup(int oldfd) {
    pthread_mutex_lock(&registry_lock);
    struct fauxdev_binding *source = find_binding_locked(oldfd);
    const int result = syscall(SYS_fcntl, oldfd, source ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
    if (result >= 0 && source && add_binding_locked(result, source->session) < 0) {
        syscall(SYS_close, result);
        pthread_mutex_unlock(&registry_lock);
        errno = ENOMEM;
        return -1;
    }
    pthread_mutex_unlock(&registry_lock);
    return result;
}

static int duplicate_to(int oldfd, int newfd, int flags, bool dup2_semantics) {
    if (dup2_semantics && oldfd == newfd) {
        if (syscall(SYS_fcntl, oldfd, F_GETFD) < 0) return -1;
        return oldfd;
    }
    pthread_mutex_lock(&registry_lock);
    struct fauxdev_binding *source = find_binding_locked(oldfd);
    const int result = syscall(SYS_dup3, oldfd, newfd, source ? flags | O_CLOEXEC : flags);
    struct fauxdev_session *destroy = NULL;
    if (result >= 0) {
        destroy = remove_binding_locked(newfd);
        if (source && add_binding_locked(newfd, source->session) < 0) {
            syscall(SYS_close, newfd);
            pthread_mutex_unlock(&registry_lock);
            destroy_session(destroy);
            errno = ENOMEM;
            return -1;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    destroy_session(destroy);
    return result;
}

int dup2(int oldfd, int newfd) { return duplicate_to(oldfd, newfd, 0, true); }
int dup3(int oldfd, int newfd, int flags) { return duplicate_to(oldfd, newfd, flags, false); }

static bool fcntl_command_has_no_argument(int command) {
    switch (command) {
    case F_GETFD:
    case F_GETFL:
    case F_GETOWN:
    case F_GETSIG:
    case F_GETLEASE:
    case F_GETPIPE_SZ:
    case F_GET_SEALS:
        return true;
    default:
        return false;
    }
}

static int fcntl_impl(int fd, int command, uintptr_t argument) {
    pthread_mutex_lock(&registry_lock);
    struct fauxdev_binding *binding = find_binding_locked(fd);
    if (!binding) {
        pthread_mutex_unlock(&registry_lock);
        return syscall(SYS_fcntl, fd, command, argument);
    }

    struct fauxdev_session *session = binding->session;
    switch (command) {
    case F_GETFD:
        pthread_mutex_unlock(&registry_lock);
        return FD_CLOEXEC;
    case F_SETFD:
        pthread_mutex_unlock(&registry_lock);
        return 0;
    case F_GETFL: {
        const int flags = session->status_flags;
        pthread_mutex_unlock(&registry_lock);
        return flags;
    }
    case F_SETFL:
        session->status_flags =
            (session->status_flags & ~O_NONBLOCK) | ((int)argument & O_NONBLOCK);
        pthread_mutex_unlock(&registry_lock);
        return 0;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        const int duplicate = syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, (int)argument);
        if (duplicate < 0) {
            pthread_mutex_unlock(&registry_lock);
            return -1;
        }
        if (add_binding_locked(duplicate, session) < 0) {
            syscall(SYS_close, duplicate);
            pthread_mutex_unlock(&registry_lock);
            errno = ENOMEM;
            return -1;
        }
        pthread_mutex_unlock(&registry_lock);
        return duplicate;
    }
    default:
        pthread_mutex_unlock(&registry_lock);
        return syscall(SYS_fcntl, fd, command, argument);
    }
}

int fcntl(int fd, int command, ...) {
    uintptr_t argument = 0;
    if (!fcntl_command_has_no_argument(command)) {
        va_list arguments;
        va_start(arguments, command);
        argument = va_arg(arguments, uintptr_t);
        va_end(arguments);
    }
    return fcntl_impl(fd, command, argument);
}

int fcntl64(int fd, int command, ...) {
    uintptr_t argument = 0;
    if (!fcntl_command_has_no_argument(command)) {
        va_list arguments;
        va_start(arguments, command);
        argument = va_arg(arguments, uintptr_t);
        va_end(arguments);
    }
    return fcntl_impl(fd, command, argument);
}

static struct fauxdev_session *acquire_session(
    int fd, int *operation_fd, int *status_flags, bool *is_fake
) {
    pthread_mutex_lock(&registry_lock);
    struct fauxdev_binding *binding = find_binding_locked(fd);
    if (!binding) {
        pthread_mutex_unlock(&registry_lock);
        *is_fake = false;
        return NULL;
    }
    *is_fake = true;
    const int duplicate = syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        pthread_mutex_unlock(&registry_lock);
        return NULL;
    }
    struct fauxdev_active_fd *active = malloc(sizeof(*active));
    if (!active) {
        syscall(SYS_close, duplicate);
        pthread_mutex_unlock(&registry_lock);
        errno = ENOMEM;
        return NULL;
    }
    active->fd = duplicate;
    active->next = active_fds;
    active_fds = active;
    binding->session->active_operations++;
    *operation_fd = duplicate;
    *status_flags = binding->session->status_flags;
    struct fauxdev_session *session = binding->session;
    pthread_mutex_unlock(&registry_lock);
    return session;
}

static void release_session(struct fauxdev_session *session, int operation_fd) {
    pthread_mutex_lock(&registry_lock);
    syscall(SYS_close, operation_fd);
    struct fauxdev_active_fd **cursor = &active_fds;
    while (*cursor) {
        if ((*cursor)->fd == operation_fd) {
            struct fauxdev_active_fd *removed = *cursor;
            *cursor = removed->next;
            free(removed);
            break;
        }
        cursor = &(*cursor)->next;
    }
    session->active_operations--;
    struct fauxdev_session *destroy = maybe_destroy_locked(session);
    pthread_mutex_unlock(&registry_lock);
    destroy_session(destroy);
}

#ifdef __BIONIC__
int ioctl(int fd, int request, ...) {
#else
int ioctl(int fd, unsigned long request, ...) {
#endif
    va_list arguments;
    va_start(arguments, request);
    void *argument = va_arg(arguments, void *);
    va_end(arguments);

    int operation_fd = -1;
    int status_flags = 0;
    bool is_fake = false;
    struct fauxdev_session *session =
        acquire_session(fd, &operation_fd, &status_flags, &is_fake);
    if (!session) {
        if (is_fake) return -1;
        return syscall(SYS_ioctl, fd, (unsigned int)request, argument);
    }
    const int result =
        session->backend->ioctl(
            session->backend_data, operation_fd, status_flags, (unsigned int)request, argument
        );
    const int saved_errno = errno;
    release_session(session, operation_fd);
    errno = saved_errno;
    return result;
}

int close(int fd) {
    pthread_mutex_lock(&registry_lock);
    const bool fake = find_binding_locked(fd) != NULL;
    const int result = syscall(SYS_close, fd);
    const int saved_errno = errno;
    struct fauxdev_session *destroy = fake ? remove_binding_locked(fd) : NULL;
    pthread_mutex_unlock(&registry_lock);
    destroy_session(destroy);
    errno = saved_errno;
    return result;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    /* Scudo may map anonymously while a binding allocation holds registry_lock. */
    if (fd < 0 || (flags & MAP_ANONYMOUS))
        return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    pthread_mutex_lock(&registry_lock);
    struct fauxdev_binding *binding = find_binding_locked(fd);
    if (!binding) {
        pthread_mutex_unlock(&registry_lock);
        return (void *)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    }
    struct fauxdev_session *session = binding->session;
    if (!session->backend->mmap) {
        pthread_mutex_unlock(&registry_lock);
        errno = ENODEV;
        return MAP_FAILED;
    }
    void *result = session->backend->mmap(session->backend_data, addr, length, prot, flags, offset);
    pthread_mutex_unlock(&registry_lock);
    return result;
}

void *fauxdev_mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset)
    __asm__("mmap64");
void *fauxdev_mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset) {
    return mmap(addr, length, prot, flags, fd, offset);
}

static struct fauxdev_session *remove_closed_range_locked(unsigned int first, unsigned int last) {
    struct fauxdev_binding **cursor = &bindings;
    struct fauxdev_session *destroy_list = NULL;
    while (*cursor) {
        if ((unsigned int)(*cursor)->fd >= first && (unsigned int)(*cursor)->fd <= last) {
            struct fauxdev_binding *removed = *cursor;
            struct fauxdev_session *session = removed->session;
            *cursor = removed->next;
            free(removed);
            session->aliases--;
            struct fauxdev_session *destroy = maybe_destroy_locked(session);
            if (destroy) {
                destroy->destroy_next = destroy_list;
                destroy_list = destroy;
            }
        } else {
            cursor = &(*cursor)->next;
        }
    }
    return destroy_list;
}

int close_range(unsigned int first, unsigned int last, int flags) {
    if (first > last || (flags & ~(CLOSE_RANGE_CLOEXEC | CLOSE_RANGE_UNSHARE))) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&registry_lock);
    int result = syscall(SYS_close_range, first, last, flags);
    if (result < 0 && errno == ENOSYS && !(flags & CLOSE_RANGE_UNSHARE)) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
            const unsigned int maximum =
                limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > UINT_MAX
                    ? UINT_MAX
                    : (limit.rlim_cur ? (unsigned int)limit.rlim_cur - 1 : 0);
            const unsigned int end = last < maximum ? last : maximum;
            for (unsigned int fd = first; limit.rlim_cur && fd <= end; fd++) {
                if (flags & CLOSE_RANGE_CLOEXEC) {
                    const int descriptor_flags = syscall(SYS_fcntl, fd, F_GETFD);
                    if (descriptor_flags >= 0) {
                        syscall(SYS_fcntl, fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
                    }
                } else {
                    syscall(SYS_close, fd);
                }
                if (fd == UINT_MAX) break;
            }
            result = 0;
        }
    }
    const int saved_errno = errno;
    struct fauxdev_session *destroy_list = NULL;
    if (result == 0 && !(flags & CLOSE_RANGE_CLOEXEC)) {
        destroy_list = remove_closed_range_locked(first, last);
    }
    pthread_mutex_unlock(&registry_lock);
    while (destroy_list) {
        struct fauxdev_session *next = destroy_list->destroy_next;
        destroy_session(destroy_list);
        destroy_list = next;
    }
    errno = saved_errno;
    return result;
}

void closefrom(int lowfd) {
    if (lowfd < 0) lowfd = 0;
    close_range((unsigned int)lowfd, UINT_MAX, 0);
}

static bool is_fake_fd(int fd) {
    pthread_mutex_lock(&registry_lock);
    const bool fake = find_binding_locked(fd) != NULL;
    pthread_mutex_unlock(&registry_lock);
    return fake;
}

ssize_t read(int fd, void *buffer, size_t count) {
    if (is_fake_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_read, fd, buffer, count);
}

ssize_t write(int fd, const void *buffer, size_t count) {
    if (is_fake_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_write, fd, buffer, count);
}

ssize_t readv(int fd, const struct iovec *iov, int count) {
    if (is_fake_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_readv, fd, iov, count);
}

ssize_t writev(int fd, const struct iovec *iov, int count) {
    if (is_fake_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_writev, fd, iov, count);
}

ssize_t pread(int fd, void *buffer, size_t count, off_t offset) {
    if (is_fake_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_pread64, fd, buffer, count, offset);
}

ssize_t fauxdev_pread64(int fd, void *buffer, size_t count, off64_t offset) __asm__("pread64");
ssize_t fauxdev_pread64(int fd, void *buffer, size_t count, off64_t offset) {
    if (is_fake_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_pread64, fd, buffer, count, offset);
}

ssize_t pwrite(int fd, const void *buffer, size_t count, off_t offset) {
    if (is_fake_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_pwrite64, fd, buffer, count, offset);
}

ssize_t fauxdev_pwrite64(int fd, const void *buffer, size_t count, off64_t offset)
    __asm__("pwrite64");
ssize_t fauxdev_pwrite64(int fd, const void *buffer, size_t count, off64_t offset) {
    if (is_fake_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(SYS_pwrite64, fd, buffer, count, offset);
}

static void atfork_prepare(void) { pthread_mutex_lock(&registry_lock); }
static void atfork_parent(void) { pthread_mutex_unlock(&registry_lock); }

static void atfork_child(void) {
    for (struct fauxdev_binding *entry = bindings; entry; entry = entry->next) {
        syscall(SYS_close, entry->fd);
    }
    for (struct fauxdev_active_fd *entry = active_fds; entry; entry = entry->next) {
        syscall(SYS_close, entry->fd);
    }
    /* shortcut: copied registry metadata is reclaimed when the child exits or execs. */
    bindings = NULL;
    active_fds = NULL;
    pthread_mutex_unlock(&registry_lock);
}

__attribute__((constructor)) static void fauxdev_initialize(void) {
    pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
}
