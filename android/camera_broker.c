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
#include <stdarg.h>
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

enum { INVENTORY_LIMIT = 128 * 1024, INVENTORY_MAX_ENTRIES = 4096 };

struct inventory_report {
    char *text;
    size_t used;
    int error;
};

static void inventory_append(struct inventory_report *report, const char *format, ...) {
    if (report->error) return;
    va_list args;
    va_start(args, format);
    int size = vsnprintf(report->text + report->used, INVENTORY_LIMIT - report->used,
                         format, args);
    va_end(args);
    if (size < 0) report->error = -EIO;
    else if ((size_t)size >= INVENTORY_LIMIT - report->used) report->error = -ENOSPC;
    else report->used += (size_t)size;
}

static void inventory_entry(struct inventory_report *report, const ACameraMetadata *metadata,
                            uint32_t tag, const char *name) {
    ACameraMetadata_const_entry entry;
    inventory_append(report, "  %s: ", name);
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) != ACAMERA_OK) {
        inventory_append(report, "unavailable\n");
        return;
    }
    if (!entry.count) {
        inventory_append(report, "empty\n");
        return;
    }
    if (entry.count > INVENTORY_MAX_ENTRIES) {
        inventory_append(report, "invalid count %u\n", entry.count);
        return;
    }
    if (!entry.data.u8) {
        inventory_append(report, "invalid metadata pointer\n");
        return;
    }
    for (uint32_t i = 0; i < entry.count && !report->error; ++i) {
        if (i) inventory_append(report, ", ");
        switch (entry.type) {
        case ACAMERA_TYPE_BYTE: inventory_append(report, "%u", entry.data.u8[i]); break;
        case ACAMERA_TYPE_INT32: inventory_append(report, "%d", entry.data.i32[i]); break;
        case ACAMERA_TYPE_INT64: inventory_append(report, "%lld", (long long)entry.data.i64[i]); break;
        case ACAMERA_TYPE_FLOAT: inventory_append(report, "%g", entry.data.f[i]); break;
        case ACAMERA_TYPE_DOUBLE: inventory_append(report, "%g", entry.data.d[i]); break;
        case ACAMERA_TYPE_RATIONAL:
            inventory_append(report, "%d/%d", entry.data.r[i].numerator,
                             entry.data.r[i].denominator);
            break;
        default:
            inventory_append(report, "unknown type %u (%u values)\n", entry.type, entry.count);
            return;
        }
    }
    inventory_append(report, "\n");
}

static void inventory_physical_ids(struct inventory_report *report, const ACameraMetadata *metadata) {
    ACameraMetadata_const_entry entry;
    inventory_append(report, "  physical camera IDs: ");
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS,
                                      &entry) != ACAMERA_OK) {
        inventory_append(report, "unavailable\n");
        return;
    }
    if (entry.type != ACAMERA_TYPE_BYTE || entry.count > INVENTORY_MAX_ENTRIES ||
        (entry.count && !entry.data.u8)) {
        inventory_append(report, "invalid metadata type\n");
        return;
    }
    if (!entry.count) {
        inventory_append(report, "none reported\n");
        return;
    }
    uint32_t start = 0;
    for (uint32_t i = 0; i < entry.count; ++i) {
        if (entry.data.u8[i]) continue;
        if (start) inventory_append(report, ", ");
        for (uint32_t j = start; j < i; ++j) {
            unsigned char c = entry.data.u8[j];
            inventory_append(report, "%c", c >= 32 && c <= 126 ? c : '?');
        }
        start = i + 1;
    }
    if (start != entry.count) inventory_append(report, " [unterminated ID]");
    inventory_append(report, "\n");
}

static void inventory_yuv_streams(struct inventory_report *report, const ACameraMetadata *metadata) {
    ACameraMetadata_const_entry entry;
    inventory_append(report, "  YUV_420_888 output sizes: ");
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &entry) != ACAMERA_OK) {
        inventory_append(report, "unavailable\n");
        return;
    }
    if (entry.type != ACAMERA_TYPE_INT32 || entry.count > INVENTORY_MAX_ENTRIES ||
        entry.count % 4 ||
        (entry.count && !entry.data.i32)) {
        inventory_append(report, "invalid metadata layout\n");
        return;
    }
    unsigned found = 0;
    for (uint32_t i = 0; i < entry.count; i += 4) {
        const int32_t *item = entry.data.i32 + i;
        if (item[0] != AIMAGE_FORMAT_YUV_420_888 ||
            item[3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) continue;
        inventory_append(report, "%s%dx%d", found++ ? ", " : "", item[1], item[2]);
    }
    inventory_append(report, "%s\n", found ? "" : "none reported");
}

