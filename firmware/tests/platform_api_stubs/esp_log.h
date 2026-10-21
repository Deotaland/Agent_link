#pragma once
#ifdef PLATFORM_TEST_CAPTURE_LOGS
void platform_test_log(const char*, const char*, ...);
#else
inline void platform_test_log(const char*, const char*, ...) {}
#endif
#define ESP_LOGW(...) platform_test_log(__VA_ARGS__)
#define ESP_LOGD(...) platform_test_log(__VA_ARGS__)
#define ESP_LOGI(...) platform_test_log(__VA_ARGS__)
#define ESP_LOGE(...) platform_test_log(__VA_ARGS__)
