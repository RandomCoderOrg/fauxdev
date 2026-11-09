#include "transport.h"
#include "yuv_rgb.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { OUTPUT_WIDTH = 320, OUTPUT_HEIGHT = 240, OUTPUT_SIZE = OUTPUT_WIDTH * OUTPUT_HEIGHT * 3 };

struct camera {
    ACameraManager *manager;
    ACameraDevice *device;
    AImageReader *reader;
    ACaptureSessionOutputContainer *outputs;
    ACaptureSessionOutput *output;
    ACameraOutputTarget *target;
    ACaptureRequest *request;
    ACameraCaptureSession *session;
    atomic_bool failed;
    atomic_bool reader_stop;
    pthread_t reader_thread;
    bool reader_started;
    pthread_mutex_t frame_lock;
    bool frame_lock_ready;
    uint8_t latest_frame[OUTPUT_SIZE];
    uint8_t *transfer_frame;
    uint64_t frame_count;
    uint64_t last_served_frame;
};

static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static int server_state; // 0 idle, 1 running, 2 stop requested, 3 prepared
static int server_stop_fd = -1;
static int server_client_fd = -1;

static int stop_requested(void) {
    pthread_mutex_lock(&server_lock);
    const int stopped = server_state == 2;
    pthread_mutex_unlock(&server_lock);
    return stopped;
}

static void on_device_disconnected(void *context, ACameraDevice *device) {
    (void)device;
    atomic_store(&((struct camera *)context)->failed, true);
}

static void on_device_error(void *context, ACameraDevice *device, int error) {
    (void)device;
    fprintf(stderr, "fauxdev camera device error: %d\n", error);
    atomic_store(&((struct camera *)context)->failed, true);
}

static int64_t monotonic_ms(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) return -1;
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int choose_camera(ACameraManager *manager, char **id, int *width, int *height) {
    ACameraIdList *ids = NULL;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK) return -EIO;
    int best_score = INT_MIN;
    for (int i = 0; i < ids->numCameras; i++) {
        ACameraMetadata *metadata = NULL;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) != ACAMERA_OK)
            continue;
        ACameraMetadata_const_entry facing;
        const int back = ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &facing) == ACAMERA_OK &&
                         facing.count && facing.data.u8[0] == ACAMERA_LENS_FACING_BACK;
        ACameraMetadata_const_entry configs;
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                          &configs) == ACAMERA_OK) {
            for (uint32_t j = 0; j + 3 < configs.count; j += 4) {
                const int *data = configs.data.i32 + j;
                const int w = data[1], h = data[2];
                if (data[0] != AIMAGE_FORMAT_YUV_420_888 ||
                    data[3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT ||
                    w < OUTPUT_WIDTH || h < OUTPUT_HEIGHT || w > 1920 || h > 1080)
                    continue;
                const int exact = w == 640 && h == 480;
                const int aspect = abs(w * 3 - h * 4) < w / 8;
                const int score = back * 100000000 + exact * 10000000 + aspect * 1000000 - w * h;
                if (score <= best_score) continue;
                char *copy = strdup(ids->cameraIds[i]);
                if (!copy) { ACameraMetadata_free(metadata); ACameraManager_deleteCameraIdList(ids); return -ENOMEM; }
                free(*id);
                *id = copy;
                *width = w;
                *height = h;
                best_score = score;
            }
        }
        ACameraMetadata_free(metadata);
    }
    ACameraManager_deleteCameraIdList(ids);
    return *id ? 0 : -ENODEV;
}

static void close_camera(struct camera *camera) {
    atomic_store(&camera->reader_stop, true);
    if (camera->session) ACameraCaptureSession_stopRepeating(camera->session);
    if (camera->reader_started) pthread_join(camera->reader_thread, NULL);
    if (camera->session) ACameraCaptureSession_close(camera->session);
    if (camera->request) ACaptureRequest_free(camera->request);
    if (camera->target) ACameraOutputTarget_free(camera->target);
    if (camera->output) ACaptureSessionOutput_free(camera->output);
    if (camera->outputs) ACaptureSessionOutputContainer_free(camera->outputs);
    if (camera->device) ACameraDevice_close(camera->device);
    if (camera->reader) AImageReader_delete(camera->reader);
    if (camera->manager) ACameraManager_delete(camera->manager);
    if (camera->frame_lock_ready) pthread_mutex_destroy(&camera->frame_lock);
    free(camera->transfer_frame);
}

