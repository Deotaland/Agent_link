#include "epaper_wifi_ui.h"
#include "cjk_font.h"
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace epaper {
esp_err_t Ui::Start(EpaperHardware* hardware) {
    hardware_ = hardware;
    if (!hardware_->DisplayReady()) return ESP_ERR_INVALID_STATE;
    frame_ = static_cast<uint8_t*>(heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 8, MALLOC_CAP_SPIRAM));
    if (!frame_) return ESP_ERR_NO_MEM;
    if (xTaskCreate(Entry, "ep397_screen", 16384, this, 2, nullptr) != pdPASS) {
        free(frame_); frame_ = nullptr; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void Ui::Status(const agent_link_status_t& value) { std::lock_guard<std::mutex> guard(lock_); status_ = value; }
void Ui::Answer(const char* text) {
    if (!text) return;
    std::lock_guard<std::mutex> guard(lock_);
    if (strcmp(answer_, text) == 0) return;
    snprintf(answer_, sizeof answer_, "%s", text); answer_at_ = esp_timer_get_time(); answer_done_ = false;
}
void Ui::EndAnswer() { std::lock_guard<std::mutex> guard(lock_); answer_done_ = true; }
void Ui::Publish(const Screen& value) { std::lock_guard<std::mutex> guard(lock_); screen_ = value; }
void Ui::Pixel(int x, int y) {
    if (x >= 0 && x < EPD_WIDTH && y >= 0 && y < EPD_HEIGHT)
        frame_[y * (EPD_WIDTH / 8) + x / 8] &= ~(0x80 >> (x % 8));
}
void Ui::Text(int x, int y, int max_y, const char* text, int scale) {
    const int left = x;
    while (*text && y + 30 * scale <= max_y) {
        uint32_t cp = static_cast<unsigned char>(*text++);
        if (cp >= 0xc2 && cp <= 0xf4) {
            int more = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            cp &= more == 1 ? 0x1f : more == 2 ? 0x0f : 0x07;
            for (int i = 0; i < more; ++i) {
                const uint8_t c = static_cast<uint8_t>(*text);
                if ((c & 0xc0) != 0x80) { cp = '?'; break; }
                cp = (cp << 6) | (c & 0x3f); ++text;
            }
        } else if (cp >= 0x80) cp = '?';
        if (cp == '\n') { x = left; y += 32 * scale; continue; }
        // Include general punctuation (curly quotes, dashes, ellipsis), not just Hanzi.
        const bool prefer_cjk = cp >= 0x2000 && cjk_font_;
        const lv_font_t* candidates[] = {
            prefer_cjk ? cjk_font_ : &lv_font_montserrat_20,
            prefer_cjk ? &lv_font_montserrat_20 : cjk_font_,
            &lv_font_simsun_14_cjk,
        };
        const lv_font_t* font = &lv_font_montserrat_20;
        lv_font_glyph_dsc_t glyph{};
        bool found = false;
        for (const lv_font_t* candidate : candidates) {
            if (!candidate) continue;
            glyph = {};
            if (lv_font_get_glyph_dsc(candidate, &glyph, cp, 0) && !glyph.is_placeholder) {
                font = candidate; found = true; break;
            }
        }
        if (!found) {
            glyph = {};
            (void)lv_font_get_glyph_dsc(font, &glyph, '?', 0);
        }
        const int zoom = font == &lv_font_simsun_14_cjk ? 2 * scale : scale;
        if (x + glyph.adv_w * zoom >= EPD_WIDTH - 20) { x = left; y += 32 * scale; }
        if (y + 30 * scale > max_y) break;
        if (glyph.box_w && glyph.box_h && glyph.box_w <= 64 && glyph.box_h <= 64) {
            alignas(16) uint8_t pixels[4096]{};
            lv_draw_buf_t buffer{};
            const uint32_t stride = lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
            if (lv_draw_buf_init(&buffer, glyph.box_w, glyph.box_h, LV_COLOR_FORMAT_A8,
                                 stride, pixels, sizeof pixels) == LV_RESULT_OK) {
                // TinyTTF returns its cached draw buffer; bitmap fonts use the supplied buffer.
                const auto* bitmap = static_cast<const lv_draw_buf_t*>(lv_font_get_glyph_bitmap(&glyph, &buffer));
                if (bitmap && bitmap->data) {
                    const int top = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
                    for (unsigned row = 0; row < glyph.box_h; ++row)
                        for (unsigned col = 0; col < glyph.box_w; ++col)
                            if (bitmap->data[row * bitmap->header.stride + col] >= 96)
                                for (int dy = 0; dy < zoom; ++dy)
                                    for (int dx = 0; dx < zoom; ++dx)
                                        Pixel(x + (glyph.ofs_x + static_cast<int>(col)) * zoom + dx, y + (top + static_cast<int>(row)) * zoom + dy);
                }
                lv_font_glyph_release_draw_data(&glyph);
            }
        }
        x += glyph.adv_w * zoom;
    }
}
void Ui::Loop() {
    lv_init();  // This profile uses LVGL fonts only; this worker owns every LVGL call.
    cjk_font_ = LoadCjkFont(CJK_FONT_PARTITION, CJK_FONT_PX, CJK_FONT_CACHE_GLYPHS);
    if (!cjk_font_) ESP_LOGW("WS397.ui", "Chinese font unavailable; using the built-in subset. Flash app and font together.");
    Screen screen{};
    agent_link_status_t status{};
    char answer[sizeof answer_]{}, previous[2048]{};
    int64_t completed = -10000000, retry = 0;
    unsigned refreshes = 0;
    while (true) {
        const int64_t now = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> guard(lock_);
            screen = screen_; status = status_;
            if (!answer_[0] || answer_done_ || now - answer_at_ >= 2000000)
                snprintf(answer, sizeof answer, "%s", answer_);
        }
        // Compare the same status line that is drawn. An activation code hides
        // the SDK's changing expiry hint, so polling alone must not refresh e-paper.
        char status_line[sizeof status.hint];
        if (status.code[0]) snprintf(status_line, sizeof status_line, "Activation code: %s", status.code);
        else snprintf(status_line, sizeof status_line, "%s", status.hint);
        char content[2048];
        snprintf(content, sizeof content, "%s|%s|%s|%s|%s|%d|%s|%s|%s|%s|%s|%s",
                 status.title, status_line, screen.title, screen.detail, screen.footer,
                 screen.selected, screen.rows[0], screen.rows[1], screen.rows[2], screen.rows[3],
                 screen.rows[4], screen.rows[5]);
        // Streamed answer updates settle before refresh and never block microphone or network workers.
        const bool changed = strcmp(content, previous) != 0 || (!screen.rows[0][0] && strcmp(answer, painted_answer_) != 0);
        if (changed && now >= retry && now - completed >= 2000000) {
            memset(frame_, 0xff, EPD_WIDTH * EPD_HEIGHT / 8);
            Text(20, 12, 60, "WavePaper 3.97 / WiFi");
            Text(20, 52, 85, status.title);
            Text(20, 86, 154, status_line);
            for (int x = 20; x < EPD_WIDTH - 20; ++x) Pixel(x, 157);
            Text(20, 166, 200, screen.title);
            Text(20, 202, 270, screen.detail);
            if (screen.rows[0][0]) {
                for (int i = 0; i < 6; ++i) {
                    if (screen.selected == i) Text(20, 276 + i * 28, 446, ">");
                    Text(50, 276 + i * 28, 446, screen.rows[i]);
                }
            } else Text(20, 276, 438, answer);
            Text(20, 446, 480, screen.footer);
            const esp_err_t error = hardware_->Display(frame_, refreshes % 16 == 0);
            completed = esp_timer_get_time();
            if (error == ESP_OK) {
                ++refreshes; snprintf(previous, sizeof previous, "%s", content);
                snprintf(painted_answer_, sizeof painted_answer_, "%s", answer);
                displayed_.store(screen.view);
            } else {
                ESP_LOGW("WS397.ui", "Refresh failed: %s", esp_err_to_name(error));
                retry = completed + 30000000;
            }
        } else if (!changed && now >= retry) displayed_.store(screen.view);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}
