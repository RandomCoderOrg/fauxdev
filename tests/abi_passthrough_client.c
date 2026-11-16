#include <assert.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char path[] = "fauxdev-abi-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(unlink(path) == 0);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);

    struct stat status;
    assert(stat(path, &status) == 0 && (status.st_mode & 0777) == 0600);
    assert(lstat(path, &status) == 0 && S_ISREG(status.st_mode));
    assert(fstatat(AT_FDCWD, path, &status, 0) == 0 && S_ISREG(status.st_mode));
    assert(unlink(path) == 0);

    const off64_t offset = argc > 1 ? 8192 :
        (sizeof(void *) == 4 ? 3LL * 1024 * 1024 * 1024 : 8192);
    if (ftruncate64(fd, offset + 4096) < 0) { perror("ftruncate64"); return 1; }

    assert(fstat(fd, &status) == 0 && status.st_size == offset + 4096);
    assert(fstatat(fd, "", &status, AT_EMPTY_PATH) == 0 && status.st_size == offset + 4096);

    unsigned char *page = mmap64(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    assert(page != MAP_FAILED);
    page[0] = 0x5a;
    unsigned char value = 0;
    ssize_t read_count = pread64(fd, &value, 1, offset);
    if (read_count != 1 || value != 0x5a) {
        fprintf(stderr, "pread64: expected 0x5a, got %zd bytes and 0x%x\n", read_count, value);
        return 1;
    }
    value = 0x6b;
    assert(pwrite64(fd, &value, 1, offset + 1) == 1 && page[1] == 0x6b);
    assert(munmap(page, 4096) == 0);
    assert(close(fd) == 0);
    puts("fauxdev ABI passthrough passed");

    if (argc == 2 && strcmp(argv[1], "--camera") == 0) {
        fd = open("/dev/video0", O_RDWR | O_NONBLOCK);
        assert(fd >= 0);
        struct v4l2_capability capability = {0};
        assert(ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0);
        struct v4l2_frmsizeenum size = {.pixel_format = V4L2_PIX_FMT_RGB24};
        assert(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0);
        printf("camera ABI: %.32s %ux%u\n", capability.driver,
               size.discrete.width, size.discrete.height);
        assert(close(fd) == 0);
    } else {
        assert(argc == 1);
    }
}
