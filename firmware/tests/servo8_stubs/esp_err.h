#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_INVALID_ARG = 0x102;
constexpr int ESP_ERR_INVALID_STATE = 0x103;
constexpr int ESP_ERR_INVALID_RESPONSE = 0x108;
inline const char* esp_err_to_name(int) { return "test"; }
