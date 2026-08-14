#include <nds.h>
#include <nds/arm9/video.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <dswifi9.h>

#include "shared.h"
#include "dsicast_protocol_min.h"

#ifndef FIONBIO
#define FIONBIO 0x8004667eu
#endif

extern int closesocket(int socket);
extern int ioctl(int socket, long cmd, void *arg);

#define WIFI_INIT_64K_INTERNAL 0x1000u
#define VRAM_CR_PTR ((volatile uint8_t *)0x04000240)
#define VRAM_ADDR(bank) ((volatile uint16_t *)(0x06800000u + ((uint32_t)(bank) * 0x20000u)))
#define VRAM_ENABLED 0x80u
#define NDS_HEADER_ADDR ((const uint8_t *)0x027FFE00u)
#define STREAM_ID 0x54435344u /* 'DSCT' */

static uint8_t s_wifi_heap[64u * 1024u] __attribute__((aligned(32)));
static bool s_malloc_claimed;

/* dswifi 0.4.2 allocates one backing heap through malloc(), then manages
   packet allocations internally.  A fixed heap avoids depending on the
   commercial game's libc heap. */
void *malloc(size_t size) {
    if (!s_malloc_claimed && size <= sizeof(s_wifi_heap)) {
        s_malloc_claimed = true;
        return s_wifi_heap;
    }
    return NULL;
}
void free(void *ptr) {
    (void)ptr;
}

static bool s_wifi_started;
static bool s_wifi_autoconnect;
static bool s_udp_ready;
static bool s_receiver_known;
static bool s_connected;
static int s_socket = -1;
static struct sockaddr_in s_receiver;
static uint32_t s_sequence;
static uint32_t s_frame_id;
static uint32_t s_timer_div;
static uint32_t s_frames_window;
static uint32_t s_window_start_vblank;
static uint16_t s_last_buttons = 0xFFFFu;

static bool s_capture_active;
static bool s_capture_backed_up;
static uint8_t s_capture_bank;
static uint8_t s_capture_original_cr;
static bool s_frame_ready;
static uint16_t s_frame_packet_index;
static uint16_t s_frame_packet_count;

static volatile DSiCastShared *shared_uncached(void) {
    return (volatile DSiCastShared *)memUncached((void *)DSICAST_SHARED_ADDR);
}

static uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static uint32_t now_ms(void) {
    volatile DSiCastShared *s = shared_uncached();
    return (s->vblank_counter * 1000u) / 60u;
}

static void copy32(void *dstv, const void *srcv, uint32_t bytes) {
    uint32_t *dst = (uint32_t *)dstv;
    const uint32_t *src = (const uint32_t *)srcv;
    uint32_t words = bytes >> 2;
    while (words--) *dst++ = *src++;
}

static void wifi_sync_to_arm7(void) {
    volatile DSiCastShared *s = shared_uncached();
    if (s->magic == DSICAST_SHARED_MAGIC) s->sync_9_to_7 = 1;
}

static void init_header(
    DSiCastPacketHeader *h, uint8_t type, uint8_t encoding,
    uint32_t frame_id, uint16_t packet_index, uint16_t packet_count,
    uint16_t payload_length, uint16_t width, uint16_t height,
    uint32_t sequence
) {
    h->magic[0] = DSICAST_MAGIC_0;
    h->magic[1] = DSICAST_MAGIC_1;
    h->magic[2] = DSICAST_MAGIC_2;
    h->magic[3] = DSICAST_MAGIC_3;
    h->version = DSICAST_VERSION;
    h->packet_type = type;
    h->encoding = encoding;
    h->flags = 0;
    h->stream_id = STREAM_ID;
    h->frame_id = frame_id;
    h->packet_index = packet_index;
    h->packet_count = packet_count;
    h->payload_length = payload_length;
    h->width = width;
    h->height = height;
    h->reserved = 0;
    h->timestamp_ms = now_ms();
    h->sequence = sequence;
}

static bool send_control(uint8_t type, const void *payload, uint16_t payload_len) {
    uint8_t *packet = (uint8_t *)DSICAST_PACKET_BUFFER;
    DSiCastPacketHeader *h = (DSiCastPacketHeader *)packet;
    int sent;
    if (!s_receiver_known || s_socket < 0 || payload_len > DSICAST_MAX_PAYLOAD) return false;
    init_header(h, type, DSICAST_ENCODING_NONE, 0, 0, 1, payload_len,
                DSICAST_NATIVE_WIDTH, DSICAST_NATIVE_HEIGHT, s_sequence++);
    if (payload_len) memcpy(packet + sizeof(*h), payload, payload_len);
    sent = sendto(s_socket, packet, sizeof(*h) + payload_len, 0,
                  (struct sockaddr *)&s_receiver, sizeof(s_receiver));
    return sent == (int)(sizeof(*h) + payload_len);
}

