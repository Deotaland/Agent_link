// ============================================================================
// agent_link — Connectivity Layer for Deotaland Agent Platform
// ============================================================================
// This module provides a reusable connectivity layer that allows devices
// to access the Deotaland Agent platform with minimal integration effort

// Design Philosophy:
//   - New hardware simply "declares capabilities + registers callbacks"
//   - Completely abstracts BLE/transport protocol details
//   - Agent → Device: via agent_output_cb_t (speech/display/vibration/actuators)
//   - Device → Agent: via agent_link_push_* (voice/buttons/sensors/battery)
//   - Only handles "transport + protocol + capability routing"
//   - Hardware actions are implemented in device callbacks → naturally cross-platform
//
// See agent_link_caps.h for capability flags, agent_link_io.h for generic I/O, and
// agent_link_ota.h for App-driven firmware upgrades (handled by the SDK; no board code needed)
// ============================================================================

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "agent_link_caps.h"
#include "agent_link_io.h"
#include "agent_link_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol version identifier */
#define AGENT_LINK_PROTO_VERSION 1

/**
 * @brief Bytes of response data an agent_output_cb_t::on_command handler may return.
 * @note Deliberately small: a command response shares one control frame with its header, and the
 *       negotiated MTU is not known to the board. A command whose answer can grow past this must
 *       define paging in its own payload.
 */
#define AGENT_LINK_CMD_RESP_MAX 128

/**
 * @brief Agent → Device output callbacks
 *
 * These callbacks are invoked by the SDK when the Agent platform sends
 * commands or data to the device. All hardware operations should be
 * implemented within these callbacks.
 *
 * @note All callbacks are optional — set to NULL if not applicable.
 * @note Every callback here runs on the transport's own task — for BLE, the NimBLE host task —
 *       not on a worker the SDK owns. Two consequences, and both bite:
 *         - Blocking stalls the link. Queue the work and return.
 *         - The stack is the transport's, not yours, and it is small
 *           (CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, 4096 by default) and already deep by the time
 *           a callback is reached. Keep locals to a few hundred bytes and do not call into
 *           filesystem or network stacks from here; a directory scan with a couple of 256-byte
 *           buffers is enough to overflow it.
 *       Callbacks are serialised onto that one task, so static scratch is safe.
 */
typedef struct {
    /**
     * @brief Audio output: TTS stream from Agent
     * @param pcm16 PCM16 audio data (16kHz, mono)
     * @param bytes Size of data in bytes
     * @param ctx   User context pointer
     * @note Arrives in chunks; on_audio_end is called when stream finishes
     */
    void (*on_audio_out)(const uint8_t* pcm16, size_t bytes, void* ctx);

    /** @brief Called when audio stream ends */
    void (*on_audio_end)(void* ctx);

    /**
     * @brief Display text
     * @param utf8 UTF-8 encoded text string
     * @param ctx  User context
     */
    void (*on_show_text)(const char* utf8, void* ctx);
    /**
     * @brief Display image
     * @param rgb565_be Big-endian RGB565 image data
     * @param w        Image width in pixels
     * @param h        Image height in pixels
     * @param ctx      User context
     */
    void (*on_show_image)(const uint8_t* rgb565_be, uint16_t w, uint16_t h, void* ctx);

    /**
     * @brief Video downlink: encoded video frame from Agent
     * @param frame Encoded video frame (format negotiated by capabilities)
     * @param bytes Frame size in bytes
     * @param ctx   User context
     * @note WiFi-only (BLE bandwidth insufficient); arrives in chunks
     */
    void (*on_video_out)(const uint8_t* frame, size_t bytes, void* ctx);

    /** @brief Haptic feedback (vibration motor) */
    void (*on_haptic)(uint32_t duration_ms, void* ctx);

    /** @brief LED control (RGB color) */
    void (*on_led)(uint32_t rgb, void* ctx);

    /** @brief General-purpose actuator */
    void (*on_actuate)(uint16_t channel, int32_t value, void* ctx);

     /**
     * @brief Agent list from platform
     * @param json_utf8 JSON array of available Agents (UTF-8)
     * @param ctx       User context
     * @note Device should present a selection UI to the user
     */
    void (*on_agent_list)(const char* json_utf8, void* ctx);
    /**
     * @brief Command/query handler (for commands that require a response)
     * @param cmd     Command ID
     * @param payload Command payload
     * @param len     Payload length
     * @param resp    Response buffer to fill
     * @param resp_cap Response buffer capacity
     * @param resp_len Output: actual response length
     * @param ctx     User context
     * @return true if command recognized and handled; false otherwise
     * @note Return true → the SDK answers status=0 with your data.
     *       Return false → the SDK answers 1001 UnknownCommand. It never invents a success for a
     *       command nobody implemented: an App told "done" for something that did not happen is
     *       worse off than one told the device cannot do it, which it can degrade around.
     * @note This is also how a board implements a command the SDK itself does not: it is consulted
     *       before the SDK's own fallback, so claiming an id here wins.
     * @note @p resp_cap is AGENT_LINK_CMD_RESP_MAX bytes. A larger answer must be paged by the
     *       command's own protocol.
     */
    bool (*on_command)(uint16_t cmd, const uint8_t* payload, size_t len,
                       uint8_t* resp, size_t resp_cap, size_t* resp_len, void* ctx);


    /**
     * @brief Start/stop microphone capture on request from the Agent (App-initiated "listen").
     * @param start  true = begin capturing and streaming mic audio; false = stop.
     * @param max_ms Suggested max capture duration in ms (0 = until stopped); board-specific.
     * @param ctx    User context.
     * @note Optional; set only if AGENT_CAP_MIC is advertised. On start, the board opens an
     *       AGENT_STREAM_AUDIO stream and pumps its mic into it. Triggered by command 0x3C/0x3D.
     */
    void (*on_listen)(bool start, uint32_t max_ms, void* ctx);

    void* ctx;  ///< Opaque pointer passed through to all callbacks
} agent_output_cb_t;

