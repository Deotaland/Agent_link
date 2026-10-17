#pragma once
#include "agent_link.h"
#include "co5300_panel.h"
#include "cst820_touch.h"
#include "watch_wifi_controls.h"

namespace watch_wifi {
struct Screen {
    bool menu = false, busy = false, calling = false, can_call = false, can_sessions = false;
    bool previous = false, next = false;
    uint32_t view = 0;
    char title[48] = {}, detail[192] = {}, rows[3][112] = {}, footer[64] = {};
};
using InputHandler = bool (*)(const Input&, void*);
esp_err_t StartUi(Co5300Panel* panel, Cst820Touch* touch, InputHandler handler, void* context);
void SetScreen(const Screen& screen);
void SetStatus(const agent_link_status_t& status);
void SetBattery(int percent, bool charging);
void SetVolume(int percent);
void SetAnswer(const char* text);
void Toast(const char* text, uint32_t ms = 2500);
void Wake();
}  // namespace watch_wifi
