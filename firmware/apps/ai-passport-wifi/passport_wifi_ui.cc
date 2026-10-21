#include "passport_wifi_ui.h"
#include "stripe_canvas.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace passport_wifi {
namespace {
struct Frame {
    agent_link_status_t status{};
    char title[80] = "Starting";
    char detail[384] = "Connecting to Agent Link";
    char footer[80] = "OK: call   Hold OK: menu";
    char answer[384]{};
    uint32_t revision = 1;
};
Frame input, frame;
std::mutex lock;
St7789Lcd* panel = nullptr;

bool Latin(const char* text) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
        if (*p >= 0x80) return false;
    return true;
}
void Text(raster::Canvas& canvas, const raster_font_t& font, const char* text,
          int x, int y, int width, int lines, uint32_t color) {
    raster::TextLayout layout;
    layout.Wrap(font, text, width);
    for (unsigned i = 0; i < layout.Count() && i < static_cast<unsigned>(lines); ++i)
        canvas.Text(font, x, y + i * (font.line_height + 3),
                    text + layout.Start(i), text + layout.End(i), color);
}
void Render(void*) {
    uint32_t seen = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> guard(lock);
            if (seen != input.revision) frame = input;
        }
        if (seen == frame.revision) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        const bool link = frame.status.phase != AGENT_LINK_PHASE_CONNECTED &&
                          frame.status.phase != AGENT_LINK_PHASE_READY;
        const bool pairing = frame.status.phase == AGENT_LINK_PHASE_PAIRING && frame.status.code[0];
        const char* title = pairing ? frame.status.code : link ? frame.status.title : frame.title;
        const char* detail = link ? frame.status.hint : frame.detail;
        const uint32_t accent = link ? 0xFFB54A : 0x54D6B4;
        bool ok = true;
        for (int y = 0; y < 320; y += 4) {
            raster::Canvas canvas(panel->StripeBuffer(), 240, y, 4);
            canvas.Fill(0x101820);
            canvas.Box({12, 40, 216, 236}, 0x1D2B36, 12);
            Text(canvas, raster_montserrat_14, "AI PASSPORT / WIFI", 16, 12, 212, 1, accent);
            canvas.Clip({24, 52, 192, 212});
            Text(canvas, pairing ? raster_montserrat_28 : raster_montserrat_20,
                 title, 24, 54, 192, 2, 0xF4F4EC);
            Text(canvas, raster_montserrat_14, detail, 24, 111, 192, 6, 0xC7D3D9);
            if (!link && frame.answer[0])
                Text(canvas, raster_montserrat_14, frame.answer, 24, 213, 192, 2, accent);
            canvas.Clip({12, 281, 216, 39});
            Text(canvas, raster_montserrat_14, link ? "UP / DOWN: volume" : frame.footer,
                 14, 284, 212, 2, 0xC7D3D9);
            if (panel->FlushStripe(y, 4) != ESP_OK) { ok = false; break; }
            if (y % 32 == 28) vTaskDelay(1);
        }
        if (ok) seen = frame.revision;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}  // namespace

esp_err_t StartUi(St7789Lcd* lcd) {
    if (!lcd || !lcd->Ready()) return ESP_ERR_INVALID_STATE;
    panel = lcd;
    return xTaskCreate(Render, "passport_ui", 4096, nullptr, 2, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
void SetStatus(const agent_link_status_t& status) {
    std::lock_guard<std::mutex> guard(lock);
    if (input.status.phase == status.phase && strcmp(input.status.title, status.title) == 0 &&
        strcmp(input.status.hint, status.hint) == 0 && strcmp(input.status.code, status.code) == 0) return;
    input.status = status; ++input.revision;
}
void SetContent(const char* title, const char* detail, const char* footer) {
    std::lock_guard<std::mutex> guard(lock);
    if (strcmp(input.title, title) == 0 && strcmp(input.detail, detail) == 0 &&
        strcmp(input.footer, footer) == 0) return;
    snprintf(input.title, sizeof input.title, "%s", title);
    snprintf(input.detail, sizeof input.detail, "%s", Latin(detail) ? detail : "Conversation selected");
    snprintf(input.footer, sizeof input.footer, "%s", footer);
    ++input.revision;
}
void SetAnswer(const char* text) {
    if (!text) return;
    std::lock_guard<std::mutex> guard(lock);
    snprintf(input.answer, sizeof input.answer, "%s", Latin(text) ? text : "Voice reply (text font unavailable)");
    ++input.revision;
}
}  // namespace passport_wifi