/**
 * @brief Connection state change callback
 * @param state New connection state
 * @param ctx   User context
 */
typedef void (*agent_state_cb_t)(agent_state_t state, void* ctx);

/**
 * @brief Transport backend selection
 *
 * "One API, two transports" — same API works over BLE or WiFi.
 * Default is BLE (0) for backward compatibility.
 */
typedef enum {
    AGENT_TRANSPORT_BLE  = 0,  // BLE:GATT control + L2CAP voice; video not supported
    AGENT_TRANSPORT_WIFI = 1,  // WiFi:control + voice + video
    AGENT_TRANSPORT_BOTH = 2,  // Hybrid:BLE control/provisioning + WiFi media
} agent_transport_kind_t;

/**
 * @brief WiFi transport configuration
 *
 * Used when transport includes WIFI. May be NULL and provisioned later via BLE.
 */
typedef struct agent_wifi_config_s {
    const char* ssid;      // WiFi SSID
    const char* password;  // WiFi password (can be NULL for open networks)
    const char* endpoint;  // Deotaland cloud endpoint (signaling/media), e.g., "wss://agent.deotaland.ai/..."
    const char* token;     // Device authentication token (can be NULL)
} agent_wifi_config_t;

/**
 * @brief Agent link configuration
 *
 * Passed to agent_link_init() to configure the device's connection.
 */
typedef struct {
    const char*              device_name; ///< BLE advertising name / platform display name (required)
    uint32_t                 caps;         ///< agent_cap_t bitmask (required)
    const agent_output_cb_t* output;       ///< Agent→Device callbacks (NULL for pure sensor devices)
    agent_state_cb_t         on_state;     ///< Connection state callback (can be NULL)
    void*                    state_ctx;    ///< Context passed to on_state
    agent_transport_kind_t   transport;    ///< Transport backend (default 0 = BLE)
    const agent_wifi_config_t* wifi;        ///< WiFi config (when transport includes WIFI; can be NULL for later provisioning)
    const char*              manufacturer;  ///< Manufacturer name (0x2A29); NULL → "Deotaland"
    /**
     * @brief Hardware model (0x2A24, reported in 0x01); NULL → device_name.
     * @note This is the string an incoming OTA image must claim, so the App cannot flash a build
     *       for other hardware onto this board. Boards that share a PCB must share this value.
     */
    const char*              model;
    /**
     * @brief Firmware revision (0x2A26, reported in 0x01); NULL → the app descriptor version
     *        (PROJECT_VER in the top-level CMakeLists), which is what you normally want.
     */
    const char*              firmware_rev;
} agent_link_config_t;

