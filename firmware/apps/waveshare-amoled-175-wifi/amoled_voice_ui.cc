#include "amoled_voice_ui.h"
#include "config.h"
#include "amoled_fonts.h"
#include "amoled_voice_controls.h"
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

namespace amoled_voice {
namespace {
constexpr size_t kBufferBytes = DISPLAY_WIDTH * UI_DRAW_BUF_ROWS * 2;
constexpr uint32_t kWhite = 0xF5F5F7, kMuted = 0x98989F, kBlue = 0x0A84FF;
AmoledFonts fonts_;
Co5300Panel* panel_ = nullptr;
Cst9217Touch* touch_ = nullptr;
InputHandler handler_ = nullptr;
void* context_ = nullptr;
portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
Screen screen_;
agent_link_status_t status_ = {};
char answer_[1536] = {}, toast_[128] = {};
int battery_ = -1, volume_ = AUDIO_DEFAULT_VOLUME;
bool charging_ = false;
int64_t toast_until_ = 0;
std::atomic<uint32_t> revision_{1}, wake_{0}, applied_{0}, flushes_{0}, max_flush_us_{0};
std::atomic<int64_t> heartbeat_{0};
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
lv_obj_t *ring_, *heading_, *battery_label_, *title_, *detail_, *back_, *footer_, *toast_label_;
lv_obj_t *list_, *list_info_, *list_code_, *list_hint_, *rows_[kListRows], *row_labels_[kListRows], *row_icons_[kListRows];
lv_obj_t *answer_view_, *answer_label_, *latest_, *volume_box_, *slider_, *volume_label_;
lv_indev_t* pointer_ = nullptr;
cst9217::Point point_;
bool swallow_ = false, pending_ = false, shown_busy_ = false, has_before_ = false, has_after_ = false;
bool list_gesture_ = false;
int contact_x_ = 0, contact_y_ = 0, list_direction_ = 0;
uint32_t shown_view_ = 0, pressed_view_ = 0;
View shown_kind_ = View::kHome;
SwipeTracker swipe_;

void Round(lv_event_t* event) {
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
    area->x1 &= ~1; area->y1 &= ~1; area->x2 |= 1; area->y2 |= 1;
}
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    const int64_t start = esp_timer_get_time();
    const int w = lv_area_get_width(area), h = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(pixels, w * h);
    const UiStage previous = stage_.load(); const int64_t previous_at = stage_at_.load();
    flush_xy_.store((static_cast<uint32_t>(area->x1) << 16) | area->y1);
    flush_wh_.store((static_cast<uint32_t>(w) << 16) | h);
    flush_started_.fetch_add(1); Mark(UiStage::kPanel);
    if (panel_->DrawBitmap(area->x1, area->y1, w, h, pixels) != ESP_OK)
        ESP_LOGE("Amoled175.ui", "display transfer failed");
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
}
void Text(lv_obj_t* label, const char* text) {
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
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
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x303034), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}
bool Dispatch(TouchAction action, int row = 0) {
    if (pending_ || shown_busy_ || !handler_) return false;
    Input input{}; input.action = action; input.row = row;
    input.view = shown_view_; input.at = esp_timer_get_time();
    if (!handler_(input, context_)) return false;
    pending_ = true;
    return true;
}
void Click(lv_event_t* event) {
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) { pressed_view_ = shown_view_; return; }
    if (pressed_view_ != shown_view_) return;
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
    auto* data = reinterpret_cast<void*>(static_cast<intptr_t>(id));
    lv_obj_add_event_cb(obj, Click, LV_EVENT_PRESSED, data);
    lv_obj_add_event_cb(obj, Click, LV_EVENT_CLICKED, data);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x38383C), LV_STATE_PRESSED);
}
void VolumeText(int value) {
    char text[48]; snprintf(text, sizeof text, Tr("音量  %d%%", "Volume  %d%%"), value);
    Text(volume_label_, text);
}
void VolumeEvent(lv_event_t* event) {
    const auto code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    const int value = lv_slider_get_value(slider_);
    VolumeText(value); SetVolume(value);
    Input input{}; input.action = TouchAction::kVolume; input.row = value;
    input.commit = code != LV_EVENT_VALUE_CHANGED;
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
    auto* root = lv_screen_active(); lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    ring_ = lv_obj_create(root); lv_obj_remove_style_all(ring_);
    lv_obj_set_pos(ring_, kRingInset, kRingInset); lv_obj_set_size(ring_, kRingDiameter, kRingDiameter);
    lv_obj_set_style_radius(ring_, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_border_width(ring_, kRingWidth, 0);
    lv_obj_set_style_border_color(ring_, lv_color_hex(ActivityColor(Activity::kOffline)), 0);
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
    auto* back_label = Label(back_, &lv_font_montserrat_20, kBlue, true);
    Text(back_label, LV_SYMBOL_LEFT); lv_obj_center(back_label);
    answer_view_ = Card(root); Position(answer_view_, kAnswer); ScrollStyle(answer_view_);
    lv_obj_set_style_pad_all(answer_view_, 12, 0);
    answer_label_ = Label(answer_view_, fonts_.Body(), kWhite);
    lv_obj_set_width(answer_label_, kAnswer.w - 32); lv_obj_set_style_text_line_space(answer_label_, 5, 0);
    list_ = lv_obj_create(root); lv_obj_remove_style_all(list_); Position(list_, kList); ScrollStyle(list_);
    lv_obj_add_event_cb(list_, ListScroll, LV_EVENT_SCROLL_END, nullptr);
    list_code_ = Label(list_, &lv_font_montserrat_48, kBlue, true);
    list_info_ = Label(list_, fonts_.Body(), kWhite);
    list_hint_ = Label(list_, fonts_.Body(), kMuted, true);
    for (int i = 0; i < kListRows; ++i) {
        rows_[i] = Card(list_, 18); BindClick(rows_[i], i);
        lv_obj_set_size(rows_[i], kList.w - 5, kRowHeight - 8);
        row_icons_[i] = Label(rows_[i], &lv_font_montserrat_20, kBlue, true);
        lv_obj_set_pos(row_icons_[i], 10, 17); lv_obj_set_size(row_icons_[i], 28, 26);
        row_labels_[i] = Label(rows_[i], fonts_.Body(), kWhite);
        lv_obj_set_pos(row_labels_[i], 48, 7); lv_obj_set_size(row_labels_[i], kList.w - 84, 46);
        lv_label_set_long_mode(row_labels_[i], LV_LABEL_LONG_DOT);
        auto* arrow = Label(rows_[i], &lv_font_montserrat_14, kMuted);
        Text(arrow, LV_SYMBOL_RIGHT); lv_obj_set_pos(arrow, kList.w - 27, 20);
    }
    volume_box_ = Card(root, 18); Position(volume_box_, kVolume);
    volume_label_ = Label(volume_box_, fonts_.Body(), kMuted, true);
    lv_obj_set_pos(volume_label_, 0, 0); lv_obj_set_size(volume_label_, kVolume.w, 25);
    slider_ = lv_slider_create(volume_box_); lv_obj_remove_style_all(slider_);
    lv_obj_set_pos(slider_, 30, 34); lv_obj_set_size(slider_, 200, 5);
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
    lv_obj_add_event_cb(slider_, VolumeEvent, LV_EVENT_ALL, nullptr);
    footer_ = Label(root, fonts_.Small(), kMuted, true); Position(footer_, kFooter);
    lv_label_set_long_mode(footer_, LV_LABEL_LONG_DOT);
    auto* handle = Card(root, 3); Position(handle, kHandle);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0xB8B8BD), 0); lv_obj_set_style_border_width(handle, 0, 0);
    latest_ = Card(root, 12); lv_obj_set_pos(latest_, 324, 286); lv_obj_set_size(latest_, 40, 32);
    BindClick(latest_, -2); auto* down = Label(latest_, &lv_font_montserrat_20, kBlue);
    Text(down, LV_SYMBOL_DOWN); lv_obj_center(down); Visible(latest_, false);
    toast_label_ = Label(root, fonts_.Body(), kWhite, true); Position(toast_label_, kToast);
    lv_obj_set_style_bg_color(toast_label_, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(toast_label_, LV_OPA_COVER, 0); lv_obj_set_style_radius(toast_label_, 18, 0);
    lv_obj_set_style_pad_all(toast_label_, 8, 0); Visible(toast_label_, false);
}
void ReadTouch(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = point_.x; data->point.y = point_.y;
    data->state = point_.pressed && !swallow_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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
    uint32_t ring_color = 0; unsigned ring_opacity = 0;
    // Large snapshots stay out of the render task stack.
    static int64_t memory_at = 0;
    static Screen screen;
    static char shown_answer[sizeof answer_] = {}, text[sizeof answer_];
    while (true) {
        const int64_t now = esp_timer_get_time(); heartbeat_.store(now);
        if (pointer_) {
            Mark(UiStage::kTouch);
            cst9217::Point next;
            if (touch_->Poll(&next) != ESP_OK || !Inside(next.x, next.y)) next.pressed = false;
            if (next.pressed) {
                activity_at = now;
                if (!point_.pressed) { contact_x_ = next.x; contact_y_ = next.y; list_direction_ = 0; }
                if (dim) { dim = false; swallow_ = true; Mark(UiStage::kBrightness); (void)panel_->SetBrightness(DISPLAY_BRIGHTNESS); }
                if (shown_kind_ != View::kHome && contact_y_ >= kList.y && contact_y_ < kList.y + kList.h &&
                    std::abs(next.y - contact_y_) > 12 && std::abs(next.y - contact_y_) > std::abs(next.x - contact_x_)) {
                    list_gesture_ = true; list_direction_ = next.y - contact_y_;
                }
            } else swallow_ = false;
            const bool volume_touch = !lv_obj_has_flag(volume_box_, LV_OBJ_FLAG_HIDDEN) &&
                contact_y_ >= kVolume.y - 10 && contact_y_ <= kVolume.y + kVolume.h + 10;
            const Swipe direction = swipe_.Update(next.pressed && !swallow_, next.x, next.y, shown_view_, now, volume_touch);
            bool sent = false;
            if (direction == Swipe::kHome) sent = Dispatch(TouchAction::kHome);
            else if (direction == Swipe::kLeft && shown_kind_ == View::kHome) sent = Dispatch(TouchAction::kMenu);
            else if (direction == Swipe::kRight && shown_kind_ != View::kHome) sent = Dispatch(TouchAction::kBack);
            if (sent) { lv_indev_wait_release(pointer_); list_gesture_ = false; }
            point_ = next;
        }
        if (seen_wake != wake_.load()) {
            seen_wake = wake_.load(); activity_at = now;
            if (dim) { dim = false; Mark(UiStage::kBrightness); (void)panel_->SetBrightness(DISPLAY_BRIGHTNESS); }
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
                shown_view_ = screen.view; pending_ = false;
                if (pointer_ && point_.pressed) lv_indev_wait_release(pointer_);
            }
            shown_kind_ = screen.kind; shown_busy_ = screen.busy;
            const bool home = screen.kind == View::kHome;
            const bool setup = home && (status.phase == AGENT_LINK_PHASE_SETUP ||
                status.phase == AGENT_LINK_PHASE_PAIRING || status.phase == AGENT_LINK_PHASE_BLOCKED);
            const bool volume_page = screen.volume_control;
            const char* state_text = home && screen.ended && state == Activity::kIdle ? Tr("通话已结束", "Call ended") : ActivityName(state);
            Text(heading_, state_text);
            lv_obj_set_style_text_color(heading_, lv_color_hex(ActivityColor(state)), 0);
            char info[80];
            if (battery >= 0) snprintf(info, sizeof info, "AGENT LINK   %d%%%s", battery, charging ? " +" : "");
            else snprintf(info, sizeof info, "AGENT LINK   USB");
            Text(battery_label_, info);
            const char* setup_title = status.phase == AGENT_LINK_PHASE_PAIRING ? Tr("激活设备", "Activate device") :
                status.phase == AGENT_LINK_PHASE_SETUP ? Tr("连接 WiFi", "Connect WiFi") : Tr("设备需要处理", "Needs attention");
            Text(title_, setup ? setup_title : screen.title);
            Position(detail_, kDetail);
            if (!home) { lv_obj_set_x(detail_, 129); lv_obj_set_width(detail_, 249); }
            Text(detail_, setup ? status.hint : screen.detail);
            Visible(back_, !home && !screen.busy);
            Visible(answer_view_, home && !setup);
            Visible(volume_box_, (home && !setup) || volume_page);
            if (!lv_slider_is_dragged(slider_)) { lv_slider_set_value(slider_, volume, LV_ANIM_OFF); VolumeText(volume); }
            Visible(list_, !home || setup); Visible(latest_, false);
            lv_obj_set_height(list_, volume_page ? kAnswer.h : kList.h);
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
            if (!home || setup) {
                Mark(UiStage::kList);
                const int old_y = lv_obj_get_scroll_y(list_);
                int y = 0;
                const char* code = setup ? status.code : screen.pair_code;
                const bool show_code = code[0] && (!setup || status.phase == AGENT_LINK_PHASE_PAIRING);
                Visible(list_code_, show_code);
                if (show_code) { Text(list_code_, code); lv_obj_set_pos(list_code_, 0, y); lv_obj_set_size(list_code_, kList.w - 6, 60); y += 68; }
                const char* description = setup ? status.hint : screen.info;
                Visible(list_info_, description[0]);
                if (description[0]) {
                    Text(list_info_, description); lv_obj_set_pos(list_info_, 8, y); lv_obj_set_width(list_info_, kList.w - 22);
                    lv_obj_set_height(list_info_, LV_SIZE_CONTENT); lv_obj_update_layout(list_info_); y += lv_obj_get_height(list_info_) + 18;
                }
                for (int i = 0; i < kListRows; ++i) {
                    const bool show = !setup && i < screen.row_count;
                    Visible(rows_[i], show);
                    if (!show) continue;
                    Text(row_labels_[i], screen.rows[i]); lv_obj_set_pos(rows_[i], 0, y); y += kRowHeight;
                    const char* icon = screen.kind == View::kLauncher ?
                        (i == 0 ? LV_SYMBOL_LIST : i == 1 ? LV_SYMBOL_SETTINGS : LV_SYMBOL_WIFI) : LV_SYMBOL_RIGHT;
                    Text(row_icons_[i], icon);
                    lv_obj_set_style_text_color(row_icons_[i], lv_color_hex(i % 3 == 0 ? kBlue : i % 3 == 1 ? 0xBF5AF2 : 0x30D158), 0);
                }
                has_before_ = screen.previous; has_after_ = screen.next;
                Visible(list_hint_, screen.next || screen.previous);
                if (screen.next || screen.previous) {
                    Text(list_hint_, screen.next ? Tr("继续上滑加载更多", "Scroll for more") : Tr("已到末尾", "End of list"));
                    lv_obj_set_pos(list_hint_, 0, y); lv_obj_set_size(list_hint_, kList.w - 6, 30);
                }
                Mark(UiStage::kListLayout); lv_obj_update_layout(list_);
                const int scroll_y = new_page ? 0 : WindowScroll(old_y, offset, screen.row_offset, kRowHeight);
                if (new_page || offset != screen.row_offset) lv_obj_scroll_to_y(list_, scroll_y, LV_ANIM_OFF);
                if (new_page) list_gesture_ = false;
            }
            navigation = screen.navigation; offset = screen.row_offset;
            Text(footer_, home ? screen.footer : Tr("底部上滑回通话", "Swipe up from bottom"));
            was_toast = message[0] && now < until; Visible(toast_label_, was_toast);
            if (was_toast) Text(toast_label_, message);
            applied_.store(revision);
        }
        if (keep_awake) activity_at = now;
        const bool pulse = state == Activity::kConnecting || state == Activity::kListening || state == Activity::kThinking || state == Activity::kSpeaking;
        const unsigned phase = (now / 100000) % 20;
        const unsigned opacity = pulse ? 170 + 8 * (phase < 10 ? phase : 20 - phase) : 255;
        const uint32_t color = ActivityColor(state);
        if (color != ring_color) { ring_color = color; lv_obj_set_style_border_color(ring_, lv_color_hex(color), 0); }
        if (opacity != ring_opacity) { ring_opacity = opacity; lv_obj_set_style_border_opa(ring_, opacity, 0); }
        if (!dim && now - activity_at > 60000000) { dim = true; Mark(UiStage::kBrightness); (void)panel_->SetBrightness(DISPLAY_BRIGHTNESS_DIM); }
        Mark(UiStage::kTimers); lv_timer_handler();
        if (now >= memory_at) {
            Mark(UiStage::kMemory);
            lv_mem_monitor_t memory{}; lv_mem_monitor(&memory);
            lv_free_.store(memory.free_size); lv_largest_.store(memory.free_biggest_size);
            ui_stack_min_.store(uxTaskGetStackHighWaterMark(nullptr));
            memory_at = now + 5000000;
        }
        Mark(UiStage::kIdle);
        vTaskDelay(pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : 1);
    }
}
}  // namespace

