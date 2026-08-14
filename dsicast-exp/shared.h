#ifndef DSICAST_EXPERIMENT_SHARED_H
#define DSICAST_EXPERIMENT_SHARED_H

#include <stdint.h>

#define DSICAST_SHARED_ADDR       0x026D6800u
#define DSICAST_SHARED_MAGIC      0x55435344u /* 'DSCU' little endian */
#define DSICAST_ARM9_BASE         0x02F92000u
#define DSICAST_ARM9_LIMIT        0x00036000u
#define DSICAST_ARM7_BASE         0x02FC8000u
#define DSICAST_ARM7_LIMIT        0x00010000u
#define DSICAST_ARM9_STACK_TOP    0x02FC8000u
#define DSICAST_ARM7_STACK_TOP    0x02FD8000u
#define DSICAST_CAPTURE_BACKUP    0x026B8000u
#define DSICAST_FRAME_BUFFER      0x026D0000u
#define DSICAST_PACKET_BUFFER     0x026D6000u

#define DSICAST_HOTKEY_TOGGLE     0x0304u /* L+R+SELECT */
#define DSICAST_HOTKEY_EMERGENCY  0x0308u /* L+R+START  */

/* Keep this well under the 0x800 bytes available before 0x026E0000. */
typedef struct __attribute__((packed, aligned(4))) DSiCastShared {
    volatile uint32_t magic;
    volatile uint32_t vblank_counter;
    volatile uint32_t stream_requested;
    volatile uint32_t emergency_stop;
    volatile uint32_t wifi_data;
    volatile uint32_t sync_9_to_7;
    volatile uint32_t sync_7_to_9;
    volatile uint32_t arm7_ready;
    volatile uint32_t arm9_ready;
    volatile uint32_t wifi_state;
    volatile uint32_t receiver_state;
    volatile uint32_t frame_id;
    volatile uint32_t frames_sent;
    volatile uint32_t packets_sent;
    volatile uint32_t network_stalls;
    volatile uint32_t capture_skips;
    volatile uint32_t last_error;
    volatile uint32_t key_event_seq;
    volatile uint16_t buttons;
    volatile uint16_t reserved0;
} DSiCastShared;

static inline volatile DSiCastShared *dsicast_shared(void) {
    return (volatile DSiCastShared *)DSICAST_SHARED_ADDR;
}

#endif