static int convert_image(AImage *image, uint8_t output[OUTPUT_SIZE]) {
    int32_t width, height, count;
    if (AImage_getWidth(image, &width) != AMEDIA_OK ||
        AImage_getHeight(image, &height) != AMEDIA_OK ||
        AImage_getNumberOfPlanes(image, &count) != AMEDIA_OK || count != 3)
        return -EIO;
    struct fauxdev_plane planes[3] = {0};
    for (int i = 0; i < 3; i++) {
        uint8_t *data = NULL;
        int length = 0;
        if (AImage_getPlaneData(image, i, &data, &length) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image, i, &planes[i].row_stride) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, i, &planes[i].pixel_stride) != AMEDIA_OK || length < 0)
            return -EIO;
        planes[i].data = data;
        planes[i].length = (size_t)length;
    }
    return fauxdev_yuv420_to_rgb24(output, OUTPUT_WIDTH, OUTPUT_HEIGHT, width, height, planes);
}

static void *drain_frames(void *context) {
    struct camera *camera = context;
    uint8_t *pixels = malloc(OUTPUT_SIZE);
    if (!pixels) {
        atomic_store(&camera->failed, true);
        return NULL;
    }
    while (!atomic_load(&camera->reader_stop) && !atomic_load(&camera->failed)) {
        AImage *image = NULL;
        media_status_t status = AImageReader_acquireLatestImage(camera->reader, &image);
        if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            usleep(10000);
            continue;
        }
        if (status != AMEDIA_OK) {
            fprintf(stderr, "fauxdev camera image acquire error: %d\n", status);
            atomic_store(&camera->failed, true);
            break;
        }
        int result = convert_image(image, pixels);
        AImage_delete(image);
        if (result < 0) {
            fprintf(stderr, "fauxdev camera image conversion error: %d\n", result);
            atomic_store(&camera->failed, true);
            break;
        }
        pthread_mutex_lock(&camera->frame_lock);
        memcpy(camera->latest_frame, pixels, OUTPUT_SIZE);
        camera->frame_count++;
        pthread_mutex_unlock(&camera->frame_lock);
    }
    free(pixels);
    return NULL;
}

