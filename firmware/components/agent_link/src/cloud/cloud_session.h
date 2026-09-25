#pragma once
// Cloud session control, internal to the WiFi backend (transport_wifi.cpp).

#include "cloud_status.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Session parameters, supplied by the WiFi backend. */
typedef struct {
    const char* base_url;    ///< Platform root, no trailing slash
    int         product_id;  ///< Platform product id for this hardware
    const char* chip_type;   ///< e.g. "esp32-s3"

    /**
     * @brief Whether requests may start now. Required.
     *
     * More than "has an IP": it also stays false while the captive portal is up, so requests start
     * only after provisioning has finished and released its resources. Polled about once a second
     * while the session waits.
     */
    bool (*net_ready)(void);

    /** @brief State changes and countdown ticks. Runs on the session task; keep it short. */
    agent_cloud_state_cb_t on_state;
    void*                  ctx;
} cloud_session_config_t;

/** @brief Start the session task. Returns immediately; the task waits for net_ready() itself. */
esp_err_t cloud_session_start(const cloud_session_config_t* cfg);

/** @brief Ask the task to exit. */
void cloud_session_stop(void);

/** @brief Current snapshot. Safe from any task. */
void cloud_session_get_status(agent_cloud_status_t* out);

/** @brief Erase the auth_key; the session re-claims on its next pass. */
esp_err_t cloud_session_forget(void);

#ifdef __cplusplus
}
#endif