static void send_device(void) {
    uint8_t payload[2 + 48];
    static const char name[] = "Nintendo DSi DSiCast In-Game";
    uint16_t port = DSICAST_DISCOVERY_PORT;
    size_t len = sizeof(name) - 1;
    payload[0] = (uint8_t)(port & 0xFF);
    payload[1] = (uint8_t)(port >> 8);
    if (len > 48) len = 48;
    memcpy(payload + 2, name, len);
    send_control(DSICAST_PACKET_DEVICE, payload, (uint16_t)(2 + len));
}

static void send_game_start(void) {
    uint8_t payload[2 + 12 + 4];
    const uint8_t *hdr = NDS_HEADER_ADDR;
    uint8_t title_len = 12;
    uint8_t code_len = 4;
    unsigned i;
    for (i = 0; i < 12; ++i) {
        if (hdr[i] == 0) { title_len = (uint8_t)i; break; }
    }
    payload[0] = title_len;
    payload[1] = code_len;
    memcpy(payload + 2, hdr, title_len);
    memcpy(payload + 2 + title_len, hdr + 0x0C, code_len);
    send_control(DSICAST_PACKET_GAME_START, payload, (uint16_t)(2 + title_len + code_len));
}

static bool valid_control_header(const DSiCastPacketHeader *h, int received) {
    return received >= (int)sizeof(*h)
        && h->magic[0] == DSICAST_MAGIC_0 && h->magic[1] == DSICAST_MAGIC_1
        && h->magic[2] == DSICAST_MAGIC_2 && h->magic[3] == DSICAST_MAGIC_3
        && h->version == DSICAST_VERSION
        && h->packet_count == 1 && h->packet_index == 0
        && h->reserved == 0
        && h->payload_length <= DSICAST_MAX_PAYLOAD
        && received == (int)(sizeof(*h) + h->payload_length);
}

static void poll_receiver(void) {
    uint8_t incoming[DSICAST_HEADER_SIZE + DSICAST_MAX_PAYLOAD] __attribute__((aligned(4)));
    for (;;) {
        struct sockaddr_in source;
        int source_len = sizeof(source);
        int received = recvfrom(s_socket, incoming, sizeof(incoming), 0,
                                (struct sockaddr *)&source, &source_len);
        DSiCastPacketHeader *h;
        uint8_t *payload;
        if (received < 0) break;
        h = (DSiCastPacketHeader *)incoming;
        payload = incoming + sizeof(*h);
        if (!valid_control_header(h, received)) continue;

        if (h->packet_type == DSICAST_PACKET_DISCOVERY && h->payload_length >= 2) {
            uint16_t port = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            s_receiver = source;
            s_receiver.sin_port = bswap16(port);
            s_receiver_known = true;
            send_device();
        } else if (h->packet_type == DSICAST_PACKET_HELLO && h->payload_length == sizeof(DSiCastHelloPayload)) {
            DSiCastHelloPayload hello;
            memcpy(&hello, payload, sizeof(hello));
            s_receiver = source;
            s_receiver_known = true;
            s_connected = true;
            /* Intentionally DO NOT use hello.preferred_fps.  This experimental
               build has no software FPS target or scheduler. */
            send_control(DSICAST_PACKET_HELLO_ACK, &hello, sizeof(hello));
            send_game_start();
            send_control(DSICAST_PACKET_STREAM_START, NULL, 0);
            shared_uncached()->receiver_state = 2;
        } else if (h->packet_type == DSICAST_PACKET_PING && h->payload_length == 4) {
            s_receiver = source;
            s_receiver_known = true;
            send_control(DSICAST_PACKET_PONG, payload, 4);
        } else if (h->packet_type == DSICAST_PACKET_STREAM_STOP) {
            s_connected = false;
            s_frame_ready = false;
            shared_uncached()->receiver_state = 1;
        } else if (h->packet_type == DSICAST_PACKET_SET_SCREEN_MODE && h->payload_length == 1) {
            DSiCastScreenModeStatusPayload st;
            st.requested_mode = payload[0];
            st.status = (payload[0] == DSICAST_SCREEN_MAIN_AUTO) ? DSICAST_SCREEN_SUPPORTED : 2;
            st.active_mode = DSICAST_SCREEN_MAIN_AUTO;
            send_control(DSICAST_PACKET_SCREEN_MODE_STATUS, &st, sizeof(st));
        }
    }
}

static bool init_udp(void) {
    struct sockaddr_in local;
    int nonblocking = 1;
    if (s_udp_ready) return true;
    s_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_socket < 0) return false;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = bswap16(DSICAST_DISCOVERY_PORT);
    local.sin_addr.s_addr = 0;
    if (bind(s_socket, (struct sockaddr *)&local, sizeof(local)) < 0
        || ioctl(s_socket, FIONBIO, &nonblocking) < 0) {
        closesocket(s_socket);
        s_socket = -1;
        return false;
    }
    s_udp_ready = true;
    shared_uncached()->receiver_state = 1;
    return true;
}

