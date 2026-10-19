#pragma once
inline void platform_test_log(const char*, const char*, ...) {}
#define ESP_LOGW(...) platform_test_log(__VA_ARGS__)
#define ESP_LOGD(...) platform_test_log(__VA_ARGS__)
