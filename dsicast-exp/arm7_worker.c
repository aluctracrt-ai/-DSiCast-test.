#include <nds.h>
#include <stdint.h>
#include <stdbool.h>
#include <dswifi7.h>

#include "shared.h"

#define REG_WIFIIRQ_DSICAST (*(volatile uint16_t *)0x04808012)

static bool s_wifi_initialized;
static bool s_toggle_latch;
static bool s_emergency_latch;

static void dsicast7_sync_to_arm9(void) {
    volatile DSiCastShared *s = dsicast_shared();
    if (s->magic == DSICAST_SHARED_MAGIC) {
        s->sync_7_to_9 = 1;
    }
}

void dsicast7_tick_impl(void) {
    volatile DSiCastShared *s = dsicast_shared();
    uint16_t held;
    bool toggle;
    bool emergency;

    if (s->magic != DSICAST_SHARED_MAGIC) return;

    s->arm7_ready = 1;
    s->vblank_counter++;

    held = (uint16_t)(~REG_KEYINPUT) & 0x03FFu;
    s->buttons = held;

    toggle = (held & DSICAST_HOTKEY_TOGGLE) == DSICAST_HOTKEY_TOGGLE;
    emergency = (held & DSICAST_HOTKEY_EMERGENCY) == DSICAST_HOTKEY_EMERGENCY;

    if (toggle && !s_toggle_latch) {
        s->stream_requested ^= 1u;
        s->key_event_seq++;
    }
    if (emergency && !s_emergency_latch) {
        s->stream_requested = 0;
        s->emergency_stop = 1;
        s->key_event_seq++;
    }
    s_toggle_latch = toggle;
    s_emergency_latch = emergency;

    if (!s_wifi_initialized && s->wifi_data != 0) {
        Wifi_Init(s->wifi_data);
        Wifi_SetSyncHandler(dsicast7_sync_to_arm9);
        s_wifi_initialized = true;
        s->wifi_state = 2;
    }

    if (!s_wifi_initialized) return;

    if (s->sync_9_to_7) {
        s->sync_9_to_7 = 0;
        Wifi_Sync();
    }

    /* Poll the latched Wi-Fi IRQ from our existing VBlank context.  This is
       deliberately experimental: no second IRQ table is installed into the game. */
    if (REG_WIFIIRQ_DSICAST != 0) {
        Wifi_Interrupt();
    }
    Wifi_Update();
}