static int choose_capture_bank(void) {
    static const uint8_t order[4] = {3, 2, 1, 0};
    unsigned i;
    for (i = 0; i < 4; ++i) {
        if ((VRAM_CR_PTR[order[i]] & VRAM_ENABLED) == 0) return order[i];
    }
    for (i = 0; i < 4; ++i) {
        if ((VRAM_CR_PTR[order[i]] & 7u) == 0) return order[i];
    }
    return 3;
}

static bool start_capture(void) {
    volatile uint16_t *vram;
    uint32_t *backup = (uint32_t *)DSICAST_CAPTURE_BACKUP;
    if (s_capture_active || s_frame_ready || (REG_DISPCAPCNT & DCAP_ENABLE)) return false;

    s_capture_bank = (uint8_t)choose_capture_bank();
    s_capture_original_cr = VRAM_CR_PTR[s_capture_bank];
    s_capture_backed_up = (s_capture_original_cr & VRAM_ENABLED) != 0;
    VRAM_CR_PTR[s_capture_bank] = VRAM_ENABLED; /* LCD mode */
    vram = VRAM_ADDR(s_capture_bank);

    if (s_capture_backed_up) {
        copy32(backup, (const void *)vram, 256u * 192u * 2u);
    }

    REG_DISPCAPCNT = DCAP_BANK(s_capture_bank)
        | DCAP_SIZE(DCAP_SIZE_256x192)
        | DCAP_MODE(DCAP_MODE_A)
        | DCAP_ENABLE;
    s_capture_active = true;
    return true;
}

static bool finish_capture_if_ready(void) {
    volatile uint16_t *src;
    uint16_t *dst = (uint16_t *)DSICAST_FRAME_BUFFER;
    uint32_t *backup = (uint32_t *)DSICAST_CAPTURE_BACKUP;
    unsigned y, x;
    if (!s_capture_active) return false;
    if (REG_DISPCAPCNT & DCAP_ENABLE) return false;

    src = VRAM_ADDR(s_capture_bank);
    /* 2x nearest-neighbour downscale.  Resolution is reduced, FPS is NOT capped. */
    for (y = 0; y < DSICAST_STREAM_HEIGHT; ++y) {
        const volatile uint16_t *row = src + (y * 2u * DSICAST_NATIVE_WIDTH);
        uint16_t *out = dst + y * DSICAST_STREAM_WIDTH;
        for (x = 0; x < DSICAST_STREAM_WIDTH; ++x) out[x] = row[x * 2u];
    }

    if (s_capture_backed_up) {
        copy32((void *)src, backup, 256u * 192u * 2u);
    }
    VRAM_CR_PTR[s_capture_bank] = s_capture_original_cr;
    s_capture_active = false;

    s_frame_ready = true;
    s_frame_packet_index = 0;
    s_frame_packet_count = (uint16_t)((DSICAST_STREAM_BYTES + DSICAST_MAX_PAYLOAD - 1u) / DSICAST_MAX_PAYLOAD);
    return true;
}

static bool pump_frame(void) {
    uint8_t *packet = (uint8_t *)DSICAST_PACKET_BUFFER;
    const uint8_t *frame = (const uint8_t *)DSICAST_FRAME_BUFFER;
    volatile DSiCastShared *s = shared_uncached();

    while (s_frame_ready && s_frame_packet_index < s_frame_packet_count) {
        uint32_t offset = (uint32_t)s_frame_packet_index * DSICAST_MAX_PAYLOAD;
        uint16_t payload_len = (uint16_t)(DSICAST_STREAM_BYTES - offset);
        DSiCastPacketHeader *h = (DSiCastPacketHeader *)packet;
        int sent;
        if (payload_len > DSICAST_MAX_PAYLOAD) payload_len = DSICAST_MAX_PAYLOAD;

        init_header(h, DSICAST_PACKET_KEYFRAME, DSICAST_ENCODING_RAW_RGB555,
                    s_frame_id, s_frame_packet_index, s_frame_packet_count,
                    payload_len, DSICAST_STREAM_WIDTH, DSICAST_STREAM_HEIGHT,
                    s_sequence + s_frame_packet_index);
        memcpy(packet + sizeof(*h), frame + offset, payload_len);
        sent = sendto(s_socket, packet, sizeof(*h) + payload_len, 0,
                      (struct sockaddr *)&s_receiver, sizeof(s_receiver));
        if (sent != (int)(sizeof(*h) + payload_len)) {
            s->network_stalls++;
            return false; /* retry same packet next VCount */
        }
        s_frame_packet_index++;
        s->packets_sent++;
    }

    if (s_frame_ready && s_frame_packet_index == s_frame_packet_count) {
        s_sequence += s_frame_packet_count;
        s_frame_ready = false;
        s_frame_id++;
        s_frames_window++;
        s->frame_id = s_frame_id;
        s->frames_sent++;
        return true;
    }
    return false;
}

