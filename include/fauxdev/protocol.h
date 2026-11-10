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
#else
_Static_assert(sizeof(struct fauxdev_video_frame_request) == 24, "unexpected video request layout");
_Static_assert(sizeof(struct fauxdev_inventory_request) == 4, "unexpected inventory request layout");
_Static_assert(sizeof(struct fauxdev_inventory_reply) == 8, "unexpected inventory reply layout");
_Static_assert(sizeof(struct fauxdev_camera_input) == 268, "unexpected camera input layout");
_Static_assert(sizeof(struct fauxdev_camera_start_request) == 4, "unexpected camera start layout");
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
