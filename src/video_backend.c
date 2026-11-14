#include "internal.h"
#include "transport.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { WIDTH = 320, HEIGHT = 240, FRAME_SIZE = WIDTH * HEIGHT * 3,
       MAX_BACKING_SIZE = 128 * 1024 * 1024, MAX_FRAME_SIZE = 64 * 1024 * 1024,
       MAX_CAMERA_MODES = 1025, MAX_CAMERA_RATES = 256 };
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
    size_t page_size;
    size_t slot_size;
    size_t backing_size;
    uint32_t width, height;
    size_t frame_size;
    struct fauxdev_camera_mode *modes;
    unsigned int mode_count;
    int mode_query_supported;
    int start_v2_unsupported;
    int config_supported;
    int queue_leased;
    uint64_t config_generation;
    uint32_t buffer_offset[VIDEO_MAX_FRAME];
    uint32_t buffer_length[VIDEO_MAX_FRAME];
    unsigned char *pixels;
    char *socket_path;
    unsigned int count;
    unsigned int sequence;
    uint64_t stream_generation;
    unsigned int frame_sequence[VIDEO_MAX_FRAME];
    struct timeval frame_timestamp[VIDEO_MAX_FRAME];
    enum buffer_state state[VIDEO_MAX_FRAME];
    unsigned int order[VIDEO_MAX_FRAME];
    unsigned int done_count;
    int streaming;
    int broker_failed;
    struct fauxdev_transport *broker;
    int shared_broker;
    struct fauxdev_camera_input *inputs;
    unsigned int input_count;
    unsigned int selected_input;
    int camera_selection_supported;
    int camera_started;
    enum v4l2_priority priority;
    struct video_session *device_next;
};

static pthread_mutex_t device_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fauxdev_transport *device_broker;
static struct video_session *queue_owner;
static unsigned int device_broker_users;
static dev_t device_broker_dev;
static ino_t device_broker_ino;
static pid_t device_launch_pid;
static struct video_session *device_sessions;

static enum v4l2_priority highest_priority_locked(const struct video_session *except) {
    enum v4l2_priority highest = V4L2_PRIORITY_UNSET;
    for (struct video_session *other = device_sessions; other; other = other->device_next)
        if (other != except && other->priority > highest) highest = other->priority;
    return highest;
}

static enum v4l2_priority highest_broker_priority_locked(
    const struct fauxdev_transport *broker, const struct video_session *except
) {
    enum v4l2_priority highest = V4L2_PRIORITY_UNSET;
    for (const struct video_session *other = device_sessions; other; other = other->device_next)
        if (other != except && other->broker == broker && other->priority > highest)
            highest = other->priority;
    return highest;
}

__attribute__((constructor)) static void video_initialize(void) {
    device_launch_pid = getpid();
}

static void drain(struct video_session *s) {
    uint64_t token;
    while (syscall(SYS_read, s->readiness_fd, &token, sizeof(token)) == sizeof(token)) {}
    if (s->broker_failed || (s->broker && fauxdev_transport_disconnected(s->broker))) {
        token = 1;
        syscall(SYS_write, s->readiness_fd, &token, sizeof(token));
    }
}

static int valid_dimensions(uint32_t width, uint32_t height) {
    return width && height && (uint64_t)width * height * 3 <= MAX_FRAME_SIZE &&
           (uint64_t)width * 3 <= UINT32_MAX;
}

static int load_modes(struct fauxdev_transport *broker, unsigned int input,
                      struct fauxdev_camera_mode **out, unsigned int *out_count,
                      int *supported) {
    struct fauxdev_camera_mode *modes = calloc(MAX_CAMERA_MODES, sizeof(*modes));
    if (!modes) return -1;
    unsigned int count = 0, total = 0;
    *supported = 0;
    if (broker) {
        for (;;) {
            const struct fauxdev_camera_modes_request query = {
                .index = htole32(input), .offset = htole32(count),
            };
            struct fauxdev_message response;
            if (fauxdev_transport_request(broker, FAUXDEV_OPCODE_CAMERA_MODES,
                    &query, sizeof(query), NULL, 0, &response) < 0) {
                if (errno == ENOTTY && !count) break;
                free(modes);
                return -1;
            }
            if (response.fd_count || response.payload_size <
                    offsetof(struct fauxdev_camera_modes_reply, modes) ||
                (response.payload_size - offsetof(struct fauxdev_camera_modes_reply, modes)) %
                    sizeof(struct fauxdev_camera_mode)) {
                fauxdev_message_close_fds(&response);
                free(modes); errno = EPROTO; return -1;
            }
            struct fauxdev_camera_modes_reply header;
            memcpy(&header, response.payload, offsetof(struct fauxdev_camera_modes_reply, modes));
            unsigned int page_total = le32toh(header.total), page_offset = le32toh(header.offset);
            unsigned int page_count = (response.payload_size -
                    offsetof(struct fauxdev_camera_modes_reply, modes)) /
                    sizeof(struct fauxdev_camera_mode);
            if (!page_total || page_total > MAX_CAMERA_MODES ||
                (total && page_total != total) || page_offset != count || !page_count ||
                page_count > FAUXDEV_CAMERA_MODE_PAGE || page_count > page_total - count) {
                free(modes); errno = EPROTO; return -1;
            }
            total = page_total;
            for (unsigned int i = 0; i < page_count; ++i) {
                struct fauxdev_camera_mode wire;
                memcpy(&wire, response.payload + offsetof(struct fauxdev_camera_modes_reply, modes) +
                       i * sizeof(wire), sizeof(wire));
                uint32_t width = le32toh(wire.width), height = le32toh(wire.height);
                if (!valid_dimensions(width, height)) { free(modes); errno = EPROTO; return -1; }
                for (unsigned int j = 0; j < count + i; ++j)
                    if (modes[j].width == width && modes[j].height == height) {
                        free(modes); errno = EPROTO; return -1;
                    }
                modes[count + i] = (struct fauxdev_camera_mode){width, height};
            }
            count += page_count;
            if (count == total) { *supported = 1; break; }
        }
    }
    if (!*supported) {
        modes[0] = (struct fauxdev_camera_mode){WIDTH, HEIGHT};
        count = 1;
    } else {
        int has_default = 0;
        for (unsigned int i = 0; i < count; ++i)
            if (modes[i].width == WIDTH && modes[i].height == HEIGHT) has_default = 1;
        if (!has_default) {
            if (count == MAX_CAMERA_MODES) { free(modes); errno = EPROTO; return -1; }
            modes[count++] = (struct fauxdev_camera_mode){WIDTH, HEIGHT};
        }
    }
    *out = modes;
    *out_count = count;
    return 0;
}

