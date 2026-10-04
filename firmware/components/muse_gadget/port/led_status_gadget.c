// led_status.h for agent_link: Home Link's status light, reported as muse_gadget_state_t instead
// of lit. Upstream each board picks one implementation of this header (led_status.c drives LEDs
// and status screens, epaper_status.c e-paper); this one hands the state to the transport, which
// turns it into agent_link_status_t for whatever the board draws.
#include "led_status.h"

#include <stdatomic.h>

#include "muse_gadget_priv.h"
#include "noise_control.h"

static atomic_int s_last = -1;

static muse_gadget_state_t map_state(led_state_t state)
{
    switch (state) {
    case LED_STATE_BOOT:                     return MUSE_GADGET_BOOT;
    case LED_STATE_SETUP_IDLE:
    case LED_STATE_BLE_ADVERTISING:          return MUSE_GADGET_ADVERTISING;
    case LED_STATE_BLE_CONNECTED:            return MUSE_GADGET_APP_CONNECTED;
    case LED_STATE_PAIRING_CONFIRM_REQUIRED: return MUSE_GADGET_CONFIRM;
    case LED_STATE_WIFI_CONNECTING:          return MUSE_GADGET_WIFI_CONNECTING;
    case LED_STATE_WIFI_CONNECTED:
    case LED_STATE_AUTH_OK:
    case LED_STATE_VM_SWITCHING:             return MUSE_GADGET_REACHING_MUSE;
    case LED_STATE_VM_OK:
    case LED_STATE_WS_CONNECTED:             return MUSE_GADGET_ONLINE;
    case LED_STATE_WS_DISCONNECTED:          return MUSE_GADGET_RECONNECTING;
    case LED_STATE_UNPAIRED:                 return MUSE_GADGET_UNPAIRED;
    case LED_STATE_ERROR:                    return MUSE_GADGET_ERROR;
    }
    return MUSE_GADGET_ERROR;
}

bool led_status_init(void)
{
    return true;
}

void led_status_set_state(led_state_t state)
{
    muse_gadget_state_t s = map_state(state);
    // The boot connect reports AUTH_OK after noise_ctrl_connect() returns, and the session's own
    // ws_connected can beat it there. Online is a fact about the session, not about the order the
    // two tasks happened to report in.
    if (s == MUSE_GADGET_REACHING_MUSE && noise_ctrl_is_connected()) s = MUSE_GADGET_ONLINE;
    if (atomic_exchange(&s_last, (int)s) == (int)s) return;
    muse_gadget_emit_state(s);
}

void led_status_set_title(const char *title)
{
    muse_gadget_emit_title(title);
}

// No display behind this backend: the board owns its screen through agent_link.
bool led_status_display_info(int *width, int *height)
{
    (void)width;
    (void)height;
    return false;
}

int led_status_display_bits(void)
{
    return 0;
}

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels)
{
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
    return false;
}

void led_status_draw_done(void) {}
void led_status_show_animation(void) {}
void led_status_set_voice(led_voice_t voice) { (void)voice; }
void led_status_set_level(float level) { (void)level; }
void led_status_show_volume(int percent) { (void)percent; }
