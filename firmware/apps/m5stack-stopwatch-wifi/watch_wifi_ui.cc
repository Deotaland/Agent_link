#include "watch_wifi_ui.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace watch_wifi {
namespace {
constexpr size_t kBufferBytes = DISPLAY_WIDTH * UI_DRAW_BUF_ROWS * 2;
Co5300Panel* panel_ = nullptr;
portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
Screen screen_;
agent_link_status_t status_ = {};
char answer_[1024] = {};
char toast_[96] = {};
int battery_ = -1;
bool charging_ = false;
int64_t toast_until_ = 0;
std::atomic<uint32_t> revision_{1};
std::atomic<uint32_t> wake_{0};

lv_obj_t *heading, *battery, *title, *detail, *footer, *code;
lv_obj_t *rows[3], *row_labels[3], *answer_view, *answer, *toast;

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

void Build() {
    auto* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    heading = Label(root, 60, 260, &lv_font_montserrat_20, 0x52CCB8);
    battery = Label(root, 88, 260, &lv_font_montserrat_14, 0x8898AD);
    title = Label(root, 122, 336, &lv_font_montserrat_24, 0xF4F8FC);
    detail = Label(root, 160, 336, BodyFont(), 0xA4B3C4);
    code = Label(root, 208, 320, &lv_font_montserrat_48, 0x52CCB8);
    for (int i = 0; i < 3; ++i) {
        rows[i] = lv_obj_create(root);
        lv_obj_set_size(rows[i], 332, 42);
        lv_obj_align(rows[i], LV_ALIGN_TOP_MID, 0, 196 + i * 48);
        lv_obj_set_style_border_width(rows[i], 0, 0);
        lv_obj_set_style_pad_all(rows[i], 0, 0);
        lv_obj_set_style_radius(rows[i], 14, 0);
        lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_SCROLLABLE);
        row_labels[i] = Label(rows[i], 12, 310, BodyFont(), 0xF4F8FC);
    }
    answer_view = lv_obj_create(root);
    lv_obj_remove_style_all(answer_view);
    lv_obj_set_size(answer_view, 332, 136);
    lv_obj_align(answer_view, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_set_scrollbar_mode(answer_view, LV_SCROLLBAR_MODE_OFF);
    answer = Label(answer_view, 0, 328, BodyFont(), 0xE6EDF5);
    lv_label_set_long_mode(answer, LV_LABEL_LONG_WRAP);
    footer = Label(root, 358, 310, &lv_font_montserrat_14, 0xA4B3C4);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_WRAP);
    toast = Label(root, 407, 246, &lv_font_montserrat_14, 0xFFCF7D);
}

void Scroll(void* object, int32_t y) {
    lv_obj_scroll_to_y(static_cast<lv_obj_t*>(object), y, LV_ANIM_OFF);
}

