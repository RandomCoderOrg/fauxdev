#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fauxdev/probe.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <dlfcn.h>
/* Bionic headers omit these functions, which fauxdev provides for this test. */
int close_range(unsigned int first, unsigned int last, int flags);
#endif

static void require_probe_metadata(int fd) {
    struct stat status;
    assert(fstat(fd, &status) == 0);
    assert(S_ISCHR(status.st_mode));
    assert(major(status.st_rdev) == 240);
    assert(minor(status.st_rdev) == 0);
}

static void require_not_ready(int fd) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    assert(poll(&descriptor, 1, 0) == 0);
}

static void signal_probe(int fd, uint64_t count) {
    assert(ioctl(fd, FAUXDEV_PROBE_SIGNAL, &count) == 0);
}

static void consume_probe(int fd) {
    uint64_t count = 0;
    assert(ioctl(fd, FAUXDEV_PROBE_CONSUME, &count) == 0);
    assert(count == 1);
}

static void test_passthrough(void) {
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    struct stat status;
    assert(fstat(fd, &status) == 0);
    assert(S_ISCHR(status.st_mode));
    assert(major(status.st_rdev) == 1);
    assert(write(fd, "x", 1) == 1);

    int duplicate = dup(fd);
    assert(duplicate >= 0);
    assert((fcntl(duplicate, F_GETFD) & FD_CLOEXEC) == 0);
    assert(close(duplicate) == 0);

    errno = ENOMEM;
    assert(ioctl(fd, FIOCLEX) == 0);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    assert(close(fd) == 0);
}

static void test_metadata_and_ioctl(void) {
    struct stat status;
    assert(stat(FAUXDEV_PROBE_PATH, &status) == 0);
    assert(S_ISCHR(status.st_mode));
    assert(major(status.st_rdev) == 240);

    int fd = open(FAUXDEV_PROBE_PATH, O_RDWR | O_NONBLOCK);
    assert(fd >= 0);
    require_probe_metadata(fd);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    assert(fcntl(fd, F_GETFL) & O_NONBLOCK);

    uint32_t version = 0;
    assert(ioctl(fd, FAUXDEV_PROBE_GET_VERSION, &version) == 0);
    assert(version == FAUXDEV_PROBE_VERSION);

    errno = 0;
    assert(read(fd, &version, sizeof(version)) == -1);
    assert(errno == EINVAL);
    errno = 0;
    assert(ioctl(fd, _IO('F', 0x7f), NULL) == -1);
    assert(errno == ENOTTY);
    assert(close(fd) == 0);
}

static void test_readiness(void) {
    int fd = open(FAUXDEV_PROBE_PATH, O_RDWR | O_NONBLOCK);
    assert(fd >= 0);
    require_not_ready(fd);

    signal_probe(fd, 2);
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    assert(poll(&descriptor, 1, 0) == 1);
    assert(descriptor.revents & POLLIN);
    consume_probe(fd);
    assert(poll(&descriptor, 1, 0) == 1);
    consume_probe(fd);
    require_not_ready(fd);

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    assert(epoll_fd >= 0);
    struct epoll_event interest = {.events = EPOLLIN, .data.fd = fd};
    assert(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &interest) == 0);
    signal_probe(fd, 1);
    struct epoll_event event;
    assert(epoll_wait(epoll_fd, &event, 1, 0) == 1);
    assert(event.data.fd == fd);
    consume_probe(fd);
    assert(close(epoll_fd) == 0);

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval timeout = {0};
    signal_probe(fd, 1);
    assert(select(fd + 1, &read_fds, NULL, NULL, &timeout) == 1);
    assert(FD_ISSET(fd, &read_fds));
    consume_probe(fd);
    assert(close(fd) == 0);
}

static void test_duplication(void) {
    int fd = open(FAUXDEV_PROBE_PATH, O_RDWR);
    assert(fd >= 0);
    int duplicate = dup(fd);
    assert(duplicate >= 0);
    require_probe_metadata(duplicate);
    assert(fcntl(duplicate, F_GETFD) & FD_CLOEXEC);

    signal_probe(fd, 1);
    struct pollfd descriptor = {.fd = duplicate, .events = POLLIN};
    assert(poll(&descriptor, 1, 0) == 1);
    consume_probe(duplicate);
    assert(close(fd) == 0);

    uint32_t version = 0;
    assert(ioctl(duplicate, FAUXDEV_PROBE_GET_VERSION, &version) == 0);
    assert(version == FAUXDEV_PROBE_VERSION);

    int fcntl_duplicate = fcntl(duplicate, F_DUPFD, 32);
    assert(fcntl_duplicate >= 32);
    require_probe_metadata(fcntl_duplicate);
    assert(close(duplicate) == 0);
    assert(close(fcntl_duplicate) == 0);
}

static void test_duplication_replacement(void) {
    int source = open(FAUXDEV_PROBE_PATH, O_RDWR);
    int target = open(FAUXDEV_PROBE_PATH, O_RDWR);
    assert(source >= 0 && target >= 0);
    assert(dup2(source, target) == target);
    signal_probe(source, 1);
    consume_probe(target);
    assert(close(source) == 0);
    assert(close(target) == 0);

    source = open("/dev/null", O_RDWR);
    target = open(FAUXDEV_PROBE_PATH, O_RDWR);
    assert(source >= 0 && target >= 0);
    assert(dup2(source, target) == target);
    struct stat status;
    assert(fstat(target, &status) == 0);
    assert(major(status.st_rdev) == 1);
    assert(close(source) == 0);
    assert(close(target) == 0);

    source = open(FAUXDEV_PROBE_PATH, O_RDWR);
    target = open("/dev/null", O_RDWR);
    assert(source >= 0 && target >= 0);
    assert(dup3(source, target, 0) == target);
    require_probe_metadata(target);
    assert(fcntl(target, F_GETFD) & FD_CLOEXEC);
    assert(close(source) == 0);
    assert(close(target) == 0);
}

