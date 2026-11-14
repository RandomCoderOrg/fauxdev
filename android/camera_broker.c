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

enum { OUTPUT_WIDTH = 320, OUTPUT_HEIGHT = 240, OUTPUT_SIZE = OUTPUT_WIDTH * OUTPUT_HEIGHT * 3,
       MAX_RGB_SIZE = 64 * 1024 * 1024, MAX_CLIENTS = 16 };

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
    uint8_t *latest_frame;
    uint8_t *transfer_frame;
    uint32_t output_width;
    uint32_t output_height;
    size_t output_size;
    uint64_t frame_count;
    uint64_t last_served_frame;
};

static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static int server_state; // 0 idle, 1 running, 2 stop requested, 3 prepared
static int server_stop_fd = -1;
struct client_camera_state;
static struct fauxdev_camera_inputs_reply *camera_catalog;
static struct client_camera_state *client_states;
static struct client_camera_state *queue_owner;
static uint64_t config_generation;
static uint32_t config_index, config_width, config_height, config_fps;
struct client_slot {
    pthread_t thread;
    int fd;
    bool started;
    bool done;
};
static struct client_slot clients[MAX_CLIENTS];
static struct client_camera_state *camera_owner;

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
                    w <= 0 || h <= 0 || (uint64_t)w * h * 3 > MAX_RGB_SIZE)
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

struct camera_mode {
    uint32_t width, height;
    bool native;
};
struct camera_modes {
    uint32_t count;
    struct camera_mode items[INVENTORY_MAX_ENTRIES / 4 + 1];
};

struct camera_rates {
    uint32_t count;
    uint32_t fps[256];
};

static int compare_modes(const void *left, const void *right) {
    const struct camera_mode *a = left, *b = right;
    uint64_t aa = (uint64_t)a->width * a->height;
    uint64_t ba = (uint64_t)b->width * b->height;
    if (aa != ba) return aa < ba ? -1 : 1;
    if (a->width != b->width) return a->width < b->width ? -1 : 1;
    if (a->height != b->height) return a->height < b->height ? -1 : 1;
    return 0;
}

static int load_camera_modes(const char *id, struct camera_modes *modes) {
    modes->count = 0;
    ACameraManager *manager = ACameraManager_create();
    if (!manager) return -EIO;
    ACameraMetadata *metadata = NULL;
    camera_status_t status = ACameraManager_getCameraCharacteristics(manager, id, &metadata);
    if (status != ACAMERA_OK || !metadata) {
        ACameraManager_delete(manager);
        return -EIO;
    }
    ACameraMetadata_const_entry configs;
    int result = 0;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &configs) != ACAMERA_OK || configs.type != ACAMERA_TYPE_INT32 ||
        configs.count > INVENTORY_MAX_ENTRIES || configs.count % 4 ||
        (configs.count && !configs.data.i32)) {
        result = -EIO;
        goto done;
    }
    for (uint32_t i = 0; i < configs.count; i += 4) {
        const int32_t *value = configs.data.i32 + i;
        if (value[0] != AIMAGE_FORMAT_YUV_420_888 ||
            value[3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT ||
            value[1] <= 0 || value[2] <= 0 ||
            (uint64_t)value[1] * value[2] * 3 > MAX_RGB_SIZE) continue;
        uint32_t width = (uint32_t)value[1], height = (uint32_t)value[2];
        bool duplicate = false;
        for (uint32_t j = 0; j < modes->count; ++j)
            if (modes->items[j].width == width && modes->items[j].height == height)
                duplicate = true;
        if (!duplicate) modes->items[modes->count++] = (struct camera_mode){width, height, true};
    }
    bool has_legacy = false;
    for (uint32_t j = 0; j < modes->count; ++j)
        if (modes->items[j].width == OUTPUT_WIDTH && modes->items[j].height == OUTPUT_HEIGHT)
            has_legacy = true;
    if (!has_legacy) modes->items[modes->count++] =
        (struct camera_mode){OUTPUT_WIDTH, OUTPUT_HEIGHT, false};
    qsort(modes->items, modes->count, sizeof(modes->items[0]), compare_modes);
done:
    ACameraMetadata_free(metadata);
    ACameraManager_delete(manager);
    return result;
}