static int open_camera(struct camera *camera) {
    atomic_init(&camera->failed, false);
    atomic_init(&camera->reader_stop, false);
    if (pthread_mutex_init(&camera->frame_lock, NULL) != 0) return -ENOMEM;
    camera->frame_lock_ready = true;
    camera->transfer_frame = malloc(OUTPUT_SIZE);
    if (!camera->transfer_frame) return -ENOMEM;
    camera->manager = ACameraManager_create();
    if (!camera->manager) return -EIO;
    char *id = NULL;
    int width = 0, height = 0;
    int result = choose_camera(camera->manager, &id, &width, &height);
    if (result < 0) return result;
    fprintf(stderr, "fauxdev camera %s: %dx%d YUV -> %dx%d RGB24\n",
            id, width, height, OUTPUT_WIDTH, OUTPUT_HEIGHT);
    if (AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, 2, &camera->reader) != AMEDIA_OK) {
        free(id);
        return -EIO;
    }
    ANativeWindow *window = NULL;
    if (AImageReader_getWindow(camera->reader, &window) != AMEDIA_OK) { free(id); return -EIO; }
    ACameraDevice_StateCallbacks device_callbacks = {
        .context = camera,
        .onDisconnected = on_device_disconnected,
        .onError = on_device_error,
    };
    camera_status_t status = ACameraManager_openCamera(camera->manager, id, &device_callbacks, &camera->device);
    free(id);
    if (status != ACAMERA_OK) {
        fprintf(stderr, "fauxdev camera open failed: %d\n", status);
        return status == ACAMERA_ERROR_PERMISSION_DENIED ? -EACCES : -EIO;
    }
    if (ACaptureSessionOutputContainer_create(&camera->outputs) != ACAMERA_OK ||
        ACaptureSessionOutput_create(window, &camera->output) != ACAMERA_OK ||
        ACaptureSessionOutputContainer_add(camera->outputs, camera->output) != ACAMERA_OK ||
        ACameraOutputTarget_create(window, &camera->target) != ACAMERA_OK ||
        ACameraDevice_createCaptureRequest(camera->device, TEMPLATE_PREVIEW, &camera->request) != ACAMERA_OK ||
        ACaptureRequest_addTarget(camera->request, camera->target) != ACAMERA_OK)
        return -EIO;
    ACameraCaptureSession_stateCallbacks session_callbacks = {
        .context = camera,
    };
    status = ACameraDevice_createCaptureSession(camera->device, camera->outputs,
                                                 &session_callbacks, &camera->session);
    if (status != ACAMERA_OK) {
        fprintf(stderr, "fauxdev camera session failed: %d\n", status);
        return -EIO;
    }
    fprintf(stderr, "fauxdev camera session created at %lldms\n", (long long)monotonic_ms());
    ACaptureRequest *request = camera->request;
    status = ACameraCaptureSession_setRepeatingRequest(camera->session, NULL, 1, &request, NULL);
    if (status != ACAMERA_OK) {
        fprintf(stderr, "fauxdev camera repeating preview failed: %d\n", status);
        return -EIO;
    }
    if (pthread_create(&camera->reader_thread, NULL, drain_frames, camera) != 0) return -EIO;
    camera->reader_started = true;
    fprintf(stderr, "fauxdev camera preview started at %lldms\n", (long long)monotonic_ms());
    return atomic_load(&camera->failed) ? -EIO : 0;
}

static int peer_gone(int client) {
    struct pollfd watched = {.fd = client, .events = POLLRDHUP};
    return poll(&watched, 1, 0) > 0 &&
           (watched.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL));
}

static int capture_frame(struct camera *camera, int client, uint8_t output[OUTPUT_SIZE]) {
    const int64_t deadline = monotonic_ms() + 15000;
    while (monotonic_ms() < deadline && !atomic_load(&camera->failed) && !stop_requested()) {
        if (peer_gone(client)) return -ECANCELED;
        pthread_mutex_lock(&camera->frame_lock);
        // Let preview and auto-exposure settle before serving the first frame.
        uint64_t minimum = camera->last_served_frame ? camera->last_served_frame + 1 : 12;
        if (camera->frame_count >= minimum) {
            memcpy(output, camera->latest_frame, OUTPUT_SIZE);
            camera->last_served_frame = camera->frame_count;
            fprintf(stderr, "fauxdev camera serving preview frame %llu at %lldms\n",
                    (unsigned long long)camera->frame_count, (long long)monotonic_ms());
            pthread_mutex_unlock(&camera->frame_lock);
            return 0;
        }
        pthread_mutex_unlock(&camera->frame_lock);
        usleep(10000);
    }
    if (stop_requested()) return -ECANCELED;
    if (atomic_load(&camera->failed)) return -EIO;
    atomic_store(&camera->failed, true);
    fprintf(stderr, "fauxdev camera image timed out at %lldms\n", (long long)monotonic_ms());
    return -ETIMEDOUT;
}

