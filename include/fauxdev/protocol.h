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
};

struct fauxdev_video_frame_request {
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t size;
};

#ifdef __cplusplus
static_assert(sizeof(struct fauxdev_video_frame_request) == 24, "unexpected video request layout");
#else
_Static_assert(sizeof(struct fauxdev_video_frame_request) == 24, "unexpected video request layout");
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