static int get_config(struct video_session *s, struct fauxdev_camera_config *config,
                      int *supported) {
    *supported = 0;
    if (!s->broker || !s->camera_selection_supported || !s->input_count) return 0;
    struct fauxdev_message response;
    if (fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_GET_CONFIG,
            NULL, 0, NULL, 0, &response) < 0) return errno == ENOTTY ? 0 : -1;
    if (response.fd_count || response.payload_size != sizeof(*config)) {
        fauxdev_message_close_fds(&response);
        errno = EPROTO; return -1;
    }
    memcpy(config, response.payload, sizeof(*config));
    config->generation = le64toh(config->generation);
    config->index = le32toh(config->index);
    config->width = le32toh(config->width);
    config->height = le32toh(config->height);
    if (!config->generation || config->index >= s->input_count ||
        !valid_dimensions(config->width, config->height) || config->reserved) {
        errno = EPROTO; return -1;
    }
    *supported = 1;
    return 0;
}

static int load_rates(struct video_session *s, uint32_t index, uint32_t width,
                      uint32_t height, uint32_t rates[MAX_CAMERA_RATES],
                      unsigned int *count, int *supported) {
    *count = 0;
    *supported = 0;
    if (!s->broker || !s->config_supported) return 0;
    unsigned int total = 0;
    for (;;) {
        struct fauxdev_camera_rates_request query = {
            .index = htole32(index), .width = htole32(width),
            .height = htole32(height), .offset = htole32(*count),
        };
        struct fauxdev_message response;
        if (fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_ENUM_RATES,
                &query, sizeof(query), NULL, 0, &response) < 0) {
            if (errno == ENOTTY && !*count) return 0;
            if (errno == EINVAL && !*count) { *supported = 1; return 0; }
            return -1;
        }
        if (response.fd_count || response.payload_size <
                offsetof(struct fauxdev_camera_rates_reply, fps) ||
            (response.payload_size - offsetof(struct fauxdev_camera_rates_reply, fps)) %
                sizeof(uint32_t)) {
            fauxdev_message_close_fds(&response); errno = EPROTO; return -1;
        }
        struct fauxdev_camera_rates_reply header;
        memcpy(&header, response.payload, offsetof(struct fauxdev_camera_rates_reply, fps));
        unsigned int page_total = le32toh(header.total), page_offset = le32toh(header.offset);
        unsigned int page_count = (response.payload_size -
            offsetof(struct fauxdev_camera_rates_reply, fps)) / sizeof(uint32_t);
        if (!page_total || page_total > MAX_CAMERA_RATES ||
            (total && page_total != total) || page_offset != *count || !page_count ||
            page_count > FAUXDEV_CAMERA_RATE_PAGE || page_count > page_total - *count) {
            errno = EPROTO; return -1;
        }
        total = page_total;
        for (unsigned int i = 0; i < page_count; ++i) {
            uint32_t wire;
            memcpy(&wire, response.payload + offsetof(struct fauxdev_camera_rates_reply, fps) +
                   i * sizeof(wire), sizeof(wire));
            uint32_t fps = le32toh(wire);
            if (!fps || fps > 1000 || (*count + i && fps <= rates[*count + i - 1])) {
                errno = EPROTO; return -1;
            }
            rates[*count + i] = fps;
        }
        *count += page_count;
        if (*count == total) { *supported = 1; return 0; }
    }
}

static uint32_t nominal_fps(const uint32_t *rates, unsigned int count) {
    if (!count) return 0;
    uint32_t chosen = rates[0];
    for (unsigned int i = 0; i < count; ++i) {
        if (rates[i] == 30) return 30;
        if (rates[i] <= 30) chosen = rates[i];
    }
    return chosen;
}

static int get_rate(struct video_session *s, struct fauxdev_camera_rate *rate,
                    int *supported) {
    *supported = 0;
    if (!s->broker || !s->config_supported) return 0;
    struct fauxdev_message response;
    if (fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_GET_RATE,
            NULL, 0, NULL, 0, &response) < 0) return errno == ENOTTY ? 0 : -1;
    if (response.fd_count || response.payload_size != sizeof(*rate)) {
        fauxdev_message_close_fds(&response); errno = EPROTO; return -1;
    }
    memcpy(rate, response.payload, sizeof(*rate));
    rate->generation = le64toh(rate->generation);
    rate->fps = le32toh(rate->fps);
    if (!rate->generation || rate->fps > 1000 || rate->reserved) {
        errno = EPROTO; return -1;
    }
    *supported = 1;
    return 0;
}

static int set_rate(struct video_session *s, uint32_t fps, uint64_t *generation) {
    struct fauxdev_camera_rate wanted = {
        .generation = htole64(s->config_generation), .fps = htole32(fps),
    };
    struct fauxdev_message response;
    if (fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_SET_RATE,
            &wanted, sizeof(wanted), NULL, 0, &response) < 0) return -1;
    if (response.fd_count || response.payload_size != sizeof(wanted)) {
        fauxdev_message_close_fds(&response); errno = EPROTO; return -1;
    }
    struct fauxdev_camera_rate current;
    memcpy(&current, response.payload, sizeof(current));
    if (le32toh(current.fps) != fps || current.reserved ||
        le64toh(current.generation) < s->config_generation) {
        errno = EPROTO; return -1;
    }
    *generation = le64toh(current.generation);
    return 0;
}

static int sync_config(struct video_session *s) {
    if (!s->config_supported) return 0;
    struct fauxdev_camera_config current;
    int supported;
    if (get_config(s, &current, &supported) < 0) return -1;
    if (!supported) { errno = EPROTO; return -1; }
    if (current.index == s->selected_input && current.width == s->width &&
        current.height == s->height) {
        if ((s->count || s->queue_leased) && current.generation != s->config_generation) {
            errno = EPROTO; return -1;
        }
        s->config_generation = current.generation;
        return 0;
    }
    if (s->count || s->queue_leased) { errno = EPROTO; return -1; }
    struct fauxdev_camera_mode *modes = NULL;
    unsigned int count = s->mode_count;
    int mode_query_supported = s->mode_query_supported;
    if (current.index != s->selected_input &&
        load_modes(s->broker, current.index, &modes, &count, &mode_query_supported) < 0) return -1;
    const struct fauxdev_camera_mode *available = modes ? modes : s->modes;
    int found = 0;
    for (unsigned int i = 0; i < count; ++i)
        if (available[i].width == current.width && available[i].height == current.height) found = 1;
    if (!found) { free(modes); errno = EPROTO; return -1; }
    size_t frame_size = (size_t)current.width * current.height * 3;
    unsigned char *pixels = malloc(frame_size);
    if (!pixels) { free(modes); return -1; }
    if (modes) {
        free(s->modes);
        s->modes = modes;
        s->mode_count = count;
        s->mode_query_supported = mode_query_supported;
    }
    free(s->pixels);
    s->pixels = pixels;
    s->selected_input = current.index;
    s->width = current.width;
    s->height = current.height;
    s->frame_size = frame_size;
    s->slot_size = (frame_size + s->page_size - 1) / s->page_size * s->page_size;
    s->config_generation = current.generation;
    return 0;
}

