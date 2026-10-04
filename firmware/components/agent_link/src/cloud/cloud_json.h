#pragma once
// JSON control plane of the WiFi channel, internal to the WiFi backend.
//
// A request published on MQTT `down` is translated into the same binary command frame BLE carries
// and handed to the core, which runs it exactly as it would for the App. The core answers through
// the transport's send_ctrl, synchronously and on the same task; that answer is translated back
// and published on `up` as the response. Neither the core nor any board knows which transport a
// command came from.
//
// Both directions use the platform's envelope, with nothing else accepted:
//   {"msg_id":"msg_<13-digit ms>_<6 hex>", "type":"request"|"response"|"event", "action":"...",
//    "timestamp":<13-digit ms>, "params":{...}, "reply_to":..., "code":..., "message":...}
// A request has reply_to, code and message null; its response names it in reply_to, carries
// code (0 = success) and a message, and its action is the request's plus "_status". The action
// "raw" carries a BLE command id and payload for commands a board implements itself.
//
// Events are not published yet: send_ctrl frames outside a request are refused.

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Hand one binary command frame to the core. Its answer comes back through cloud_json_on_frame(). */
    void (*deliver)(const uint8_t* frame, size_t len);
    /** Publish one JSON response on `up`. */
    esp_err_t (*publish)(const char* json, size_t len);
} cloud_json_hooks_t;

/** @brief Install the hooks. The struct is copied. */
void cloud_json_set_hooks(const cloud_json_hooks_t* hooks);

/**
 * @brief Run one message from `down` and publish the response to it.
 * @note Call with every other entry into the core held off: it calls deliver(), and through it
 *       the board's callbacks.
 */
void cloud_json_handle_command(const char* json, size_t len);

/**
 * @brief A frame the core sent through send_ctrl, from any task.
 * @return ESP_OK if it belonged to the request being run (its answer, or the manifest a 0x34
 *         produces), which is then consumed; ESP_ERR_NOT_SUPPORTED for anything else, which is
 *         dropped.
 */
esp_err_t cloud_json_on_frame(const uint8_t* frame, size_t len);

#ifdef __cplusplus
}
#endif