static void inventory_camera(struct inventory_report *report, ACameraManager *manager, const char *id) {
    inventory_append(report, "\nCamera ");
    if (!id) {
        inventory_append(report, "<missing ID>\n  characteristics: unavailable\n");
        return;
    }
    if (strnlen(id, 257) > 256) {
        inventory_append(report, "<ID exceeds 256 bytes>\n  characteristics: unavailable\n");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        inventory_append(report, "%c", *p >= 32 && *p <= 126 ? *p : '?');
    inventory_append(report, "\n");
    ACameraMetadata *metadata = NULL;
    camera_status_t status = ACameraManager_getCameraCharacteristics(manager, id, &metadata);
    if (status != ACAMERA_OK) {
        inventory_append(report, "  characteristics: unavailable (status %d)\n", status);
        return;
    }
    if (!metadata) {
        inventory_append(report, "  characteristics: unavailable (null metadata)\n");
        return;
    }
    inventory_entry(report, metadata, ACAMERA_LENS_FACING, "lens facing");
    inventory_entry(report, metadata, ACAMERA_SENSOR_ORIENTATION, "sensor orientation degrees");
    inventory_entry(report, metadata, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, "hardware level");
    inventory_entry(report, metadata, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, "capability IDs");
    inventory_physical_ids(report, metadata);
    inventory_yuv_streams(report, metadata);
    inventory_entry(report, metadata, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                    "stream min frame durations (format,width,height,ns)");
    inventory_entry(report, metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS, "request tag IDs");
    inventory_entry(report, metadata, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS, "result tag IDs");
    inventory_entry(report, metadata, ACAMERA_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                    "characteristic tag IDs");
    inventory_entry(report, metadata, ACAMERA_CONTROL_AE_AVAILABLE_MODES, "AE modes");
    inventory_entry(report, metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, "AE FPS ranges");
    inventory_entry(report, metadata, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, "AE compensation range");
    inventory_entry(report, metadata, ACAMERA_CONTROL_AE_COMPENSATION_STEP, "AE compensation step");
    inventory_entry(report, metadata, ACAMERA_CONTROL_AF_AVAILABLE_MODES, "AF modes");
    inventory_entry(report, metadata, ACAMERA_CONTROL_AWB_AVAILABLE_MODES, "AWB modes");
    inventory_entry(report, metadata, ACAMERA_FLASH_INFO_AVAILABLE, "flash available");
    inventory_entry(report, metadata, ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, "max digital zoom");
    inventory_entry(report, metadata, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, "zoom ratio range");
    inventory_entry(report, metadata, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, "ISO range");
    inventory_entry(report, metadata, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, "exposure time range ns");
    int32_t tag_count = 0;
    const uint32_t *tags = NULL;
    status = ACameraMetadata_getAllTags(metadata, &tag_count, &tags);
    inventory_append(report, "  all characteristic tags: ");
    if (status != ACAMERA_OK || tag_count < 0 || tag_count > INVENTORY_MAX_ENTRIES ||
        (tag_count && !tags))
        inventory_append(report, "unavailable (status %d)\n", status);
    else {
        for (int32_t i = 0; i < tag_count; ++i)
            inventory_append(report, "%s0x%08x", i ? ", " : "", tags[i]);
        inventory_append(report, "\n");
    }
    ACameraMetadata_free(metadata);
}

static int inventory_snapshot(char **text, size_t *size) {
    struct inventory_report report = {.text = calloc(1, INVENTORY_LIMIT)};
    if (!report.text) return -ENOMEM;
    inventory_append(&report, "Camera2 inventory (read-only; values are device metadata, not V4L2 support)\n");
    ACameraManager *manager = ACameraManager_create();
    if (!manager) inventory_append(&report, "Camera manager unavailable\n");
    else {
        ACameraIdList *ids = NULL;
        camera_status_t status = ACameraManager_getCameraIdList(manager, &ids);
        if (status != ACAMERA_OK || !ids) {
            inventory_append(&report, "Camera ID list unavailable (status %d)\n", status);
        } else if (ids->numCameras < 0 || ids->numCameras > 64 ||
                   (ids->numCameras && !ids->cameraIds)) {
            inventory_append(&report, "Camera ID list has invalid layout\n");
            ACameraManager_deleteCameraIdList(ids);
        } else {
            inventory_append(&report, "%d visible camera IDs; live availability not queried\n",
                             ids->numCameras);
            for (int i = 0; i < ids->numCameras && !report.error; ++i)
                inventory_camera(&report, manager, ids->cameraIds[i]);
            ACameraManager_deleteCameraIdList(ids);
        }
        ACameraManager_delete(manager);
    }
    if (report.error) {
        free(report.text);
        return report.error;
    }
    *text = report.text;
    *size = report.used;
    return 0;
}

JNIEXPORT jstring JNICALL
Java_org_randomcoder_udroid_camera_CameraBrokerBridge_inventory(JNIEnv *env, jclass cls) {
    (void)cls;
    char *text = NULL;
    size_t size = 0;
    int status = inventory_snapshot(&text, &size);
    (void)size;
    const char *message = status == -ENOMEM ? "Camera inventory: out of memory" :
                          status == -ENOSPC ? "Camera inventory exceeds the 128 KiB diagnostic limit" :
                          status ? "Camera inventory formatting failed" : text;
    jstring result = (*env)->NewStringUTF(env, message);
    free(text);
    return result;
}

static int collect_inputs(ACameraManager *manager, struct fauxdev_camera_inputs_reply *out) {
    ACameraIdList *ids = NULL;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || !ids) return -EIO;
    if (ids->numCameras < 0 || ids->numCameras > (int)FAUXDEV_CAMERA_MAX_INPUTS ||
        (ids->numCameras && !ids->cameraIds)) {
        ACameraManager_deleteCameraIdList(ids);
        return -EIO;
    }
    int best_score = INT_MIN;
    for (int i = 0; i < ids->numCameras; i++) {
        const char *id = ids->cameraIds[i];
        if (!id || strnlen(id, FAUXDEV_CAMERA_ID_SIZE) == FAUXDEV_CAMERA_ID_SIZE) continue;
        ACameraMetadata *metadata = NULL;
        if (ACameraManager_getCameraCharacteristics(manager, id, &metadata) != ACAMERA_OK || !metadata)
            continue;
        ACameraMetadata_const_entry facing;
        const int has_facing = ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &facing) == ACAMERA_OK &&
                               facing.type == ACAMERA_TYPE_BYTE && facing.count && facing.data.u8;
        const uint32_t direction = has_facing ? facing.data.u8[0] : UINT32_MAX;
        const int back = direction == ACAMERA_LENS_FACING_BACK;
        ACameraMetadata_const_entry configs;
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                          &configs) == ACAMERA_OK && configs.type == ACAMERA_TYPE_INT32 &&
            configs.count <= INVENTORY_MAX_ENTRIES && configs.data.i32 && !(configs.count % 4)) {
            int camera_score = INT_MIN, width = 0, height = 0;
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
                if (score > camera_score) { camera_score = score; width = w; height = h; }
            }
            if (camera_score != INT_MIN) {
                uint32_t index = le32toh(out->count);
                if (index == FAUXDEV_CAMERA_MAX_INPUTS) {
                    ACameraMetadata_free(metadata);
                    ACameraManager_deleteCameraIdList(ids);
                    return -EOVERFLOW;
                }
                struct fauxdev_camera_input *input = &out->inputs[index];
                input->facing = htole32(direction);
                input->source_width = htole32((uint32_t)width);
                input->source_height = htole32((uint32_t)height);
                strcpy(input->id, id);
                out->count = htole32(index + 1);
                if (camera_score > best_score) {
                    best_score = camera_score;
                    out->default_index = htole32(index);
                }
            }
        }
        ACameraMetadata_free(metadata);
    }
    ACameraManager_deleteCameraIdList(ids);
    return 0;
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

