#ifndef FAUXDEV_PROTOCOL_H
#define FAUXDEV_PROTOCOL_H

#include <stdint.h>

#define FAUXDEV_PROTOCOL_MAGIC 0x46584456u
#define FAUXDEV_PROTOCOL_VERSION 1u
#define FAUXDEV_PROTOCOL_MAX_PAYLOAD (64u * 1024u)
#define FAUXDEV_PROTOCOL_MAX_FDS 4u

enum fauxdev_message_flags {
    FAUXDEV_MESSAGE_REQUEST = 1u,
    FAUXDEV_MESSAGE_RESPONSE = 2u,
};

enum fauxdev_opcode {
    FAUXDEV_OPCODE_HELLO = 1u,
    FAUXDEV_OPCODE_VIDEO_FRAME = 2u,
    FAUXDEV_OPCODE_CAMERA_INVENTORY = 3u,
    FAUXDEV_OPCODE_CAMERA_INPUTS = 4u,
    FAUXDEV_OPCODE_CAMERA_START = 5u,
    FAUXDEV_OPCODE_CAMERA_STOP = 6u,
    FAUXDEV_OPCODE_CAMERA_MODES = 7u,
    FAUXDEV_OPCODE_CAMERA_START_V2 = 8u,
    FAUXDEV_OPCODE_CAMERA_GET_CONFIG = 9u,
    FAUXDEV_OPCODE_CAMERA_SET_CONFIG = 10u,
    FAUXDEV_OPCODE_CAMERA_ACQUIRE_QUEUE = 11u,
    FAUXDEV_OPCODE_CAMERA_RELEASE_QUEUE = 12u,
    FAUXDEV_OPCODE_CAMERA_GET_PRIORITY = 13u,
    FAUXDEV_OPCODE_CAMERA_SET_PRIORITY = 14u,
    FAUXDEV_OPCODE_CAMERA_ENUM_RATES = 15u,
    FAUXDEV_OPCODE_CAMERA_GET_RATE = 16u,
    FAUXDEV_OPCODE_CAMERA_SET_RATE = 17u,
};

#define FAUXDEV_CAMERA_MAX_INPUTS 64u
#define FAUXDEV_CAMERA_ID_SIZE 256u

/* All integer fields are little-endian; IDs are NUL-terminated. */
struct fauxdev_camera_input {
    uint32_t facing;
    uint32_t source_width;
    uint32_t source_height;
    char id[FAUXDEV_CAMERA_ID_SIZE];
};

struct fauxdev_camera_inputs_reply {
    uint32_t count;
    uint32_t default_index;
    struct fauxdev_camera_input inputs[FAUXDEV_CAMERA_MAX_INPUTS];
};

struct fauxdev_camera_start_request {
    uint32_t index;
};

/* Optional extensions to wire v1. Older brokers reply ENOTTY. */
#define FAUXDEV_CAMERA_MODE_PAGE 64u
struct fauxdev_camera_mode {
    uint32_t width;
    uint32_t height;
};
struct fauxdev_camera_modes_request {
    uint32_t index;
    uint32_t offset;
};
struct fauxdev_camera_modes_reply {
    uint32_t total;
    uint32_t offset;
    struct fauxdev_camera_mode modes[FAUXDEV_CAMERA_MODE_PAGE];
};
struct fauxdev_camera_start_v2_request {
    uint32_t index;
    uint32_t width;
    uint32_t height;
};

struct fauxdev_camera_config {
    uint64_t generation;
    uint32_t index;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
};
struct fauxdev_camera_queue_request {
    uint64_t generation;
};
struct fauxdev_camera_priority {
    uint32_t value;
    uint32_t enforce;
};

#define FAUXDEV_CAMERA_RATE_PAGE 64u
struct fauxdev_camera_rates_request {
    uint32_t index;
    uint32_t width;
    uint32_t height;
    uint32_t offset;
};
struct fauxdev_camera_rates_reply {
    uint32_t total;
    uint32_t offset;
    uint32_t fps[FAUXDEV_CAMERA_RATE_PAGE];
};
struct fauxdev_camera_rate {
    uint64_t generation;
    uint32_t fps;
    uint32_t reserved;
};

struct fauxdev_video_frame_request {
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t size;
};

/* Inventory fields are little-endian. Data following the reply header is UTF-8 text. */
struct fauxdev_inventory_request {
    uint32_t offset;
};

struct fauxdev_inventory_reply {
    uint32_t total;
    uint32_t offset;
};

#ifdef __cplusplus
static_assert(sizeof(struct fauxdev_video_frame_request) == 24, "unexpected video request layout");
static_assert(sizeof(struct fauxdev_inventory_request) == 4, "unexpected inventory request layout");
static_assert(sizeof(struct fauxdev_inventory_reply) == 8, "unexpected inventory reply layout");
static_assert(sizeof(struct fauxdev_camera_input) == 268, "unexpected camera input layout");
static_assert(sizeof(struct fauxdev_camera_start_request) == 4, "unexpected camera start layout");
static_assert(sizeof(struct fauxdev_camera_mode) == 8, "unexpected camera mode layout");
static_assert(sizeof(struct fauxdev_camera_start_v2_request) == 12, "unexpected camera start v2 layout");
static_assert(sizeof(struct fauxdev_camera_config) == 24, "unexpected camera config layout");
static_assert(sizeof(struct fauxdev_camera_queue_request) == 8, "unexpected queue layout");
static_assert(sizeof(struct fauxdev_camera_rates_request) == 16, "unexpected rate request layout");
static_assert(sizeof(struct fauxdev_camera_rate) == 16, "unexpected rate layout");
#else
_Static_assert(sizeof(struct fauxdev_video_frame_request) == 24, "unexpected video request layout");
_Static_assert(sizeof(struct fauxdev_inventory_request) == 4, "unexpected inventory request layout");
_Static_assert(sizeof(struct fauxdev_inventory_reply) == 8, "unexpected inventory reply layout");
_Static_assert(sizeof(struct fauxdev_camera_input) == 268, "unexpected camera input layout");
_Static_assert(sizeof(struct fauxdev_camera_start_request) == 4, "unexpected camera start layout");
_Static_assert(sizeof(struct fauxdev_camera_mode) == 8, "unexpected camera mode layout");
_Static_assert(sizeof(struct fauxdev_camera_start_v2_request) == 12, "unexpected camera start v2 layout");
_Static_assert(sizeof(struct fauxdev_camera_config) == 24, "unexpected camera config layout");
_Static_assert(sizeof(struct fauxdev_camera_queue_request) == 8, "unexpected queue layout");
_Static_assert(sizeof(struct fauxdev_camera_rates_request) == 16, "unexpected rate request layout");
_Static_assert(sizeof(struct fauxdev_camera_rate) == 16, "unexpected rate layout");
#endif

struct fauxdev_wire_header {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t flags;
    uint32_t payload_size;
    uint64_t request_id;
    int32_t status;
    uint32_t fd_count;
};

#ifdef __cplusplus
static_assert(sizeof(struct fauxdev_wire_header) == 32, "unexpected fauxdev header layout");
#else
_Static_assert(sizeof(struct fauxdev_wire_header) == 32, "unexpected fauxdev header layout");
#endif

#endif