static int compare_rate(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static uint32_t nominal_rate(const struct camera_rates *rates) {
    if (!rates->count) return 0;
    uint32_t chosen = rates->fps[0];
    for (uint32_t i = 0; i < rates->count; ++i) {
        if (rates->fps[i] == 30) return 30;
        if (rates->fps[i] <= 30) chosen = rates->fps[i];
    }
    return chosen;
}

static int load_camera_rates(const struct fauxdev_camera_input *input,
                             uint32_t width, uint32_t height, struct camera_rates *rates) {
    rates->count = 0;
    struct camera_modes modes;
    int result = load_camera_modes(input->id, &modes);
    if (result < 0) return result;
    const struct camera_mode *mode = NULL;
    for (uint32_t i = 0; i < modes.count; ++i)
        if (modes.items[i].width == width && modes.items[i].height == height)
            mode = &modes.items[i];
    if (!mode) return -EINVAL;
    uint32_t source_width = mode->native ? width : le32toh(input->source_width);
    uint32_t source_height = mode->native ? height : le32toh(input->source_height);
    ACameraManager *manager = ACameraManager_create();
    if (!manager) return -EIO;
    ACameraMetadata *metadata = NULL;
    camera_status_t status = ACameraManager_getCameraCharacteristics(manager, input->id, &metadata);
    if (status != ACAMERA_OK || !metadata) { result = -EIO; goto done_rates; }
    ACameraMetadata_const_entry durations, ranges;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                                     &durations) != ACAMERA_OK ||
        ACameraMetadata_getConstEntry(metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                                     &ranges) != ACAMERA_OK ||
        durations.type != ACAMERA_TYPE_INT64 || ranges.type != ACAMERA_TYPE_INT32 ||
        durations.count > INVENTORY_MAX_ENTRIES || ranges.count > INVENTORY_MAX_ENTRIES ||
        durations.count % 4 || ranges.count % 2 ||
        (durations.count && !durations.data.i64) || (ranges.count && !ranges.data.i32)) {
        result = -EIO; goto done_rates;
    }
    uint64_t minimum_ns = 0;
    for (uint32_t i = 0; i < durations.count; i += 4) {
        const int64_t *item = durations.data.i64 + i;
        if (item[0] == AIMAGE_FORMAT_YUV_420_888 && item[1] == source_width &&
            item[2] == source_height && item[3] > 0 && (uint64_t)item[3] > minimum_ns)
            minimum_ns = (uint64_t)item[3];
    }
    if (!minimum_ns) goto done_rates;
    for (uint32_t i = 0; i < ranges.count; i += 2) {
        int32_t low = ranges.data.i32[i], high = ranges.data.i32[i + 1];
        if (low <= 0 || low != high || high > 1000 ||
            minimum_ns > UINT64_C(1000000000) / (uint32_t)high) continue;
        bool duplicate = false;
        for (uint32_t j = 0; j < rates->count; ++j)
            if (rates->fps[j] == (uint32_t)high) duplicate = true;
        if (duplicate) continue;
        if (rates->count == sizeof(rates->fps) / sizeof(rates->fps[0])) {
            result = -EOVERFLOW; goto done_rates;
        }
        rates->fps[rates->count++] = (uint32_t)high;
    }
    qsort(rates->fps, rates->count, sizeof(rates->fps[0]), compare_rate);
done_rates:
    if (metadata) ACameraMetadata_free(metadata);
    ACameraManager_delete(manager);
    return result;
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
    free(camera->latest_frame);
    free(camera->transfer_frame);
}

static int convert_image(AImage *image, const struct camera *camera, uint8_t *output) {
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
    return fauxdev_yuv420_to_rgb24(output, (int)camera->output_width,
                                  (int)camera->output_height, width, height, planes);
}

static void *drain_frames(void *context) {
    struct camera *camera = context;
    uint8_t *pixels = malloc(camera->output_size);
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
        int result = convert_image(image, camera, pixels);
        AImage_delete(image);
        if (result < 0) {
            fprintf(stderr, "fauxdev camera image conversion error: %d\n", result);
            atomic_store(&camera->failed, true);
            break;
        }
        pthread_mutex_lock(&camera->frame_lock);
        memcpy(camera->latest_frame, pixels, camera->output_size);
        camera->frame_count++;
        pthread_mutex_unlock(&camera->frame_lock);
    }
    free(pixels);
    return NULL;
}

