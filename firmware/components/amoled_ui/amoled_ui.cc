#include "amoled_ui.h"
#include "config.h"
#include "amoled_fonts.h"
#include "amoled_ui_controls.h"
#include "amoled_orientation.h"
#include "amoled_display_transfer.h"
#include "amoled_label_layout.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace amoled_ui {
namespace {
constexpr size_t kBufferBytes = DISPLAY_WIDTH * UI_DRAW_BUF_ROWS * 2;
constexpr size_t kFrameBytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2;
AmoledDisplayTransfer display_transfer_;
lv_display_t* pending_display_ = nullptr;
bool fast_ui_ = false;
bool pending_last_flush_ = false;
uint32_t pending_flush_view_ = 0;
std::atomic<uint32_t> presented_view_{0};
int64_t last_frame_at_ = 0;
std::atomic<uint32_t> completed_frames_{0}, active_frame_samples_{0};
std::atomic<uint32_t> active_frame_us_{0};
static_assert(DISPLAY_ROTATION_CW == 0 || DISPLAY_ROTATION_CW == 90);
static_assert(DISPLAY_WIDTH == DISPLAY_HEIGHT, "Circular AMOLED UI requires a square panel");
static_assert(kDisplaySize == DISPLAY_WIDTH, "UI geometry must match the native panel size");
uint8_t* rotated_pixels_ = nullptr;
bool panel_fault_ = false;
int64_t panel_retry_at_ = 0;
std::atomic<uint32_t> panel_errors_{0}, panel_recoveries_{0};
std::atomic<uint32_t> touch_polls_{0}, touch_errors_{0}, max_touch_gap_ms_{0};
std::atomic<uint32_t> volume_events_{0}, volume_commits_{0};
int64_t last_touch_poll_at_ = 0;
uint32_t kWhite = 0xF5F5F7, kMuted = 0x98989F, kBlue = 0x0A84FF;
uint32_t kBackground = 0, kCard = 0x1C1C1E, kPressed = 0x38383C;
AmoledFonts fonts_;
RoundDisplay* panel_ = nullptr;
RoundTouch* touch_ = nullptr;
InputHandler handler_ = nullptr;
void* context_ = nullptr;
portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
Screen screen_;
agent_link_status_t status_ = {};
char answer_[1536] = {}, toast_[128] = {};
int battery_ = -1, volume_ = AUDIO_DEFAULT_VOLUME;
bool charging_ = false;
std::atomic<int> backlight_{(DISPLAY_BRIGHTNESS * 100 + 127) / 255};
std::atomic<bool> sleep_requested_{false}, sleep_ready_{false};
uint8_t applied_backlight_ = DISPLAY_BRIGHTNESS;
int64_t toast_until_ = 0;
std::atomic<uint32_t> revision_{1}, wake_{0}, applied_{0}, flushes_{0}, max_flush_us_{0};
std::atomic<int64_t> heartbeat_{0};
std::atomic<uint32_t> ui_heartbeat_{0};
enum class UiStage { kStarting, kFonts, kTouch, kSnapshot, kHeader, kReplyMeasure, kReplyText,
    kReplyLayout, kReplyScroll, kList, kListLayout, kBrightness, kTimers, kPanel, kMemory, kIdle };
std::atomic<UiStage> stage_{UiStage::kStarting};
std::atomic<int64_t> stage_at_{0};
std::atomic<uint32_t> flush_started_{0}, flush_xy_{0}, flush_wh_{0};
std::atomic<uint32_t> text_received_{0}, text_changed_{0}, text_applied_{0}, text_bytes_{0};
std::atomic<uint32_t> status_received_{0}, status_changed_{0};
std::atomic<uint32_t> ui_stack_min_{0}, lv_free_{0}, lv_largest_{0};
void Mark(UiStage stage) { stage_at_.store(esp_timer_get_time()); stage_.store(stage); }
const char* StageName(UiStage stage) {
    switch (stage) {
    case UiStage::kStarting: return "starting";
    case UiStage::kFonts: return "fonts/build";
    case UiStage::kTouch: return "touch/I2C";
    case UiStage::kSnapshot: return "snapshot/lock";
    case UiStage::kHeader: return "header";
    case UiStage::kReplyMeasure: return "reply/measure";
    case UiStage::kReplyText: return "reply/set-text";
    case UiStage::kReplyLayout: return "reply/layout";
    case UiStage::kReplyScroll: return "reply/scroll";
    case UiStage::kList: return "list/update";
    case UiStage::kListLayout: return "list/layout";
    case UiStage::kBrightness: return "brightness/SPI";
    case UiStage::kTimers: return "LVGL/timers-draw";
    case UiStage::kPanel: return "panel/SPI";
    case UiStage::kMemory: return "LVGL/heap-check";
    case UiStage::kIdle: return "idle";
    }
    return "unknown";
}
lv_obj_t *ring_, *heading_, *battery_label_, *title_, *detail_, *back_, *back_label_, *footer_,
    *toast_label_;
lv_obj_t *list_, *list_info_, *list_code_, *list_hint_, *rows_[kListRows], *row_labels_[kListRows], *row_icons_[kListRows];
lv_obj_t *row_switches_[kListRows], *row_arrows_[kListRows];
lv_obj_t *answer_view_, *answer_label_, *latest_, *volume_box_, *slider_, *volume_label_;
lv_obj_t* home_handle_ = nullptr;
lv_obj_t *quick_menu_, *quick_header_, *quick_buttons_[4], *quick_labels_[4];
lv_obj_t *edge_back_badge_, *edge_back_text_;
lv_obj_t *vertical_control_, *vertical_slider_, *vertical_fill_, *vertical_value_, *vertical_icon_;
lv_obj_t *vertical_title_, *vertical_hint_;
lv_obj_t *vertical_sun_, *vertical_sun_disc_, *vertical_sun_rays_[8];
lv_point_precise_t vertical_sun_points_[8][2];
const CardRenderer* card_renderer_ = nullptr;
CardRenderer::TouchPolicy touch_policy_{};
void ConfigureTouch(const CardRenderer* cards, CardRenderer::TouchPolicy touch = {}) {
    card_renderer_ = cards;
    touch_policy_ = cards ? cards->touch : touch;
}
lv_indev_t* pointer_ = nullptr;
cst9217::Point point_;
bool swallow_ = false, pending_ = false, shown_busy_ = false, has_before_ = false, has_after_ = false;
bool list_gesture_ = false;
bool click_cancelled_ = true;
bool contact_down_ = false;
lv_obj_t* pressed_obj_ = nullptr;
int contact_x_ = 0, contact_y_ = 0, list_direction_ = 0;
uint32_t shown_view_ = 0, pressed_view_ = 0, contact_view_ = 0;
View shown_kind_ = View::kHome;
SwipeTracker swipe_;

void Round(lv_event_t* event) {
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
    area->x1 &= ~1; area->y1 &= ~1; area->x2 |= 1; area->y2 |= 1;
}
void PanelFailed(esp_err_t err, const char* operation) {
    panel_errors_.fetch_add(1);
    if (!panel_fault_) ESP_LOGE("Amoled175.ui", "%s failed: %s; scheduling panel recovery", operation, esp_err_to_name(err));
    panel_fault_ = true;
    presented_view_ = 0;
    panel_retry_at_ = esp_timer_get_time() + 1000000;
}
void CompleteTransfer(bool wait) {
    AmoledDisplayTransfer::Result result;
    if (!display_transfer_.Finish(wait, &result)) return;
    if (result.error != ESP_OK) PanelFailed(result.error, "display transfer");
    if (result.error == ESP_OK && pending_last_flush_) {
        if (!panel_fault_) presented_view_ = pending_flush_view_;
        const int64_t now = esp_timer_get_time();
        if (last_frame_at_ && now > last_frame_at_ && now - last_frame_at_ <= 250000) {
            active_frame_us_.fetch_add(static_cast<uint32_t>(now - last_frame_at_)); active_frame_samples_.fetch_add(1);
        }
        last_frame_at_ = now; completed_frames_.fetch_add(1);
    } else if (result.error != ESP_OK) last_frame_at_ = 0;
    flushes_.fetch_add(1);
    if (result.elapsed_us > max_flush_us_.load()) max_flush_us_.store(result.elapsed_us);
    lv_display_flush_ready(pending_display_); pending_display_ = nullptr;
}
void WaitFlush(lv_display_t*) { CompleteTransfer(true); }
void Brightness(uint8_t level) {
    CompleteTransfer(true);  // Keep panel commands outside an outstanding pixel window.
    const esp_err_t err = panel_->SetBrightness(level);
    if (err != ESP_OK) PanelFailed(err, "brightness");
    else
        applied_backlight_ = level;
}
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    // Complete LVGL's flush even when the transport is faulted. Touch, BOOT and
    // audio must keep running while the IO retains an outstanding DMA buffer.
    if (panel_fault_) { lv_display_flush_ready(display); return; }
    const int64_t start = esp_timer_get_time();
    const int w = lv_area_get_width(area), h = lv_area_get_height(area);
    if (display_transfer_.Active()) {
        const DisplayRect logical{area->x1, area->y1, w, h};
        const auto target = DISPLAY_ROTATION_CW == 90 ? Clockwise90Rect(logical, DISPLAY_WIDTH) : logical;
        flush_xy_.store((static_cast<uint32_t>(target.x) << 16) | target.y);
        flush_wh_.store((static_cast<uint32_t>(target.w) << 16) | target.h);
        const esp_err_t err = display_transfer_.Submit(logical, pixels);
        if (err == ESP_OK) {
            pending_display_ = display; pending_last_flush_ = lv_display_flush_is_last(display);
            pending_flush_view_ = shown_view_;
            flush_started_.fetch_add(1);
        }
        else { PanelFailed(err, "display submit"); lv_display_flush_ready(display); }
        return;
    }
    lv_draw_sw_rgb565_swap(pixels, w * h);
    DisplayRect target{area->x1, area->y1, w, h};
    if constexpr (DISPLAY_ROTATION_CW == 90) {
        if (!RotateRgb565Clockwise90(pixels, rotated_pixels_, w, h, kBufferBytes)) {
            ESP_LOGE("Amoled175.ui", "display rotation buffer invalid");
            lv_display_flush_ready(display);
            return;
        }
        target = Clockwise90Rect(target, DISPLAY_WIDTH);
        pixels = rotated_pixels_;
    }
    const UiStage previous = stage_.load(); const int64_t previous_at = stage_at_.load();
    flush_xy_.store((static_cast<uint32_t>(target.x) << 16) | target.y);
    flush_wh_.store((static_cast<uint32_t>(target.w) << 16) | target.h);
    flush_started_.fetch_add(1); Mark(UiStage::kPanel);
    const esp_err_t err = panel_->DrawBitmap(target.x, target.y, target.w, target.h, pixels);
    if (err != ESP_OK) PanelFailed(err, "display transfer");
    else if (lv_display_flush_is_last(display) && !panel_fault_) presented_view_ = shown_view_;
    flushes_.fetch_add(1);
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - start);
    if (us > max_flush_us_.load()) max_flush_us_.store(us);
    lv_display_flush_ready(display);
    stage_at_.store(previous_at); stage_.store(previous);
}
uint32_t Tick() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
void Visible(lv_obj_t* obj, bool on) {
    if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
void Position(lv_obj_t* obj, const Rect& rect) {
    lv_obj_set_pos(obj, rect.x, rect.y); lv_obj_set_size(obj, rect.w, rect.h);
    if (lv_obj_check_type(obj, &lv_label_class)) CenterLabelVertically(obj);
}
void Text(lv_obj_t* label, const char* text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
        CenterLabelVertically(label);
    }
}
lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, bool center = false) {
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, center ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP); lv_label_set_text(label, "");
    return label;
}
lv_obj_t* Card(lv_obj_t* parent, int radius = 20) {
    auto* obj = lv_obj_create(parent); lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kCard), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x303034), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}