esp_err_t StartUi(Co5300Panel* panel, Cst9217Touch* touch, InputHandler handler, void* context) {
    if (!panel || !panel->Ready()) return ESP_ERR_INVALID_STATE;
    panel_ = panel; touch_ = touch; handler_ = handler; context_ = context;
    auto* pixels = static_cast<uint8_t*>(heap_caps_aligned_alloc(4, kBufferBytes, MALLOC_CAP_SPIRAM));
    if (!pixels) return ESP_ERR_NO_MEM;
    lv_init(); lv_tick_set_cb(Tick);
    auto* display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!display) { heap_caps_free(pixels); return ESP_ERR_NO_MEM; }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, Flush);
    lv_display_add_event_cb(display, Round, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_set_buffers(display, pixels, nullptr, kBufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    if (touch && touch->Ready()) {
        pointer_ = lv_indev_create();
        if (!pointer_) { lv_display_delete(display); heap_caps_free(pixels); return ESP_ERR_NO_MEM; }
        lv_indev_set_type(pointer_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(pointer_, display); lv_indev_set_read_cb(pointer_, ReadTouch);
    }
    Mark(UiStage::kBrightness); (void)panel_->SetBrightness(DISPLAY_BRIGHTNESS);
    if (xTaskCreatePinnedToCore(Render, "amoled_ui", 8192, nullptr, 4, nullptr, portNUM_PROCESSORS - 1) != pdPASS) {
        if (pointer_) { lv_indev_delete(pointer_); pointer_ = nullptr; }
        lv_display_delete(display); heap_caps_free(pixels); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
bool SameScreen(const Screen& a, const Screen& b) {
    if (a.kind != b.kind || a.busy != b.busy || a.calling != b.calling || a.local_mode != b.local_mode ||
        a.previous != b.previous || a.next != b.next || a.ended != b.ended || a.volume_control != b.volume_control ||
        a.row_count != b.row_count || a.row_offset != b.row_offset || a.navigation != b.navigation ||
        a.activity != b.activity || a.view != b.view || strcmp(a.pair_code, b.pair_code) ||
        strcmp(a.title, b.title) || strcmp(a.detail, b.detail) || strcmp(a.footer, b.footer) || strcmp(a.info, b.info)) return false;
    for (int i = 0; i < kListRows; ++i) if (strcmp(a.rows[i], b.rows[i])) return false;
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
    ESP_LOGI("Amoled175.ui", "render revision=%lu/%lu flushes=%lu max_flush=%luus heartbeat_age=%lldms",
        static_cast<unsigned long>(applied_.load()), static_cast<unsigned long>(revision_.load()),
        static_cast<unsigned long>(flushes_.load()), static_cast<unsigned long>(max_flush_us_.exchange(0)),
        static_cast<long long>(heartbeat ? (now - heartbeat) / 1000 : -1));
    const uint32_t xy = flush_xy_.load(), wh = flush_wh_.load();
    ESP_LOGI("Amoled175.ui", "stage=%s age=%lldms flush=%lu/%lu area=%lu,%lu %lux%lu stack_min=%luB lv_free=%luB largest=%luB",
        StageName(stage_.load()), static_cast<long long>((now - stage_at_.load()) / 1000),
        static_cast<unsigned long>(flushes_.load()), static_cast<unsigned long>(flush_started_.load()),
        static_cast<unsigned long>(xy >> 16), static_cast<unsigned long>(xy & 0xFFFF),
        static_cast<unsigned long>(wh >> 16), static_cast<unsigned long>(wh & 0xFFFF),
        static_cast<unsigned long>(ui_stack_min_.load()), static_cast<unsigned long>(lv_free_.load()), static_cast<unsigned long>(lv_largest_.load()));
    ESP_LOGI("Amoled175.ui", "text rx=%lu changed=%lu applied=%lu bytes=%lu status rx=%lu changed=%lu",
        static_cast<unsigned long>(text_received_.load()), static_cast<unsigned long>(text_changed_.load()),
        static_cast<unsigned long>(text_applied_.load()), static_cast<unsigned long>(text_bytes_.load()),
        static_cast<unsigned long>(status_received_.load()), static_cast<unsigned long>(status_changed_.load()));
    if (heartbeat && now - heartbeat > 2000000)
        ESP_LOGW("Amoled175.ui", "render task stalled at %s; retain this log and matching ELF", StageName(stage_.load()));
}
}  // namespace amoled_voice
