#include "internal.h"
#include "transport.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 320, HEIGHT = 240, FRAME_SIZE = WIDTH * HEIGHT * 3 };
enum buffer_state { FREE, QUEUED, CAPTURING, DONE };

struct video_session {
    pthread_mutex_t lock;
    pthread_mutex_t lifecycle_lock;
    pthread_cond_t work_ready;
    pthread_t worker;
    int worker_started;
    int worker_stop;
    int memfd;
    int readiness_fd;
    size_t slot_size;
    unsigned char *pixels;
    char *socket_path;
    unsigned int count;
    unsigned int sequence;
    uint64_t stream_generation;
    unsigned int frame_sequence[2];
    struct timeval frame_timestamp[2];
    enum buffer_state state[2];
    unsigned int order[2];
    unsigned int done_count;
    int streaming;
    int broker_failed;
    struct fauxdev_transport *broker;
};

static void drain(struct video_session *s) {
    uint64_t token;
    while (syscall(SYS_read, s->readiness_fd, &token, sizeof(token)) == sizeof(token)) {}
    if (s->broker_failed || (s->broker && fauxdev_transport_disconnected(s->broker))) {
        token = 1;
        syscall(SYS_write, s->readiness_fd, &token, sizeof(token));
    }
}

static int video_open(int readiness_fd, int flags, void **data) {
    (void)flags;
    struct video_session *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->memfd = -1;
    s->readiness_fd = syscall(SYS_fcntl, readiness_fd, F_DUPFD_CLOEXEC, 0);
    if (s->readiness_fd < 0) { free(s); return -1; }
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || page_size > UINT32_MAX / 2) {
        syscall(SYS_close, s->readiness_fd);
        free(s);
        errno = EINVAL;
        return -1;
    }
    s->slot_size = ((FRAME_SIZE + (size_t)page_size - 1) / (size_t)page_size) * (size_t)page_size;
    if (s->slot_size > UINT32_MAX / 2) {
        syscall(SYS_close, s->readiness_fd);
        free(s);
        errno = EOVERFLOW;
        return -1;
    }
    int error = pthread_mutex_init(&s->lock, NULL);
    if (error) {
        syscall(SYS_close, s->readiness_fd);
        free(s);
        errno = error;
        return -1;
    }
    error = pthread_mutex_init(&s->lifecycle_lock, NULL);
    if (error) { pthread_mutex_destroy(&s->lock); syscall(SYS_close, s->readiness_fd); free(s); errno = error; return -1; }
    error = pthread_cond_init(&s->work_ready, NULL);
    if (error) { pthread_mutex_destroy(&s->lifecycle_lock); pthread_mutex_destroy(&s->lock); syscall(SYS_close, s->readiness_fd); free(s); errno = error; return -1; }
    s->pixels = malloc(FRAME_SIZE);
    if (!s->pixels) goto fail;
    const char *socket_path = getenv("FAUXDEV_VIDEO_SOCKET");
    if (socket_path && *socket_path) {
        s->socket_path = strdup(socket_path);
        if (!s->socket_path) goto fail;
        s->broker = fauxdev_transport_connect(socket_path, readiness_fd);
        if (!s->broker) goto fail;
    }
    *data = s;
    return 0;
fail: {
        int saved_errno = errno;
        free(s->pixels);
        free(s->socket_path);
        pthread_cond_destroy(&s->work_ready);
        pthread_mutex_destroy(&s->lifecycle_lock);
        pthread_mutex_destroy(&s->lock);
        syscall(SYS_close, s->readiness_fd);
        free(s);
        errno = saved_errno;
        return -1;
    }
}

static void fill_format(struct v4l2_format *f) {
    memset(&f->fmt, 0, sizeof(f->fmt));
    f->fmt.pix.width = WIDTH;
    f->fmt.pix.height = HEIGHT;
    f->fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    f->fmt.pix.field = V4L2_FIELD_NONE;
    f->fmt.pix.bytesperline = WIDTH * 3;
    f->fmt.pix.sizeimage = FRAME_SIZE;
    f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
}

static void describe_buffer(struct video_session *s, struct v4l2_buffer *b) {
    const unsigned int index = b->index;
    memset(b, 0, sizeof(*b));
    b->index = index;
    b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->memory = V4L2_MEMORY_MMAP;
    b->length = FRAME_SIZE;
    b->m.offset = index * s->slot_size;
    if (s->state[index] == QUEUED || s->state[index] == CAPTURING)
        b->flags = V4L2_BUF_FLAG_QUEUED;
    if (s->state[index] == DONE) b->flags = V4L2_BUF_FLAG_DONE;
}