//=================================================================
// Lifecycle Management
//=================================================================

/** Initialize the agent link module */
esp_err_t     agent_link_init(const agent_link_config_t* cfg);

/** Start the connection (BLE advertising or WiFi connection) */
esp_err_t     agent_link_start(void);

/** Stop the connection and release resources */
void          agent_link_stop(void);

/** Get current connection state */
agent_state_t agent_link_state(void);

// ============================================================================
// Device → Agent: Input / Event / Status
// ============================================================================
// These functions are called by the device to report data to the Agent platform.
// They are safely ignored if the connection is not ready.
// ============================================================================


/**
 * @brief Declare how many milliseconds of downlink audio this board can hold for playback.
 *
 * The App sends a spoken reply as fast as the link allows and only stops when the device says
 * so, so the SDK has to throttle it to whatever the board can actually buffer. It measures how
 * far ahead of real time it has pushed and pauses the App before that exceeds this figure.
 *
 * Call once during board setup with the size of your play buffer. The default is deliberately
 * small (400ms); a board with a deeper buffer that does not say so simply gets less margin
 * against link jitter, never dropouts.
 *
 * @param playable_ms Milliseconds of PCM16 the board's play buffer holds (e.g. a 16KB buffer
 *                    at 16kHz/16-bit/mono = 512).
 * @note A board that never calls this still plays correctly.
 */
void agent_link_playback_set_buffer_ms(uint32_t playable_ms);

/**
 * @brief Push a device→Agent event
 *
 * The event_id on the wire is the agent_event_t value. Use AGENT_EVT_CUSTOM (0x64) for a board-private packet
 * the board defines its own payload and the App matches on event_id 0x64
 * a dropped frame is not resent, so don't rely on it for state that must never desync
 * carry the explicit state in the payload rather than a bare toggle.
 * @param type Event type (from agent_event_t; AGENT_EVT_CUSTOM for board-private packets)
 * @param data Event payload (can be NULL if len == 0)
 * @param len  Payload length
 */
esp_err_t agent_link_push_event(agent_event_t type, const uint8_t* data, size_t len);

/**
 * @brief Push firmware-authored text for the Agent to treat as a prompt
 *
 * Sends AGENT_EVT_PROMPT (event 0x04) as a single control-plane event (Notify 0xFFC4); the
 * App forwards the text verbatim to the Agent, as if the user had said/typed it. For short,
 * discrete strings the firmware composes itself — canned phrases, sensor-triggered context,
 * button macros.a prompt must fit in one BLE notify, so an oversized one is rejected up front
 * rather than silently truncated or split — call it again per chunk if the
 * firmware text is longer than the budget below.
 *
 * @param utf8 NUL-terminated UTF-8 text (the terminator itself is not sent).
 * @return ESP_OK once queued for send.
 *         ESP_ERR_INVALID_ARG if utf8 is NULL or empty.
 *         ESP_ERR_INVALID_SIZE if it exceeds the current single-frame budget: BLE = negotiated
 *         ATT_MTU − 9, capped at 480B (≈238B at the recommended MTU 247; only ≈14B if the App
 *         has not yet performed MTU exchange); WiFi = 1024B.
 * @note a dropped frame is not resent.
 */
esp_err_t agent_link_push_prompt(const char* utf8);

/**
 * @brief Report battery status
 * @param percent  Battery percentage (0-100)
 * @param charging true if charging
 * @note SDK internally deduplicates — only pushes when value changes /
 *       charging state changes / low-battery edge (<5%) is detected.
 *       If send fails, cache is not updated → next call retries.
 *       First call after connection forces a retransmission for initial sync.
 */
esp_err_t agent_link_report_battery(uint8_t percent, bool charging);

/**
 * @brief Report the user's selected Agent ID
 * @param agent_id The selected Agent ID
 */
esp_err_t agent_link_report_selected_agent(const char* agent_id);

#ifdef __cplusplus
}
#endif
