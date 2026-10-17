#pragma once

#include "agent_link.h"
#include "co5300_panel.h"

namespace watch_wifi {

struct Screen {
    bool menu = false;
    int selected = 0;
    char title[48] = {};
    char detail[192] = {};
    char rows[3][112] = {};
    char footer[128] = {};
};

esp_err_t StartUi(Co5300Panel* panel);
void SetScreen(const Screen& screen);
void SetStatus(const agent_link_status_t& status);
void SetBattery(int percent, bool charging);
void SetAnswer(const char* text);
void Toast(const char* text, uint32_t ms = 2500);
void Wake();

}  // namespace watch_wifi
