#pragma once
#include "agent_link.h"
#include "st7789_lcd.h"

namespace passport_wifi {
esp_err_t StartUi(St7789Lcd* lcd);
void SetStatus(const agent_link_status_t& status);
void SetContent(const char* title, const char* detail, const char* footer);
void SetAnswer(const char* text);
}  // namespace passport_wifi
