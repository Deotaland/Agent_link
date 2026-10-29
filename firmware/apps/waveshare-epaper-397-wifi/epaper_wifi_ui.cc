#include "epaper_wifi_ui.h"
#include "epaper_frame.h"
#include "epaper_refresh.h"
#include "cjk_font.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace epaper {
namespace {
bool SameLayout(const Screen& a, const Screen& b, bool selection = true) {
    if (a.page != b.page || (selection && a.selected != b.selected) || strcmp(a.title, b.title) ||
        strcmp(a.detail, b.detail) || strcmp(a.footer, b.footer) || strcmp(a.agent, b.agent) ||
        strcmp(a.session, b.session) || strcmp(a.setup_ssid, b.setup_ssid)) return false;
    for (int i = 0; i < 6; ++i) if (strcmp(a.rows[i], b.rows[i])) return false;
    return true;
}
bool SameHeader(const Screen& a, const Screen& b) {
    return a.volume == b.volume && a.battery == b.battery && a.charging == b.charging;
}
bool SameScreen(const Screen& a, const Screen& b) {
    return a.view == b.view && SameHeader(a, b) && SameLayout(a, b);
}
bool SameStatus(const agent_link_status_t& a, const agent_link_status_t& b) {
    return a.phase == b.phase && !strcmp(a.code, b.code) && !strcmp(a.title, b.title) &&
           (a.code[0] || !strcmp(a.hint, b.hint));
}
Scene RenderScene(const agent_link_status_t& status) {
    if (status.phase == AGENT_LINK_PHASE_SETUP) return Scene::kSetup;
    if (status.code[0]) return Scene::kActivation;
    if (status.phase == AGENT_LINK_PHASE_CONNECTING || status.phase == AGENT_LINK_PHASE_PAIRING) return Scene::kConnecting;
    if (status.phase == AGENT_LINK_PHASE_BLOCKED) return Scene::kBlocked;
    if (status.phase == AGENT_LINK_PHASE_CONNECTED || status.phase == AGENT_LINK_PHASE_READY) return Scene::kOnline;
    return Scene::kIdle;
}
}
esp_err_t Ui::Start(EpaperHardware* hardware) {
    hardware_ = hardware;
    if (!hardware_->DisplayReady()) return ESP_ERR_INVALID_STATE;
    frame_ = static_cast<uint8_t*>(heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 8, MALLOC_CAP_SPIRAM));
    if (!frame_) return ESP_ERR_NO_MEM;
    previous_frame_ = static_cast<uint8_t*>(heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 8, MALLOC_CAP_SPIRAM));
    if (!previous_frame_) { free(frame_); frame_ = nullptr; return ESP_ERR_NO_MEM; }
    if (xTaskCreate(Entry, "ep397_screen", 20480, this, 2, &task_) != pdPASS) {
        free(previous_frame_); previous_frame_ = nullptr; free(frame_); frame_ = nullptr; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void Ui::Status(const agent_link_status_t& value) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (SameStatus(status_, value)) return;
        status_ = value;
    }
    Wake();
}
void Ui::Answer(const char* text) {
    if (!text) return;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (strcmp(answer_, text) == 0) return;
        snprintf(answer_, sizeof answer_, "%s", text); answer_at_ = esp_timer_get_time(); answer_done_ = false;
    }
    Wake();
}
void Ui::EndAnswer() { { std::lock_guard<std::mutex> guard(lock_); answer_done_ = true; } Wake(); }
void Ui::Publish(const Screen& value) {
    { std::lock_guard<std::mutex> guard(lock_); if (SameScreen(screen_, value)) return; screen_ = value; }
    Wake();
}
void Ui::Pixel(int x, int y) {
    BlackPixel(frame_, x, y);
}
void Ui::Text(int x, int y, int max_x, int max_y, const char* text, int scale) {
    const int left = x;
    const int top_limit = y;
    const int line_height = std::max(32, cjk_font_ ? int(cjk_font_->line_height) : 32) * scale;
    while (*text && y + line_height <= max_y) {
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
        if (cp == '\n') { x = left; y += line_height; continue; }
        // Use one font for Chinese and Latin to keep a common baseline.
        const lv_font_t* candidates[] = {
            cjk_font_, &lv_font_montserrat_20,
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
        const int glyph_right = x + (glyph.ofs_x + glyph.box_w) * zoom;
        if (WrapGlyph(x, left, max_x, glyph.adv_w * zoom, glyph_right)) { x = left; y += line_height; }
        if (y + line_height > max_y) break;
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
                                    for (int dx = 0; dx < zoom; ++dx) {
                                        const int px = x + (glyph.ofs_x + static_cast<int>(col)) * zoom + dx;
                                        const int py = y + (top + static_cast<int>(row)) * zoom + dy;
                                        if (InTextBox(px, py, left, top_limit, max_x, max_y)) Pixel(px, py);
                                    }
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
    Screen painted_screen{};
    agent_link_status_t status{};
    agent_link_status_t painted_status{};
    char answer[sizeof answer_]{}, previous[2048]{};
    RefreshPolicy refresh;
    int64_t answer_painted_at = 0;
    bool painted_valid = false;
    while (true) {
        // Notifications wake changes immediately. The short timeout services
        // text coalescing and bounded recovery, without imposing a refresh gap.
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        const int64_t now = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> guard(lock_);
            screen = screen_; status = status_;
            if (!answer_[0] || answer_done_ || now - answer_at_ >= 150000 || now - answer_painted_at >= 500000)
                snprintf(answer, sizeof answer, "%s", answer_);
        }
        // Compare the same status line that is drawn. An activation code hides
        // the SDK's changing expiry hint, so polling alone must not refresh e-paper.
        char status_line[sizeof status.hint];
        if (status.code[0]) snprintf(status_line, sizeof status_line, "Activation code: %s", status.code);
        else snprintf(status_line, sizeof status_line, "%s", status.hint);
        char content[2048];
        snprintf(content, sizeof content, "%d|%d|%d|%d|%d|%s|%s|%s|%s|%s|%s|%s|%s|%d|%s|%s|%s|%s|%s|%s",
                 int(status.phase), screen.volume, screen.battery, int(screen.charging), screen.page,
                 status.title, status_line, screen.title, screen.detail, screen.footer,
                 screen.agent, screen.session, screen.setup_ssid,
                 screen.selected, screen.rows[0], screen.rows[1], screen.rows[2], screen.rows[3],
                 screen.rows[4], screen.rows[5]);
        // Streamed answer updates settle before refresh and never block microphone or network workers.
        const bool changed = strcmp(content, previous) != 0 || (!screen.rows[0][0] && strcmp(answer, painted_answer_) != 0);
        if (changed && refresh.Ready(now)) {
            const int64_t render_started = esp_timer_get_time();
            const bool same_status = painted_valid && SameStatus(status, painted_status);
            const bool selection_only = same_status && screen.rows[0][0] &&
                screen.selected != painted_screen.selected && SameHeader(screen, painted_screen) &&
                SameLayout(screen, painted_screen, false);
            const bool header_only = same_status && SameLayout(screen, painted_screen) &&
                (screen.rows[0][0] || !strcmp(answer, painted_answer_));
            if (selection_only || header_only) memcpy(frame_, previous_frame_, EPD_WIDTH * EPD_HEIGHT / 8);
            else memset(frame_, 0xff, EPD_WIDTH * EPD_HEIGHT / 8);
            if (selection_only) {
                if (painted_screen.selected >= 0 && painted_screen.selected < 6) {
                    const int y = 312 + painted_screen.selected * 62;
                    ClearBox(frame_, 20, y, 44, y + 50);
                }
                if (screen.selected >= 0 && screen.selected < 6) {
                    const int y = 312 + screen.selected * 62;
                    Text(20, y, 44, y + 50, ">");
                }
            } else {
                if (header_only) ClearBox(frame_, 16, 12, 464, 58);
                const char* link = status.phase == AGENT_LINK_PHASE_READY || status.phase == AGENT_LINK_PHASE_CONNECTED ? "在线" :
                                   status.phase == AGENT_LINK_PHASE_PAIRING ? "待激活" :
                                   status.phase == AGENT_LINK_PHASE_SETUP ? "待配网" :
                                   status.phase == AGENT_LINK_PHASE_CONNECTING ? "连接中" : "离线";
                char volume[32], battery[48];
                snprintf(volume, sizeof volume, "音量 %d%%", screen.volume);
                if (screen.battery >= 0) snprintf(battery, sizeof battery, "%s%d%%", screen.charging ? "+" : "", screen.battery);
                else snprintf(battery, sizeof battery, "%s", screen.battery == -1 ? "USB" : "电池?");
                Text(16, 12, 150, 58, link);
                Text(154, 12, 330, 58, volume);
                Text(336, 12, 464, 58, battery);
                for (int x = 16; x < 464; ++x) Pixel(x, 70);
                if (!header_only) {
                    if (status.phase == AGENT_LINK_PHASE_SETUP) {
                        Text(20, 94, 460, 146, "连接 WiFi");
                        Text(20, 164, 460, 210, "1. 手机连接设备热点");
                        Text(20, 214, 460, 262, screen.setup_ssid[0] ? screen.setup_ssid : "热点启动中…");
                        Text(20, 286, 460, 332, "2. 用浏览器打开");
                        Text(20, 336, 460, 384, "192.168.4.1");
                        Text(20, 408, 460, 454, "3. 输入 WiFi 名称和密码");
                        if (status.hint[0] && !strstr(status.hint, "Join "))
                            Text(20, 478, 460, 568, status.hint);
                    } else if (status.code[0]) {
                        Text(20, 94, 460, 146, "设备激活");
                        Text(20, 158, 460, 244, status.code, 2);
                        Text(20, 262, 460, 310, "请在平台输入激活码");
                    } else if (status.phase != AGENT_LINK_PHASE_READY && status.phase != AGENT_LINK_PHASE_CONNECTED) {
                        Text(20, 94, 460, 146, screen.title);
                        Text(20, 158, 460, 204, status.title);
                        Text(20, 214, 460, 304, status_line);
                    } else {
                        Text(20, 94, 460, 146, screen.title);
                        char identity[220];
                        snprintf(identity, sizeof identity, "助手：%s\n会话：%s", screen.agent[0] ? screen.agent : "请在平台绑定", screen.session[0] ? screen.session : "未选择");
                        Text(20, 158, 460, 240, identity);
                        Text(20, 244, 460, 306, screen.detail);
                    }
                    if (status.phase != AGENT_LINK_PHASE_SETUP && screen.rows[0][0]) {
                        for (int i = 0; i < 6; ++i) {
                            const int y = 312 + i * 62;
                            if (screen.selected == i) Text(20, y, 44, y + 50, ">");
                            Text(50, y, 460, y + 60, screen.rows[i]);
                        }
                    } else if (status.phase != AGENT_LINK_PHASE_SETUP && answer[0]) {
                        Text(20, 312, 460, 360, "回复");
                        Text(20, 364, 460, 680, answer);
                    }
                    for (int x = 16; x < 464; ++x) Pixel(x, 692);
                    Text(20, 708, 460, 795, status.phase == AGENT_LINK_PHASE_SETUP ? "请在手机上完成配网\n配网时按键不可用" : screen.footer);
                }
            }
            Rect rect{};
            const bool pixels_changed = !painted_valid || ChangedRect(frame_, previous_frame_, rect);
            if (!painted_valid) rect = {0, 0, EPD_WIDTH, EPD_HEIGHT};
            const Scene scene = RenderScene(status);
            const bool full = refresh.Full(scene, screen.page, rect);
            const bool setup_page = status.phase == AGENT_LINK_PHASE_SETUP;
            const int64_t transfer_started = esp_timer_get_time();
            const esp_err_t error = !pixels_changed ? ESP_OK : full ? hardware_->Display(frame_, true)
                : hardware_->DisplayPartial(frame_);
            const int64_t completed = esp_timer_get_time();
            if (error == ESP_OK) {
                if (pixels_changed) {
                    memcpy(previous_frame_, frame_, EPD_WIDTH * EPD_HEIGHT / 8);
                    refresh.Success(full, scene, screen.page);
                    ESP_LOGI("WS397.ui", "%s scene=%d page=%d view=%u render=%ums transfer=%ums dirty=%dx%d window=%dx%d",
                             full ? "full" : "partial", int(scene), screen.page, unsigned(screen.view),
                             unsigned((transfer_started - render_started) / 1000), unsigned((completed - transfer_started) / 1000),
                             rect.x1 - rect.x0, rect.y1 - rect.y0, EPD_WIDTH, EPD_HEIGHT);
                }
                snprintf(previous, sizeof previous, "%s", content);
                painted_screen = screen; painted_status = status; painted_valid = true;
                if (strcmp(painted_answer_, answer)) answer_painted_at = completed;
                snprintf(painted_answer_, sizeof painted_answer_, "%s", answer);
                displayed_setup_.store(setup_page);
                displayed_.store(screen.view);
            } else {
                refresh.Failure(completed);
                painted_valid = false;
                ESP_LOGW("WS397.ui", "Refresh failed: %s", esp_err_to_name(error));
            }
        } else if (!changed && refresh.Ready(now)) displayed_.store(screen.view);
    }
}
}
