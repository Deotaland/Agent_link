#pragma once
#include "agent_link.h"
#include "co5300_panel.h"
#include "cst9217_touch.h"
#include "amoled_ui_layout.h"
#include "lvgl.h"
class AmoledFonts;

namespace amoled_ui {
enum class TouchAction { kMenu, kHome, kRow, kBack, kLoadBefore, kLoadAfter, kVolume, kSystem, kPreviousCard, kNextCard };
enum class View { kHome, kLauncher, kList, kInfo, kFriend, kCarousel, kAppActions };
struct Input { TouchAction action; int row = 0; uint32_t view = 0; int64_t at = 0; bool commit = false; };
inline bool CurrentInput(const Input& input, uint32_t view, int64_t now) {
    return input.view == view && now >= input.at && now - input.at < 500000;
}
struct Screen {
    View kind = View::kHome;
    bool busy = false, calling = false, local_mode = false;
    bool previous = false, next = false, ended = false, volume_control = false;
    bool launcher_shell = false, add_card = false;
    size_t card_index = 0, card_count = 0;
    int cover_slot = -1;
    uint32_t cover_revision = 0;
    char app_id[32] = {};
    int row_count = 0;
    size_t row_offset = 0;
    uint32_t navigation = 0;
    Activity activity = Activity::kIdle;
    uint32_t view = 0;
    char pair_code[7] = {};
    char title[96] = {}, detail[192] = {}, rows[kListRows][112] = {}, footer[128] = {}, info[384] = {};
};
using InputHandler = bool (*)(const Input&, void*);
// Optional application-owned card pages. The shared UI never depends on factory sources.
struct CardRenderer {
    void (*create)(lv_obj_t*, AmoledFonts&, void (*bind)(lv_obj_t*, int));
    void (*update)(const Screen&);
    void (*hide)();
};
enum class DisplayPerformance { kBalanced, kLauncher };
esp_err_t StartUi(Co5300Panel* panel, Cst9217Touch* touch, InputHandler handler, void* context,
                  const CardRenderer* cards = nullptr, DisplayPerformance performance = DisplayPerformance::kBalanced);
void SetScreen(const Screen& screen);
void SetStatus(const agent_link_status_t& status);
void SetBattery(int percent, bool charging);
void SetVolume(int percent);
void SetAnswer(const char* text);
void Toast(const char* text, uint32_t ms = 2500);
void Wake();
void ReportUiDiagnostics();
}  // namespace amoled_ui