bool Dispatch(TouchAction action, int row = 0, int x = -1, int y = -1) {
    const bool system_swipe = action == TouchAction::kSystem && touch_policy_.edge_system_swipe;
    if (pending_ || (shown_busy_ && action != TouchAction::kHome && !system_swipe) || !handler_)
        return false;
    Input input{}; input.action = action; input.row = row;
    input.x = x; input.y = y;
    input.view = shown_view_; input.at = esp_timer_get_time();
    if (!handler_(input, context_)) return false;
    pending_ = true;
    return true;
}
void Click(lv_event_t* event) {
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST) { click_cancelled_ = true; return; }
    auto* obj = lv_event_get_current_target_obj(event);
    if (code == LV_EVENT_PRESSED) {
        pressed_view_ = shown_view_; pressed_obj_ = obj;
        const lv_point_t start{contact_x_, contact_y_};
        if (!lv_obj_hit_test(obj, &start)) click_cancelled_ = true;
        return;
    }
    if (click_cancelled_ || pressed_obj_ != obj || pressed_view_ != shown_view_) return;
    click_cancelled_ = true;
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (id >= 0) Dispatch(TouchAction::kRow, id);
    else if (id == -1) Dispatch(TouchAction::kBack);
    else {
        lv_obj_update_layout(answer_view_);
        lv_obj_scroll_to_y(answer_view_, lv_obj_get_scroll_y(answer_view_) +
                           lv_obj_get_scroll_bottom(answer_view_), LV_ANIM_ON);
        Visible(latest_, false);
    }
}
void BindClick(lv_obj_t* obj, int id) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    if (id == -1)
        lv_obj_set_ext_click_area(obj, touch_policy_.back_padding);
    auto* data = reinterpret_cast<void*>(static_cast<intptr_t>(id));
    lv_obj_add_event_cb(obj, Click, LV_EVENT_PRESSED, data);
    lv_obj_add_event_cb(obj, Click, LV_EVENT_PRESS_LOST, data);
    lv_obj_add_event_cb(obj, Click, LV_EVENT_CLICKED, data);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kPressed), LV_STATE_PRESSED);
}
void VerticalLevelText(int value, bool brightness) {
    if (vertical_slider_) {
        char text[16];
        snprintf(text, sizeof text, "%d%%", value);
        Text(vertical_value_, text);
        CenterLabelVertically(vertical_value_);
        const int height = Scale(264);
        const int filled = (height * std::clamp(value, 0, 100) + 50) / 100;
        lv_obj_set_pos(vertical_fill_, 0, height - filled);
        lv_obj_set_height(vertical_fill_, filled);
        Visible(vertical_fill_, filled > 0);
        Visible(vertical_icon_, !brightness);
        Visible(vertical_sun_, brightness);
        Text(vertical_icon_, value ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
        const auto color = lv_color_hex(value >= 30 ? 0x25313A : 0xDCE4EC);
        lv_obj_set_style_text_color(vertical_icon_, color, 0);
        lv_obj_set_style_border_color(vertical_sun_disc_, color, 0);
        for (auto* ray : vertical_sun_rays_)
            lv_obj_set_style_line_color(ray, color, 0);
    }
}
void VolumeText(int value) {
    char text[48]; snprintf(text, sizeof text, Tr("音量  %d%%", "Volume  %d%%"), value);
    Text(volume_label_, text);
    if (shown_kind_ == View::kVolumeControl)
        VerticalLevelText(value, false);
}
void LevelEvent(lv_event_t* event) {
    const auto code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    const int value = lv_slider_get_value(lv_event_get_current_target_obj(event));
    const bool brightness = lv_event_get_current_target_obj(event) == vertical_slider_ &&
                            shown_kind_ == View::kBrightnessControl;
    if (brightness) {
        VerticalLevelText(value, true);
        SetBacklight(value);
    } else {
        VolumeText(value);
        SetVolume(value);
    }
    Input input{};
    input.action = brightness ? TouchAction::kBrightness : TouchAction::kVolume;
    input.row = value;
    input.commit = code != LV_EVENT_VALUE_CHANGED;
    if (!brightness) {
        volume_events_.fetch_add(1);
        if (input.commit)
            volume_commits_.fetch_add(1);
    }
    input.view = shown_view_; input.at = esp_timer_get_time();
    if (handler_) handler_(input, context_);
}
void ListScroll(lv_event_t*) {
    if (!list_gesture_) return;
    list_gesture_ = false;
    if (list_direction_ < 0 && has_after_ && lv_obj_get_scroll_bottom(list_) < 50)
        Dispatch(TouchAction::kLoadAfter);
    else if (list_direction_ > 0 && has_before_ && lv_obj_get_scroll_y(list_) < 50)
        Dispatch(TouchAction::kLoadBefore);
}
void CancelClick(lv_event_t*) { click_cancelled_ = true; }
void ScrollStyle(lv_obj_t* obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_style_width(obj, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x636366), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, 2, LV_PART_SCROLLBAR);
}
void Build() {
    if (card_renderer_ && card_renderer_->palette) {
        const auto& colors = *card_renderer_->palette;
        kBackground = colors.background; kCard = colors.card;
        kWhite = colors.text; kMuted = colors.muted;
        kBlue = colors.accent; kPressed = colors.pressed;
    }
    auto* root = lv_screen_active(); lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(kBackground), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    ring_ = lv_obj_create(root); lv_obj_remove_style_all(ring_);
    lv_obj_set_pos(ring_, kRingInset, kRingInset); lv_obj_set_size(ring_, kRingDiameter, kRingDiameter);
    lv_obj_set_style_radius(ring_, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_border_width(ring_, kRingWidth, 0);
    lv_obj_set_style_border_color(ring_, lv_color_hex(ActivityColor(Activity::kOffline)), 0);
    lv_obj_set_style_border_opa(ring_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ring_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring_, LV_OBJ_FLAG_SCROLLABLE);
    // Every viewport is inscribed in the circle. No circular subtree mask or
    // translucent full-screen layer is needed during software rendering.
    heading_ = Label(root, fonts_.Heading(), kBlue, true); Position(heading_, kHeading);
    battery_label_ = Label(root, &lv_font_montserrat_14, kMuted, true); Position(battery_label_, kBattery);
    title_ = Label(root, fonts_.Title(), kWhite, true); Position(title_, kTitle);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);
    detail_ = Label(root, fonts_.Body(), kMuted, true); Position(detail_, kDetail);
    lv_label_set_long_mode(detail_, LV_LABEL_LONG_DOT);
    back_ = Card(root, 12); Position(back_, kBack); BindClick(back_, -1);
    back_label_ = Label(back_, &lv_font_montserrat_20, kBlue, true);
    Text(back_label_, LV_SYMBOL_LEFT);
    lv_obj_center(back_label_);
    answer_view_ = Card(root); Position(answer_view_, kAnswer); ScrollStyle(answer_view_);
    lv_obj_set_style_pad_all(answer_view_, 12, 0);
    answer_label_ = Label(answer_view_, fonts_.Body(), kWhite);
    lv_obj_set_width(answer_label_, kAnswer.w - Scale(32)); lv_obj_set_style_text_line_space(answer_label_, Scale(5), 0);
    list_ = lv_obj_create(root); lv_obj_remove_style_all(list_); Position(list_, kList); ScrollStyle(list_);
    // Keep the thumb visible while idle whenever a list has more content.
    lv_obj_set_style_width(list_, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list_, lv_color_hex(0xA0AAB8), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(list_, CancelClick, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(list_, ListScroll, LV_EVENT_SCROLL_END, nullptr);
#if AMOLED_UI_SIZE <= 360
    list_code_ = Label(list_, &lv_font_montserrat_36, kBlue, true);
#else
    list_code_ = Label(list_, &lv_font_montserrat_48, kBlue, true);
#endif
    list_info_ = Label(list_, fonts_.Body(), kWhite);
    list_hint_ = Label(list_, fonts_.Body(), kMuted, true);
    for (int i = 0; i < kListRows; ++i) {
        rows_[i] = Card(list_, 18); BindClick(rows_[i], i);
        lv_obj_set_size(rows_[i], kList.w - Scale(5), kRowHeight - Scale(8));
        row_icons_[i] = Label(rows_[i], &lv_font_montserrat_20, kBlue, true);
        lv_obj_set_size(row_icons_[i], Scale(28), lv_font_get_line_height(&lv_font_montserrat_20));
        lv_obj_align(row_icons_[i], LV_ALIGN_LEFT_MID, Scale(10), 0);
        row_labels_[i] = Label(rows_[i], fonts_.Body(), kWhite);
        lv_obj_set_size(row_labels_[i], kList.w - Scale(84), lv_font_get_line_height(fonts_.Body()));
        lv_obj_align(row_labels_[i], LV_ALIGN_LEFT_MID, Scale(48), 0);
        lv_label_set_long_mode(row_labels_[i], LV_LABEL_LONG_DOT);
        auto* arrow = row_arrows_[i] = Label(rows_[i], &lv_font_montserrat_14, kMuted);
        Text(arrow, LV_SYMBOL_RIGHT); lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -Scale(10), 0);
        auto* toggle = row_switches_[i] = lv_switch_create(rows_[i]);
        lv_obj_remove_style_all(toggle);
        lv_obj_set_size(toggle, Scale(48), Scale(28));
        lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -Scale(10), 0);
        lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(toggle, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(toggle, lv_color_hex(0x51565F), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(toggle, lv_color_hex(0x3488FA),
                                  (uint32_t(LV_PART_INDICATOR) | uint32_t(LV_STATE_CHECKED)));
        lv_obj_set_style_bg_opa(toggle, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, (uint32_t(LV_PART_INDICATOR) | uint32_t(LV_STATE_CHECKED)));
        lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(toggle, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_pad_all(toggle, -Scale(3), LV_PART_KNOB);
        Visible(toggle, false);
    }
    volume_box_ = Card(root, 18); Position(volume_box_, kVolume);
    volume_label_ = Label(volume_box_, fonts_.Body(), kMuted, true);
    lv_obj_set_pos(volume_label_, 0, 0); lv_obj_set_size(volume_label_, kVolume.w, Scale(25));
    slider_ = lv_slider_create(volume_box_); lv_obj_remove_style_all(slider_);
    lv_obj_set_pos(slider_, Scale(30), Scale(34)); lv_obj_set_size(slider_, Scale(200), Scale(5));
    lv_obj_set_ext_click_area(slider_, 14);
    lv_slider_set_range(slider_, 0, 100);
    lv_obj_set_style_bg_color(slider_, lv_color_hex(0x48484A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_, lv_color_hex(kBlue), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_, lv_color_hex(kWhite), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_, 7, LV_PART_KNOB);
    lv_obj_set_style_radius(slider_, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_, LevelEvent, LV_EVENT_ALL, nullptr);
    footer_ = Label(root, fonts_.Small(), kMuted, true); Position(footer_, kFooter);
    lv_label_set_long_mode(footer_, LV_LABEL_LONG_DOT);
    auto* handle = Card(root, 3); Position(handle, kHandle);
    home_handle_ = handle;
    lv_obj_set_style_bg_color(handle, lv_color_hex(0xB8B8BD), 0); lv_obj_set_style_border_width(handle, 0, 0);
    latest_ = Card(root, Scale(12)); lv_obj_set_pos(latest_, Scale(324), Scale(286)); lv_obj_set_size(latest_, Scale(40), Scale(32));
    BindClick(latest_, -2); auto* down = Label(latest_, &lv_font_montserrat_20, kBlue);
    Text(down, LV_SYMBOL_DOWN); lv_obj_center(down); Visible(latest_, false);
    if (card_renderer_) card_renderer_->create(root, fonts_, BindClick);
    quick_menu_ = Card(root, 0);
    lv_obj_set_pos(quick_menu_, 0, 0);
    lv_obj_set_size(quick_menu_, kDisplaySize, kDisplaySize);
    lv_obj_set_style_bg_color(quick_menu_, lv_color_hex(0x080B10), 0);
    lv_obj_set_style_border_width(quick_menu_, 0, 0);
    lv_obj_remove_flag(quick_menu_, LV_OBJ_FLAG_CLICKABLE);
    quick_header_ = Label(quick_menu_, fonts_.Small(), 0xBCC5D0, true);
    Position(quick_header_, ScaledRect(90, 65, 286, 36));
    const char* quick_icons[]{LV_SYMBOL_WIFI, LV_SYMBOL_AUDIO, LV_SYMBOL_CLOSE, LV_SYMBOL_SETTINGS};
    for (int i = 0; i < 4; ++i) {
        quick_buttons_[i] = Card(quick_menu_, 24);
        Position(quick_buttons_[i], ScaledRect(83 + (i % 2) * 158, 124 + (i / 2) * 150, 142, 134));
        lv_obj_set_style_bg_color(quick_buttons_[i], lv_color_hex(0x10151C), 0);
        lv_obj_set_style_border_color(quick_buttons_[i], lv_color_hex(0x262D36), 0);
        BindClick(quick_buttons_[i], i);
        lv_obj_set_style_bg_color(quick_buttons_[i], lv_color_hex(0x202832), LV_STATE_PRESSED);
        auto* icon = Label(quick_buttons_[i], &lv_font_montserrat_24, 0xEFF3F8);
        Position(icon, ScaledRect(20, 20, 102, 40));
        Text(icon, quick_icons[i]);
        CenterLabelVertically(icon);
        quick_labels_[i] = Label(quick_buttons_[i], fonts_.Body(), 0xEFF3F8);
        Position(quick_labels_[i], ScaledRect(20, 78, 102, 36));
    }
    Visible(quick_menu_, false);
    vertical_control_ = Card(root, 0);
    lv_obj_set_pos(vertical_control_, 0, 0);
    lv_obj_set_size(vertical_control_, kDisplaySize, kDisplaySize);
    lv_obj_set_style_bg_color(vertical_control_, lv_color_hex(0x080D16), 0);
    lv_obj_set_style_border_width(vertical_control_, 0, 0);
    lv_obj_remove_flag(vertical_control_, LV_OBJ_FLAG_CLICKABLE);
    auto* volume_back = Card(vertical_control_, LV_RADIUS_CIRCLE);
    const auto level_back_rect =
        kDisplaySize == 360 ? ScaledRect(78, 61, 58, 58) : ScaledRect(100, 65, 46, 46);
    Position(volume_back, level_back_rect);
    BindClick(volume_back, -1);
    if (kDisplaySize == 360)
        lv_obj_set_ext_click_area(volume_back, std::max(touch_policy_.back_padding, Scale(16)));
    auto* volume_back_label =
        Label(volume_back, kDisplaySize == 360 ? &lv_font_montserrat_24 : &lv_font_montserrat_20,
              kBlue, true);
    lv_obj_set_size(volume_back_label, level_back_rect.w, level_back_rect.h);
    Text(volume_back_label, LV_SYMBOL_LEFT);
    CenterLabelVertically(volume_back_label);
    vertical_title_ = Label(vertical_control_, fonts_.Title(), kWhite, true);
    Position(vertical_title_, ScaledRect(143, 42, 180, 32));
    vertical_value_ = Label(vertical_control_, &lv_font_montserrat_24, kWhite, true);
    Position(vertical_value_, ScaledRect(143, 83, 180, 42));
    vertical_slider_ = lv_slider_create(vertical_control_);
    lv_obj_remove_style_all(vertical_slider_);
    Position(vertical_slider_, ScaledRect(161, 128, 144, 264));
    lv_slider_set_range(vertical_slider_, 0, 100);
    // Keep the slider's touch padding clear of the bottom-edge Home gesture.
    lv_obj_set_ext_click_area(vertical_slider_, 4);
    lv_obj_set_style_radius(vertical_slider_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vertical_slider_, lv_color_hex(0x2D3745), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vertical_slider_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vertical_slider_, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(vertical_slider_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_clip_corner(vertical_slider_, true, 0);
    lv_obj_add_event_cb(vertical_slider_, LevelEvent, LV_EVENT_ALL, nullptr);
    vertical_fill_ = Card(vertical_slider_, 0);
    lv_obj_set_size(vertical_fill_, Scale(144), Scale(264));
    lv_obj_set_style_border_width(vertical_fill_, 0, 0);
    lv_obj_set_style_bg_color(vertical_fill_, lv_color_hex(0xEAF2F4), 0);
    lv_obj_remove_flag(vertical_fill_, LV_OBJ_FLAG_CLICKABLE);
    vertical_icon_ = Label(vertical_slider_, &lv_font_montserrat_24, 0x25313A, true);
    Position(vertical_icon_, ScaledRect(0, 192, 144, 48));
    vertical_sun_ = lv_obj_create(vertical_slider_);
    lv_obj_remove_style_all(vertical_sun_);
    Position(vertical_sun_, ScaledRect(46, 190, 52, 52));
    lv_obj_remove_flag(vertical_sun_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(vertical_sun_, LV_OBJ_FLAG_SCROLLABLE);
    vertical_sun_disc_ = lv_obj_create(vertical_sun_);
    lv_obj_remove_style_all(vertical_sun_disc_);
    Position(vertical_sun_disc_, ScaledRect(17, 17, 18, 18));
    lv_obj_set_style_radius(vertical_sun_disc_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(vertical_sun_disc_, 2, 0);
    lv_obj_remove_flag(vertical_sun_disc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(vertical_sun_disc_, LV_OBJ_FLAG_SCROLLABLE);
    constexpr int rays[8][4]{{17, 0, 24, 0},     {12, 12, 17, 17},  {0, 17, 0, 24},
                             {-12, 12, -17, 17}, {-17, 0, -24, 0},  {-12, -12, -17, -17},
                             {0, -17, 0, -24},   {12, -12, 17, -17}};
    for (int ray = 0; ray < 8; ++ray) {
        for (int end = 0; end < 2; ++end) {
            vertical_sun_points_[ray][end].x = Scale(26 + rays[ray][end * 2]);
            vertical_sun_points_[ray][end].y = Scale(26 + rays[ray][end * 2 + 1]);
        }
        vertical_sun_rays_[ray] = lv_line_create(vertical_sun_);
        lv_line_set_points(vertical_sun_rays_[ray], vertical_sun_points_[ray], 2);
        lv_obj_set_style_line_width(vertical_sun_rays_[ray], 2, 0);
        lv_obj_remove_flag(vertical_sun_rays_[ray], LV_OBJ_FLAG_CLICKABLE);
    }
    vertical_hint_ = Label(vertical_control_, fonts_.Small(), kMuted, true);
    Position(vertical_hint_, ScaledRect(123, 407, 220, 24));
    Visible(vertical_control_, false);
    edge_back_badge_ = Card(root, LV_RADIUS_CIRCLE);
    Position(edge_back_badge_, ScaledRect(10, 209, 112, 48));
    lv_obj_set_style_bg_color(edge_back_badge_, lv_color_hex(0x111820), 0);
    lv_obj_set_style_border_color(edge_back_badge_, lv_color_hex(0x2A333E), 0);
    lv_obj_remove_flag(edge_back_badge_, LV_OBJ_FLAG_CLICKABLE);
    auto* back_hint_arrow = Label(edge_back_badge_, &lv_font_montserrat_16, 0xEFF3F8, true);
    Position(back_hint_arrow, ScaledRect(10, 8, 24, 32));
    Text(back_hint_arrow, LV_SYMBOL_RIGHT);
    CenterLabelVertically(back_hint_arrow);
    edge_back_text_ = Label(edge_back_badge_, fonts_.Small(), 0xEFF3F8, true);
    Position(edge_back_text_, ScaledRect(36, 8, 66, 32));
    Visible(edge_back_badge_, false);
    toast_label_ = Label(root, fonts_.Body(), kWhite, true); Position(toast_label_, kToast);
    lv_obj_set_style_bg_color(toast_label_, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(toast_label_, LV_OPA_COVER, 0); lv_obj_set_style_radius(toast_label_, 18, 0);
    lv_obj_set_style_pad_all(toast_label_, 8, 0); Visible(toast_label_, false);
}
void ReadTouch(lv_indev_t*, lv_indev_data_t* data) {
    // LVGL preloads the last delivered position. CST9217 release/error frames
    // have no coordinates; forwarding their zeroes makes slider RELEASED
    // handling jump to the minimum and commit the wrong volume.
    if (point_.pressed) { data->point.x = point_.x; data->point.y = point_.y; }
    data->state = point_.pressed && !swallow_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
void TrackTap(const cst9217::Point& next) {
    const bool first = next.pressed && !contact_down_;
    contact_down_ = next.pressed;
    if (!next.pressed) return;  // Keep cancellation latched through the release/click events.
    if (first) {
        contact_x_ = next.x; contact_y_ = next.y; list_direction_ = 0;
        contact_view_ = shown_view_;
        click_cancelled_ = false; pressed_obj_ = nullptr;
    }
    // Only standalone buttons get more finger jitter tolerance. List rows keep
    // the existing scroll guard, and free touch-test targets keep their precision.
    const int slop =
        pressed_obj_ && lv_obj_get_parent(pressed_obj_) != list_ ? touch_policy_.button_slop : 12;
    if (std::abs(static_cast<int>(next.x) - contact_x_) > slop ||
        std::abs(static_cast<int>(next.y) - contact_y_) > slop) {
        click_cancelled_ = true;
    }
    const lv_point_t position{next.x, next.y};
    if (pressed_obj_ && !lv_obj_hit_test(pressed_obj_, &position)) click_cancelled_ = true;
}
Activity Resolve(const Screen& screen, const agent_link_status_t& status) {
    if (screen.local_mode) return screen.activity;
    if (screen.activity == Activity::kError || status.phase == AGENT_LINK_PHASE_BLOCKED) return Activity::kError;
    if (status.phase == AGENT_LINK_PHASE_SETUP || status.phase == AGENT_LINK_PHASE_PAIRING) return Activity::kSetup;
    if (screen.calling || screen.busy) return screen.busy ? Activity::kConnecting : screen.activity;
    if (status.phase == AGENT_LINK_PHASE_CONNECTED || status.phase == AGENT_LINK_PHASE_READY) return Activity::kIdle;
    return status.phase == AGENT_LINK_PHASE_IDLE ? Activity::kOffline : Activity::kConnecting;
}
void Render(void*) {
    Mark(UiStage::kFonts);
    fonts_.Init(); Build();
    uint32_t seen = 0, seen_wake = 0, navigation = 0;
    size_t offset = 0;
    bool dim = false, was_toast = false, keep_awake = false;
    int64_t activity_at = esp_timer_get_time();
    Activity state = Activity::kOffline;
    uint32_t ring_color = 0;
    // Large snapshots stay out of the render task stack.
    static int64_t memory_at = 0;
    static Screen screen;
    static char shown_answer[sizeof answer_] = {}, text[sizeof answer_];
    while (true) {
        if (sleep_requested_.load()) {
            CompleteTransfer(true);
            if (!sleep_ready_.load())
                Brightness(0);
            sleep_ready_.store(true);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        sleep_ready_.store(false);
        const int64_t now = esp_timer_get_time(); heartbeat_.store(now);
        CompleteTransfer(false);
        const uint8_t backlight = (backlight_.load() * 255 + 50) / 100;
        if (!dim && !panel_fault_ && backlight != applied_backlight_)
            Brightness(backlight);
        if (panel_fault_ && now >= panel_retry_at_) {
            Mark(UiStage::kPanel);
            bool recovered = false;
            const esp_err_t err = panel_->Recover(&recovered);
            panel_retry_at_ = esp_timer_get_time() + 1000000;
            if (err == ESP_OK) {
                panel_fault_ = false;
                panel_recoveries_.fetch_add(1);
                lv_obj_invalidate(lv_screen_active());
            }
        }
        if (pointer_) {
            Mark(UiStage::kTouch);
            const int64_t poll_at = esp_timer_get_time();
            if (last_touch_poll_at_) {
                const uint32_t gap = static_cast<uint32_t>((poll_at - last_touch_poll_at_) / 1000);
                if (gap > max_touch_gap_ms_.load()) max_touch_gap_ms_.store(gap);
            }
            last_touch_poll_at_ = poll_at;
            touch_polls_.fetch_add(1);
            cst9217::Point next;
            const bool touch_ok = touch_->Poll(&next) == ESP_OK;
            if (!touch_ok) { next.pressed = false; click_cancelled_ = true; touch_errors_.fetch_add(1); }
            if constexpr (DISPLAY_ROTATION_CW == 90) {
                if (next.pressed) {
                    const auto logical = Clockwise90Touch(next.x, next.y, DISPLAY_WIDTH);
                    next.x = logical.x; next.y = logical.y;
                }
            }
            // Test taps use the last pressed coordinates (release frames have none).
            // Target pages keep Back on its LVGL click path; solid-color pages
            // accept taps anywhere. Motion and I2C errors cannot advance a test.
            const bool touch_test = screen.kind == View::kHardwareTest &&
                screen.hardware_test.kind == HardwareTest::Kind::kTouch;
            const bool color_test = screen.kind == View::kHardwareTest &&
                                    screen.hardware_test.kind == HardwareTest::Kind::kColors;
            if ((touch_test || color_test) && touch_ok && point_.pressed && !next.pressed &&
                contact_view_ == shown_view_ && !click_cancelled_ && !swallow_ && card_renderer_ &&
                (color_test || (card_renderer_->capture_test_tap &&
                                card_renderer_->capture_test_tap(point_.x, point_.y))) &&
                Inside(point_.x, point_.y, kDisplaySize / 2))
                Dispatch(TouchAction::kTouchPoint, 0, point_.x, point_.y);
            if (touch_ok) TrackTap(next);
            const bool physical_down = next.pressed;
            // Native card layouts can place visible buttons outside the shared
            // list inset. Their taps must use the same physical circle as gestures.
            const bool gesture_down = next.pressed && Inside(next.x, next.y, kDisplaySize / 2);
            const bool full_circle = touch_test || touch_policy_.full_circle;
            if (next.pressed && !Inside(next.x, next.y, full_circle ? kDisplaySize / 2 : kContentRadius)) {
                next.pressed = false; click_cancelled_ = true;
            }
            if (gesture_down) {
                activity_at = now;
                if (dim) {
                    dim = false;
                    swallow_ = true;
                    Mark(UiStage::kBrightness);
                    Brightness(backlight);
                }
                if (shown_kind_ != View::kHome && contact_y_ >= kList.y && contact_y_ < kList.y + kList.h &&
                    std::abs(next.y - contact_y_) > 12 && std::abs(next.y - contact_y_) > std::abs(next.x - contact_x_)) {
                    list_gesture_ = true; list_direction_ = next.y - contact_y_;
                }
            } else
                swallow_ = false;
            const bool vertical_drag =
                (shown_kind_ == View::kVolumeControl || shown_kind_ == View::kBrightnessControl) &&
                contact_x_ >= Scale(161) - 4 && contact_x_ <= Scale(305) + 4 &&
                contact_y_ >= Scale(128) - 4 && contact_y_ <= Scale(392) + 4;
            const bool volume_touch =
                vertical_drag ||
                (!lv_obj_has_flag(volume_box_, LV_OBJ_FLAG_HIDDEN) &&
                 contact_y_ >= kVolume.y - 10 && contact_y_ <= kVolume.y + kVolume.h + 10);
            const bool edge_system_swipe = touch_policy_.edge_system_swipe;
            const bool carousel =
                shown_kind_ == View::kCarousel || shown_kind_ == View::kFactoryHome;
            const bool edge_back_swipe =
                touch_policy_.edge_back_swipe && !carousel && shown_kind_ != View::kHome;
            const bool edge_back_start = contact_x_ <= Scale(SwipeTracker::kBackEdge);
            const bool test_target = touch_test &&
                                     (!edge_system_swipe || contact_y_ >= Scale(85)) &&
                                     !(edge_back_swipe && edge_back_start);
            const bool stale_touch = contact_view_ != shown_view_;
            const int gesture_x = next.x * 466 / DISPLAY_WIDTH;
            const int gesture_y = next.y * 466 / DISPLAY_HEIGHT;
            const Swipe direction =
                swipe_.Update(physical_down, gesture_x, gesture_y, shown_view_, now,
                              volume_touch || test_target || stale_touch || swallow_ || !touch_ok ||
                                  (physical_down && !gesture_down),
                              edge_system_swipe, edge_back_swipe);
            const bool back_hint = gesture_down && !swallow_ && !pending_ && !shown_busy_ &&
                                   edge_back_swipe &&
                                   swipe_.BackHint(gesture_x, gesture_y, shown_view_);
            if (back_hint && lv_obj_has_flag(edge_back_badge_, LV_OBJ_FLAG_HIDDEN))
                lv_indev_wait_release(pointer_);  // The captured edge gesture cannot press a tile.
            Visible(edge_back_badge_, back_hint);
            if (back_hint) {
                Text(edge_back_text_, Tr("返回", "Back"));
                CenterLabelVertically(edge_back_text_);
            }
            bool sent = false;
            if (direction == Swipe::kSystem) sent = Dispatch(TouchAction::kSystem);
            else if ((shown_kind_ == View::kCarousel || shown_kind_ == View::kFactoryHome) && direction == Swipe::kLeft) sent = Dispatch(TouchAction::kNextCard);
            else if (shown_kind_ == View::kCarousel && direction == Swipe::kRight) sent = Dispatch(TouchAction::kPreviousCard);
            else if (direction == Swipe::kHome) sent = Dispatch(TouchAction::kHome);
            else if (direction == Swipe::kLeft && shown_kind_ == View::kHome) sent = Dispatch(TouchAction::kMenu);
            else if (direction == Swipe::kRight && shown_kind_ != View::kHome) sent = Dispatch(TouchAction::kBack);
            if (sent) { lv_indev_wait_release(pointer_); list_gesture_ = false; }
            point_ = next;
        }
        if (seen_wake != wake_.load()) {
            seen_wake = wake_.load(); activity_at = now;
            if (dim) {
                dim = false;
                Mark(UiStage::kBrightness);
                Brightness(backlight);
            }
        }
        const uint32_t revision = revision_.load(std::memory_order_acquire);
        if (revision != seen || was_toast) {
            agent_link_status_t status;
            char message[sizeof toast_]; int battery, volume; bool charging; int64_t until;
            Mark(UiStage::kSnapshot);
            taskENTER_CRITICAL(&lock_);
            const uint32_t text_sequence = text_changed_.load();
            screen = screen_; status = status_; battery = battery_; charging = charging_; volume = volume_;
            memcpy(text, answer_, sizeof text); memcpy(message, toast_, sizeof message); until = toast_until_;
            taskEXIT_CRITICAL(&lock_);
            Mark(UiStage::kHeader);
            seen = revision; state = Resolve(screen, status);
            keep_awake = screen.calling || screen.busy || screen.local_mode;
            const bool new_page = navigation != screen.navigation;
            if (shown_view_ != screen.view) {
                Visible(edge_back_badge_, false);
                shown_view_ = screen.view; pending_ = false;
                if (pointer_ && point_.pressed) lv_indev_wait_release(pointer_);
            }
            shown_kind_ = screen.kind; shown_busy_ = screen.busy;
            const bool home = screen.kind == View::kHome;
            const bool quick = screen.kind == View::kQuickMenu;
            const bool brightness_control = screen.kind == View::kBrightnessControl;
            const bool large_level = screen.kind == View::kVolumeControl || brightness_control;
            const bool settings = screen.kind == View::kSettings;
            const bool poster =
                screen.kind == View::kCarousel || screen.kind == View::kAppActions || quick ||
                large_level ||
                (card_renderer_ && card_renderer_->accepts && card_renderer_->accepts(screen));
            const bool quiet = screen.launcher_shell;
            const bool roomy_menu = quiet && kDisplaySize == 360;
            const int row_step = roomy_menu ? Scale(88) : kRowHeight;
            const bool setup = home && (status.phase == AGENT_LINK_PHASE_SETUP ||
                status.phase == AGENT_LINK_PHASE_PAIRING || status.phase == AGENT_LINK_PHASE_BLOCKED);
            const bool binding_guide =
                setup && status.phase == AGENT_LINK_PHASE_PAIRING && screen.info[0];
            const bool volume_page = screen.volume_control;
            const char* state_text = home && screen.ended && state == Activity::kIdle ? Tr("通话已结束", "Call ended") : ActivityName(state);
            Text(heading_, state_text);
            lv_obj_set_style_text_color(heading_, lv_color_hex(ActivityColor(state)), 0);
            char info[80];
            if (battery >= 0) snprintf(info, sizeof info, "AGENT LINK   %d%%%s", battery, charging ? " " LV_SYMBOL_CHARGE : "");
            else
                snprintf(info, sizeof info, "AGENT LINK   --%%");
            Text(battery_label_, info);
            const char* setup_title = status.phase == AGENT_LINK_PHASE_PAIRING ? Tr("激活设备", "Activate device") :
                status.phase == AGENT_LINK_PHASE_SETUP ? Tr("连接 WiFi", "Connect WiFi") : Tr("设备需要处理", "Needs attention");
            Text(title_, setup ? setup_title : screen.title);
            Position(title_, quiet ? Rect{Scale(111),Scale(72),Scale(244),Scale(36)} : kTitle);
            Position(back_, quiet ? Rect{Scale(85),Scale(111),Scale(38),Scale(35)} : kBack);
            Position(list_, quiet ? Rect{Scale(85),Scale(154),Scale(296),Scale(222)} : kList);
            if (settings)
                Position(list_, ScaledRect(80, 106, 306, 290));
            else if (binding_guide)
                Position(list_, ScaledRect(39, 181, 388, 125));
            Position(detail_, quiet ? Rect{Scale(129),Scale(115),Scale(249),Scale(24)} : kDetail);
            if (settings) {
                Position(title_, ScaledRect(110, 50, 246, 36));
                Position(back_, ScaledRect(80, 48, 38, 38));
            }
            if (roomy_menu) {
                Position(title_, ScaledRect(136, 68, 194, 44));
                Position(back_, ScaledRect(78, 61, 58, 58));
                Position(detail_, ScaledRect(129, 119, 208, 24));
                Position(list_, screen.detail[0] ? ScaledRect(72, 160, 322, 254)
                                                 : ScaledRect(72, 132, 322, 282));
            }
            lv_obj_set_style_radius(back_, roomy_menu ? LV_RADIUS_CIRCLE : 12, 0);
            lv_obj_set_ext_click_area(back_, roomy_menu
                                                 ? std::max(touch_policy_.back_padding, Scale(16))
                                                 : touch_policy_.back_padding);
            lv_obj_set_style_text_font(
                back_label_, roomy_menu ? &lv_font_montserrat_24 : &lv_font_montserrat_20, 0);
            lv_obj_center(back_label_);
            if (!home && !roomy_menu) {
                lv_obj_set_x(detail_, Scale(129));
                lv_obj_set_width(detail_, Scale(249));
            }
            Text(detail_, setup ? status.hint : screen.detail);
            Visible(back_, !home && !screen.busy);
            Visible(answer_view_, home && !setup);
            Visible(volume_box_, (home && !setup) || volume_page);
            if (!lv_slider_is_dragged(slider_)) { lv_slider_set_value(slider_, volume, LV_ANIM_OFF); VolumeText(volume); }
            Visible(list_, !home || setup); Visible(latest_, false);
            lv_obj_set_height(list_, roomy_menu      ? Scale(screen.detail[0] ? 254 : 282)
                                     : settings      ? Scale(290)
                                     : binding_guide ? Scale(125)
                                     : volume_page   ? kAnswer.h
                                     : quiet         ? Scale(222)
                                                     : kList.h);
            const char* answer = text[0] ? text : screen.calling && state == Activity::kListening ?
                Tr("我在听，请说话。", "Listening. Go ahead.") : screen.calling ?
                Tr("正在建立通话，准备好后请说话…", "Connecting. Speak when ready...") :
                Tr("与 Agent 开始一次对话。\n\n短按 BOOT 开始\n左滑查看应用", "Talk to your Agent.\n\nPress BOOT to call.\nSwipe left for apps.");
            Mark(UiStage::kReplyMeasure);
            lv_obj_update_layout(answer_view_);
            const bool follow = FollowReply(lv_obj_get_scroll_bottom(answer_view_), point_.pressed);
            const int old_answer_y = lv_obj_get_scroll_y(answer_view_);
            if (strcmp(shown_answer, answer) != 0) {
                snprintf(shown_answer, sizeof shown_answer, "%s", answer);
                Mark(UiStage::kReplyText); Text(answer_label_, answer);
                Mark(UiStage::kReplyLayout); lv_obj_update_layout(answer_view_);
                const int end = lv_obj_get_scroll_y(answer_view_) + lv_obj_get_scroll_bottom(answer_view_);
                Mark(UiStage::kReplyScroll);
                lv_obj_scroll_to_y(answer_view_, follow ? std::max(0, end) : old_answer_y, LV_ANIM_OFF);
            }
            Visible(latest_, home && !setup && lv_obj_get_scroll_bottom(answer_view_) > 24);
            text_applied_.store(text_sequence);
            if ((!home || setup) && !poster) {
                Mark(UiStage::kList);
                const int old_y = lv_obj_get_scroll_y(list_);
                int y = 0;
                const int content_width = roomy_menu      ? Scale(322)
                                          : binding_guide ? Scale(388)
                                                          : kList.w;
                const char* code = setup ? status.code : screen.pair_code;
                const bool show_code = code[0] && (!setup || status.phase == AGENT_LINK_PHASE_PAIRING);
                Visible(list_code_, show_code);
                if (show_code) {
                    Text(list_code_, code); lv_obj_set_pos(list_code_, 0, y);
                    lv_obj_set_size(list_code_, content_width - Scale(6), Scale(60));
                    CenterLabelVertically(list_code_); y += Scale(68);
                }
                const auto render_row = [&](int i) {
                    const bool show = !setup && i < screen.row_count;
                    Visible(rows_[i], show);
                    if (!show)
                        return;
                    const bool toggle = (screen.toggle_rows & (1u << i)) != 0;
                    Visible(row_switches_[i], toggle);
                    Visible(row_arrows_[i], !toggle);
                    const int row_width = content_width - Scale(roomy_menu ? 8 : 5);
                    const int row_height = row_step - Scale(roomy_menu ? 12 : 8);
                    lv_obj_set_size(rows_[i], row_width, row_height);
                    const auto* row_font = roomy_menu ? fonts_.Title() : fonts_.Body();
                    lv_obj_set_style_text_font(row_labels_[i], row_font, 0);
                    lv_obj_set_size(
                        row_labels_[i], row_width - Scale(toggle ? 112 : 80),
                        roomy_menu ? row_height - Scale(12) : lv_font_get_line_height(row_font));
                    lv_obj_align(row_labels_[i], LV_ALIGN_LEFT_MID, Scale(48), 0);
                    lv_obj_set_style_text_font(
                        row_icons_[i], roomy_menu ? &lv_font_montserrat_24 : &lv_font_montserrat_20,
                        0);
                    lv_obj_set_size(row_icons_[i], Scale(28),
                                    lv_font_get_line_height(roomy_menu ? &lv_font_montserrat_24
                                                                       : &lv_font_montserrat_20));
                    lv_obj_align(row_icons_[i], LV_ALIGN_LEFT_MID, Scale(10), 0);
                    lv_obj_set_style_text_font(
                        row_arrows_[i],
                        roomy_menu ? &lv_font_montserrat_18 : &lv_font_montserrat_14, 0);
                    lv_obj_align(row_arrows_[i], LV_ALIGN_RIGHT_MID, -Scale(10), 0);
                    lv_obj_align(row_switches_[i], LV_ALIGN_RIGHT_MID, -Scale(10), 0);
                    if (screen.checked_rows & (1u << i))
                        lv_obj_add_state(row_switches_[i], LV_STATE_CHECKED);
                    else
                        lv_obj_remove_state(row_switches_[i], LV_STATE_CHECKED);
                    lv_obj_set_style_bg_color(rows_[i], lv_color_hex(settings ? 0x10151C : kCard),
                                              0);
                    lv_obj_set_style_border_color(rows_[i],
                                                  lv_color_hex(settings ? 0x262D36 : 0x303034), 0);
                    lv_obj_set_style_bg_color(
                        rows_[i], lv_color_hex(settings ? 0x202832 : kPressed), LV_STATE_PRESSED);
                    Text(row_labels_[i], screen.rows[i]);
                    CenterLabelVertically(row_labels_[i]);
                    lv_obj_set_pos(rows_[i], roomy_menu ? (content_width - row_width) / 2 : 0, y);
                    y += row_step;
                    const char* icon = screen.kind == View::kLauncher
                                           ? (i == 0   ? LV_SYMBOL_LIST
                                              : i == 1 ? LV_SYMBOL_SETTINGS
                                                       : LV_SYMBOL_WIFI)
                                           : LV_SYMBOL_RIGHT;
                    Text(row_icons_[i], icon);
                    lv_obj_set_style_text_color(row_icons_[i],
                                                lv_color_hex(i % 3 == 0   ? kBlue
                                                             : i % 3 == 1 ? 0xBF5AF2
                                                                          : 0x30D158),
                                                0);
                };
                for (int i = 0; i < kListRows; ++i)
                    if (screen.toggle_rows & (1u << i))
                        render_row(i);
                const char* description =
                    setup && status.phase == AGENT_LINK_PHASE_PAIRING && screen.info[0]
                        ? screen.info
                    : setup ? status.hint
                            : screen.info;
                const bool link_guide = screen.kind == View::kDownloadHelp || binding_guide;
                lv_obj_set_style_text_font(list_info_, link_guide ? fonts_.Small() : fonts_.Body(),
                                           0);
                Visible(list_info_, description[0]);
                if (description[0]) {
                    Text(list_info_, description);
                    lv_obj_set_pos(list_info_, Scale(8), y);
                    lv_obj_set_width(list_info_, content_width - Scale(22));
                    lv_obj_set_height(list_info_, LV_SIZE_CONTENT); lv_obj_update_layout(list_info_); y += lv_obj_get_height(list_info_) + Scale(18);
                }
                for (int i = 0; i < kListRows; ++i)
                    if (!(screen.toggle_rows & (1u << i)))
                        render_row(i);
                has_before_ = screen.previous; has_after_ = screen.next;
                Visible(list_hint_, screen.next || screen.previous);
                if (screen.next || screen.previous) {
                    Text(list_hint_, screen.next ? Tr("继续上滑加载更多", "Scroll for more") : Tr("已到末尾", "End of list"));
                    lv_obj_set_pos(list_hint_, 0, y); lv_obj_set_size(list_hint_, kList.w - Scale(6), Scale(30));
                    CenterLabelVertically(list_hint_);
                }
                Mark(UiStage::kListLayout); lv_obj_update_layout(list_);
                const int scroll_y =
                    new_page ? 0 : WindowScroll(old_y, offset, screen.row_offset, row_step);
                if (new_page || offset != screen.row_offset) lv_obj_scroll_to_y(list_, scroll_y, LV_ANIM_OFF);
                if (new_page) list_gesture_ = false;
            }
            navigation = screen.navigation; offset = screen.row_offset;
            Text(footer_, screen.footer[0] ? screen.footer : Tr("底部上滑回通话", "Swipe up from bottom"));
            Visible(ring_, !poster && !quiet); Visible(heading_, !poster && !quiet);
            Visible(battery_label_, !poster && !quiet); Visible(home_handle_, !poster && !quiet);
            Visible(title_, !poster); Visible(detail_, !poster); Visible(footer_, !poster && !quiet);
            Visible(quick_menu_, quick);
            Visible(vertical_control_, large_level);
            if (quick || large_level) {
                Visible(back_, false);
                Visible(list_, false);
                Visible(answer_view_, false);
                Visible(volume_box_, false);
                Visible(latest_, false);
                if (quick) {
                    char header[64];
                    const int percent =
                        screen.battery_percent >= 0 ? screen.battery_percent : battery;
                    if (percent >= 0)
                        snprintf(header, sizeof header, "%d%% · Quick Panel",
                                 std::clamp(percent, 0, 100));
                    else
                        snprintf(header, sizeof header, "--%% · Quick Panel");
                    Text(quick_header_, header);
                    CenterLabelVertically(quick_header_);
                    for (int i = 0; i < 4; ++i) {
                        Text(quick_labels_[i], screen.rows[i]);
                        CenterLabelVertically(quick_labels_[i]);
                    }
                }
                if (large_level) {
                    Text(vertical_title_,
                         brightness_control ? Tr("亮度", "Brightness") : Tr("音量", "Volume"));
                    CenterLabelVertically(vertical_title_);
                    Text(vertical_hint_, Tr("上下滑动调节", "Swipe up or down"));
                    CenterLabelVertically(vertical_hint_);
                    if (!lv_slider_is_dragged(vertical_slider_)) {
                        lv_slider_set_range(vertical_slider_, brightness_control ? 5 : 0, 100);
                        lv_slider_set_value(vertical_slider_,
                                            brightness_control ? screen.brightness_percent : volume,
                                            LV_ANIM_OFF);
                    }
                    VerticalLevelText(lv_slider_get_value(vertical_slider_), brightness_control);
                }
                if (card_renderer_)
                    card_renderer_->hide();
            } else if (poster && card_renderer_) {
                Visible(back_,false); Visible(list_,false); Visible(answer_view_,false);
                Visible(volume_box_,false); Visible(latest_,false);
                card_renderer_->update(screen);
            } else if (card_renderer_)
                card_renderer_->hide();
            const bool color_test = screen.kind == View::kHardwareTest &&
                                    screen.hardware_test.kind == HardwareTest::Kind::kColors;
            was_toast = !color_test && message[0] && now < until;
            Visible(toast_label_, was_toast);
            if (was_toast) Text(toast_label_, message);
            applied_.store(revision);
        }
        if (keep_awake) activity_at = now;
        // Changing opacity invalidates the ring's whole 452x452 rectangle.
        // Repainting it continuously during calls delays this task's next touch
        // poll by hundreds of milliseconds. State colors need only one redraw.
        const uint32_t color = ActivityColor(state);
        if (color != ring_color) { ring_color = color; lv_obj_set_style_border_color(ring_, lv_color_hex(color), 0); }
        if (!dim && now - activity_at > 60000000) {
            dim = true;
            Mark(UiStage::kBrightness);
            Brightness(std::min<uint8_t>(backlight, DISPLAY_BRIGHTNESS_DIM));
        }
        Mark(UiStage::kTimers); const uint32_t next_timer_ms = lv_timer_handler();
        ui_heartbeat_.store(panel_fault_ ? 0 : static_cast<uint32_t>(esp_timer_get_time() / 1000));
        if (now >= memory_at) {
            Mark(UiStage::kMemory);
            lv_mem_monitor_t memory{}; lv_mem_monitor(&memory);
            lv_free_.store(memory.free_size); lv_largest_.store(memory.free_biggest_size);
            ui_stack_min_.store(uxTaskGetStackHighWaterMark(nullptr));
            memory_at = now + 5000000;
            if (fast_ui_) ReportUiDiagnostics();
        }
        Mark(UiStage::kIdle);
        // The launcher targets the next LVGL deadline instead of adding a fixed
        // 20 ms sleep after every draw. Poll touch at least every 5 ms when idle.
        const uint32_t sleep_ms = fast_ui_ ? std::clamp<uint32_t>(next_timer_ms, 1, 5) : 20;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms) ? pdMS_TO_TICKS(sleep_ms) : 1);
    }
}
}  // namespace

bool UiResponsive() {
    const auto at = ui_heartbeat_.load();
    return at && completed_frames_.load() &&
           static_cast<uint32_t>(esp_timer_get_time() / 1000) - at < 1000;
}
bool FramePresented(uint32_t view) {
    const uint32_t shown = presented_view_.load();
    return shown && int32_t(shown - view) >= 0;
}

esp_err_t StartUi(RoundDisplay* panel, RoundTouch* touch, InputHandler handler, void* context,
                  const CardRenderer* cards, DisplayPerformance performance,
                  CardRenderer::TouchPolicy touch_policy) {
    if (!panel || !panel->Ready()) return ESP_ERR_INVALID_STATE;
    panel_ = panel;
    touch_ = touch;
    handler_ = handler;
    context_ = context;
    ConfigureTouch(cards, touch_policy);
    fast_ui_ = performance == DisplayPerformance::kLauncher;
    const size_t bytes = fast_ui_ ? kFrameBytes : kBufferBytes;
    auto* pixels = static_cast<uint8_t*>(heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_SPIRAM));
    if (!pixels) return ESP_ERR_NO_MEM;
    uint8_t* second = nullptr;
    if (fast_ui_) {
        second = static_cast<uint8_t*>(heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_SPIRAM));
        if (!second) { heap_caps_free(pixels); return ESP_ERR_NO_MEM; }
        const esp_err_t err = display_transfer_.Start(panel, bytes, DISPLAY_ROTATION_CW, DISPLAY_WIDTH);
        if (err != ESP_OK) { heap_caps_free(second); heap_caps_free(pixels); return err; }
    } else if constexpr (DISPLAY_ROTATION_CW == 90) {
        rotated_pixels_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(4, kBufferBytes, MALLOC_CAP_SPIRAM));
        if (!rotated_pixels_) { heap_caps_free(pixels); return ESP_ERR_NO_MEM; }
    }
    const auto free_buffers = [pixels, second]() {
        display_transfer_.Stop(); heap_caps_free(second);
        heap_caps_free(rotated_pixels_); rotated_pixels_ = nullptr;
        heap_caps_free(pixels);
    };
    lv_init(); lv_tick_set_cb(Tick);
    auto* display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!display) { free_buffers(); return ESP_ERR_NO_MEM; }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, Flush);
    lv_display_add_event_cb(display, Round, LV_EVENT_INVALIDATE_AREA, nullptr);
    // Screen-sized PARTIAL buffers retain dirty-area updates without DIRECT's
    // persistent-framebuffer stride/byte-order requirements during rotation.
    lv_display_set_buffers(display, pixels, second, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    if (fast_ui_) {
        lv_display_set_flush_wait_cb(display, WaitFlush);
        lv_timer_set_period(lv_display_get_refr_timer(display), 8);
        lv_timer_set_period(lv_anim_get_timer(), 8);
    }
    if (touch && touch->Ready()) {
        pointer_ = lv_indev_create();
        if (!pointer_) { lv_display_delete(display); free_buffers(); return ESP_ERR_NO_MEM; }
        lv_indev_set_type(pointer_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(pointer_, display); lv_indev_set_read_cb(pointer_, ReadTouch);
        if (fast_ui_) lv_timer_set_period(lv_indev_get_read_timer(pointer_), 5);
    }
    Mark(UiStage::kBrightness);
    Brightness((backlight_.load() * 255 + 50) / 100);
    if (xTaskCreatePinnedToCore(Render, "amoled_ui", 8192, nullptr, 4, nullptr, portNUM_PROCESSORS - 1) != pdPASS) {
        if (pointer_) { lv_indev_delete(pointer_); pointer_ = nullptr; }
        lv_display_delete(display); free_buffers(); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
bool SameScreen(const Screen& a, const Screen& b) {
    const auto& ta = a.hardware_test;
    const auto& tb = b.hardware_test;
    if (ta.kind != tb.kind || ta.color_index != tb.color_index || ta.x != tb.x || ta.y != tb.y ||
        ta.target_x != tb.target_x || ta.target_y != tb.target_y || ta.completed != tb.completed ||
        ta.tilt_x != tb.tilt_x || ta.tilt_y != tb.tilt_y || ta.valid != tb.valid ||
        ta.centered != tb.centered || ta.passed != tb.passed)
        return false;
    if (a.kind != b.kind || a.busy != b.busy || a.calling != b.calling ||
        a.local_mode != b.local_mode || a.previous != b.previous || a.next != b.next ||
        a.ended != b.ended || a.volume_control != b.volume_control ||
        a.launcher_shell != b.launcher_shell || a.add_card != b.add_card || a.launching != b.launching ||
        a.card_index != b.card_index || a.card_count != b.card_count ||
        a.cover_slot != b.cover_slot || a.cover_revision != b.cover_revision ||
        a.storage_total != b.storage_total || a.storage_used != b.storage_used ||
        a.brightness_percent != b.brightness_percent || a.battery_percent != b.battery_percent ||
        a.wifi_connected != b.wifi_connected || a.wifi_setup_active != b.wifi_setup_active ||
        strcmp(a.wifi_setup_ssid, b.wifi_setup_ssid) || a.progress != b.progress ||
        strcmp(a.app_id, b.app_id) || a.row_count != b.row_count || a.row_offset != b.row_offset ||
        a.toggle_rows != b.toggle_rows || a.checked_rows != b.checked_rows ||
        a.navigation != b.navigation || a.activity != b.activity || a.view != b.view ||
        strcmp(a.pair_code, b.pair_code) || strcmp(a.download_code, b.download_code) ||
        strcmp(a.title, b.title) || strcmp(a.detail, b.detail) || strcmp(a.footer, b.footer) ||
        strcmp(a.info, b.info) || strcmp(a.hardware_status, b.hardware_status) ||
        strcmp(a.job_status, b.job_status))
        return false;
    for (int i = 0; i < kListRows; ++i) if (strcmp(a.rows[i], b.rows[i])) return false;
    for (int i = 0; i < 4; ++i)
        if (a.grid[i].cover_slot != b.grid[i].cover_slot ||
            a.grid[i].cover_revision != b.grid[i].cover_revision) return false;
    return true;
}
void SetScreen(const Screen& screen) {
    taskENTER_CRITICAL(&lock_);
    const bool changed = !SameScreen(screen_, screen);
    if (changed) screen_ = screen;
    taskEXIT_CRITICAL(&lock_);
    if (changed) revision_.fetch_add(1, std::memory_order_release);
}
void SetStatus(const agent_link_status_t& status) {
    status_received_.fetch_add(1);
    taskENTER_CRITICAL(&lock_);
    // Compare semantic fields, not padding or unused trailing bytes in SDK strings.
    const bool changed = status_.phase != status.phase || status_.transport != status.transport ||
        status_.expires_s != status.expires_s || status_.detail != status.detail ||
        strcmp(status_.title, status.title) || strcmp(status_.hint, status.hint) || strcmp(status_.code, status.code);
    if (changed) status_ = status;
    taskEXIT_CRITICAL(&lock_);
    if (changed) { status_changed_.fetch_add(1); revision_.fetch_add(1, std::memory_order_release); }
}
void SetBattery(int percent, bool charging) {
    taskENTER_CRITICAL(&lock_);
    const bool changed = battery_ != percent || charging_ != charging;
    battery_ = percent; charging_ = charging;
    taskEXIT_CRITICAL(&lock_);
    if (changed) revision_.fetch_add(1, std::memory_order_release);
}
void SetBacklight(int percent) {
    backlight_.store(std::clamp(percent, 5, 100));
    Wake();
}
bool PrepareForSleep() {
    sleep_ready_.store(false);
    sleep_requested_.store(true);
    for (unsigned n = 0; n < 100 && !sleep_ready_.load(); ++n)
        vTaskDelay(pdMS_TO_TICKS(10));
    return sleep_ready_.load();
}
void CancelSleep() {
    sleep_requested_.store(false);
    sleep_ready_.store(false);
    Wake();
}
void SetVolume(int percent) {
    taskENTER_CRITICAL(&lock_);
    const bool changed = volume_ != percent; volume_ = percent;
    taskEXIT_CRITICAL(&lock_);
    if (changed) revision_.fetch_add(1, std::memory_order_release);
}
void SetAnswer(const char* text) {
    text_received_.fetch_add(1);
    if (!text) text = "";
    const size_t bytes = strnlen(text, sizeof answer_ - 1);
    taskENTER_CRITICAL(&lock_);
    const bool changed = strlen(answer_) != bytes || memcmp(answer_, text, bytes) != 0;
    if (changed) {
        memcpy(answer_, text, bytes); answer_[bytes] = '\0';
        text_bytes_.store(bytes); text_changed_.fetch_add(1);
    }
    taskEXIT_CRITICAL(&lock_);
    if (changed) revision_.fetch_add(1, std::memory_order_release);
}
void Toast(const char* text, uint32_t ms) {
    const int64_t until = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
    taskENTER_CRITICAL(&lock_);
    snprintf(toast_, sizeof toast_, "%s", text ? text : ""); toast_until_ = until;
    taskEXIT_CRITICAL(&lock_); revision_.fetch_add(1, std::memory_order_release);
}
void Wake() { wake_.fetch_add(1); }
void ReportUiDiagnostics() {
    static int64_t next = 0;
    const int64_t now = esp_timer_get_time(); if (now < next) return; next = now + 5000000;
    const int64_t heartbeat = heartbeat_.load();
    const auto max_flush_us = max_flush_us_.exchange(0);
    const auto touch_polls = touch_polls_.exchange(0);
    const auto touch_errors = touch_errors_.exchange(0);
    const auto max_touch_gap_ms = max_touch_gap_ms_.exchange(0);
    const auto volume_events = volume_events_.exchange(0);
    const auto volume_commits = volume_commits_.exchange(0);
    if (fast_ui_) {
        const auto samples = active_frame_samples_.exchange(0);
        const auto span = active_frame_us_.exchange(0);
        const auto fps10 = span ? static_cast<uint32_t>(static_cast<uint64_t>(samples) * 10000000 / span) : 0;
        ESP_LOGD("Amoled175.ui", "launcher frames=%lu active_refresh=%lu.%lu fps samples=%lu",
            static_cast<unsigned long>(completed_frames_.load()), static_cast<unsigned long>(fps10 / 10),
            static_cast<unsigned long>(fps10 % 10), static_cast<unsigned long>(samples));
    }
    ESP_LOGD("Amoled175.ui", "render revision=%lu/%lu flushes=%lu max_flush=%luus heartbeat_age=%lldms",
        static_cast<unsigned long>(applied_.load()), static_cast<unsigned long>(revision_.load()),
        static_cast<unsigned long>(flushes_.load()), static_cast<unsigned long>(max_flush_us),
        static_cast<long long>(heartbeat ? (now - heartbeat) / 1000 : -1));
    const uint32_t xy = flush_xy_.load(), wh = flush_wh_.load();
    ESP_LOGD("Amoled175.ui", "stage=%s age=%lldms flush=%lu/%lu area=%lu,%lu %lux%lu stack_min=%luB lv_free=%luB largest=%luB",
        StageName(stage_.load()), static_cast<long long>((now - stage_at_.load()) / 1000),
        static_cast<unsigned long>(flushes_.load()), static_cast<unsigned long>(flush_started_.load()),
        static_cast<unsigned long>(xy >> 16), static_cast<unsigned long>(xy & 0xFFFF),
        static_cast<unsigned long>(wh >> 16), static_cast<unsigned long>(wh & 0xFFFF),
        static_cast<unsigned long>(ui_stack_min_.load()), static_cast<unsigned long>(lv_free_.load()), static_cast<unsigned long>(lv_largest_.load()));
    ESP_LOGD("Amoled175.ui", "text rx=%lu changed=%lu applied=%lu bytes=%lu status rx=%lu changed=%lu",
        static_cast<unsigned long>(text_received_.load()), static_cast<unsigned long>(text_changed_.load()),
        static_cast<unsigned long>(text_applied_.load()), static_cast<unsigned long>(text_bytes_.load()),
        static_cast<unsigned long>(status_received_.load()), static_cast<unsigned long>(status_changed_.load()));
    ESP_LOGD("Amoled175.ui", "panel errors=%lu recoveries=%lu", (unsigned long)panel_errors_.load(), (unsigned long)panel_recoveries_.load());
    ESP_LOGD("Amoled175.ui", "input polls=%lu errors=%lu max_gap=%lums volume_events=%lu commits=%lu",
        static_cast<unsigned long>(touch_polls), static_cast<unsigned long>(touch_errors),
        static_cast<unsigned long>(max_touch_gap_ms), static_cast<unsigned long>(volume_events),
        static_cast<unsigned long>(volume_commits));
    if (heartbeat && now - heartbeat > 2000000)
        ESP_LOGW("Amoled175.ui", "render task stalled at %s; retain this log and matching ELF", StageName(stage_.load()));
}
}  // namespace amoled_ui