void Render(void*) {
    Build();
    uint32_t seen = 0, seen_wake = 0;
    bool toast_visible = false, dim = false;
    int64_t activity = esp_timer_get_time();
    static char shown_answer[sizeof answer_] = {};
    static char text[sizeof answer_];
    while (true) {
        const int64_t now = esp_timer_get_time();
        const uint32_t current = revision_.load(std::memory_order_acquire);
        if (current != seen) {
            seen = current;
            Screen screen;
            agent_link_status_t status;
            char notification[sizeof toast_];
            int pct;
            bool chg;
            int64_t toast_until;
            taskENTER_CRITICAL(&lock_);
            screen = screen_; status = status_; pct = battery_; chg = charging_;
            memcpy(text, answer_, sizeof text);
            memcpy(notification, toast_, sizeof notification);
            toast_until = toast_until_;
            taskEXIT_CRITICAL(&lock_);
            const bool link = !screen.menu && status.phase != AGENT_LINK_PHASE_CONNECTED &&
                              status.phase != AGENT_LINK_PHASE_READY;
            lv_label_set_text(heading, "STOPWATCH / WIFI");
            char charge[48];
            if (pct >= 0) snprintf(charge, sizeof charge, "%s%d%%", chg ? "Charging  " : "Battery  ", pct);
            else snprintf(charge, sizeof charge, "Battery --");
            lv_label_set_text(battery, charge);
            lv_label_set_text(title, link ? status.title : screen.title);
            lv_label_set_long_mode(detail, link ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
            lv_label_set_text(detail, link ? status.hint : screen.detail);
            lv_obj_set_height(detail, link ? 72 : LV_SIZE_CONTENT);
            lv_label_set_text(code, link ? status.code : "");
            lv_obj_align(code, LV_ALIGN_TOP_MID, 0, 252);
            Visible(code, link && status.code[0]);
            for (int i = 0; i < 3; ++i) {
                Visible(rows[i], screen.menu && screen.rows[i][0]);
                lv_label_set_text(row_labels[i], screen.rows[i]);
                lv_obj_set_style_bg_color(rows[i], lv_color_hex(i == screen.selected ? 0x17675C : 0x142029), 0);
            }
            Visible(answer_view, !screen.menu && !link);
            if (strcmp(shown_answer, text) != 0) {
                snprintf(shown_answer, sizeof shown_answer, "%s", text);
                lv_anim_delete(answer_view, Scroll);
                lv_label_set_text(answer, text);
                lv_obj_scroll_to_y(answer_view, 0, LV_ANIM_OFF);
                lv_obj_update_layout(answer_view);
                const int overflow = lv_obj_get_height(answer) - lv_obj_get_height(answer_view);
                if (overflow > 0) {
                    lv_anim_t animation;
                    lv_anim_init(&animation);
                    lv_anim_set_var(&animation, answer_view);
                    lv_anim_set_exec_cb(&animation, Scroll);
                    lv_anim_set_values(&animation, 0, overflow);
                    lv_anim_set_delay(&animation, 2000);
                    lv_anim_set_duration(&animation, overflow * 1000 / 25);
                    lv_anim_start(&animation);
                }
            }
            lv_label_set_text(footer, screen.footer);
            toast_visible = toast_until > now;
            lv_label_set_text(toast, notification);
            Visible(toast, toast_visible);
            activity = now;
        }
        if (toast_visible) {
            taskENTER_CRITICAL(&lock_);
            const bool expired = now >= toast_until_;
            taskEXIT_CRITICAL(&lock_);
            if (expired) { Visible(toast, false); toast_visible = false; }
        }
        const uint32_t awake = wake_.load();
        if (seen_wake != awake) { seen_wake = awake; activity = now; }
        const bool want_dim = agent_link_state() == AGENT_STATE_CONNECTED && now - activity > 30000000;
        if (dim != want_dim) {
            (void)panel_->SetBrightness(want_dim ? DISPLAY_BRIGHTNESS_DIM : DISPLAY_BRIGHTNESS);
            dim = want_dim;
        }
        uint32_t delay = lv_timer_handler();
        if (delay == LV_NO_TIMER_READY || delay > 30) delay = 30;
        vTaskDelay(pdMS_TO_TICKS(delay < 10 ? 10 : delay));
    }
}
}  // namespace

esp_err_t StartUi(Co5300Panel* panel) {
    if (!panel || !panel->Ready()) return ESP_ERR_INVALID_STATE;
    panel_ = panel;
    auto* pixels = static_cast<uint8_t*>(heap_caps_aligned_alloc(4, kBufferBytes, MALLOC_CAP_SPIRAM));
    if (!pixels) return ESP_ERR_NO_MEM;
    lv_init();
    lv_tick_set_cb(Tick);
    auto* display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!display) { heap_caps_free(pixels); return ESP_ERR_NO_MEM; }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, Flush);
    lv_display_add_event_cb(display, Round, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_set_buffers(display, pixels, nullptr, kBufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    if (xTaskCreatePinnedToCore(Render, "sw_wifi_ui", 8192, nullptr, 4, nullptr,
                                portNUM_PROCESSORS - 1) != pdPASS) {
        lv_display_delete(display); heap_caps_free(pixels); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void SetScreen(const Screen& screen) {
    taskENTER_CRITICAL(&lock_);
    const bool changed = memcmp(&screen_, &screen, sizeof screen) != 0;
    screen_ = screen;
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
    battery_ = percent; charging_ = charging;
    taskEXIT_CRITICAL(&lock_);
    if (changed) revision_.fetch_add(1, std::memory_order_release);
}
void SetAnswer(const char* text) {
    taskENTER_CRITICAL(&lock_);
    snprintf(answer_, sizeof answer_, "%s", text ? text : "");
    taskEXIT_CRITICAL(&lock_);
    revision_.fetch_add(1, std::memory_order_release);
}
void Toast(const char* text, uint32_t ms) {
    taskENTER_CRITICAL(&lock_);
    snprintf(toast_, sizeof toast_, "%s", text ? text : "");
    toast_until_ = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
    taskEXIT_CRITICAL(&lock_);
    revision_.fetch_add(1, std::memory_order_release);
}
void Wake() { wake_.fetch_add(1); }
}  // namespace watch_wifi