static int open_camera(struct camera *camera, const struct fauxdev_camera_input *input) {
    atomic_init(&camera->failed, false);
    atomic_init(&camera->reader_stop, false);
    if (pthread_mutex_init(&camera->frame_lock, NULL) != 0) return -ENOMEM;
    camera->frame_lock_ready = true;
    camera->transfer_frame = malloc(OUTPUT_SIZE);
    if (!camera->transfer_frame) return -ENOMEM;
    camera->manager = ACameraManager_create();
    if (!camera->manager) return -EIO;
    const char *id = input->id;
    const int width = (int)le32toh(input->source_width);
    const int height = (int)le32toh(input->source_height);
    fprintf(stderr, "fauxdev camera %s: %dx%d YUV -> %dx%d RGB24\n",
            id, width, height, OUTPUT_WIDTH, OUTPUT_HEIGHT);
    if (AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, 2, &camera->reader) != AMEDIA_OK) {
        return -EIO;
    }
    ANativeWindow *window = NULL;
    if (AImageReader_getWindow(camera->reader, &window) != AMEDIA_OK) return -EIO;
    ACameraDevice_StateCallbacks device_callbacks = {
        .context = camera,
        .onDisconnected = on_device_disconnected,
        .onError = on_device_error,
    };
    camera_status_t status = ACameraManager_openCamera(camera->manager, id, &device_callbacks, &camera->device);
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