static int capture_to_fd(struct camera *camera, int client, const struct fauxdev_message *message) {
    if (message->payload_size != sizeof(struct fauxdev_video_frame_request) || message->fd_count != 1)
        return -EINVAL;
    struct fauxdev_video_frame_request frame;
    memcpy(&frame, message->payload, sizeof(frame));
    const uint32_t offset = le32toh(frame.offset);
    if (le32toh(frame.width) != OUTPUT_WIDTH || le32toh(frame.height) != OUTPUT_HEIGHT ||
        le32toh(frame.stride) != OUTPUT_WIDTH * 3 ||
        le32toh(frame.pixel_format) != V4L2_PIX_FMT_RGB24 || le32toh(frame.size) != OUTPUT_SIZE)
        return -EINVAL;
    struct stat statbuf;
    if (fstat(message->fds[0], &statbuf) < 0) return -errno;
    if (!S_ISREG(statbuf.st_mode) || offset > (uint64_t)statbuf.st_size ||
        OUTPUT_SIZE > (uint64_t)statbuf.st_size - offset) return -EINVAL;
    uint8_t *pixels = camera->transfer_frame;
    int result = capture_frame(camera, client, pixels);
    if (result < 0) return result;
    if (peer_gone(client)) return -ECANCELED;
    size_t done = 0;
    while (done < OUTPUT_SIZE) {
        ssize_t written = pwrite(message->fds[0], pixels + done,
                                 OUTPUT_SIZE - done, offset + done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return written < 0 ? -errno : -EIO;
        done += (size_t)written;
    }
    return 0;
}

static int serve_client(int client, struct camera *camera) {
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) < 0 ||
        peer_size != sizeof(peer) || peer.uid != geteuid()) return -EACCES;
    struct fauxdev_message request;
    if (fauxdev_message_receive(client, &request) < 0) return -errno;
    if (request.flags != FAUXDEV_MESSAGE_REQUEST || request.opcode != FAUXDEV_OPCODE_HELLO ||
        request.payload_size || request.fd_count) {
        fauxdev_message_close_fds(&request);
        return -EPROTO;
    }
    struct fauxdev_message response = {
        .opcode = FAUXDEV_OPCODE_HELLO, .flags = FAUXDEV_MESSAGE_RESPONSE,
        .request_id = request.request_id,
    };
    if (fauxdev_message_send(client, &response) < 0) return -errno;
    for (;;) {
        if (fauxdev_message_receive(client, &request) < 0)
            return errno == ECONNRESET ? 0 : -errno;
        fprintf(stderr, "fauxdev camera request opcode=%u payload=%zu fds=%zu at %lldms\n",
                request.opcode, request.payload_size, request.fd_count, (long long)monotonic_ms());
        const int status = request.opcode == FAUXDEV_OPCODE_VIDEO_FRAME
            ? capture_to_fd(camera, client, &request) : -ENOTTY;
        fprintf(stderr, "fauxdev camera request result: %d at %lldms\n",
                status, (long long)monotonic_ms());
        fauxdev_message_close_fds(&request);
        response = (struct fauxdev_message){
            .opcode = request.opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
            .request_id = request.request_id, .status = status,
        };
        if (fauxdev_message_send(client, &response) < 0) return -errno;
        fprintf(stderr, "fauxdev camera reply sent at %lldms\n", (long long)monotonic_ms());
    }
}

