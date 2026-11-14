#include "transport.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { WIDTH = 320, HEIGHT = 240, FRAME_SIZE = WIDTH * HEIGHT * 3 };

static void check(int ok, const char *step) {
    if (!ok) { perror(step); exit(1); }
}

int main(void) {
    const char *inherited = getenv("FAUXDEV_VIDEO_FD");
    check(inherited && *inherited, "FAUXDEV_VIDEO_FD is required");
    char *end = NULL;
    errno = 0;
    long inherited_fd = strtol(inherited, &end, 10);
    check(!errno && end != inherited && !*end && inherited_fd >= 0 && inherited_fd <= INT_MAX,
          "parse FAUXDEV_VIDEO_FD");
    int ready = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    check(ready >= 0, "eventfd");
    struct fauxdev_transport *broker = fauxdev_transport_from_fd((int)inherited_fd, ready);
    check(broker != NULL, "HELLO");
    struct fauxdev_message reply;
    errno = 0;
    check(fauxdev_transport_request(broker, FAUXDEV_OPCODE_VIDEO_FRAME,
          NULL, 0, NULL, 0, &reply) == -1 && errno == EINVAL,
          "malformed legacy VIDEO_FRAME");
#ifdef __ANDROID__
    int frame_fd = syscall(SYS_memfd_create, "legacy-camera-frame", MFD_CLOEXEC);
#else
    int frame_fd = memfd_create("legacy-camera-frame", MFD_CLOEXEC);
#endif
    check(frame_fd >= 0 && ftruncate(frame_fd, FRAME_SIZE) == 0, "frame buffer");
    const struct fauxdev_video_frame_request frame = {
        .offset = htole32(0), .width = htole32(WIDTH), .height = htole32(HEIGHT),
        .stride = htole32(WIDTH * 3), .pixel_format = htole32(V4L2_PIX_FMT_RGB24),
        .size = htole32(FRAME_SIZE),
    };
    // Protocol-v1 client: HELLO, then VIDEO_FRAME, without INPUTS or START.
    check(fauxdev_transport_request(broker, FAUXDEV_OPCODE_VIDEO_FRAME,
          &frame, sizeof(frame), &frame_fd, 1, &reply) == 0, "legacy VIDEO_FRAME");
    check(!reply.payload_size && !reply.fd_count, "legacy frame reply");
    unsigned char buffer[4096];
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t offset = 0; offset < FRAME_SIZE; offset += sizeof(buffer)) {
        size_t size = FRAME_SIZE - offset < sizeof(buffer) ? FRAME_SIZE - offset : sizeof(buffer);
        check(pread(frame_fd, buffer, size, offset) == (ssize_t)size, "read frame");
        for (size_t i = 0; i < size; ++i) hash = (hash ^ buffer[i]) * UINT64_C(1099511628211);
    }
    printf("legacy VIDEO_FRAME succeeded: %016llx\n", (unsigned long long)hash);
    close(frame_fd);
    fauxdev_transport_destroy(broker);
    close(ready);
    return 0;
}
