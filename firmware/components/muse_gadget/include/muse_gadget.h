// muse_gadget — Meta's Muse Home Link, run as a library under agent_link's Muse transport.
//
// Home Link (link/, vendored from facebookincubator/muse-gadget-sdk) owns everything a Muse gadget
// does on its own: BLE pairing with the Muse app (community pairing v5), Wi-Fi from that pairing,
// the device token, and the Noise session to the user's Muse VM. This header is the small surface
// the agent_link backend drives it through. Nothing here is board-facing; boards see agent_link.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where setup and the connection are, as Home Link's status light would show it. */
typedef enum {
    MUSE_GADGET_BOOT = 0,
    MUSE_GADGET_ADVERTISING,       ///< Not paired: waiting for the Muse app (Add Device)
    MUSE_GADGET_APP_CONNECTED,     ///< The Muse app is connected over BLE, pairing
    MUSE_GADGET_CONFIRM,           ///< Pairing waits for a press on the device (muse_gadget_confirm_press)
    MUSE_GADGET_WIFI_CONNECTING,   ///< Joining the Wi-Fi the app provisioned
    MUSE_GADGET_REACHING_MUSE,     ///< Wi-Fi up; device token / VM lookup / session handshake
    MUSE_GADGET_ONLINE,            ///< Registered with the Muse: voice notes can be sent
    MUSE_GADGET_RECONNECTING,      ///< Was online, session dropped; retrying by itself
    MUSE_GADGET_UNPAIRED,          ///< The Muse removed this device; set it up again
    MUSE_GADGET_ERROR,             ///< Wi-Fi or authentication failed; see the log
} muse_gadget_state_t;

typedef struct {
    /** Every state change. Runs on a Home Link task; copy and return. */
    void (*on_state)(muse_gadget_state_t state, void* ctx);
    /** The Muse's own name once the session knows it (GET /identity). May be NULL. */
    void (*on_title)(const char* name, void* ctx);
    void* ctx;
} muse_gadget_config_t;

/**
 * @brief Start Home Link on its own task.
 *
 * It initialises NVS, the Wi-Fi station and (while unpaired) the BLE setup server itself; nothing
 * else in the firmware may bring up those radios on this transport.
 */
esp_err_t muse_gadget_start(const muse_gadget_config_t* cfg);

/** "MuseGadget-XXXXXX", the name the Muse app lists. Empty until muse_gadget_start() has run. */
const char* muse_gadget_ble_name(void);

/** The control session to the VM is up and registered. */
bool muse_gadget_online(void);

/** A press on the device while pairing waits for one (MUSE_GADGET_CONFIRM). True if it confirmed. */
bool muse_gadget_confirm_press(void);

/** Forget the pairing and the Wi-Fi, then reboot into setup. Returns at once; the reset is async. */
void muse_gadget_forget(void);

// ── Voice notes ─────────────────────────────────────────────────────────────────────────────
// One push-to-talk turn: the speech goes to the Muse's chat as a voice note (a WAV the VM
// transcribes), and the reply comes back as text. All muse_note_* calls must come from one task.

typedef enum {
    MUSE_NOTE_EV_NONE = 0,
    MUSE_NOTE_EV_SENT,    ///< The VM acknowledged the note
    MUSE_NOTE_EV_HEARD,   ///< text = the transcript of what was said
    MUSE_NOTE_EV_REPLY,   ///< text = the reply so far (whole messages, newest appended)
    MUSE_NOTE_EV_DONE,    ///< The reply is complete; the turn is over
    MUSE_NOTE_EV_ERROR,   ///< The turn failed; text says why, in capitals
} muse_note_ev_t;

/** Sample rate of the PCM muse_note_audio() takes (16-bit mono). */
#define MUSE_NOTE_SAMPLE_RATE 16000

/** Online and able to start a turn. */
bool muse_note_ready(void);
/** Press: opens the note on the session. False (and an ERROR event) if it could not. */
bool muse_note_begin(void);
/** Speech, in order. Blocks while the session is behind: a slow uplink delays the note rather
 *  than ending it, so the caller buffers the speech that keeps coming meanwhile. The turn fails
 *  (an ERROR event) only when the session drops or takes nothing for 10 s. */
void muse_note_audio(const int16_t* pcm, size_t frames);
/** Release: the note is complete; the reply follows as events. Blocks like muse_note_audio. */
void muse_note_end(void);
/** Abandon the turn and anything still coming for it. */
void muse_note_cancel(void);
/** Moves the turn along and returns its next event (non-blocking); copies the event's text. */
muse_note_ev_t muse_note_event(char* text, size_t cap);

#ifdef __cplusplus
}
#endif
