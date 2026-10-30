#pragma once
#include "esp_err.h"
#include <cstdint>
constexpr int WIFI_STORAGE_RAM = 1, WIFI_MODE_STA = 1, WIFI_SECOND_CHAN_NONE = 0;
constexpr int ESP_ERR_WIFI_NOT_STARTED = 0x3002;
struct wifi_init_config_t { bool nvs_enable = true; };
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
esp_err_t esp_wifi_init(const wifi_init_config_t*);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_start();
esp_err_t esp_wifi_stop();
esp_err_t esp_wifi_set_channel(uint8_t, int);
