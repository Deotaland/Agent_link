#pragma once
#include "agent_link.h"
#include "co5300_panel.h"
#include "cst9217_touch.h"
#include "amoled_voice_layout.h"

namespace amoled_voice {
enum class TouchAction { kMenu, kHome, kRow, kBack, kLoadBefore, kLoadAfter, kVolume };
enum class View { kHome, kLauncher, kList, kInfo, kFriend };
struct Input { TouchAction action; int row = 0; uint32_t view = 0; int64_t at = 0; bool commit = false; };
inline bool CurrentInput(const Input& input, uint32_t view, int64_t now) {
    return input.view == view && now >= input.at && now - input.at < 500000;
}
struct Screen {
    View kind = View::kHome;
    bool busy = false, calling = false, local_mode = false;
    bool previous = false, next = false, ended = false, volume_control = false;
    int row_count = 0;
    size_t row_offset = 0;
    uint32_t navigation = 0;
    Activity activity = Activity::kIdle;
    uint32_t view = 0;
    char pair_code[7] = {};
    char title[96] = {}, detail[192] = {}, rows[kListRows][112] = {}, footer[128] = {}, info[384] = {};
};
using InputHandler = bool (*)(const Input&, void*);
esp_err_t StartUi(Co5300Panel* panel, Cst9217Touch* touch, InputHandler handler, void* context);
void SetScreen(const Screen& screen);
void SetStatus(const agent_link_status_t& status);
void SetBattery(int percent, bool charging);
void SetVolume(int percent);
void SetAnswer(const char* text);
void Toast(const char* text, uint32_t ms = 2500);
void Wake();
void ReportUiDiagnostics();
}  // namespace amoled_voice
