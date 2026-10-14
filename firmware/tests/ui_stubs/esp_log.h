#pragma once
template<typename... Args> void TestLog(const char*, const char*, Args...) {}
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
#define ESP_LOGE(...) TestLog(__VA_ARGS__)
