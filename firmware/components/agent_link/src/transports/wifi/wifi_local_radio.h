#pragma once
#include "esp_err.h"
#include <cstdint>
// Exclusive local radio lease. Keep cloud workers alive but unable to join or
// authenticate; releasing the lease resumes the existing credential lifecycle.
// Entry waits for the cloud owner's MQTT/HTTP close and realtime cleanup before
// disconnecting WiFi. Cleanup timeout leaves the station running. Call on a
// worker, outside WiFi/core locks; this can wait up to 15s for each cleanup.
esp_err_t agent_wifi_local_begin(uint8_t channel);
esp_err_t agent_wifi_local_end();