static int set_config(struct video_session *s, uint32_t index, uint32_t width,
                      uint32_t height, uint64_t *generation) {
    struct fauxdev_camera_config wanted = {
        .generation = htole64(s->config_generation), .index = htole32(index),
        .width = htole32(width), .height = htole32(height),
    };
    struct fauxdev_message response;
    if (fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_SET_CONFIG,
            &wanted, sizeof(wanted), NULL, 0, &response) < 0) return -1;
    if (response.fd_count || response.payload_size != sizeof(wanted)) {
        fauxdev_message_close_fds(&response);
        errno = EPROTO; return -1;
    }
    struct fauxdev_camera_config current;
    memcpy(&current, response.payload, sizeof(current));
    if (le32toh(current.index) != index || le32toh(current.width) != width ||
        le32toh(current.height) != height || current.reserved ||
        le64toh(current.generation) < s->config_generation) {
        errno = EPROTO; return -1;
    }
    *generation = le64toh(current.generation);
    return 0;
}

static int queue_request(struct video_session *s, int acquire) {
    struct fauxdev_message response;
    struct fauxdev_camera_queue_request wanted = {
        .generation = htole64(s->config_generation),
    };
    if (fauxdev_transport_request(s->broker,
            acquire ? FAUXDEV_OPCODE_CAMERA_ACQUIRE_QUEUE : FAUXDEV_OPCODE_CAMERA_RELEASE_QUEUE,
            acquire ? &wanted : NULL, acquire ? sizeof(wanted) : 0,
            NULL, 0, &response) < 0) return -1;
    if (response.payload_size || response.fd_count) {
        fauxdev_message_close_fds(&response);
        errno = EPROTO; return -1;
    }
    s->queue_leased = acquire;
    return 0;
}

static int broker_priority(struct video_session *s, int update,
                           enum v4l2_priority value, int enforce,
                           enum v4l2_priority *highest) {
    struct fauxdev_camera_priority wanted = {
        .value = htole32(value), .enforce = htole32(enforce),
    };
    struct fauxdev_message response;
    if (fauxdev_transport_request(s->broker,
            update ? FAUXDEV_OPCODE_CAMERA_SET_PRIORITY : FAUXDEV_OPCODE_CAMERA_GET_PRIORITY,
            update ? &wanted : NULL, update ? sizeof(wanted) : 0,
            NULL, 0, &response) < 0) return -1;
    if (response.fd_count || response.payload_size != sizeof(wanted)) {
        fauxdev_message_close_fds(&response);
        errno = EPROTO; return -1;
    }
    struct fauxdev_camera_priority current;
    memcpy(&current, response.payload, sizeof(current));
    uint32_t priority = le32toh(current.value);
    if (priority > V4L2_PRIORITY_RECORD || current.enforce) { errno = EPROTO; return -1; }
    if (highest) *highest = (enum v4l2_priority)priority;
    return 0;
}