static int open_camera(struct camera *camera, const struct fauxdev_camera_input *input,
                       uint32_t output_width, uint32_t output_height,
                       uint32_t source_width, uint32_t source_height, uint32_t fps) {
    camera->output_width = output_width;
    camera->output_height = output_height;
    camera->output_size = (size_t)output_width * output_height * 3;
    atomic_init(&camera->failed, false);
    atomic_init(&camera->reader_stop, false);
    if (pthread_mutex_init(&camera->frame_lock, NULL) != 0) return -ENOMEM;
    camera->frame_lock_ready = true;
    camera->transfer_frame = malloc(camera->output_size);
    camera->latest_frame = malloc(camera->output_size);
    if (!camera->transfer_frame || !camera->latest_frame) return -ENOMEM;
    camera->manager = ACameraManager_create();
    if (!camera->manager) return -EIO;
    const char *id = input->id;
    const int width = (int)source_width;
    const int height = (int)source_height;
    fprintf(stderr, "fauxdev camera %s: %dx%d YUV -> %dx%d RGB24\n",
            id, width, height, output_width, output_height);
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
    if (fps) {
        const int32_t range[2] = {(int32_t)fps, (int32_t)fps};
        status = ACaptureRequest_setEntry_i32(camera->request,
                                               ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);
        if (status != ACAMERA_OK) {
            fprintf(stderr, "fauxdev camera AE FPS %u rejected: %d\n", fps, status);
            return -EIO;
        }
    }
    ACameraCaptureSession_stateCallbacks session_callbacks = {
        .context = camera,
    };
    status = fps ? ACameraDevice_createCaptureSessionWithSessionParameters(
                       camera->device, camera->outputs, camera->request,
                       &session_callbacks, &camera->session) :
                   ACameraDevice_createCaptureSession(camera->device, camera->outputs,
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

static int capture_frame(struct camera *camera, int client, uint8_t *output) {
    const int64_t deadline = monotonic_ms() + 15000;
    while (monotonic_ms() < deadline && !atomic_load(&camera->failed) && !stop_requested()) {
        if (peer_gone(client)) return -ECANCELED;
        pthread_mutex_lock(&camera->frame_lock);
        // Let preview and auto-exposure settle before serving the first frame.
        uint64_t minimum = camera->last_served_frame ? camera->last_served_frame + 1 : 12;
        if (camera->frame_count >= minimum) {
            memcpy(output, camera->latest_frame, camera->output_size);
            camera->last_served_frame = camera->frame_count;
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
                                  uint32_t width, uint32_t height, size_t size,
                                  struct fauxdev_video_frame_request *frame) {
    if (message->payload_size != sizeof(struct fauxdev_video_frame_request) || message->fd_count != 1)
        return -EINVAL;
    memcpy(frame, message->payload, sizeof(*frame));
    const uint32_t offset = le32toh(frame->offset);
    if (le32toh(frame->width) != width || le32toh(frame->height) != height ||
        le32toh(frame->stride) != width * 3 ||
        le32toh(frame->pixel_format) != V4L2_PIX_FMT_RGB24 || le32toh(frame->size) != size)
        return -EINVAL;
    struct stat statbuf;
    if (fstat(message->fds[0], &statbuf) < 0) return -errno;
    if (!S_ISREG(statbuf.st_mode) || offset > (uint64_t)statbuf.st_size ||
        size > (uint64_t)statbuf.st_size - offset) return -EINVAL;
    return 0;
}

static int capture_to_fd(struct camera *camera, int client, const struct fauxdev_message *message) {
    struct fauxdev_video_frame_request frame;
    int validation = validate_frame_request(message, camera->output_width,
                                            camera->output_height, camera->output_size, &frame);
    if (validation < 0) return validation;
    const uint32_t offset = le32toh(frame.offset);
    uint8_t *pixels = camera->transfer_frame;
    int result = capture_frame(camera, client, pixels);
    if (result < 0) return result;
    if (peer_gone(client)) return -ECANCELED;
    size_t done = 0;
    while (done < camera->output_size) {
        ssize_t written = pwrite(message->fds[0], pixels + done,
                                 camera->output_size - done, offset + done);
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
    bool implicit_queue;
    uint64_t queue_generation;
    enum v4l2_priority priority;
    struct client_camera_state *next;
};

static enum v4l2_priority highest_priority_locked(const struct client_camera_state *except) {
    enum v4l2_priority highest = V4L2_PRIORITY_UNSET;
    for (const struct client_camera_state *other = client_states; other; other = other->next)
        if (other != except && other->priority > highest) highest = other->priority;
    return highest;
}

static int reserve_camera(struct client_camera_state *state) {
    pthread_mutex_lock(&server_lock);
    int result = server_state == 2 ? -ECANCELED : camera_owner ? -EBUSY : 0;
    if (!result) camera_owner = state;
    pthread_mutex_unlock(&server_lock);
    return result;
}

static void release_camera(struct client_camera_state *state) {
    pthread_mutex_lock(&server_lock);
    if (camera_owner == state) camera_owner = NULL;
    pthread_mutex_unlock(&server_lock);
}

static int load_inputs(struct client_camera_state *state) {
    if (state->inputs) return 0;
    if (!camera_catalog) return -EIO;
    state->inputs = malloc(sizeof(*state->inputs));
    if (!state->inputs) return -ENOMEM;
    memcpy(state->inputs, camera_catalog, sizeof(*state->inputs));
    return 0;
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

static int serve_modes(const struct fauxdev_message *request, struct fauxdev_message *response,
                       struct client_camera_state *state) {
    if (request->payload_size != sizeof(struct fauxdev_camera_modes_request) || request->fd_count)
        return -EINVAL;
    struct fauxdev_camera_modes_request query;
    memcpy(&query, request->payload, sizeof(query));
    uint32_t index = le32toh(query.index), offset = le32toh(query.offset);
    int result = load_inputs(state);
    if (result < 0) return result;
    if (index >= le32toh(state->inputs->count)) return -EINVAL;
    struct camera_modes modes;
    result = load_camera_modes(state->inputs->inputs[index].id, &modes);
    if (result < 0) return result;
    if (offset >= modes.count) return -EINVAL;
    struct fauxdev_camera_modes_reply reply = {
        .total = htole32(modes.count), .offset = htole32(offset),
    };
    uint32_t count = modes.count - offset;
    if (count > FAUXDEV_CAMERA_MODE_PAGE) count = FAUXDEV_CAMERA_MODE_PAGE;
    for (uint32_t i = 0; i < count; ++i) {
        reply.modes[i].width = htole32(modes.items[offset + i].width);
        reply.modes[i].height = htole32(modes.items[offset + i].height);
    }
    response->payload_size = offsetof(struct fauxdev_camera_modes_reply, modes) +
                             count * sizeof(reply.modes[0]);
    memcpy(response->payload, &reply, response->payload_size);
    return 0;
}

static int serve_rates(const struct fauxdev_message *request, struct fauxdev_message *response) {
    if (request->payload_size != sizeof(struct fauxdev_camera_rates_request) || request->fd_count)
        return -EINVAL;
    struct fauxdev_camera_rates_request query;
    memcpy(&query, request->payload, sizeof(query));
    uint32_t index = le32toh(query.index), width = le32toh(query.width);
    uint32_t height = le32toh(query.height), offset = le32toh(query.offset);
    if (index >= le32toh(camera_catalog->count)) return -EINVAL;
    struct camera_rates rates;
    int result = load_camera_rates(&camera_catalog->inputs[index], width, height, &rates);
    if (result < 0) return result;
    if (offset >= rates.count) return -EINVAL;
    struct fauxdev_camera_rates_reply reply = {
        .total = htole32(rates.count), .offset = htole32(offset),
    };
    uint32_t count = rates.count - offset;
    if (count > FAUXDEV_CAMERA_RATE_PAGE) count = FAUXDEV_CAMERA_RATE_PAGE;
    for (uint32_t i = 0; i < count; ++i) reply.fps[i] = htole32(rates.fps[offset + i]);
    response->payload_size = offsetof(struct fauxdev_camera_rates_reply, fps) +
                             count * sizeof(reply.fps[0]);
    memcpy(response->payload, &reply, response->payload_size);
    return 0;
}

static int serve_get_rate(const struct fauxdev_message *request,
                          struct fauxdev_message *response) {
    if (request->payload_size || request->fd_count) return -EINVAL;
    pthread_mutex_lock(&server_lock);
    struct fauxdev_camera_rate rate = {
        .generation = htole64(config_generation), .fps = htole32(config_fps),
    };
    pthread_mutex_unlock(&server_lock);
    memcpy(response->payload, &rate, sizeof(rate));
    response->payload_size = sizeof(rate);
    return 0;
}

static int serve_set_rate(const struct fauxdev_message *request,
                          struct fauxdev_message *response,
                          struct client_camera_state *state) {
    if (request->payload_size != sizeof(struct fauxdev_camera_rate) || request->fd_count)
        return -EINVAL;
    struct fauxdev_camera_rate wanted;
    memcpy(&wanted, request->payload, sizeof(wanted));
    uint64_t expected = le64toh(wanted.generation);
    uint32_t fps = le32toh(wanted.fps);
    if (wanted.reserved || !fps) return -EINVAL;
    pthread_mutex_lock(&server_lock);
    uint32_t index = config_index, width = config_width, height = config_height;
    pthread_mutex_unlock(&server_lock);
    struct camera_rates rates;
    int result = load_camera_rates(&camera_catalog->inputs[index], width, height, &rates);
    if (result < 0) return result;
    bool found = false;
    for (uint32_t i = 0; i < rates.count; ++i)
        if (rates.fps[i] == fps) found = true;
    if (!found) return -EINVAL;
    pthread_mutex_lock(&server_lock);
    result = server_state == 2 ? -ECANCELED :
             expected != config_generation || config_index != index ||
             config_width != width || config_height != height ? -EAGAIN :
             queue_owner ? -EBUSY :
             state->priority < highest_priority_locked(state) ? -EBUSY :
             config_generation == UINT64_MAX ? -EOVERFLOW : 0;
    if (!result && config_fps != fps) { config_fps = fps; ++config_generation; }
    struct fauxdev_camera_rate current = {
        .generation = htole64(config_generation), .fps = htole32(config_fps),
    };
    pthread_mutex_unlock(&server_lock);
    if (!result) {
        memcpy(response->payload, &current, sizeof(current));
        response->payload_size = sizeof(current);
    }
    return result;
}

static int serve_get_config(const struct fauxdev_message *request,
                            struct fauxdev_message *response) {
    if (request->payload_size || request->fd_count) return -EINVAL;
    pthread_mutex_lock(&server_lock);
    struct fauxdev_camera_config config = {
        .generation = htole64(config_generation), .index = htole32(config_index),
        .width = htole32(config_width), .height = htole32(config_height),
    };
    pthread_mutex_unlock(&server_lock);
    memcpy(response->payload, &config, sizeof(config));
    response->payload_size = sizeof(config);
    return 0;
}

static int serve_set_config(const struct fauxdev_message *request,
                            struct fauxdev_message *response,
                            struct client_camera_state *state) {
    if (request->payload_size != sizeof(struct fauxdev_camera_config) || request->fd_count)
        return -EINVAL;
    struct fauxdev_camera_config wanted;
    memcpy(&wanted, request->payload, sizeof(wanted));
    uint32_t index = le32toh(wanted.index), width = le32toh(wanted.width);
    uint32_t height = le32toh(wanted.height);
    uint64_t expected = le64toh(wanted.generation);
    if (wanted.reserved || index >= le32toh(camera_catalog->count)) return -EINVAL;
    struct camera_rates rates;
    int result = load_camera_rates(&camera_catalog->inputs[index], width, height, &rates);
    if (result < 0) return result;
    pthread_mutex_lock(&server_lock);
    result = server_state == 2 ? -ECANCELED :
             expected != config_generation ? -EAGAIN :
             queue_owner ? -EBUSY :
             state->priority < highest_priority_locked(state) ? -EBUSY :
             config_generation == UINT64_MAX ? -EOVERFLOW : 0;
    if (!result && (config_index != index || config_width != width || config_height != height)) {
        config_index = index;
        config_width = width;
        config_height = height;
        config_fps = nominal_rate(&rates);
        ++config_generation;
    }
    struct fauxdev_camera_config current = {
        .generation = htole64(config_generation), .index = htole32(config_index),
        .width = htole32(config_width), .height = htole32(config_height),
    };
    pthread_mutex_unlock(&server_lock);
    if (!result) {
        memcpy(response->payload, &current, sizeof(current));
        response->payload_size = sizeof(current);
    }
    return result;
}

static int serve_acquire_queue(const struct fauxdev_message *request,
                               struct client_camera_state *state) {
    if (request->payload_size != sizeof(struct fauxdev_camera_queue_request) || request->fd_count)
        return -EINVAL;
    struct fauxdev_camera_queue_request wanted;
    memcpy(&wanted, request->payload, sizeof(wanted));
    pthread_mutex_lock(&server_lock);
    int result = server_state == 2 ? -ECANCELED :
                 le64toh(wanted.generation) != config_generation ? -EAGAIN :
                 queue_owner && queue_owner != state ? -EBUSY :
                 state->priority < highest_priority_locked(state) ? -EBUSY : 0;
    if (!result) {
        queue_owner = state;
        state->queue_generation = config_generation;
    }
    pthread_mutex_unlock(&server_lock);
    return result;
}

static int serve_release_queue(const struct fauxdev_message *request,
                               struct client_camera_state *state) {
    if (request->payload_size || request->fd_count) return -EINVAL;
    pthread_mutex_lock(&server_lock);
    int result = queue_owner != state ? -EINVAL : state->active ? -EBUSY : 0;
    if (!result) { queue_owner = NULL; state->queue_generation = 0; state->implicit_queue = false; }
    pthread_mutex_unlock(&server_lock);
    return result;
}

static int serve_priority(const struct fauxdev_message *request,
                          struct fauxdev_message *response,
                          struct client_camera_state *state, bool update) {
    if (request->fd_count || request->payload_size !=
            (update ? sizeof(struct fauxdev_camera_priority) : 0)) return -EINVAL;
    struct fauxdev_camera_priority wanted = {0};
    if (update) {
        memcpy(&wanted, request->payload, sizeof(wanted));
        wanted.value = le32toh(wanted.value);
        wanted.enforce = le32toh(wanted.enforce);
        if (wanted.value > V4L2_PRIORITY_RECORD || wanted.enforce > 1) return -EINVAL;
    }
    pthread_mutex_lock(&server_lock);
    int result = update && wanted.enforce &&
                 wanted.value < (uint32_t)highest_priority_locked(state) ? -EBUSY : 0;
    if (!result && update) state->priority = (enum v4l2_priority)wanted.value;
    enum v4l2_priority highest = highest_priority_locked(NULL);
    pthread_mutex_unlock(&server_lock);
    if (result) return result;
    struct fauxdev_camera_priority reply = {.value = htole32(highest)};
    memcpy(response->payload, &reply, sizeof(reply));
    response->payload_size = sizeof(reply);
    return 0;
}

static int start_camera(struct client_camera_state *state, uint32_t index,
                        uint32_t width, uint32_t height, bool v2) {
    if (state->active) return -EBUSY;
    int result = load_inputs(state);
    if (result < 0) return result;
    if (index >= le32toh(state->inputs->count)) return -EINVAL;
    const struct fauxdev_camera_input *input = &state->inputs->inputs[index];
    uint32_t source_width = le32toh(input->source_width);
    uint32_t source_height = le32toh(input->source_height);
    if (v2) {
        struct camera_modes modes;
        result = load_camera_modes(input->id, &modes);
        if (result < 0) return result;
        bool found = false;
        for (uint32_t i = 0; i < modes.count; ++i) {
            if (modes.items[i].width == width && modes.items[i].height == height) {
                found = true;
                if (modes.items[i].native) { source_width = width; source_height = height; }
                break;
            }
        }
        if (!found) return -EINVAL;
    } else if (width != OUTPUT_WIDTH || height != OUTPUT_HEIGHT) return -EINVAL;
    struct camera_rates legacy_rates;
    if (!v2) {
        result = load_camera_rates(input, width, height, &legacy_rates);
        if (result < 0) return result;
    }
    pthread_mutex_lock(&server_lock);
    uint32_t target_fps = config_fps;
    if (v2) {
        result = queue_owner != state ? -EBUSY :
                 state->queue_generation != config_generation ? -EAGAIN :
                 config_index != index || config_width != width || config_height != height ? -EINVAL : 0;
    } else {
        result = queue_owner && queue_owner != state ? -EBUSY :
                 queue_owner == state &&
                 (config_index != index || config_width != width || config_height != height) ? -EINVAL :
                 config_generation == UINT64_MAX ? -EOVERFLOW : 0;
        if (!result && !queue_owner) {
            uint32_t nominal = nominal_rate(&legacy_rates);
            if (config_index != index || config_width != width || config_height != height ||
                config_fps != nominal) {
                config_index = index;
                config_width = width;
                config_height = height;
                config_fps = nominal;
                ++config_generation;
            }
            queue_owner = state;
            state->queue_generation = config_generation;
            state->implicit_queue = true;
        }
        target_fps = config_fps;
    }
    pthread_mutex_unlock(&server_lock);
    if (result < 0) return result;
    result = reserve_camera(state);
    if (result < 0) goto fail_start;
    struct camera *camera = calloc(1, sizeof(*camera));
    if (!camera) { result = -ENOMEM; release_camera(state); goto fail_start; }
    result = open_camera(camera, input, width, height, source_width, source_height, target_fps);
    if (result >= 0 && stop_requested()) result = -ECANCELED;
    if (result < 0) {
        close_camera(camera);
        free(camera);
        release_camera(state);
        goto fail_start;
    }
    state->active = camera;
    return 0;
fail_start:
    if (state->implicit_queue) {
        pthread_mutex_lock(&server_lock);
        if (queue_owner == state) queue_owner = NULL;
        state->queue_generation = 0;
        state->implicit_queue = false;
        pthread_mutex_unlock(&server_lock);
    }
    return result;
}

static int serve_start(const struct fauxdev_message *request, struct client_camera_state *state) {
    if (request->payload_size != sizeof(struct fauxdev_camera_start_request) || request->fd_count)
        return -EINVAL;
    struct fauxdev_camera_start_request start;
    memcpy(&start, request->payload, sizeof(start));
    return start_camera(state, le32toh(start.index), OUTPUT_WIDTH, OUTPUT_HEIGHT, false);
}

static int serve_start_v2(const struct fauxdev_message *request, struct client_camera_state *state) {
    if (request->payload_size != sizeof(struct fauxdev_camera_start_v2_request) || request->fd_count)
        return -EINVAL;
    struct fauxdev_camera_start_v2_request start;
    memcpy(&start, request->payload, sizeof(start));
    return start_camera(state, le32toh(start.index), le32toh(start.width),
                        le32toh(start.height), true);
}

static int serve_legacy_frame(int client, const struct fauxdev_message *request,
                              struct client_camera_state *state) {
    if (!state->active) {
        if (state->explicit_lifecycle) return -EPIPE;
        struct fauxdev_video_frame_request frame;
        int result = validate_frame_request(request, OUTPUT_WIDTH, OUTPUT_HEIGHT, OUTPUT_SIZE, &frame);
        if (result < 0) return result;
        result = load_inputs(state);
        if (result < 0) return result;
        if (!le32toh(state->inputs->count)) return -ENODEV;
        result = start_camera(state, le32toh(state->inputs->default_index),
                              OUTPUT_WIDTH, OUTPUT_HEIGHT, false);
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
        release_camera(state);
    }
    if (state->implicit_queue) {
        pthread_mutex_lock(&server_lock);
        if (queue_owner == state) queue_owner = NULL;
        state->queue_generation = 0;
        state->implicit_queue = false;
        pthread_mutex_unlock(&server_lock);
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
        case FAUXDEV_OPCODE_CAMERA_MODES:
            status = serve_modes(&request, &response, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_ENUM_RATES:
            status = serve_rates(&request, &response);
            break;
        case FAUXDEV_OPCODE_CAMERA_GET_RATE:
            status = serve_get_rate(&request, &response);
            break;
        case FAUXDEV_OPCODE_CAMERA_SET_RATE:
            status = serve_set_rate(&request, &response, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_GET_CONFIG:
            status = serve_get_config(&request, &response);
            break;
        case FAUXDEV_OPCODE_CAMERA_SET_CONFIG:
            status = serve_set_config(&request, &response, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_ACQUIRE_QUEUE:
            status = serve_acquire_queue(&request, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_RELEASE_QUEUE:
            status = serve_release_queue(&request, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_GET_PRIORITY:
            status = serve_priority(&request, &response, state, false);
            break;
        case FAUXDEV_OPCODE_CAMERA_SET_PRIORITY:
            status = serve_priority(&request, &response, state, true);
            break;
        case FAUXDEV_OPCODE_CAMERA_START:
            state->explicit_lifecycle = true;
            status = serve_start(&request, state);
            break;
        case FAUXDEV_OPCODE_CAMERA_START_V2:
            state->explicit_lifecycle = true;
            status = serve_start_v2(&request, state);
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

static void *client_worker(void *argument) {
    struct client_slot *slot = argument;
    struct inventory_cache cache = {0};
    struct client_camera_state state = {.priority = V4L2_PRIORITY_DEFAULT};
    pthread_mutex_lock(&server_lock);
    state.next = client_states;
    client_states = &state;
    pthread_mutex_unlock(&server_lock);
    int status = serve_client(slot->fd, &state, &cache);
    if (state.active) {
        close_camera(state.active);
        free(state.active);
    }
    release_camera(&state);
    pthread_mutex_lock(&server_lock);
    if (queue_owner == &state) queue_owner = NULL;
    for (struct client_camera_state **cursor = &client_states; *cursor; cursor = &(*cursor)->next)
        if (*cursor == &state) { *cursor = state.next; break; }
    pthread_mutex_unlock(&server_lock);
    free(state.inputs);
    free(cache.text);
    if (status < 0 && status != -ECANCELED)
        fprintf(stderr, "fauxdev camera client error: %d\n", status);
    pthread_mutex_lock(&server_lock);
    slot->done = true;
    pthread_mutex_unlock(&server_lock);
    return NULL;
}

static void reap_clients(bool all) {
    for (unsigned int i = 0; i < MAX_CLIENTS; ++i) {
        pthread_mutex_lock(&server_lock);
        struct client_slot *slot = &clients[i];
        const bool reap = slot->started && (all || slot->done);
        pthread_t thread = slot->thread;
        pthread_mutex_unlock(&server_lock);
        if (!reap) continue;
        pthread_join(thread, NULL);
        pthread_mutex_lock(&server_lock);
        close(slot->fd);
        slot->fd = -1;
        slot->started = false;
        slot->done = false;
        pthread_mutex_unlock(&server_lock);
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
    for (unsigned int i = 0; i < MAX_CLIENTS; ++i) clients[i].fd = -1;
    pthread_mutex_unlock(&server_lock);

    int result;
    camera_catalog = calloc(1, sizeof(*camera_catalog));
    if (!camera_catalog) { result = -ENOMEM; goto finish_server; }
    ACameraManager *catalog_manager = ACameraManager_create();
    if (!catalog_manager) { result = -EIO; goto finish_server; }
    result = collect_inputs(catalog_manager, camera_catalog);
    ACameraManager_delete(catalog_manager);
    if (result < 0) goto finish_server;
    config_index = le32toh(camera_catalog->default_index);
    config_width = OUTPUT_WIDTH;
    config_height = OUTPUT_HEIGHT;
    config_generation = 1;
    struct camera_rates initial_rates;
    config_fps = 0;
    if (le32toh(camera_catalog->count)) {
        result = load_camera_rates(&camera_catalog->inputs[config_index],
                                   config_width, config_height, &initial_rates);
        if (result < 0) goto finish_server;
        config_fps = nominal_rate(&initial_rates);
    }
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
        chmod(socket_path, 0600) < 0 || listen(listener, MAX_CLIENTS) < 0) {
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
            reap_clients(false);
            if (poll(watched, 2, -1) < 0) {
                if (errno == EINTR) continue;
                result = -errno;
                break;
            }
            if (watched[1].revents) break;
            if (!(watched[0].revents & POLLIN)) { result = -EIO; break; }
            int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            if (client < 0) { if (errno == EINTR) continue; result = -errno; break; }
            reap_clients(false);
            pthread_mutex_lock(&server_lock);
            const int client_stopped = server_state == 2;
            struct client_slot *slot = NULL;
            if (!client_stopped) for (unsigned int i = 0; i < MAX_CLIENTS; ++i)
                if (clients[i].fd < 0) { slot = &clients[i]; break; }
            if (slot) {
                slot->fd = client;
                slot->done = false;
                int error = pthread_create(&slot->thread, NULL, client_worker, slot);
                if (error) {
                    slot->fd = -1;
                    result = -error;
                    slot = NULL;
                } else slot->started = true;
            }
            pthread_mutex_unlock(&server_lock);
            if (!slot) close(client);
            if (result < 0) break;
        }
    }
    pthread_mutex_lock(&server_lock);
    for (unsigned int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].fd >= 0) shutdown(clients[i].fd, SHUT_RDWR);
    pthread_mutex_unlock(&server_lock);
    reap_clients(true);
    pthread_mutex_lock(&server_lock);
    server_stop_fd = -1;
    pthread_mutex_unlock(&server_lock);
    close(stop_fd);
    close(listener);
finish_socket:
    unlink(socket_path);
finish_server:
    free(camera_catalog);
    camera_catalog = NULL;
    config_generation = 0;
    config_fps = 0;
    queue_owner = NULL;
    client_states = NULL;
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
    for (unsigned int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].fd >= 0) shutdown(clients[i].fd, SHUT_RDWR);
    pthread_mutex_unlock(&server_lock);
}
