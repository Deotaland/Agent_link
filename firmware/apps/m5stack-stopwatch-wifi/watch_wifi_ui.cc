#include "watch_wifi_ui.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace watch_wifi {
namespace {
constexpr size_t kBufferBytes = DISPLAY_WIDTH * UI_DRAW_BUF_ROWS * 2;
Co5300Panel* panel_ = nullptr;
Cst820Touch* touch_ = nullptr;
InputHandler handler_ = nullptr;
void* context_ = nullptr;
portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
Screen screen_;
agent_link_status_t status_ = {};
char answer_[1024] = {}, toast_[96] = {};
int battery_ = -1, volume_ = AUDIO_DEFAULT_VOLUME;
bool charging_ = false;
int64_t toast_until_ = 0;
std::atomic<uint32_t> revision_{1}, wake_{0};

lv_obj_t *heading, *battery, *title, *detail, *footer, *code;
lv_obj_t *rows[3], *row_labels[3], *home[3], *home_labels[3], *nav[3];
lv_obj_t *answer_view, *answer, *toast;
lv_indev_t* pointer_ = nullptr;
cst820::Point point_;
bool swallow_ = false, pending_ = false;
uint32_t shown_view_ = 0, pressed_view_ = 0;

void Round(lv_event_t* event) {
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
    area->x1 &= ~1; area->y1 &= ~1;
    area->x2 |= 1; area->y2 |= 1;
}
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    const int w = lv_area_get_width(area), h = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(pixels, w * h);
    (void)panel_->DrawBitmap(area->x1, area->y1, w, h, pixels);
    lv_display_flush_ready(display);
}
uint32_t Tick() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
const lv_font_t* BodyFont() {
#if LV_FONT_SIMSUN_14_CJK
    return &lv_font_simsun_14_cjk;
#else
    return &lv_font_montserrat_14;
#endif
}
lv_obj_t* Label(lv_obj_t* parent, int y, int width, const lv_font_t* font, uint32_t color) {
    auto* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, "");
    return label;
}
void Visible(lv_obj_t* object, bool visible) {
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void Enabled(lv_obj_t* object, bool enabled) {
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}
void Click(lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) { pressed_view_ = shown_view_; return; }
    if (pending_ || pressed_view_ != shown_view_ || !handler_) return;
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    Input input{};
    input.view = shown_view_; input.at = esp_timer_get_time();
    if (id < 3) {
        input.action = id == 0 ? TouchAction::kCall : id == 1 ? TouchAction::kSessions : TouchAction::kSettings;
    } else if (id < 6) { input.action = TouchAction::kRow; input.row = id - 3; }
    else input.action = id == 6 ? TouchAction::kBack : id == 7 ? TouchAction::kPrevious : TouchAction::kNext;
    if (handler_(input, context_)) {
        pending_ = true;
        for (int i = 0; i < 3; ++i) { Enabled(rows[i], false); Enabled(home[i], false); Enabled(nav[i], false); }
    }
}
lv_obj_t* Button(lv_obj_t* root, int x, int y, int width, int height, int id) {
    auto* button = lv_button_create(root);
    lv_obj_set_size(button, width, height);
    lv_obj_align(button, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x17675C), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x142029), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, Click, LV_EVENT_PRESSED, reinterpret_cast<void*>(static_cast<intptr_t>(id)));
    lv_obj_add_event_cb(button, Click, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(id)));
    return button;
}
void Build() {
    auto* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    heading = Label(root, 48, 260, &lv_font_montserrat_20, 0x52CCB8);
    battery = Label(root, 76, 300, &lv_font_montserrat_14, 0x8898AD);
    title = Label(root, 108, 336, &lv_font_montserrat_24, 0xF4F8FC);
    detail = Label(root, 146, 336, BodyFont(), 0xA4B3C4);
    code = Label(root, 252, 320, &lv_font_montserrat_48, 0x52CCB8);
    for (int i = 0; i < 3; ++i) {
        rows[i] = Button(root, 0, 204 + i * 56, 332, 50, 3 + i);
        row_labels[i] = Label(rows[i], 17, 310, BodyFont(), 0xF4F8FC);
        home[i] = Button(root, (i - 1) * 96, 350, 90, 56, i);
        home_labels[i] = Label(home[i], 10, 86, &lv_font_montserrat_14, 0xF4F8FC);
        lv_label_set_long_mode(home_labels[i], LV_LABEL_LONG_WRAP);
        nav[i] = Button(root, (i - 1) * 88, 382, 80, 44, 6 + i);
        auto* label = Label(nav[i], 14, 78, &lv_font_montserrat_14, 0xF4F8FC);
        lv_label_set_text(label, i == 0 ? "Back" : i == 1 ? "Prev" : "Next");
    }
    answer_view = lv_obj_create(root);
    lv_obj_remove_style_all(answer_view);
    lv_obj_set_size(answer_view, 332, 144);
    lv_obj_align(answer_view, LV_ALIGN_TOP_MID, 0, 184);
    lv_obj_set_scrollbar_mode(answer_view, LV_SCROLLBAR_MODE_AUTO);
    answer = Label(answer_view, 0, 328, BodyFont(), 0xE6EDF5);
    lv_label_set_long_mode(answer, LV_LABEL_LONG_WRAP);
    footer = Label(root, 367, 280, &lv_font_montserrat_14, 0xA4B3C4);
    toast = Label(root, 432, 160, &lv_font_montserrat_14, 0xFFCF7D);
    // Bottom-most notification is within the circular glass; it never covers buttons.
    lv_obj_set_style_text_font(toast, &lv_font_montserrat_14, 0);
}
void ReadTouch(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = point_.x; data->point.y = point_.y;
    data->state = point_.pressed && !swallow_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
void Render(void*) {
    Build();
    uint32_t seen = 0, seen_wake = 0;
    bool toast_visible = false, dim = false;
    int64_t activity = esp_timer_get_time();
    static char shown_answer[sizeof answer_] = {}, text[sizeof answer_];
    while (true) {
        const int64_t now = esp_timer_get_time();
        if (pointer_) {
            cst820::Point point;
            const esp_err_t error = touch_->Poll(&point);
            if (error != ESP_OK) {
                // Cancel the gesture instead of turning a lost frame into a click.
                lv_indev_reset(pointer_, nullptr); swallow_ = true; point_.pressed = false;
            } else {
                if (point.pressed) {
                    if (dim && !point_.pressed) swallow_ = true;
                    activity = now; point_ = point;
                } else { point_.pressed = false; }
                // Keep the failed/wake gesture cancelled through its release sample.
                if (!point.pressed && swallow_) { lv_indev_reset(pointer_, nullptr); swallow_ = false; }
            }
        }
        const uint32_t current = revision_.load(std::memory_order_acquire);
        if (current != seen) {
            seen = current;
            Screen screen;
            agent_link_status_t status;
            char notification[sizeof toast_];
            int pct, volume; bool chg; int64_t toast_until;
            taskENTER_CRITICAL(&lock_);
            screen = screen_; status = status_; pct = battery_; volume = volume_; chg = charging_;
            memcpy(text, answer_, sizeof text); memcpy(notification, toast_, sizeof notification);
            toast_until = toast_until_;
            taskEXIT_CRITICAL(&lock_);
            if (shown_view_ != screen.view) {
                shown_view_ = screen.view; pending_ = false;
                if (pointer_) { lv_indev_reset(pointer_, nullptr); if (point_.pressed) swallow_ = true; }
            }
            const bool link = !screen.menu && status.phase != AGENT_LINK_PHASE_CONNECTED &&
                              status.phase != AGENT_LINK_PHASE_READY;
            lv_label_set_text(heading, "STOPWATCH / WIFI");
            char charge[64];
            if (pct >= 0) snprintf(charge, sizeof charge, "%s%d%%    Vol %d%%", chg ? "Charging " : "Battery ", pct, volume);
            else snprintf(charge, sizeof charge, "Battery --    Vol %d%%", volume);
            lv_label_set_text(battery, charge);
            lv_label_set_text(title, link ? status.title : screen.title);
            lv_label_set_long_mode(detail, link || screen.menu ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
            lv_label_set_text(detail, link ? status.hint : screen.detail);
            lv_obj_set_height(detail, link ? 88 : screen.menu ? 48 : LV_SIZE_CONTENT);
            lv_label_set_text(code, link ? status.code : "");
            Visible(code, link && status.code[0]);
            const bool available = !screen.busy && !pending_ && pointer_;
            for (int i = 0; i < 3; ++i) {
                Visible(rows[i], screen.menu && screen.rows[i][0]);
                lv_label_set_text(row_labels[i], screen.rows[i]);
                Enabled(rows[i], available);
                Visible(home[i], !screen.menu);
                Enabled(home[i], available && (i == 2 || (i == 0 ? screen.calling || screen.can_call : screen.can_sessions)));
                Visible(nav[i], screen.menu);
                Enabled(nav[i], available && (i == 0 || (i == 1 ? screen.previous : screen.next)));
            }
            lv_label_set_text(home_labels[0], screen.calling ? "End\ncall" : "Start\ncall");
            lv_label_set_text(home_labels[1], "Sessions");
            lv_label_set_text(home_labels[2], "Settings");
            Visible(answer_view, !screen.menu && !link);
            if (strcmp(shown_answer, text) != 0) {
                snprintf(shown_answer, sizeof shown_answer, "%s", text);
                lv_label_set_text(answer, text);
                lv_obj_scroll_to_y(answer_view, 0, LV_ANIM_OFF);
            }
            lv_label_set_text(footer, screen.footer); Visible(footer, screen.menu);
            toast_visible = toast_until > now;
            lv_label_set_text(toast, notification); Visible(toast, toast_visible);
            // Status or volume changes wake the display; periodic identical screens do not.
            activity = now;
        }
        if (toast_visible) {
            taskENTER_CRITICAL(&lock_); const bool expired = now >= toast_until_; taskEXIT_CRITICAL(&lock_);
            if (expired) { Visible(toast, false); toast_visible = false; }
        }
        const uint32_t awake = wake_.load();
        if (seen_wake != awake) { seen_wake = awake; activity = now; }
        const bool want_dim = agent_link_state() == AGENT_STATE_CONNECTED && now - activity > 30000000;
        if (dim != want_dim) {
            (void)panel_->SetBrightness(want_dim ? DISPLAY_BRIGHTNESS_DIM : DISPLAY_BRIGHTNESS); dim = want_dim;
        }
        uint32_t delay = lv_timer_handler();
        if (delay == LV_NO_TIMER_READY || delay > 20) delay = 20;
        vTaskDelay(pdMS_TO_TICKS(delay < 10 ? 10 : delay));
    }
}
}  // namespace
esp_err_t StartUi(Co5300Panel* panel, Cst820Touch* touch, InputHandler handler, void* context) {
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
        lv_indev_set_display(pointer_, display);
        lv_indev_set_read_cb(pointer_, ReadTouch);
    }
    if (xTaskCreatePinnedToCore(Render, "sw_wifi_ui", 8192, nullptr, 4, nullptr,
                              portNUM_PROCESSORS - 1) != pdPASS) {
        if (pointer_) { lv_indev_delete(pointer_); pointer_ = nullptr; }
        lv_display_delete(display); heap_caps_free(pixels); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void SetScreen(const Screen& screen) {
    taskENTER_CRITICAL(&lock_);
    const bool changed = memcmp(&screen_, &screen, sizeof screen) != 0; screen_ = screen;
    taskEXIT_CRITICAL(&lock_);
    if (changed) revision_.fetch_add(1, std::memory_order_release);
}
void SetStatus(const agent_link_status_t& status) {
    taskENTER_CRITICAL(&lock_); status_ = status; taskEXIT_CRITICAL(&lock_);
    revision_.fetch_add(1, std::memory_order_release);
}
void SetBattery(int percent, bool charging) {
    taskENTER_CRITICAL(&lock_);
    const bool changed = percent != battery_ || charging != charging_;
    battery_ = percent; charging_ = charging; taskEXIT_CRITICAL(&lock_);
    if (changed) revision_.fetch_add(1, std::memory_order_release);
}
void SetVolume(int percent) {
    taskENTER_CRITICAL(&lock_); volume_ = percent; taskEXIT_CRITICAL(&lock_);
    revision_.fetch_add(1, std::memory_order_release);
}
void SetAnswer(const char* text) {
    taskENTER_CRITICAL(&lock_); snprintf(answer_, sizeof answer_, "%s", text ? text : ""); taskEXIT_CRITICAL(&lock_);
    revision_.fetch_add(1, std::memory_order_release);
}
void Toast(const char* text, uint32_t ms) {
    taskENTER_CRITICAL(&lock_);
    snprintf(toast_, sizeof toast_, "%s", text ? text : "");
    toast_until_ = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
    taskEXIT_CRITICAL(&lock_); revision_.fetch_add(1, std::memory_order_release);
}
void Wake() { wake_.fetch_add(1); }
}  // namespace watch_wifi