static int run_server(const char *socket_path) {
    if (!socket_path || socket_path[0] != '/') return -EINVAL;
    pthread_mutex_lock(&server_lock);
    if (server_state == 2) {
        server_state = 0;
        pthread_mutex_unlock(&server_lock);
        return -ECANCELED;
    }
    if (server_state != 3) { pthread_mutex_unlock(&server_lock); return -EBUSY; }
    server_state = 1;
    pthread_mutex_unlock(&server_lock);

    struct camera *camera = calloc(1, sizeof(*camera));
    if (!camera) {
        pthread_mutex_lock(&server_lock);
        server_state = 0;
        pthread_mutex_unlock(&server_lock);
        return -ENOMEM;
    }
    int result = open_camera(camera);
    if (result < 0) {
        fprintf(stderr, "fauxdev camera unavailable: %d\n", result);
        goto finish_camera;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof(address.sun_path)) { result = -ENAMETOOLONG; goto finish_camera; }
    strcpy(address.sun_path, socket_path);
    struct stat existing;
    if (lstat(socket_path, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid() || unlink(socket_path) < 0) {
            result = -EACCES; goto finish_camera;
        }
    } else if (errno != ENOENT) { result = -errno; goto finish_camera; }
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0) { result = -errno; goto finish_camera; }
    if (bind(listener, (const struct sockaddr *)&address,
             offsetof(struct sockaddr_un, sun_path) + strlen(socket_path) + 1) < 0 ||
        chmod(socket_path, 0600) < 0 || listen(listener, 1) < 0) {
        result = -errno;
        close(listener);
        goto finish_socket;
    }
    int stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_fd < 0) { result = -errno; close(listener); goto finish_socket; }
    pthread_mutex_lock(&server_lock);
    server_stop_fd = stop_fd;
    const int already_stopped = server_state == 2;
    pthread_mutex_unlock(&server_lock);
    fprintf(stderr, "fauxdev camera broker listening: %s\n", socket_path);
    result = 0;
    if (!already_stopped) {
        struct pollfd watched[2] = {
            {.fd = listener, .events = POLLIN}, {.fd = stop_fd, .events = POLLIN},
        };
        while (!stop_requested()) {
            if (poll(watched, 2, -1) < 0) {
                if (errno == EINTR) continue;
                result = -errno;
                break;
            }
            if (watched[1].revents) break;
            if (!(watched[0].revents & POLLIN)) { result = -EIO; break; }
            int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            if (client < 0) { if (errno == EINTR) continue; result = -errno; break; }
            pthread_mutex_lock(&server_lock);
            server_client_fd = client;
            const int client_stopped = server_state == 2;
            pthread_mutex_unlock(&server_lock);
            if (!client_stopped) {
                int session_status = serve_client(client, camera);
                if (session_status < 0 && session_status != -ECANCELED)
                    fprintf(stderr, "fauxdev camera client error: %d\n", session_status);
            }
            pthread_mutex_lock(&server_lock);
            server_client_fd = -1;
            pthread_mutex_unlock(&server_lock);
            close(client);
        }
    }
    pthread_mutex_lock(&server_lock);
    server_stop_fd = -1;
    pthread_mutex_unlock(&server_lock);
    close(stop_fd);
    close(listener);
finish_socket:
    unlink(socket_path);
finish_camera:
    close_camera(camera);
    free(camera);
    pthread_mutex_lock(&server_lock);
    server_state = 0;
    pthread_mutex_unlock(&server_lock);
    return result;
}

JNIEXPORT jint JNICALL
Java_org_randomcoder_udroid_camera_CameraBrokerBridge_run(
    JNIEnv *env, jobject instance, jstring path
) {
    (void)instance;
    if (!path) return -EINVAL;
    const char *socket_path = (*env)->GetStringUTFChars(env, path, NULL);
    if (!socket_path) return -ENOMEM;
    int result = run_server(socket_path);
    (*env)->ReleaseStringUTFChars(env, path, socket_path);
    return result;
}

JNIEXPORT jboolean JNICALL
Java_org_randomcoder_udroid_camera_CameraBrokerBridge_prepare(JNIEnv *env, jobject instance) {
    (void)env;
    (void)instance;
    pthread_mutex_lock(&server_lock);
    const int idle = server_state == 0;
    if (idle) server_state = 3;
    pthread_mutex_unlock(&server_lock);
    return idle ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_randomcoder_udroid_camera_CameraBrokerBridge_stop(JNIEnv *env, jobject instance) {
    (void)env;
    (void)instance;
    pthread_mutex_lock(&server_lock);
    if (server_state != 1 && server_state != 3) { pthread_mutex_unlock(&server_lock); return; }
    server_state = 2;
    const uint64_t token = 1;
    if (server_stop_fd >= 0) write(server_stop_fd, &token, sizeof(token));
    if (server_client_fd >= 0) shutdown(server_client_fd, SHUT_RDWR);
    pthread_mutex_unlock(&server_lock);
}
