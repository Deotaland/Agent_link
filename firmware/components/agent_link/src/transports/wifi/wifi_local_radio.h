#pragma once
#include "esp_err.h"
#include <cstdint>
// Exclusive local radio lease. Keep cloud workers alive but unable to join or
// authenticate; releasing the lease resumes the existing credential lifecycle.
esp_err_t agent_wifi_local_begin(uint8_t channel);
esp_err_t agent_wifi_local_end();
