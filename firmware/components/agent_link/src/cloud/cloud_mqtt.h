#pragma once
// The cloud session's MQTT connection (esp-mqtt), internal to the WiFi backend. It logs in with the
// credentials from mqtt-token and subscribes to the device's downlink topics.
//
// cloud_session.cc owns the lifecycle and calls everything here from its own task. esp-mqtt
// reconnects by itself after a lost connection; the session stops it when the network goes, and
// when the broker refuses the credentials, which needs a new token rather than another try.
//
// Every message on the two downlink topics is logged and handed to on_downlink, which runs it as a
// command; responses go back with cloud_mqtt_publish_up().

#include <stdbool.h>
#include <stddef.h>
#include "cloud_api.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The broker refused CONNECT (@p connack_code is the MQTT 3.1.1 return code, 1-5).
 * @note Runs on the MQTT task: notify the session and return. esp-mqtt keeps retrying with the
 *       same credentials until cloud_mqtt_stop().
 */
typedef void (*cloud_mqtt_refused_cb_t)(int connack_code, void* ctx);

/**
 * @brief One whole message arrived on `down` or `ota/down`. @p data is not NUL-terminated.
 * @note Runs on the MQTT task, which is where the board's callbacks then run too. Messages larger
 *       than esp-mqtt's 1KB receive buffer arrive in pieces and are dropped rather than passed on.
 */
typedef void (*cloud_mqtt_downlink_cb_t)(const char* data, size_t len, void* ctx);

typedef struct {
    cloud_mqtt_refused_cb_t  on_refused;
    cloud_mqtt_downlink_cb_t on_downlink;
    void*                    ctx;          ///< passed to both
} cloud_mqtt_callbacks_t;

/**
 * @brief Connect with @p tok and keep the connection up.
 *
 * Returns once the client is started; the connection itself comes up in the background. @p tok and
 * @p cb are copied, so the caller may overwrite them as soon as this returns.
 *
 * @return ESP_ERR_NOT_SUPPORTED if @p tok names a protocol without TLS; ESP_ERR_INVALID_STATE if
 *         already started.
 */
esp_err_t cloud_mqtt_start(const cloud_mqtt_token_t* tok, const cloud_mqtt_callbacks_t* cb);

/**
 * @brief Queue one message on `up`, QoS 1. The MQTT task sends it, so this never blocks.
 * @note Call it from the downlink callback, i.e. on the MQTT task. From any other task it would
 *       race cloud_mqtt_stop() freeing the client, and a lock would deadlock against esp-mqtt's own.
 *       While disconnected the message waits in esp-mqtt's outbox, for up to 30s.
 * @return ESP_ERR_INVALID_STATE when no client is started.
 */
esp_err_t cloud_mqtt_publish_up(const char* data, size_t len);

/**
 * @brief Disconnect and free the client. A no-op when not started.
 * @note Blocks until the MQTT task has exited, so no callback runs once it returns: about a second
 *       when connected, longer if a connection attempt has to time out first.
 */
void cloud_mqtt_stop(void);

/** @brief Whether a client is started, connected or not. */
bool cloud_mqtt_started(void);

#ifdef __cplusplus
}
#endif