static void send_input_if_changed(void) {
    volatile DSiCastShared *s = shared_uncached();
    uint16_t buttons = s->buttons & 0x0FFFu;
    if (buttons != s_last_buttons) {
        DSiCastInputPayload in;
        in.buttons = buttons;
        if (send_control(DSICAST_PACKET_INPUT_STATE, &in, sizeof(in))) s_last_buttons = buttons;
    }
}

static void send_stats_if_due(void) {
    volatile DSiCastShared *s = shared_uncached();
    uint32_t now_vb = s->vblank_counter;
    if (s_window_start_vblank == 0) s_window_start_vblank = now_vb;
    if (now_vb - s_window_start_vblank >= 60u) {
        DSiCastStatsPayload st;
        uint32_t elapsed = now_vb - s_window_start_vblank;
        memset(&st, 0, sizeof(st));
        st.capture_drops = s->capture_skips;
        st.network_drops = s->network_stalls;
        st.actual_fps_milli = elapsed ? (s_frames_window * 60000u) / elapsed : 0;
        send_control(DSICAST_PACKET_STATS, &st, sizeof(st));
        s_frames_window = 0;
        s_window_start_vblank = now_vb;
    }
}

static void maintain_wifi(void) {
    volatile DSiCastShared *s = shared_uncached();
    if (!s_wifi_started) return;
    if (s->sync_7_to_9) {
        s->sync_7_to_9 = 0;
        Wifi_Sync();
    }
    Wifi_Update();
    if (++s_timer_div >= 3u) {
        s_timer_div = 0;
        Wifi_Timer(50);
    }
}

static bool ensure_wifi_and_udp(void) {
    volatile DSiCastShared *s = shared_uncached();
    int status;

    if (!s_wifi_started) {
        unsigned long pass = Wifi_Init(WIFI_INIT_64K_INTERNAL);
        if (!pass) { s->last_error = 0x9001; return false; }
        Wifi_SetSyncHandler(wifi_sync_to_arm7);
        s->wifi_data = (uint32_t)pass;
        DC_FlushAll();
        s_wifi_started = true;
        s->wifi_state = 1;
        return false;
    }

    if (!Wifi_CheckInit()) return false;

    if (!s_wifi_autoconnect) {
        Wifi_AutoConnect();
        s_wifi_autoconnect = true;
        s->wifi_state = 3;
        return false;
    }

    status = Wifi_AssocStatus();
    if (status == ASSOCSTATUS_CANNOTCONNECT) {
        s->last_error = 0x9002;
        return false;
    }
    if (status != ASSOCSTATUS_ASSOCIATED) return false;
    s->wifi_state = 4;

    if (!init_udp()) {
        s->last_error = 0x9003;
        return false;
    }
    s->wifi_state = 5;
    return true;
}

static void stop_stream_only(void) {
    if (s_connected) send_control(DSICAST_PACKET_STREAM_STOP, NULL, 0);
    s_connected = false;
    s_receiver_known = false;
    if (s_capture_active && !(REG_DISPCAPCNT & DCAP_ENABLE)) finish_capture_if_ready();
    s_frame_ready = false;
    shared_uncached()->receiver_state = s_udp_ready ? 1 : 0;
}

void dsicast9_tick_impl(void) {
    volatile DSiCastShared *s = shared_uncached();
    if (s->magic != DSICAST_SHARED_MAGIC) return;
    s->arm9_ready = 1;

    /* Once our Wi-Fi stack has started it must keep being serviced, even while
       streaming is toggled off, so association state does not rot. */
    if (s_wifi_started) maintain_wifi();

    if (!s->stream_requested || s->emergency_stop) {
        if (s_connected || s_frame_ready || s_capture_active) stop_stream_only();
        if (s->emergency_stop) s->emergency_stop = 0;
        return;
    }

    if (!ensure_wifi_and_udp()) return;

    poll_receiver();
    if (!s_connected) return;

    send_input_if_changed();
    finish_capture_if_ready();

    if (s_frame_ready) pump_frame();

    /* No target FPS.  As soon as the previous frame has left the UDP queue,
       request the next hardware Display Capture.  Throughput is limited only by
       the display (~60 unique frames/s), capture cost, CPU and Wi-Fi backpressure. */
    if (!s_frame_ready && !s_capture_active) {
        if (!start_capture()) s->capture_skips++;
    }
    send_stats_if_due();
}