static int video_open(int readiness_fd, int flags, void **data) {
    (void)flags;
    int device_locked = 0;
    struct stat broker_stat;
    struct video_session *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->priority = V4L2_PRIORITY_DEFAULT;
    s->memfd = -1;
    s->readiness_fd = syscall(SYS_fcntl, readiness_fd, F_DUPFD_CLOEXEC, 0);
    if (s->readiness_fd < 0) { free(s); return -1; }
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || page_size > MAX_BACKING_SIZE) {
        syscall(SYS_close, s->readiness_fd);
        free(s);
        errno = EINVAL;
        return -1;
    }
    s->slot_size = ((FRAME_SIZE + (size_t)page_size - 1) / (size_t)page_size) * (size_t)page_size;
    s->page_size = (size_t)page_size;
    s->width = WIDTH;
    s->height = HEIGHT;
    s->frame_size = FRAME_SIZE;
    if (s->slot_size > MAX_BACKING_SIZE / 2) {
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
    const char *inherited_fd = getenv("FAUXDEV_VIDEO_FD");
    const char *socket_path = getenv("FAUXDEV_VIDEO_SOCKET");
    if (inherited_fd && *inherited_fd) {
        // shortcut: a forked child needs exec before reusing this process-scoped broker state.
        if (getpid() != device_launch_pid) { errno = EPIPE; goto fail; }
        char *end = NULL;
        errno = 0;
        long fd = strtol(inherited_fd, &end, 10);
        if (errno || end == inherited_fd || *end || fd < 0 || fd > INT_MAX) {
            errno = EINVAL;
            goto fail;
        }
        pthread_mutex_lock(&device_lock);
        device_locked = 1;
        if (syscall(SYS_fstat, (int)fd, &broker_stat) < 0) goto fail;
        if (device_broker &&
            (device_broker_dev != broker_stat.st_dev || device_broker_ino != broker_stat.st_ino)) {
            if (device_broker_users) { errno = EBUSY; goto fail; }
            fauxdev_transport_destroy(device_broker);
            device_broker = NULL;
        }
        if (device_broker) {
            s->broker = device_broker;
            s->shared_broker = 1;
        } else {
            int broker_ready = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (broker_ready < 0) goto fail;
            s->broker = fauxdev_transport_from_fd((int)fd, broker_ready);
            int saved_errno = errno;
            syscall(SYS_close, broker_ready);
            errno = saved_errno;
            if (!s->broker) goto fail;
        }
    } else if (socket_path && *socket_path) {
        s->socket_path = strdup(socket_path);
        if (!s->socket_path) goto fail;
        s->broker = fauxdev_transport_connect(socket_path, readiness_fd);
        if (!s->broker) goto fail;
    }
    s->input_count = 1;
    if (s->broker) {
        struct fauxdev_message response;
        if (fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_INPUTS,
                NULL, 0, NULL, 0, &response) == 0) {
            if (response.payload_size < offsetof(struct fauxdev_camera_inputs_reply, inputs) ||
                response.fd_count) {
                fauxdev_message_close_fds(&response);
                errno = EPROTO;
                goto fail;
            }
            uint32_t header[2];
            memcpy(header, response.payload, sizeof(header));
            unsigned count = le32toh(header[0]), selected = le32toh(header[1]);
            if (count > FAUXDEV_CAMERA_MAX_INPUTS ||
                response.payload_size != offsetof(struct fauxdev_camera_inputs_reply, inputs) +
                                         count * sizeof(struct fauxdev_camera_input) ||
                (count && selected >= count)) {
                fauxdev_message_close_fds(&response);
                errno = EPROTO;
                goto fail;
            }
            s->inputs = calloc(count ? count : 1, sizeof(*s->inputs));
            if (!s->inputs) goto fail;
            memcpy(s->inputs, response.payload + offsetof(struct fauxdev_camera_inputs_reply, inputs),
                   count * sizeof(*s->inputs));
            for (unsigned i = 0; i < count; ++i)
                if (!memchr(s->inputs[i].id, '\0', sizeof(s->inputs[i].id))) {
                    errno = EPROTO;
                    goto fail;
                }
            s->input_count = count;
            s->selected_input = selected;
            s->camera_selection_supported = 1;
        } else if (errno != ENOTTY) goto fail;
    }
    if (load_modes(s->camera_selection_supported && s->input_count ? s->broker : NULL,
                   s->selected_input, &s->modes, &s->mode_count,
                   &s->mode_query_supported) < 0) goto fail;
    struct fauxdev_camera_config initial_config;
    int config_supported;
    if (get_config(s, &initial_config, &config_supported) < 0) goto fail;
    s->config_supported = config_supported;
    if (s->config_supported && sync_config(s) < 0) goto fail;
    if (device_locked) {
        if (s->camera_selection_supported) {
            int fd = (int)strtol(inherited_fd, NULL, 10);
            int fd_flags = syscall(SYS_fcntl, fd, F_GETFD, 0);
            if (fd_flags < 0 || syscall(SYS_fcntl, fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
                goto fail;
            if (!device_broker) {
                device_broker = s->broker;
                device_broker_dev = broker_stat.st_dev;
                device_broker_ino = broker_stat.st_ino;
            }
            s->shared_broker = 1;
            device_broker_users++;
        } else {
            // Legacy brokers cannot stop a stream without closing their connection.
            syscall(SYS_close, (int)strtol(inherited_fd, NULL, 10));
        }
        pthread_mutex_unlock(&device_lock);
    }
    pthread_mutex_lock(&device_lock);
    s->device_next = device_sessions;
    device_sessions = s;
    pthread_mutex_unlock(&device_lock);
    *data = s;
    return 0;
fail: {
        int saved_errno = errno;
        if (device_locked) pthread_mutex_unlock(&device_lock);
        free(s->pixels);
        if (!s->shared_broker) fauxdev_transport_destroy(s->broker);
        free(s->inputs);
        free(s->modes);
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

static void fill_format(struct v4l2_format *f, uint32_t width, uint32_t height) {
    memset(&f->fmt, 0, sizeof(f->fmt));
    f->fmt.pix.width = width;
    f->fmt.pix.height = height;
    f->fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    f->fmt.pix.field = V4L2_FIELD_NONE;
    f->fmt.pix.bytesperline = width * 3;
    f->fmt.pix.sizeimage = width * height * 3;
    f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    f->fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
}

static void fill_capture_parm(struct v4l2_streamparm *parm, uint32_t fps) {
    memset(&parm->parm, 0, sizeof(parm->parm));
    parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    parm->parm.capture.timeperframe.numerator = 1;
    parm->parm.capture.timeperframe.denominator = fps;
}

static struct fauxdev_camera_mode nearest_mode(const struct video_session *s,
                                                 uint32_t width, uint32_t height) {
    struct fauxdev_camera_mode best = s->modes[0];
    uint64_t best_distance = UINT64_MAX, best_area = UINT64_MAX;
    for (unsigned int i = 0; i < s->mode_count; ++i) {
        const struct fauxdev_camera_mode mode = s->modes[i];
        uint64_t dx = mode.width > width ? mode.width - (uint64_t)width : width - (uint64_t)mode.width;
        uint64_t dy = mode.height > height ? mode.height - (uint64_t)height : height - (uint64_t)mode.height;
        uint64_t distance = dx + dy, area = (uint64_t)mode.width * mode.height;
        if (distance < best_distance ||
            (distance == best_distance && (area < best_area ||
             (area == best_area && (mode.width < best.width ||
              (mode.width == best.width && mode.height < best.height)))))) {
            best = mode;
            best_distance = distance;
            best_area = area;
        }
    }
    return best;
}

static void describe_buffer(struct video_session *s, struct v4l2_buffer *b) {
    const unsigned int index = b->index;
    memset(b, 0, sizeof(*b));
    b->index = index;
    b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->memory = V4L2_MEMORY_MMAP;
    b->field = V4L2_FIELD_NONE;
    b->length = s->buffer_length[index];
    b->m.offset = s->buffer_offset[index];
    b->flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    if (s->state[index] == QUEUED || s->state[index] == CAPTURING)
        b->flags |= V4L2_BUF_FLAG_QUEUED;
    if (s->state[index] == DONE) b->flags |= V4L2_BUF_FLAG_DONE;
}

static int capture_slot(struct video_session *s, unsigned int sequence, unsigned char *pixels) {
    if (s->broker) {
        int staging = syscall(SYS_memfd_create, "fauxdev-frame-staging", MFD_CLOEXEC);
        if (staging < 0) return -1;
        if (ftruncate(staging, s->frame_size) < 0) {
            syscall(SYS_close, staging);
            return -1;
        }
        const struct fauxdev_video_frame_request request = {
            .offset = htole32(0),
            .width = htole32(s->width),
            .height = htole32(s->height),
            .stride = htole32(s->width * 3),
            .pixel_format = htole32(V4L2_PIX_FMT_RGB24),
            .size = htole32((uint32_t)s->frame_size),
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
        while (done < s->frame_size) {
            ssize_t got = pread(staging, pixels + done, s->frame_size - done, done);
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
        for (unsigned int y = 0; y < s->height; y++) {
            for (unsigned int x = 0; x < s->width; x++) {
                const size_t at = ((size_t)y * s->width + x) * 3;
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
        while (done < s->frame_size) {
            ssize_t written = pwrite(s->memfd, s->pixels + done, s->frame_size - done,
                                     s->buffer_offset[index] + done);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            done += written;
        }
        if (done != s->frame_size) {
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

static int stop_worker(struct video_session *s, int release) {
    // Legacy brokers have no STOP request, so close their connection between streams.
    const int discard_broker = (release && !s->shared_broker) || !s->camera_selection_supported;
    pthread_mutex_lock(&s->lock);
    s->streaming = 0;
    s->stream_generation++;
    s->worker_stop = 1;
    pthread_cond_broadcast(&s->work_ready);
    struct fauxdev_transport *broker = s->broker;
    const int started = s->worker_started;
    pthread_mutex_unlock(&s->lock);
    // shortcut: STREAMOFF may wait for one in-flight frame (at most the transport timeout); a cancellable request would reduce this latency.
    if (discard_broker) fauxdev_transport_cancel(broker);
    if (started) pthread_join(s->worker, NULL);
    int result = 0, saved_errno = 0;
    if (!discard_broker && s->camera_started && broker) {
        struct fauxdev_message response;
        if (fauxdev_transport_request(broker, FAUXDEV_OPCODE_CAMERA_STOP,
                NULL, 0, NULL, 0, &response) < 0) { result = -1; saved_errno = errno; }
        else if (response.payload_size || response.fd_count) {
            fauxdev_message_close_fds(&response);
            result = -1;
            saved_errno = EPROTO;
        }
        if (result < 0) fauxdev_transport_cancel(broker);
    }
    pthread_mutex_lock(&s->lock);
    if (discard_broker) s->broker = NULL;
    s->worker_started = 0;
    s->worker_stop = 0;
    s->camera_started = 0;
    s->broker_failed = 0;
    s->done_count = 0;
    memset(s->state, 0, sizeof(s->state));
    pthread_mutex_unlock(&s->lock);
    if (discard_broker) fauxdev_transport_destroy(broker);
    pthread_mutex_lock(&s->lock);
    drain(s);
    pthread_mutex_unlock(&s->lock);
    if (result < 0) errno = saved_errno;
    return result;
}

static void video_release(void *data) {
    struct video_session *s = data;
    stop_worker(s, 1);
    pthread_mutex_lock(&device_lock);
    if (s->queue_leased && s->broker && queue_request(s, 0) < 0)
        fauxdev_transport_cancel(s->broker);
    if (queue_owner == s) queue_owner = NULL;
    if (s->shared_broker) device_broker_users--;
    for (struct video_session **cursor = &device_sessions; *cursor; cursor = &(*cursor)->device_next) {
        if (*cursor == s) { *cursor = s->device_next; break; }
    }
    if (s->config_supported && s->broker && !fauxdev_transport_disconnected(s->broker)) {
        enum v4l2_priority remaining = highest_broker_priority_locked(s->broker, NULL);
        if (broker_priority(s, 1, remaining, 0, NULL) < 0)
            fauxdev_transport_cancel(s->broker);
    }
    pthread_mutex_unlock(&device_lock);
    if (s->memfd >= 0) syscall(SYS_close, s->memfd);
    syscall(SYS_close, s->readiness_fd);
    free(s->pixels);
    free(s->inputs);
    free(s->modes);
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
    if (request == VIDIOC_LOG_STATUS) {
        pthread_mutex_lock(&s->lock);
        fprintf(stderr, "fauxdev video: input=%u buffers=%u streaming=%d broker=%s\n",
                s->selected_input, s->count, s->streaming,
                !s->broker ? "none" : fauxdev_transport_disconnected(s->broker) ? "disconnected" : "connected");
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    if (!argument) {
        switch (request) {
        case VIDIOC_G_INPUT: case VIDIOC_ENUMINPUT: case VIDIOC_S_INPUT:
        case VIDIOC_QUERYCAP: case VIDIOC_ENUM_FMT:
        case VIDIOC_ENUM_FRAMESIZES:
        case VIDIOC_ENUM_FRAMEINTERVALS: case VIDIOC_G_PARM: case VIDIOC_S_PARM:
        case VIDIOC_G_FMT: case VIDIOC_TRY_FMT: case VIDIOC_S_FMT:
        case VIDIOC_REQBUFS: case VIDIOC_CREATE_BUFS:
        case VIDIOC_QUERYBUF: case VIDIOC_QBUF:
        case VIDIOC_STREAMON: case VIDIOC_DQBUF: case VIDIOC_STREAMOFF:
        case VIDIOC_G_PRIORITY: case VIDIOC_S_PRIORITY:
            errno = EFAULT; break;
        default:
            errno = ENOTTY;
        }
        return -1;
    }
    const int lifecycle = request == VIDIOC_S_INPUT || request == VIDIOC_S_PARM ||
                          request == VIDIOC_QBUF ||
                          request == VIDIOC_REQBUFS || request == VIDIOC_CREATE_BUFS ||
                          request == VIDIOC_STREAMON || request == VIDIOC_STREAMOFF;
    if (lifecycle) pthread_mutex_lock(&s->lifecycle_lock);
    pthread_mutex_lock(&s->lock);
    int result = 0;
    switch (request) {
    case VIDIOC_G_PRIORITY:
        pthread_mutex_lock(&device_lock);
        if (s->config_supported) {
            if (broker_priority(s, 0, 0, 0, argument) < 0) result = -1;
        } else *(enum v4l2_priority *)argument = highest_priority_locked(NULL);
        pthread_mutex_unlock(&device_lock);
        break;
    case VIDIOC_S_PRIORITY: {
        enum v4l2_priority priority = *(enum v4l2_priority *)argument;
        if (priority < V4L2_PRIORITY_BACKGROUND || priority > V4L2_PRIORITY_RECORD) {
            errno = EINVAL; result = -1; break;
        }
        pthread_mutex_lock(&device_lock);
        if (priority < highest_priority_locked(s)) {
            errno = EBUSY; result = -1;
        } else {
            enum v4l2_priority effective = highest_broker_priority_locked(s->broker, s);
            if (priority > effective) effective = priority;
            if (s->config_supported && broker_priority(s, 1, effective, 1, NULL) < 0)
                result = -1;
            else s->priority = priority;
        }
        pthread_mutex_unlock(&device_lock);
        break;
    }
    case VIDIOC_G_INPUT:
        if (sync_config(s) < 0) { result = -1; break; }
        if (!s->input_count) { errno = EINVAL; result = -1; break; }
        *(int *)argument = (int)s->selected_input;
        break;
    case VIDIOC_ENUMINPUT: {
        struct v4l2_input *input = argument;
        if (input->index >= s->input_count) { errno = EINVAL; result = -1; break; }
        const unsigned index = input->index;
        memset(input, 0, sizeof(*input));
        input->index = index;
        if (s->inputs) {
            uint32_t facing = le32toh(s->inputs[index].facing);
            const char *label = facing == 0 ? "Front" : facing == 1 ? "Rear" :
                                facing == 2 ? "External" : "Unknown";
            snprintf((char *)input->name, sizeof(input->name), "%s camera %.15s", label,
                     s->inputs[index].id);
        } else memcpy(input->name, s->broker ? "Camera2 preview" : "fauxdev test pattern",
                      s->broker ? 16 : 21);
        input->type = V4L2_INPUT_TYPE_CAMERA;
        break;
    }
    case VIDIOC_S_INPUT: {
        int index = *(int *)argument;
        if (s->count || s->streaming) { errno = EBUSY; result = -1; break; }
        if (sync_config(s) < 0) { result = -1; break; }
        pthread_mutex_lock(&device_lock);
        const int lower_priority = s->priority < highest_priority_locked(s);
        pthread_mutex_unlock(&device_lock);
        if (lower_priority) { errno = EBUSY; result = -1; break; }
        if (s->broker && fauxdev_transport_disconnected(s->broker)) {
            errno = EPIPE; result = -1; break;
        }
        if (index < 0 || (unsigned)index >= s->input_count) { errno = EINVAL; result = -1; break; }
        struct fauxdev_camera_mode *modes = NULL;
        unsigned int mode_count = 0;
        int mode_query_supported = 0;
        if (load_modes(s->camera_selection_supported ? s->broker : NULL, (unsigned int)index,
                       &modes, &mode_count, &mode_query_supported) < 0) {
            result = -1; break;
        }
        unsigned char *pixels = malloc(FRAME_SIZE);
        if (!pixels) { free(modes); result = -1; break; }
        uint64_t generation = s->config_generation;
        if (s->config_supported && set_config(s, (uint32_t)index, WIDTH, HEIGHT, &generation) < 0) {
            int saved_errno = errno;
            free(modes);
            free(pixels);
            if (saved_errno == EAGAIN) (void)sync_config(s);
            errno = saved_errno;
            result = -1;
            break;
        }
        free(s->modes);
        free(s->pixels);
        s->modes = modes;
        s->mode_count = mode_count;
        s->mode_query_supported = mode_query_supported;
        s->pixels = pixels;
        s->width = WIDTH;
        s->height = HEIGHT;
        s->frame_size = FRAME_SIZE;
        s->slot_size = (FRAME_SIZE + s->page_size - 1) / s->page_size * s->page_size;
        s->selected_input = (unsigned)index;
        s->config_generation = generation;
        break;
    }
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *cap = argument;
        memset(cap, 0, sizeof(*cap));
        memcpy(cap->driver, "fauxdev", 8);
        memcpy(cap->card, s->broker ? "Camera2 preview" : "fauxdev test pattern",
               s->broker ? 16 : 21);
        memcpy(cap->bus_info, "platform:fauxdev", 17);
        // Userspace emulates the V4L2 3.1 API baseline, not the Android kernel or app version.
        cap->version = (3u << 16) | (1u << 8);
        cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_EXT_PIX_FORMAT;
        cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
        break;
    }
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *desc = argument;
        if (desc->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || desc->index) { errno = EINVAL; result = -1; break; }
        memset(desc->reserved, 0, sizeof(desc->reserved));
        desc->flags = 0;
        desc->pixelformat = V4L2_PIX_FMT_RGB24;
        memset(desc->description, 0, sizeof(desc->description));
        memcpy(desc->description, "24-bit RGB 8-8-8", 17);
        break;
    }
    case VIDIOC_ENUM_FRAMESIZES: {
        struct v4l2_frmsizeenum *size = argument;
        if (sync_config(s) < 0) { result = -1; break; }
        if (size->index >= s->mode_count || size->pixel_format != V4L2_PIX_FMT_RGB24) {
            errno = EINVAL; result = -1; break;
        }
        size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        size->discrete.width = s->modes[size->index].width;
        size->discrete.height = s->modes[size->index].height;
        memset(size->reserved, 0, sizeof(size->reserved));
        break;
    }
    case VIDIOC_ENUM_FRAMEINTERVALS: {
        struct v4l2_frmivalenum *interval = argument;
        if (sync_config(s) < 0) { result = -1; break; }
        int valid = 0;
        for (unsigned int i = 0; i < s->mode_count; ++i)
            if (s->modes[i].width == interval->width &&
                s->modes[i].height == interval->height) valid = 1;
        if (!valid || interval->pixel_format != V4L2_PIX_FMT_RGB24) {
            errno = EINVAL; result = -1; break;
        }
        uint32_t rates[MAX_CAMERA_RATES];
        unsigned int count;
        int supported;
        if (load_rates(s, s->selected_input, interval->width, interval->height,
                       rates, &count, &supported) < 0) { result = -1; break; }
        if (!supported) { errno = ENOTTY; result = -1; break; }
        if (interval->index >= count) { errno = EINVAL; result = -1; break; }
        interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;
        interval->discrete.numerator = 1;
        interval->discrete.denominator = rates[interval->index];
        memset(interval->reserved, 0, sizeof(interval->reserved));
        break;
    }
    case VIDIOC_G_PARM:
    case VIDIOC_S_PARM: {
        struct v4l2_streamparm *parm = argument;
        if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            errno = EINVAL; result = -1; break;
        }
        if (sync_config(s) < 0) { result = -1; break; }
        if (request == VIDIOC_G_PARM) {
            struct fauxdev_camera_rate rate;
            int supported;
            if (get_rate(s, &rate, &supported) < 0) { result = -1; break; }
            if (!supported || !rate.fps) { errno = ENOTTY; result = -1; break; }
            if (rate.generation != s->config_generation) {
                if (sync_config(s) < 0 || get_rate(s, &rate, &supported) < 0) {
                    result = -1; break;
                }
                if (!supported || !rate.fps || rate.generation != s->config_generation) {
                    errno = EAGAIN; result = -1; break;
                }
            }
            fill_capture_parm(parm, rate.fps);
            break;
        }
        if (s->count || s->queue_leased || s->streaming) {
            errno = EBUSY; result = -1; break;
        }
        uint32_t rates[MAX_CAMERA_RATES];
        unsigned int count;
        int supported;
        if (load_rates(s, s->selected_input, s->width, s->height,
                       rates, &count, &supported) < 0) { result = -1; break; }
        if (!supported || !count) { errno = ENOTTY; result = -1; break; }
        uint32_t numerator = parm->parm.capture.timeperframe.numerator;
        uint32_t denominator = parm->parm.capture.timeperframe.denominator;
        uint32_t fps = nominal_fps(rates, count);
        if (numerator && denominator) {
            uint64_t best_error = UINT64_MAX;
            for (unsigned int i = 0; i < count; ++i) {
                uint64_t product = (uint64_t)numerator * rates[i];
                uint64_t error = product > denominator ? product - denominator :
                                 denominator - product;
                if (best_error == UINT64_MAX ||
                    error * fps < best_error * rates[i]) {
                    fps = rates[i];
                    best_error = error;
                }
            }
        }
        pthread_mutex_lock(&device_lock);
        int lower_priority = s->priority < highest_priority_locked(s);
        pthread_mutex_unlock(&device_lock);
        if (lower_priority) { errno = EBUSY; result = -1; break; }
        uint64_t generation;
        if (set_rate(s, fps, &generation) < 0) {
            int saved_errno = errno;
            if (saved_errno == EAGAIN) (void)sync_config(s);
            errno = saved_errno; result = -1; break;
        }
        s->config_generation = generation;
        fill_capture_parm(parm, fps);
        break;
    }
    case VIDIOC_G_FMT:
    case VIDIOC_TRY_FMT:
    case VIDIOC_S_FMT: {
        struct v4l2_format *f = argument;
        if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            (request == VIDIOC_S_FMT && s->count)) { errno = request == VIDIOC_S_FMT && s->count ? EBUSY : EINVAL; result = -1; break; }
        if (sync_config(s) < 0) { result = -1; break; }
        if (request == VIDIOC_G_FMT) { fill_format(f, s->width, s->height); break; }
        struct fauxdev_camera_mode chosen = nearest_mode(s, f->fmt.pix.width, f->fmt.pix.height);
        if (request == VIDIOC_S_FMT) {
            pthread_mutex_lock(&device_lock);
            const int lower_priority = s->priority < highest_priority_locked(s);
            pthread_mutex_unlock(&device_lock);
            if (lower_priority) { errno = EBUSY; result = -1; break; }
            size_t frame_size = (size_t)chosen.width * chosen.height * 3;
            size_t slot_size = (frame_size + s->page_size - 1) / s->page_size * s->page_size;
            if (slot_size > MAX_BACKING_SIZE / 2) { errno = EOVERFLOW; result = -1; break; }
            unsigned char *pixels = malloc(frame_size);
            if (!pixels) { result = -1; break; }
            uint64_t generation = s->config_generation;
            if (s->config_supported && set_config(s, s->selected_input,
                    chosen.width, chosen.height, &generation) < 0) {
                int saved_errno = errno;
                free(pixels);
                if (saved_errno == EAGAIN) (void)sync_config(s);
                errno = saved_errno;
                result = -1;
                break;
            }
            free(s->pixels);
            s->pixels = pixels;
            s->width = chosen.width;
            s->height = chosen.height;
            s->frame_size = frame_size;
            s->slot_size = slot_size;
            s->config_generation = generation;
        }
        fill_format(f, chosen.width, chosen.height);
        break;
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *req = argument;
        if (req->count && s->camera_selection_supported && !s->input_count) {
            errno = ENODEV; result = -1; break;
        }
        if (req->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || req->memory != V4L2_MEMORY_MMAP) {
            errno = EINVAL; result = -1; break;
        }
        if (req->count && !s->count && sync_config(s) < 0) { result = -1; break; }
        pthread_mutex_lock(&device_lock);
        const int denied = s->priority < highest_priority_locked(s) ||
                           (queue_owner && queue_owner != s);
        pthread_mutex_unlock(&device_lock);
        if (denied) { errno = EBUSY; result = -1; break; }
        if (s->streaming) {
            pthread_mutex_unlock(&s->lock);
            result = stop_worker(s, 0);
            pthread_mutex_lock(&s->lock);
            if (result < 0) break;
        }
        pthread_mutex_lock(&device_lock);
        if (s->priority < highest_priority_locked(s)) {
            pthread_mutex_unlock(&device_lock);
            errno = EBUSY; result = -1; break;
        }
        if (queue_owner && queue_owner != s) {
            pthread_mutex_unlock(&device_lock);
            errno = EBUSY; result = -1; break;
        }
        unsigned int max_count = MAX_BACKING_SIZE / s->slot_size;
        if (max_count > VIDEO_MAX_FRAME) max_count = VIDEO_MAX_FRAME;
        unsigned int count = req->count > max_count ? max_count : req->count;
        int memfd = -1;
        if (count) {
            memfd = syscall(SYS_memfd_create, "fauxdev-video", MFD_CLOEXEC);
            if (memfd < 0 || ftruncate(memfd, count * s->slot_size) < 0) {
                if (memfd >= 0) syscall(SYS_close, memfd);
                pthread_mutex_unlock(&device_lock);
                result = -1; break;
            }
        }
        if (s->config_supported && ((count && !s->queue_leased) ||
                                    (!count && s->queue_leased))) {
            if (queue_request(s, !!count) < 0) {
                int saved_errno = errno;
                if (memfd >= 0) syscall(SYS_close, memfd);
                pthread_mutex_unlock(&device_lock);
                if (saved_errno == EAGAIN) (void)sync_config(s);
                errno = saved_errno;
                result = -1; break;
            }
        }
        if (s->memfd >= 0) syscall(SYS_close, s->memfd);
        s->memfd = memfd;
        s->count = count;
        s->backing_size = count * s->slot_size;
        for (unsigned int i = 0; i < count; ++i) {
            s->buffer_offset[i] = i * s->slot_size;
            s->buffer_length[i] = (uint32_t)s->frame_size;
        }
        queue_owner = count ? s : NULL;
        s->done_count = 0;
        memset(s->state, 0, sizeof(s->state));
        drain(s);
        req->count = count;
        req->flags = 0;
        req->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP | V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS;
        pthread_mutex_unlock(&device_lock);
        break;
    }
    case VIDIOC_CREATE_BUFS: {
        struct v4l2_create_buffers *created = argument;
        if (created->count && s->camera_selection_supported && !s->input_count) {
            errno = ENODEV; result = -1; break;
        }
        if (created->memory != V4L2_MEMORY_MMAP ||
            created->format.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            errno = EINVAL; result = -1; break;
        }
        if (!created->count) {
            created->index = s->count;
            created->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP |
                                    V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS;
            created->flags = 0;
            created->max_num_buffers = 0;
            memset(created->reserved, 0, sizeof(created->reserved));
            break;
        }
        if (!s->count && sync_config(s) < 0) { result = -1; break; }
        const struct v4l2_pix_format *format = &created->format.fmt.pix;
        if (format->width != s->width || format->height != s->height ||
            format->pixelformat != V4L2_PIX_FMT_RGB24 || format->field != V4L2_FIELD_NONE ||
            format->sizeimage < s->frame_size || format->sizeimage > MAX_BACKING_SIZE) {
            errno = EINVAL; result = -1; break;
        }
        pthread_mutex_lock(&device_lock);
        if (s->priority < highest_priority_locked(s) ||
            (queue_owner && queue_owner != s)) {
            pthread_mutex_unlock(&device_lock);
            errno = EBUSY; result = -1; break;
        }
        const unsigned int first = s->count;
        size_t end = s->backing_size;
        unsigned int allocated = 0;
        uint32_t offsets[VIDEO_MAX_FRAME];
        while (allocated < created->count && first + allocated < VIDEO_MAX_FRAME) {
            if (end > MAX_BACKING_SIZE - format->sizeimage) break;
            offsets[allocated++] = (uint32_t)end;
            end += format->sizeimage;
            end = (end + s->page_size - 1) / s->page_size * s->page_size;
            if (end > MAX_BACKING_SIZE) { allocated--; break; }
        }
        if (!allocated) {
            pthread_mutex_unlock(&device_lock);
            errno = ENOBUFS; result = -1; break;
        }
        int memfd = s->memfd;
        if (memfd < 0) memfd = syscall(SYS_memfd_create, "fauxdev-video", MFD_CLOEXEC);
        if (memfd < 0 || ftruncate(memfd, end) < 0) {
            if (s->memfd < 0 && memfd >= 0) syscall(SYS_close, memfd);
            pthread_mutex_unlock(&device_lock);
            result = -1; break;
        }
        if (s->config_supported && !s->queue_leased && queue_request(s, 1) < 0) {
            int saved_errno = errno;
            if (s->memfd < 0) syscall(SYS_close, memfd);
            pthread_mutex_unlock(&device_lock);
            if (saved_errno == EAGAIN) (void)sync_config(s);
            errno = saved_errno;
            result = -1; break;
        }
        s->memfd = memfd;
        for (unsigned int i = 0; i < allocated; ++i) {
            s->buffer_offset[first + i] = offsets[i];
            s->buffer_length[first + i] = format->sizeimage;
        }
        s->count += allocated;
        s->backing_size = end;
        queue_owner = s;
        created->index = first;
        created->count = allocated;
        created->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP |
                                V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS;
        created->flags = 0;
        created->max_num_buffers = 0;
        memset(created->reserved, 0, sizeof(created->reserved));
        pthread_mutex_unlock(&device_lock);
        break;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *b = argument;
        if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->index >= s->count) {
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
        if (s->camera_selection_supported && !s->input_count) {
            errno = ENODEV; result = -1; break;
        }
        if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE || !s->count) { errno = EINVAL; result = -1; break; }
        if (s->config_supported && !s->queue_leased) { errno = EBUSY; result = -1; break; }
        if (s->streaming) break;
        if (s->camera_selection_supported) {
            if (!s->input_count) { errno = ENODEV; result = -1; break; }
            if (s->start_v2_unsupported &&
                (s->width != WIDTH || s->height != HEIGHT)) {
                errno = ENOTTY; result = -1; break;
            }
            struct fauxdev_message response;
            struct fauxdev_camera_start_v2_request start_v2 = {
                .index = htole32(s->selected_input), .width = htole32(s->width),
                .height = htole32(s->height),
            };
            struct fauxdev_camera_start_request start_v1 = {.index = htole32(s->selected_input)};
            int use_v2 = s->mode_query_supported && !s->start_v2_unsupported;
            int started = use_v2 ?
                fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_START_V2,
                    &start_v2, sizeof(start_v2), NULL, 0, &response) :
                fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_START,
                    &start_v1, sizeof(start_v1), NULL, 0, &response);
            if (started < 0 && use_v2 && errno == ENOTTY) {
                s->start_v2_unsupported = 1;
                if (s->width == WIDTH && s->height == HEIGHT)
                    started = fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_START,
                        &start_v1, sizeof(start_v1), NULL, 0, &response);
            }
            if (started < 0) {
                int saved_errno = errno;
                // A timed-out START may still open later; disconnect so the broker releases it.
                if (fauxdev_transport_disconnected(s->broker)) fauxdev_transport_cancel(s->broker);
                errno = saved_errno;
                result = -1;
                break;
            }
            if (response.payload_size || response.fd_count) {
                fauxdev_message_close_fds(&response);
                fauxdev_transport_cancel(s->broker);
                errno = EPROTO;
                result = -1;
                break;
            }
            s->camera_started = 1;
        }
        s->streaming = 1;
        s->broker_failed = 0;
        int error = pthread_create(&s->worker, NULL, produce_frames, s);
        if (error) {
            s->streaming = 0;
            if (s->camera_started) {
                struct fauxdev_message response;
                if (fauxdev_transport_request(s->broker, FAUXDEV_OPCODE_CAMERA_STOP,
                        NULL, 0, NULL, 0, &response) < 0) fauxdev_transport_cancel(s->broker);
                else if (response.payload_size || response.fd_count) {
                    fauxdev_message_close_fds(&response);
                    fauxdev_transport_cancel(s->broker);
                }
                s->camera_started = 0;
            }
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
        memmove(&s->order[0], &s->order[1], (s->done_count - 1) * sizeof(s->order[0]));
        s->done_count--;
        uint64_t token;
        if (syscall(SYS_read, s->readiness_fd, &token, sizeof(token)) != sizeof(token)) { result = -1; break; }
        s->state[index] = FREE;
        b->index = index;
        describe_buffer(s, b);
        b->bytesused = (uint32_t)s->frame_size;
        b->sequence = s->frame_sequence[index];
        b->timestamp = s->frame_timestamp[index];
        b->flags |= V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
        break;
    }
    case VIDIOC_STREAMOFF: {
        enum v4l2_buf_type *type = argument;
        if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE) { errno = EINVAL; result = -1; break; }
        pthread_mutex_unlock(&s->lock);
        result = stop_worker(s, 0);
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
    unsigned int index = 0;
    while (index < s->count && (off_t)s->buffer_offset[index] != offset) ++index;
    if (s->memfd < 0 || offset < 0 || index == s->count ||
        length != s->buffer_length[index] || !(flags & MAP_SHARED)) {
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
