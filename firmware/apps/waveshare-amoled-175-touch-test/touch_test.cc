#include "config.h"
#include "amoled_hardware.h"
#include "amoled_fonts.h"
#include "touch_test_state.h"
#include <cstdio>
#include <cstring>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace {
using namespace amoled_touch_test;
constexpr const char* TAG = "Amoled175.touchtest";
constexpr const char* kModel = "ESP32-S3-Touch-AMOLED-1.75";
constexpr unsigned kTrailSize = 64;
AmoledHardware hardware;
AmoledFonts fonts;
Monitor monitor;
lv_obj_t *ring, *content, *coordinates, *state_text, *raw_text, *hint, *stats;
lv_obj_t *target[kTargetCount], *trail[kTrailSize], *cross_x, *cross_y, *dot;
lv_indev_t* pointer = nullptr;
unsigned trail_cursor = 0;
PointerGate pointer_gate;

uint32_t Tick() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
void Round(lv_event_t* event) {
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
    area->x1 &= ~1; area->y1 &= ~1; area->x2 |= 1; area->y2 |= 1;
}
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    const int w = lv_area_get_width(area), h = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(pixels, w * h);
    const esp_err_t err = hardware.panel_.DrawBitmap(area->x1, area->y1, w, h, pixels);
    if (err != ESP_OK) ESP_LOGE(TAG, "display transfer: %s", esp_err_to_name(err));
    lv_display_flush_ready(display);
}
void Show(lv_obj_t* object, bool visible) {
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void SetText(lv_obj_t* label, const char* text) {
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}
void Place(lv_obj_t* object, int x, int y, int width, int height) {
    lv_obj_set_pos(object, x - kContentInset, y - kContentInset);
    lv_obj_set_size(object, width, height);
}
lv_obj_t* Label(int y, int width, int height, const lv_font_t* font, const char* text) {
    auto* label = lv_label_create(content);
    Place(label, kCenter - width / 2, y, width, height);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    return label;
}
lv_obj_t* Shape(int width, int height, uint32_t color) {
    auto* shape = lv_obj_create(content);
    lv_obj_remove_style_all(shape);
    lv_obj_set_size(shape, width, height);
    lv_obj_set_style_bg_color(shape, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_remove_flag(shape, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(shape, LV_OBJ_FLAG_SCROLLABLE);
    return shape;
}
void Clear(lv_event_t*) {
    monitor.Reset();
    trail_cursor = 0;
    for (auto* point : trail) Show(point, false);
    ESP_LOGI(TAG, "RESET: counters, targets and trail cleared by touchscreen");
}
void Build() {
    auto* root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    ring = lv_obj_create(root);
    lv_obj_remove_style_all(ring);
    lv_obj_set_pos(ring, kRingInset, kRingInset);
    lv_obj_set_size(ring, kRingSize, kRingSize);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, kRingWidth, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x4B8DFF), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    content = lv_obj_create(root);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, kContentInset, kContentInset);
    lv_obj_set_size(content, kContentSize, kContentSize);
    lv_obj_set_style_radius(content, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(content, true, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    Label(48, 180, 28, fonts.Heading(), "触控测试");
    Label(82, 240, 20, fonts.Body(), fonts.Ready() ? "中文字库：已加载" : "字库未加载 / CJK missing");
    coordinates = Label(108, 300, 30, fonts.Title(), "屏幕 X:--- Y:---");
    state_text = Label(140, 316, 20, fonts.Body(), "等待触控数据");
    raw_text = Label(160, 336, 18, fonts.Body(), "D000: --");
    for (unsigned i = 0; i < kTargetCount; ++i) {
        target[i] = Shape(2 * kTargetRadius, 2 * kTargetRadius, 0x101925);
        const auto& t = kTargets[i];
        Place(target[i], t.x - kTargetRadius, t.y - kTargetRadius, 2 * kTargetRadius, 2 * kTargetRadius);
        lv_obj_set_style_radius(target[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(target[i], 2, 0);
        lv_obj_set_style_border_color(target[i], lv_color_hex(0x425067), 0);
        auto* number = lv_label_create(target[i]);
        lv_obj_set_style_text_font(number, fonts.Body(), 0);
        lv_obj_set_style_text_color(number, lv_color_hex(0xEAF2FF), 0);
        lv_label_set_text_fmt(number, "%u", i + 1); lv_obj_center(number);
    }
    for (auto*& point : trail) {
        point = Shape(5, 5, 0x227C79);
        lv_obj_set_style_radius(point, LV_RADIUS_CIRCLE, 0); Show(point, false);
    }
    cross_x = Shape(28, 2, 0x34DDD2);
    cross_y = Shape(2, 28, 0x34DDD2);
    dot = Shape(8, 8, 0xFFFFFF); lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    Show(cross_x, false); Show(cross_y, false); Show(dot, false);
    hint = Label(343, 280, 22, fonts.Body(), "请按顺序点击五个圆靶");
    stats = Label(367, 240, 18, fonts.Body(), "按下:0 松开:0 错误:0");
    auto* clear = lv_button_create(content);
    lv_obj_remove_style_all(clear);
    Place(clear, 163, 390, 140, 30);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0x1D3944), 0);
    lv_obj_set_style_bg_opa(clear, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(clear, 12, 0);
    lv_obj_add_flag(clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(clear, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clear, Clear, LV_EVENT_CLICKED, nullptr);
    auto* caption = lv_label_create(clear);
    lv_obj_set_style_text_font(caption, fonts.Body(), 0);
    lv_obj_set_style_text_color(caption, lv_color_white(), 0);
    lv_label_set_text(caption, "清空重测"); lv_obj_center(caption);
}
void ReadTouch(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = monitor.X(); data->point.y = monitor.Y();
    data->state = pointer_gate.Down(monitor.Pressed()) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
void Paint(const uint8_t (&raw)[10], esp_err_t error, int64_t now) {
    static uint32_t painted_color = 0;
    static unsigned painted_target = kTargetCount + 1;
    const bool stale = monitor.Stale(now);
    if (pointer_gate.Update(stale, monitor.Pressed(), error == ESP_OK)) lv_indev_reset(pointer, nullptr);
    const bool down = monitor.Pressed() && !stale;
    const uint32_t color = stale ? 0xFF626F : monitor.Complete() ? 0x52D68A : down ? 0x34DDD2 : 0x4B8DFF;
    if (painted_color != color) { painted_color = color; lv_obj_set_style_border_color(ring, lv_color_hex(color), 0); }
    char text[160];
    snprintf(text, sizeof text, "屏幕 X:%03u Y:%03u", monitor.X(), monitor.Y());
    SetText(coordinates, text);
    snprintf(text, sizeof text, "%s  IRQ:%d  有效:%lu", stale ? "数据中断" : down ? "按下 / 移动" : "已松开",
             gpio_get_level(TOUCH_INT_PIN), static_cast<unsigned long>(monitor.Valid()));
    SetText(state_text, text);
    if (error == ESP_OK || error == ESP_ERR_INVALID_RESPONSE)
        snprintf(text, sizeof text, "D000: %02X %02X %02X %02X %02X %02X %02X",
                 raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]);
    else snprintf(text, sizeof text, "I2C: %s", esp_err_to_name(error));
    SetText(raw_text, text);
    if (monitor.Complete()) SetText(hint, "五点点击完成！请继续滑动检查边缘");
    else {
        snprintf(text, sizeof text, "请松手后点击：%s（%u/5）", kTargets[monitor.NextTarget()].name, monitor.NextTarget() + 1);
        SetText(hint, text);
    }
    if (painted_target != monitor.NextTarget()) {
        painted_target = monitor.NextTarget();
        for (unsigned i = 0; i < kTargetCount; ++i)
            lv_obj_set_style_border_color(target[i], lv_color_hex(i < painted_target ? 0x52D68A :
                                          i == painted_target ? 0x34DDD2 : 0x425067), 0);
    }
    snprintf(text, sizeof text, "按下:%lu 松开:%lu 错误:%lu/%lu", static_cast<unsigned long>(monitor.Downs()),
             static_cast<unsigned long>(monitor.Ups()), static_cast<unsigned long>(monitor.Invalid()),
             static_cast<unsigned long>(monitor.BusErrors()));
    SetText(stats, text);
    Show(cross_x, down); Show(cross_y, down); Show(dot, down);
    if (down) {
        const int x = monitor.X(), y = monitor.Y();
        Place(cross_x, x - 14, y - 1, 28, 2);
        Place(cross_y, x - 1, y - 14, 2, 28);
        Place(dot, x - 4, y - 4, 8, 8);
    }
}
}  // namespace

extern "C" void app_main() {
    ESP_LOGI(TAG, "board = Amoled175, model = %s, profile = touch-test, fw = %s",
             kModel, esp_app_get_description()->version);
    ESP_LOGI(TAG, "Offline test: no WiFi, BLE, audio, activation or NVS writes");
    hardware.InitDisplayAndTouch();
    if (!hardware.panel_.Ready()) { ESP_LOGE(TAG, "display init failed; test cannot draw"); return; }
    const size_t bytes = DISPLAY_WIDTH * UI_DRAW_BUF_ROWS * 2;
    auto* buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_SPIRAM));
    if (!buffer) { ESP_LOGE(TAG, "display buffer allocation failed"); return; }
    lv_init(); lv_tick_set_cb(Tick); fonts.Init();
    auto* display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!display) { heap_caps_free(buffer); ESP_LOGE(TAG, "LVGL display unavailable"); return; }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, Flush);
    lv_display_add_event_cb(display, Round, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_set_buffers(display, buffer, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    pointer = lv_indev_create();
    if (!pointer) { lv_display_delete(display); heap_caps_free(buffer); ESP_LOGE(TAG, "LVGL pointer unavailable"); return; }
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display); lv_indev_set_read_cb(pointer, ReadTouch);
    Build();
    (void)hardware.panel_.SetBrightness(DISPLAY_BRIGHTNESS);
    ESP_LOGI(TAG, "touch init = %s; Chinese font = %s; SDA=15 SCL=14 RST=40 IRQ=11 address=0x5A",
             hardware.touch_.Ready() ? "OK" : "FAILED", fonts.Ready() ? "OK" : "MISSING");
    ESP_LOGI(TAG, "touch orientation: swap_xy=0 mirror_x=%d mirror_y=%d; screen=(465-raw_x,465-raw_y)",
             TOUCH_MIRROR_X, TOUCH_MIRROR_Y);
    int64_t report_at = 0, move_at = 0;
    while (true) {
        const int64_t now = esp_timer_get_time() / 1000;
        cst9217::Point point;
        uint8_t raw[10] = {};
        const esp_err_t error = hardware.touch_.Poll(&point, raw);
        const Read result = error == ESP_OK ? Read::kValid :
                            error == ESP_ERR_INVALID_RESPONSE ? Read::kInvalidReport : Read::kBusError;
        const Event event = monitor.Observe(result, point, now);
        if (event == Event::kDown || event == Event::kMove) {
            auto* trace = trail[trail_cursor++ % kTrailSize];
            Place(trace, static_cast<int>(monitor.X()) - 2, static_cast<int>(monitor.Y()) - 2, 5, 5); Show(trace, true);
        }
        if (event == Event::kDown || event == Event::kUp || (event == Event::kMove && now >= move_at)) {
            cst9217::Point original;
            const bool contact = error == ESP_OK && cst9217::Decode(raw, sizeof raw, DISPLAY_WIDTH, DISPLAY_HEIGHT, &original) && original.pressed;
            ESP_LOGI(TAG, "%s screen_x=%u screen_y=%u raw_x=%d raw_y=%d targets=%u/5",
                     event == Event::kDown ? "DOWN" : event == Event::kUp ? "UP" : "MOVE",
                     monitor.X(), monitor.Y(), contact ? original.x : -1, contact ? original.y : -1, monitor.NextTarget());
            move_at = now + 200;
        }
        if (now >= report_at) {
            ESP_LOGI(TAG, "valid=%lu invalid=%lu i2c_errors=%lu down=%lu up=%lu move=%lu irq=%d result=%s raw=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                     static_cast<unsigned long>(monitor.Valid()), static_cast<unsigned long>(monitor.Invalid()),
                     static_cast<unsigned long>(monitor.BusErrors()), static_cast<unsigned long>(monitor.Downs()),
                     static_cast<unsigned long>(monitor.Ups()), static_cast<unsigned long>(monitor.Moves()),
                     gpio_get_level(TOUCH_INT_PIN), esp_err_to_name(error), raw[0], raw[1], raw[2], raw[3], raw[4],
                     raw[5], raw[6], raw[7], raw[8], raw[9]);
            report_at = now + 1000;
        }
        Paint(raw, error, now);
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