static int capture_slot(struct video_session *s, unsigned int sequence, unsigned char pixels[FRAME_SIZE]) {
    if (s->broker) {
        int staging = memfd_create("fauxdev-frame-staging", MFD_CLOEXEC);
        if (staging < 0) return -1;
        if (ftruncate(staging, FRAME_SIZE) < 0) {
            syscall(SYS_close, staging);
            return -1;
        }
        const struct fauxdev_video_frame_request request = {
            .offset = htole32(0),
            .width = htole32(WIDTH),
            .height = htole32(HEIGHT),
            .stride = htole32(WIDTH * 3),
            .pixel_format = htole32(V4L2_PIX_FMT_RGB24),
            .size = htole32(FRAME_SIZE),
        };
        struct fauxdev_message response;
        if (fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_VIDEO_FRAME,
                &request, sizeof(request), &staging, 1, &response) < 0) {
            syscall(SYS_close, staging);
            return -1;
        }
        if (response.payload_size || response.fd_count) {
            fauxdev_message_close_fds(&response);
            syscall(SYS_close, staging);
            errno = EPROTO;
            return -1;
        }
        size_t done = 0;
        while (done < FRAME_SIZE) {
            ssize_t got = pread(staging, pixels + done, FRAME_SIZE - done, done);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                int saved_errno = got < 0 ? errno : EIO;
                syscall(SYS_close, staging);
                errno = saved_errno;
                return -1;
            }
            done += got;
        }
        syscall(SYS_close, staging);
    } else {
        for (unsigned int y = 0; y < HEIGHT; y++) {
            for (unsigned int x = 0; x < WIDTH; x++) {
                const unsigned int at = (y * WIDTH + x) * 3;
                pixels[at] = (unsigned char)(x * 4);
                pixels[at + 1] = (unsigned char)(y * 5);
                pixels[at + 2] = (unsigned char)(sequence & 255);
            }
        }
    }
    return 0;
}

