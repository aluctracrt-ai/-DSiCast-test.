#ifndef DSICAST_PROTOCOL_MIN_H
#define DSICAST_PROTOCOL_MIN_H

#include <stdint.h>

#define DSICAST_MAGIC_0 'D'
#define DSICAST_MAGIC_1 'S'
#define DSICAST_MAGIC_2 'C'
#define DSICAST_MAGIC_3 '1'
#define DSICAST_VERSION 1u
#define DSICAST_NATIVE_WIDTH 256u
#define DSICAST_NATIVE_HEIGHT 192u
#define DSICAST_STREAM_WIDTH 128u
#define DSICAST_STREAM_HEIGHT 96u
#define DSICAST_STREAM_BYTES (DSICAST_STREAM_WIDTH * DSICAST_STREAM_HEIGHT * 2u)
#define DSICAST_DISCOVERY_PORT 4661u
#define DSICAST_MAX_PAYLOAD 1200u
#define DSICAST_HEADER_SIZE 36u

#define DSICAST_PACKET_HELLO 1u
#define DSICAST_PACKET_HELLO_ACK 2u
#define DSICAST_PACKET_STREAM_START 3u
#define DSICAST_PACKET_STREAM_STOP 4u
#define DSICAST_PACKET_KEYFRAME 5u
#define DSICAST_PACKET_PING 7u
#define DSICAST_PACKET_PONG 8u
#define DSICAST_PACKET_STATS 9u
#define DSICAST_PACKET_DISCOVERY 11u
#define DSICAST_PACKET_DEVICE 12u
#define DSICAST_PACKET_INPUT_STATE 13u
#define DSICAST_PACKET_GAME_START 15u
#define DSICAST_PACKET_GAME_EXIT 16u
#define DSICAST_PACKET_SET_SCREEN_MODE 17u
#define DSICAST_PACKET_SCREEN_MODE_STATUS 18u

#define DSICAST_ENCODING_NONE 0u
#define DSICAST_ENCODING_RAW_RGB555 1u
#define DSICAST_SCREEN_MAIN_AUTO 0u
#define DSICAST_SCREEN_SUPPORTED 1u

#pragma pack(push, 1)
typedef struct DSiCastPacketHeader {
    char magic[4];
    uint8_t version;
    uint8_t packet_type;
    uint8_t encoding;
    uint8_t flags;
    uint32_t stream_id;
    uint32_t frame_id;
    uint16_t packet_index;
    uint16_t packet_count;
    uint16_t payload_length;
    uint16_t width;
    uint16_t height;
    uint16_t reserved;
    uint32_t timestamp_ms;
    uint32_t sequence;
} DSiCastPacketHeader;

typedef struct DSiCastHelloPayload {
    uint16_t width;
    uint16_t height;
    uint8_t preferred_encoding;
    uint8_t preferred_fps;
} DSiCastHelloPayload;

typedef struct DSiCastInputPayload {
    uint16_t buttons;
} DSiCastInputPayload;

typedef struct DSiCastStatsPayload {
    uint32_t capture_us;
    uint32_t downscale_us;
    uint32_t encode_us;
    uint32_t enqueue_us;
    uint32_t capture_drops;
    uint32_t network_drops;
    uint32_t queue_depth;
    uint32_t actual_fps_milli;
} DSiCastStatsPayload;

typedef struct DSiCastGameInfoPayloadHeader {
    uint8_t title_length;
    uint8_t game_code_length;
} DSiCastGameInfoPayloadHeader;

typedef struct DSiCastScreenModeStatusPayload {
    uint8_t requested_mode;
    uint8_t status;
    uint8_t active_mode;
} DSiCastScreenModeStatusPayload;
#pragma pack(pop)

typedef char dsicast_hdr_must_be_36[(sizeof(DSiCastPacketHeader) == 36) ? 1 : -1];

#endif
