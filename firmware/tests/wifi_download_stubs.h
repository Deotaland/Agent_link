#pragma once
#include "agent_system_stubs.h"

enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM };
namespace wifi_fixture {
inline wifi_ps_type_t power_save = WIFI_PS_MIN_MODEM;
inline std::vector<wifi_ps_type_t> changes;
inline esp_err_t query_error = ESP_OK, set_error = ESP_OK;
}  // namespace wifi_fixture
inline esp_err_t esp_wifi_get_ps(wifi_ps_type_t* mode) {
    if (wifi_fixture::query_error != ESP_OK)
        return wifi_fixture::query_error;
    *mode = wifi_fixture::power_save;
    return ESP_OK;
}
inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t mode) {
    wifi_fixture::changes.push_back(mode);
    if (wifi_fixture::set_error != ESP_OK)
        return wifi_fixture::set_error;
    wifi_fixture::power_save = mode;
    return ESP_OK;
}
inline esp_err_t esp_wifi_sta_get_rssi(int* rssi) {
    *rssi = -57;
    return ESP_OK;
}