static void *produce_frames(void *data) {
    struct video_session *s = data;
    if (s->socket_path && !s->broker) {
        struct fauxdev_transport *broker = fauxdev_transport_connect(s->socket_path, s->readiness_fd);
        pthread_mutex_lock(&s->lock);
        if (s->worker_stop) {
            pthread_mutex_unlock(&s->lock);
            fauxdev_transport_destroy(broker);
            return NULL;
        }
        if (!broker) {
            s->broker_failed = 1;
            const uint64_t token = 1;
            syscall(SYS_write, s->readiness_fd, &token, sizeof(token));
            pthread_mutex_unlock(&s->lock);
            return NULL;
        }
        s->broker = broker;
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_lock(&s->lock);
    while (!s->worker_stop) {
        unsigned int index = 0;
        while (index < s->count && s->state[index] != QUEUED) index++;
        if (index == s->count) {
            pthread_cond_wait(&s->work_ready, &s->lock);
            continue;
        }
        s->state[index] = CAPTURING;
        const unsigned int sequence = s->sequence;
        pthread_mutex_unlock(&s->lock);
        const int result = capture_slot(s, sequence, s->pixels);
        pthread_mutex_lock(&s->lock);
        if (s->worker_stop || !s->streaming) break;
        if (result < 0) {
            s->broker_failed = 1;
            const uint64_t token = 1;
            syscall(SYS_write, s->readiness_fd, &token, sizeof(token));
            break;
        }
        size_t done = 0;
        while (done < FRAME_SIZE) {
            ssize_t written = pwrite(s->memfd, s->pixels + done, FRAME_SIZE - done,
                                     index * s->slot_size + done);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            done += written;
        }
        if (done != FRAME_SIZE) {
            s->broker_failed = 1;
            const uint64_t token = 1;
            syscall(SYS_write, s->readiness_fd, &token, sizeof(token));
            break;
        }
        struct timespec completed;
        clock_gettime(CLOCK_MONOTONIC, &completed);
        s->state[index] = DONE;
        s->frame_sequence[index] = s->sequence++;
        s->frame_timestamp[index] = (struct timeval){
            .tv_sec = completed.tv_sec, .tv_usec = completed.tv_nsec / 1000
        };
        s->order[s->done_count++] = index;
        const uint64_t token = 1;
        syscall(SYS_write, s->readiness_fd, &token, sizeof(token));
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static void stop_worker(struct video_session *s) {
    pthread_mutex_lock(&s->lock);
    s->streaming = 0;
    s->stream_generation++;
    s->worker_stop = 1;
    pthread_cond_broadcast(&s->work_ready);
    struct fauxdev_transport *broker = s->broker;
    const int started = s->worker_started;
    pthread_mutex_unlock(&s->lock);
    fauxdev_transport_cancel(broker);
    if (started) pthread_join(s->worker, NULL);
    pthread_mutex_lock(&s->lock);
    broker = s->broker;
    s->broker = NULL;
    s->worker_started = 0;
    s->worker_stop = 0;
    s->broker_failed = 0;
    s->done_count = 0;
    memset(s->state, 0, sizeof(s->state));
    pthread_mutex_unlock(&s->lock);
    fauxdev_transport_destroy(broker);
    pthread_mutex_lock(&s->lock);
    drain(s);
    pthread_mutex_unlock(&s->lock);
}

static void video_release(void *data) {
    struct video_session *s = data;
    stop_worker(s);
    if (s->memfd >= 0) syscall(SYS_close, s->memfd);
    syscall(SYS_close, s->readiness_fd);
    free(s->pixels);
    free(s->socket_path);
    pthread_cond_destroy(&s->work_ready);
    pthread_mutex_destroy(&s->lifecycle_lock);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

static int video_ioctl(
    void *data, int readiness_fd, int status_flags, unsigned long request, void *argument
) {
    (void)readiness_fd;
    struct video_session *s = data;
    if (!argument) {
        errno = EFAULT;
        return -1;
    }
    const int lifecycle = request == VIDIOC_QBUF || request == VIDIOC_REQBUFS ||
                          request == VIDIOC_STREAMON || request == VIDIOC_STREAMOFF;
    if (lifecycle) pthread_mutex_lock(&s->lifecycle_lock);
    pthread_mutex_lock(&s->lock);
    int result = 0;
    switch (request) {
    case VIDIOC_G_INPUT:
        *(int *)argument = 0;
        break;
    case VIDIOC_ENUMINPUT: {
        struct v4l2_input *input = argument;
        if (input->index != 0) { errno = EINVAL; result = -1; break; }
        memset(input, 0, sizeof(*input));
        memcpy(input->name, "fauxdev test pattern", 21);
        input->type = V4L2_INPUT_TYPE_CAMERA;
        break;
    }
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *cap = argument;
        memset(cap, 0, sizeof(*cap));
        memcpy(cap->driver, "fauxdev", 8);
        memcpy(cap->card, "fauxdev test pattern", 21);
        memcpy(cap->bus_info, "platform:fauxdev", 17);
        cap->version = 1;
        cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
        break;
    }
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *desc = argument;
        if (desc->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || desc->index) { errno = EINVAL; result = -1; break; }
        desc->flags = 0;
        desc->pixelformat = V4L2_PIX_FMT_RGB24;
        memset(desc->description, 0, sizeof(desc->description));
        memcpy(desc->description, "RGB24 test pattern", 19);
        break;
    }
    case VIDIOC_G_FMT:
    case VIDIOC_TRY_FMT:
    case VIDIOC_S_FMT: {
        struct v4l2_format *f = argument;
        if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            (request == VIDIOC_S_FMT && s->count)) { errno = request == VIDIOC_S_FMT && s->count ? EBUSY : EINVAL; result = -1; break; }
        fill_format(f);
        break;
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *req = argument;
        if (req->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || req->memory != V4L2_MEMORY_MMAP || s->streaming) {
            errno = s->streaming ? EBUSY : EINVAL; result = -1; break;
        }
        unsigned int count = req->count > 2 ? 2 : req->count;
        int memfd = -1;
        if (count) {
            memfd = memfd_create("fauxdev-video", MFD_CLOEXEC);
            if (memfd < 0 || ftruncate(memfd, count * s->slot_size) < 0) {
                if (memfd >= 0) syscall(SYS_close, memfd);
                result = -1; break;
            }
        }
        if (s->memfd >= 0) syscall(SYS_close, s->memfd);
        s->memfd = memfd;
        s->count = count;
        s->done_count = 0;
        memset(s->state, 0, sizeof(s->state));
        drain(s);
        req->count = count;
        req->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP | V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS;
        break;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *b = argument;
        if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->memory != V4L2_MEMORY_MMAP || b->index >= s->count) {
            errno = EINVAL; result = -1; break;
        }
        describe_buffer(s, b);
        break;
    }
    case VIDIOC_QBUF: {
        struct v4l2_buffer *b = argument;
        if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->memory != V4L2_MEMORY_MMAP ||
            b->index >= s->count || s->state[b->index] != FREE) { errno = EINVAL; result = -1; break; }
        s->state[b->index] = QUEUED;
        if (s->streaming) pthread_cond_signal(&s->work_ready);
        describe_buffer(s, b);
        break;
    }
    case VIDIOC_STREAMON: {
        enum v4l2_buf_type *type = argument;
        if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE || !s->count) { errno = EINVAL; result = -1; break; }
        if (s->streaming) break;
        s->streaming = 1;
        s->broker_failed = 0;
        int error = pthread_create(&s->worker, NULL, produce_frames, s);
        if (error) {
            s->streaming = 0;
            errno = error;
            result = -1;
            break;
        }
        s->worker_started = 1;
        break;
    }
    case VIDIOC_DQBUF: {
        struct v4l2_buffer *b = argument;
        if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->memory != V4L2_MEMORY_MMAP || !s->streaming) {
            errno = EINVAL; result = -1; break;
        }
        const uint64_t generation = s->stream_generation;
        for (;;) {
            if (generation != s->stream_generation || !s->streaming) {
                errno = EINVAL; result = -1; break;
            }
            if (s->done_count) break;
            if (s->broker_failed || (s->broker && fauxdev_transport_disconnected(s->broker))) {
                errno = EPIPE; result = -1; break;
            }
            if (status_flags & O_NONBLOCK) { errno = EAGAIN; result = -1; break; }
            struct pollfd ready = {.fd = s->readiness_fd, .events = POLLIN};
            pthread_mutex_unlock(&s->lock);
            int polled = poll(&ready, 1, 100);
            const int saved_errno = errno;
            pthread_mutex_lock(&s->lock);
            if (polled < 0) { errno = saved_errno; result = -1; break; }
            if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = EIO; result = -1; break;
            }
        }
        if (result < 0) break;
        unsigned int index = s->order[0];
        s->order[0] = s->order[1];
        s->done_count--;
        uint64_t token;
        if (syscall(SYS_read, s->readiness_fd, &token, sizeof(token)) != sizeof(token)) { result = -1; break; }
        s->state[index] = FREE;
        b->index = index;
        describe_buffer(s, b);
        b->bytesused = FRAME_SIZE;
        b->sequence = s->frame_sequence[index];
        b->timestamp = s->frame_timestamp[index];
        b->flags |= V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
        break;
    }
    case VIDIOC_STREAMOFF: {
        enum v4l2_buf_type *type = argument;
        if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE) { errno = EINVAL; result = -1; break; }
        pthread_mutex_unlock(&s->lock);
        stop_worker(s);
        pthread_mutex_lock(&s->lock);
        break;
    }
    default:
        errno = ENOTTY;
        result = -1;
    }
    pthread_mutex_unlock(&s->lock);
    if (lifecycle) pthread_mutex_unlock(&s->lifecycle_lock);
    return result;
}

static void *video_mmap(void *data, void *addr, size_t length, int prot, int flags, off_t offset) {
    struct video_session *s = data;
    pthread_mutex_lock(&s->lock);
    if (s->memfd < 0 || offset < 0 || offset % s->slot_size ||
        (unsigned long long)offset / s->slot_size >= s->count || length != FRAME_SIZE ||
        !(flags & MAP_SHARED)) {
        pthread_mutex_unlock(&s->lock);
        errno = EINVAL;
        return MAP_FAILED;
    }
    void *result = (void *)syscall(SYS_mmap, addr, length, prot, flags, s->memfd, offset);
    pthread_mutex_unlock(&s->lock);
    return result;
}

__attribute__((visibility("hidden"))) const struct fauxdev_backend fauxdev_video_backend = {
    .path = "/dev/video0",
    .mode = 0660,
    .major = 81,
    .minor = 0,
    .open = video_open,
    .ioctl = video_ioctl,
    .mmap = video_mmap,
    .release = video_release,
};
