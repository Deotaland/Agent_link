#pragma once
#include "agent_link.h"
#include "round_display.h"
#include "amoled_ui_layout.h"
#include "amoled_hardware_test.h"
#include "lvgl.h"
class AmoledFonts;

namespace amoled_ui {
enum class TouchAction {
    kMenu,
    kHome,
    kRow,
    kBack,
    kLoadBefore,
    kLoadAfter,
    kVolume,
    kSystem,
    kPreviousCard,
    kNextCard,
    kTouchPoint,
    kBrightness
};
enum class View {
    kHome,
    kLauncher,
    kList,
    kInfo,
    kFriend,
    kCarousel,
    kAppActions,
    kFactoryHome,
    kAppGrid,
    kDownloadCode,
    kDownloadDetail,
    kHardwareTest,
    kQuickMenu,
    kSettings,
    kAppRemove,
    kDownloadHelp,
    kVolumeControl,
    kBrightnessControl
};
struct GridItem {
    int cover_slot = -1;
    uint32_t cover_revision = 0;
};
struct Input { TouchAction action; int row = 0; uint32_t view = 0; int64_t at = 0; bool commit = false; int x = -1, y = -1; };
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
    uint32_t storage_total = 0, storage_used = 0;
    int battery_percent = -1, progress = -1;
    int brightness_percent = 75;
    bool wifi_connected = false, wifi_setup_active = false;
    char wifi_setup_ssid[33] = {};
    GridItem grid[4];
    HardwareTest hardware_test;
    char app_id[32] = {};
    int row_count = 0;
    size_t row_offset = 0;
    uint32_t navigation = 0;
    Activity activity = Activity::kIdle;
    uint32_t view = 0;
    char pair_code[7] = {};
    char download_code[7] = {};
    char hardware_status[64] = {};
    char job_status[96] = {};
    char title[96] = {}, detail[192] = {}, rows[kListRows][112] = {}, footer[128] = {}, info[384] = {};
};
using InputHandler = bool (*)(const Input&, void*);
// Optional application-owned card pages. The shared UI never depends on factory sources.
struct CardRenderer {
    void (*create)(lv_obj_t*, AmoledFonts&, void (*bind)(lv_obj_t*, int));
    void (*update)(const Screen&);
    void (*hide)();
    bool (*accepts)(const Screen&) = nullptr;
    struct Palette {
        uint32_t background, card, text, muted, accent, pressed;
    };
    const Palette* palette = nullptr;
    // Return false over application-owned controls that need normal LVGL clicks.
    bool (*capture_test_tap)(int x, int y) = nullptr;
    struct TouchPolicy {
        bool full_circle = false;
        int button_slop = 12;
        int back_padding = 0;
        bool edge_system_swipe = false;
        bool edge_back_swipe = false;
    };
    TouchPolicy touch{};
};
enum class DisplayPerformance { kBalanced, kLauncher };
// Card pages keep their own policy. Apps without card pages can supply edge gestures separately.
esp_err_t StartUi(RoundDisplay* panel, RoundTouch* touch, InputHandler handler, void* context,
                  const CardRenderer* cards = nullptr,
                  DisplayPerformance performance = DisplayPerformance::kBalanced,
                  CardRenderer::TouchPolicy touch_policy = {});
void SetScreen(const Screen& screen);
bool UiResponsive();  // A recent completed UI loop, for Launcher first-boot confirmation.
void SetStatus(const agent_link_status_t& status);
void SetBattery(int percent, bool charging);
void SetVolume(int percent);
void SetAnswer(const char* text);
void Toast(const char* text, uint32_t ms = 2500);
void Wake();
void SetBacklight(int percent);
bool PrepareForSleep();  // Drain display DMA on its owner task before switching off rails.
void CancelSleep();
void ReportUiDiagnostics();
}  // namespace amoled_ui