static int validate_frame_request(const struct fauxdev_message *message,
                                  struct fauxdev_video_frame_request *frame) {
    if (message->payload_size != sizeof(struct fauxdev_video_frame_request) || message->fd_count != 1)
        return -EINVAL;
    memcpy(frame, message->payload, sizeof(*frame));
    const uint32_t offset = le32toh(frame->offset);
    if (le32toh(frame->width) != OUTPUT_WIDTH || le32toh(frame->height) != OUTPUT_HEIGHT ||
        le32toh(frame->stride) != OUTPUT_WIDTH * 3 ||
        le32toh(frame->pixel_format) != V4L2_PIX_FMT_RGB24 || le32toh(frame->size) != OUTPUT_SIZE)
        return -EINVAL;
    struct stat statbuf;
    if (fstat(message->fds[0], &statbuf) < 0) return -errno;
    if (!S_ISREG(statbuf.st_mode) || offset > (uint64_t)statbuf.st_size ||
        OUTPUT_SIZE > (uint64_t)statbuf.st_size - offset) return -EINVAL;
    return 0;
}

static int capture_to_fd(struct camera *camera, int client, const struct fauxdev_message *message) {
    struct fauxdev_video_frame_request frame;
    int validation = validate_frame_request(message, &frame);
    if (validation < 0) return validation;
    const uint32_t offset = le32toh(frame.offset);
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

struct inventory_cache {
    char *text;
    size_t size;
};

struct client_camera_state {
    struct fauxdev_camera_inputs_reply *inputs;
    struct camera *active;
    bool explicit_lifecycle;
};

static int load_inputs(struct client_camera_state *state) {
    if (state->inputs) return 0;
    state->inputs = calloc(1, sizeof(*state->inputs));
    if (!state->inputs) return -ENOMEM;
    ACameraManager *manager = ACameraManager_create();
    if (!manager) { free(state->inputs); state->inputs = NULL; return -EIO; }
    int result = collect_inputs(manager, state->inputs);
    ACameraManager_delete(manager);
    if (result < 0) { free(state->inputs); state->inputs = NULL; }
    return result;
}

static int serve_inputs(const struct fauxdev_message *request, struct fauxdev_message *response,
                        struct client_camera_state *state) {
    if (request->payload_size || request->fd_count) return -EINVAL;
    int result = load_inputs(state);
    if (result < 0) return result;
    uint32_t count = le32toh(state->inputs->count);
    response->payload_size = offsetof(struct fauxdev_camera_inputs_reply, inputs) +
                             count * sizeof(struct fauxdev_camera_input);
    memcpy(response->payload, state->inputs, response->payload_size);
    return 0;
}

static int start_camera(struct client_camera_state *state, uint32_t index) {
    if (state->active) return -EBUSY;
    int result = load_inputs(state);
    if (result < 0) return result;
    if (index >= le32toh(state->inputs->count)) return -EINVAL;
    struct camera *camera = calloc(1, sizeof(*camera));
    if (!camera) return -ENOMEM;
    result = open_camera(camera, &state->inputs->inputs[index]);
    if (result < 0) {
        close_camera(camera);
        free(camera);
        return result;
    }
    state->active = camera;
    return 0;
}

static int serve_start(const struct fauxdev_message *request, struct client_camera_state *state) {
    if (request->payload_size != sizeof(struct fauxdev_camera_start_request) || request->fd_count)
        return -EINVAL;
    struct fauxdev_camera_start_request start;
    memcpy(&start, request->payload, sizeof(start));
    return start_camera(state, le32toh(start.index));
}

static int serve_legacy_frame(int client, const struct fauxdev_message *request,
                              struct client_camera_state *state) {
    if (!state->active) {
        if (state->explicit_lifecycle) return -EPIPE;
        struct fauxdev_video_frame_request frame;
        int result = validate_frame_request(request, &frame);
        if (result < 0) return result;
        result = load_inputs(state);
        if (result < 0) return result;
        if (!le32toh(state->inputs->count)) return -ENODEV;
        result = start_camera(state, le32toh(state->inputs->default_index));
        if (result < 0) return result;
    }
    return capture_to_fd(state->active, client, request);
}

static int serve_stop(const struct fauxdev_message *request, struct client_camera_state *state) {
    if (request->payload_size || request->fd_count) return -EINVAL;
    if (state->active) {
        close_camera(state->active);
        free(state->active);
        state->active = NULL;
    }
    return 0;
}

static int serve_inventory(const struct fauxdev_message *request,
                           struct fauxdev_message *response, struct inventory_cache *cache) {
    if (request->payload_size != sizeof(struct fauxdev_inventory_request) || request->fd_count)
        return -EINVAL;
    struct fauxdev_inventory_request wire_request;
    memcpy(&wire_request, request->payload, sizeof(wire_request));
    uint32_t offset = le32toh(wire_request.offset);
    if (!cache->text) {
        int result = inventory_snapshot(&cache->text, &cache->size);
        if (result < 0) return result;
    }
    if (offset >= cache->size) return -EINVAL;
    size_t chunk = cache->size - offset;
    if (chunk > 60 * 1024) chunk = 60 * 1024;
    struct fauxdev_inventory_reply header = {
        .total = htole32((uint32_t)cache->size), .offset = htole32(offset),
    };
    memcpy(response->payload, &header, sizeof(header));
    memcpy(response->payload + sizeof(header), cache->text + offset, chunk);
    response->payload_size = sizeof(header) + chunk;
    return 0;
}

static int serve_client(int client, struct client_camera_state *state, struct inventory_cache *cache) {
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
        response = (struct fauxdev_message){
            .opcode = request.opcode, .flags = FAUXDEV_MESSAGE_RESPONSE,
            .request_id = request.request_id,
        };
        int status;
        switch (request.opcode) {
        case FAUXDEV_OPCODE_VIDEO_FRAME:
            status = serve_legacy_frame(client, &request, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_INVENTORY:
            status = serve_inventory(&request, &response, cache);
            break;
        case FAUXDEV_OPCODE_CAMERA_INPUTS:
            status = serve_inputs(&request, &response, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_START:
            state->explicit_lifecycle = true;
            status = serve_start(&request, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_STOP:
            state->explicit_lifecycle = true;
            status = serve_stop(&request, state);
            break;
        default:
            status = -ENOTTY;
        }
        fprintf(stderr, "fauxdev camera request result: %d at %lldms\n",
                status, (long long)monotonic_ms());
        fauxdev_message_close_fds(&request);
        response.status = status;
        if (status < 0) response.payload_size = 0;
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

    int result;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof(address.sun_path)) { result = -ENAMETOOLONG; goto finish_server; }
    strcpy(address.sun_path, socket_path);
    struct stat existing;
    if (lstat(socket_path, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid() || unlink(socket_path) < 0) {
            result = -EACCES; goto finish_server;
        }
    } else if (errno != ENOENT) { result = -errno; goto finish_server; }
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0) { result = -errno; goto finish_server; }
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
                struct inventory_cache cache = {0};
                struct client_camera_state state = {0};
                int session_status = serve_client(client, &state, &cache);
                if (state.active) { close_camera(state.active); free(state.active); }
                free(state.inputs);
                free(cache.text);
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
finish_server:
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