static void test_independent_sessions(void) {
    int first = open(FAUXDEV_PROBE_PATH, O_RDWR | O_NONBLOCK);
    int second = open(FAUXDEV_PROBE_PATH, O_RDWR | O_NONBLOCK);
    assert(first >= 0 && second >= 0);
    signal_probe(first, 1);
    struct pollfd descriptors[2] = {
        {.fd = first, .events = POLLIN},
        {.fd = second, .events = POLLIN},
    };
    assert(poll(descriptors, 2, 0) == 1);
    assert(descriptors[0].revents & POLLIN);
    assert(descriptors[1].revents == 0);
    consume_probe(first);
    assert(close(first) == 0);
    assert(close(second) == 0);
}

static void test_fork_isolation(void) {
    int fd = open(FAUXDEV_PROBE_PATH, O_RDWR);
    assert(fd >= 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        errno = 0;
        _exit(fcntl(fd, F_GETFD) == -1 && errno == EBADF ? 0 : 1);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    uint32_t version = 0;
    assert(ioctl(fd, FAUXDEV_PROBE_GET_VERSION, &version) == 0);
    assert(close(fd) == 0);
}

static void test_close_range_cleanup(void) {
    errno = 0;
    assert(close_range(4, 3, 0) == -1 && errno == EINVAL);
    errno = 0;
    assert(close_range(0, 0, 1 << 30) == -1 && errno == EINVAL);

    int first = open(FAUXDEV_PROBE_PATH, O_RDWR);
    int second = dup(first);
    assert(first >= 3 && second >= 3);
    unsigned int low = first < second ? (unsigned int)first : (unsigned int)second;
    unsigned int high = first > second ? (unsigned int)first : (unsigned int)second;
    if (close_range(low, high, 0) != 0) {
        perror("close_range");
        abort();
    }
    assert(fcntl(first, F_GETFD) == -1 && errno == EBADF);
    assert(fcntl(second, F_GETFD) == -1 && errno == EBADF);

    int reused = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(reused >= 0);
    struct stat status;
    assert(fstat(reused, &status) == 0);
    assert(major(status.st_rdev) == 1);
    assert(close(reused) == 0);
}

static void test_closefrom_cleanup(void) {
    int fd = open(FAUXDEV_PROBE_PATH, O_RDWR);
    assert(fd >= 0);
    int high = fcntl(fd, F_DUPFD, 128);
    assert(high >= 128);
#ifdef __ANDROID__
    void (*closefrom_fn)(int) = dlsym(RTLD_DEFAULT, "closefrom");
    assert(closefrom_fn);
    closefrom_fn(high);
#else
    closefrom(high);
#endif
    assert(fcntl(high, F_GETFD) == -1 && errno == EBADF);

    uint32_t version = 0;
    assert(ioctl(fd, FAUXDEV_PROBE_GET_VERSION, &version) == 0);
    assert(version == FAUXDEV_PROBE_VERSION);
    assert(close(fd) == 0);
}

static void *thread_open_probe(void *unused) {
    (void)unused;
    for (int iteration = 0; iteration < 100; iteration++) {
        int fd = open(FAUXDEV_PROBE_PATH, O_RDWR | O_NONBLOCK);
        assert(fd >= 0);
        uint32_t version = 0;
        assert(ioctl(fd, FAUXDEV_PROBE_GET_VERSION, &version) == 0);
        assert(version == FAUXDEV_PROBE_VERSION);
        assert(close(fd) == 0);
    }
    return NULL;
}

static void test_concurrent_opens(void) {
    pthread_t threads[4];
    for (size_t index = 0; index < 4; index++) {
        assert(pthread_create(&threads[index], NULL, thread_open_probe, NULL) == 0);
    }
    for (size_t index = 0; index < 4; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
    }
}

static void *thread_shared_ioctl(void *data) {
    const int fd = *(const int *)data;
    for (int iteration = 0; iteration < 1000; iteration++) {
        uint32_t version = 0;
        assert(ioctl(fd, FAUXDEV_PROBE_GET_VERSION, &version) == 0);
        assert(version == FAUXDEV_PROBE_VERSION);
        int unrelated = open("/dev/null", O_RDONLY);
        assert(unrelated >= 0);
        assert(close(unrelated) == 0);
    }
    return NULL;
}

static void test_concurrent_ioctls(void) {
    int fd = open(FAUXDEV_PROBE_PATH, O_RDWR);
    assert(fd >= 0);
    pthread_t threads[4];
    for (size_t index = 0; index < 4; index++) {
        assert(pthread_create(&threads[index], NULL, thread_shared_ioctl, &fd) == 0);
    }
    for (size_t index = 0; index < 4; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
    }
    assert(close(fd) == 0);
}

int main(void) {
    test_passthrough();
    test_metadata_and_ioctl();
    test_readiness();
    test_duplication();
    test_duplication_replacement();
    test_independent_sessions();
    test_fork_isolation();
    test_concurrent_opens();
    test_concurrent_ioctls();
    test_close_range_cleanup();
    test_closefrom_cleanup();
    puts("fauxdev foundation tests passed");
    return 0;
}
