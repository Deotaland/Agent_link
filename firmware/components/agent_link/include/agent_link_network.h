#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool connected;
    bool setup_active;
    char ssid[33];
    char setup_ssid[33];
    char ip[16];
    int rssi;
} agent_link_wifi_info_t;
esp_err_t agent_link_wifi_info(agent_link_wifi_info_t* info);
// Opens the existing phone hotspot provisioning flow, preserving platform binding.
esp_err_t agent_link_wifi_setup(void);
// Optional BLE discovery service on desktop profiles. No classic-BT audio profile.
esp_err_t agent_link_ble_enable(bool enable);
bool agent_link_ble_enabled(void);
#ifdef __cplusplus
}
#endif
