#include "paper_hardware.h"
#include "epd.h"
#include "raster_font.h"
#include "stripe_canvas.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <cstdlib>
#include <cstring>

extern "C" esp_err_t paper_display_lines(const char* const* lines, size_t count) {
    if (!lines || count > 14) return ESP_ERR_INVALID_ARG;
    auto* frame = static_cast<uint8_t*>(heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!frame) return ESP_ERR_NO_MEM;
    if (!epd_probe()) {
        ESP_LOGE("paper.display", "Display probe failed; no frame sent");
        free(frame); return ESP_ERR_NOT_FOUND;
    }
    std::memset(frame, 0xff, EPD_WIDTH * EPD_HEIGHT);
    const auto& font = raster_cjk_14;
    for (size_t row = 0; row < count; ++row) {
        const char* text = lines[row];
        if (!text) continue;
        const char* end = text + std::strlen(text);
        int x = 14;
        while (text < end) {
            uint16_t id = raster::GlyphId(font, raster::NextCodepoint(text, end));
            if (!id) id = raster::GlyphId(font, '?');
            const auto& glyph = font.glyphs[id];
            const int advance = ((glyph.adv_w + 8) >> 4) * 2;
            if (x + advance > EPD_WIDTH - 8) break;
            const int gy = 8 + row * 40 + 2 * (font.line_height - font.baseline - glyph.box_h - glyph.ofs_y);
            const uint8_t* bitmap = font.bitmap + glyph.bitmap_index;
            for (unsigned y = 0; y < glyph.box_h; ++y) for (unsigned c = 0; c < glyph.box_w; ++c) {
                const unsigned index = y * glyph.box_w + c;
                if (((bitmap[index / 2] >> (index & 1 ? 0 : 4)) & 15) < 7) continue;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                    const int px = x + 2 * (glyph.ofs_x + c) + dx, py = gy + y * 2 + dy;
                    if (px >= 0 && px < EPD_WIDTH && py >= 0 && py < EPD_HEIGHT) frame[py * EPD_WIDTH + px] = 0;
                }
            }
            x += advance;
        }
    }
#if CONFIG_PAPER_S31_DISPLAY_DIAGNOSTIC
    uint32_t hash = 2166136261u;
    size_t white = 0, black = 0;
    for (size_t i = 0; i < EPD_WIDTH * EPD_HEIGHT; ++i) {
        hash = (hash ^ frame[i]) * 16777619u;
        white += frame[i] == 0xff;
        black += frame[i] == 0;
    }
    const int64_t started = esp_timer_get_time();
    ESP_LOGI("paper.display", "[display-diag] frame begin: %dx%d hash=%08lx white=%u black=%u other=%u",
             EPD_WIDTH, EPD_HEIGHT, (unsigned long)hash, (unsigned)white, (unsigned)black,
             (unsigned)(EPD_WIDTH * EPD_HEIGHT - white - black));
#endif
    const int before = epd_busy_timeouts();
    EPD_init(); PIC_display(frame); EPD_sleep(); free(frame);
    const esp_err_t result = epd_busy_timeouts() == before ? ESP_OK : ESP_ERR_TIMEOUT;
#if CONFIG_PAPER_S31_DISPLAY_DIAGNOSTIC
    ESP_LOGI("paper.display", "[display-diag] frame end: %s elapsed=%lld ms timeouts=%d; driver returned, verify optical result",
             esp_err_to_name(result), (long long)((esp_timer_get_time() - started) / 1000), epd_busy_timeouts() - before);
#endif
    return result;
}
