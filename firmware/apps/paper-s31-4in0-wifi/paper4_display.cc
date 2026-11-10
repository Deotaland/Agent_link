#include "config.h"
#include "paper4_display.h"
#include "paper4_touch.h"
#include "epd.h"
#include "epd_4in0_window.h"
#include "raster_font.h"
#include "paper_resources.h"
#include "stripe_canvas.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>
#include <cstdio>

namespace paper4 {
namespace {
class Canvas {
public:
    explicit Canvas(uint8_t* pixels) : pixels_(pixels) {}
    void Box(int x, int y, int w, int h, uint8_t color) {
        for (int yy = std::max(y, 0); yy < std::min(y + h, EPD_HEIGHT); ++yy)
            for (int xx = std::max(x, 0); xx < std::min(x + w, EPD_WIDTH); ++xx)
                pixels_[yy * EPD_WIDTH + xx] = color;
    }
    void Text(int x, int y, const std::string& text, int scale = 2, uint8_t color = 0, int right = EPD_WIDTH - 20) {
        const auto& font = paper_resources::Font();
        const char* cursor = text.c_str();
        const char* end = cursor + text.size();
        while (cursor < end) {
            uint16_t id = raster::GlyphId(font, raster::NextCodepoint(cursor, end));
            if (!id) id = raster::GlyphId(font, '?');
            const auto& glyph = font.glyphs[id];
            const int advance = ((glyph.adv_w + 8) >> 4) * scale;
            if (x + advance > right) break;
            const int gy = y + scale * (font.line_height - font.baseline - glyph.box_h - glyph.ofs_y);
            const auto* bitmap = font.bitmap + glyph.bitmap_index;
            for (unsigned yy = 0; yy < glyph.box_h; ++yy) {
                for (unsigned xx = 0; xx < glyph.box_w; ++xx) {
                    const unsigned i = yy * glyph.box_w + xx;
                    if (((bitmap[i / 2] >> (i & 1 ? 0 : 4)) & 15) >= 7)
                        Box(x + scale * (glyph.ofs_x + int(xx)), gy + scale * int(yy), scale, scale, color);
                }
            }
            x += advance;
        }
    }
    void Body(int y, const std::string& text, int bottom) {
        // Wrap by actual glyph width, preserving UTF-8 boundaries and explicit newlines.
        const char* cursor = text.c_str();
        const char* end = cursor + text.size();
        while (cursor < end && y + 32 <= bottom) {
            const char* start = cursor;
            int width = 0;
            while (cursor < end && *cursor != '\n') {
                const char* next = cursor;
                const auto& font = paper_resources::Font();
                uint16_t id = raster::GlyphId(font, raster::NextCodepoint(next, end));
                if (!id) id = raster::GlyphId(font, '?');
                const int advance = ((font.glyphs[id].adv_w + 8) >> 4) * 2;
                if (width + advance > EPD_WIDTH - 48) break;
                width += advance;
                cursor = next;
            }
            Text(24, y, std::string(start, cursor));
            if (cursor < end && *cursor == '\n') ++cursor;
            y += 36;
        }
    }
    void Calendar(int year, int month, int today) {
        constexpr const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        constexpr int left = 26, cell = 68;
        for (int i = 0; i < 7; ++i) Text(left + i * cell + 20, 240, weekdays[i]);
        Box(24, 286, EPD_WIDTH - 48, 1, 0);
        std::tm first{};
        first.tm_year = year - 1900; first.tm_mon = month - 1; first.tm_mday = 1; first.tm_isdst = -1;
        (void)std::mktime(&first);
        for (int day = 1; day <= MonthDays(year, month); ++day) {
            const int index = first.tm_wday + day - 1;
            const int x = left + (index % 7) * cell, y = 307 + (index / 7) * 62;
            if (day == today) Box(x + 6, y - 5, 52, 49, 0);
            char text[16]; std::snprintf(text, sizeof text, "%2d", day);
            Text(x + 17, y, text, 2, day == today ? 0xff : 0);
        }
    }
    void Button(const TouchButton& button) {
        Box(button.x, button.y, button.w, button.h, 0);
        if (!button.selected) Box(button.x+2,button.y+2,button.w-4,button.h-4,255);
        const uint8_t ink = button.selected ? 255 : 0;
        const int natural_width = raster::TextWidth(paper_resources::Font(), button.label.c_str(), button.label.c_str()+button.label.size());
        const int scale = natural_width * 2 <= button.w-8 ? 2 : 1;
        const int width = natural_width * scale;
        Text(button.x + std::max(4,(button.w-width)/2),button.y+7,button.label,scale,ink,button.x+button.w-3);
        if (!button.hint.empty()) Text(button.x+4,button.y+39,button.hint,1,ink,button.x+button.w-3);
    }
    void Input(const InputView& input, const std::string& body) {
        if (input.kind == InputKind::Calibrate) {
            const auto target = kCalibrationTargets[std::clamp(input.calibration_step,0,3)];
            Box(target.x-20,target.y-2,41,5,0); Box(target.x-2,target.y-20,5,41,0);
            Text(70,390,"请点按十字中心后松手");
            Text(70,438,"长按 KEY1 可取消");
            return;
        }
        if (input.kind == InputKind::Compose) {
            Box(20,136,488,136,0); Box(21,137,486,134,255);
            raster::TextLayout wrapped;
            wrapped.Wrap(paper_resources::Font(),input.text.c_str(),238);
            const unsigned first = wrapped.Count() > 3 ? wrapped.Count()-3 : 0;
            Body(142,input.text.substr(wrapped.Count() ? wrapped.Start(first) : 0),268);
            Text(24,282,!input.notice.empty() ? input.notice : input.pending.empty() ? "点按键盘输入 / 上方候选选字" : "拼音："+input.pending);
        } else Body(232,body,652);
        for (const auto& key : InputButtons(input)) Button(key);
    }
private:
    uint8_t* pixels_;
};
}
Display::~Display() {
    if (pixels_) heap_caps_free(pixels_);
    if (previous_) heap_caps_free(previous_);
}

esp_err_t Display::Draw(const Frame& frame, bool force_full) {
    static_assert(EPD_WIDTH == 528 && EPD_HEIGHT == 792 && EPD_COLORS == 2);
    constexpr size_t bytes = EPD_WIDTH * EPD_HEIGHT;
    if (!pixels_) pixels_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!previous_) previous_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels_ || !previous_) return ESP_ERR_NO_MEM;
    memset(pixels_, 0xff, bytes);
    Canvas canvas(pixels_);
    canvas.Text(24, 15, "PAPER  /  4IN0", 1);
    canvas.Text(24, 42, paper_resources::Ready() ? frame.title : "Resources missing", 3);
    canvas.Box(24, 99, EPD_WIDTH - 48, 2, 0);
    canvas.Text(24, 110, frame.status, 1);
    if (!paper_resources::Ready()) canvas.Body(140,"Install the full Paper 4IN0 v2 USB package.",220);
    else if (frame.input.kind != InputKind::Compose) canvas.Body(140, frame.detail, 220);
    if (frame.input.kind != InputKind::None) canvas.Input(frame.input,frame.body);
    else if (!frame.rows.empty()) {
        const size_t start = frame.selected >= 7 ? size_t(frame.selected) / 7 * 7 : 0;
        for (size_t i = start; i < frame.rows.size() && i < start + 7; ++i) {
            const int y = 232 + int(i - start) * 61;
            const bool selected = int(i) == frame.selected;
            if (selected) canvas.Box(20, y - 5, EPD_WIDTH - 40, 53, 0);
            canvas.Text(32, y, frame.rows[i], 2, selected ? 0xff : 0);
        }
    } else if (frame.calendar_year) canvas.Calendar(frame.calendar_year, frame.calendar_month, frame.today);
    else canvas.Body(232, frame.body, 710);
    if (frame.input.kind == InputKind::None) {
        canvas.Box(24, 719, EPD_WIDTH - 48, 2, 0);
        if (frame.touch) {
            for (const auto& key : MenuButtons(frame.rows.size(),frame.selected))
                if (key.command != TouchCommand::Row) canvas.Button(key);
        } else {
            canvas.Text(24, 731, frame.footer, 1);
            canvas.Text(24, 756, "KEY3 上 / KEY2 下 / KEY1 确认 / 长按返回", 1);
        }
    }

    int left = EPD_WIDTH, top = EPD_HEIGHT, right = -1, bottom = -1;
    if (valid_) {
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            for (int x = 0; x < EPD_WIDTH; ++x) {
                const size_t index = y * EPD_WIDTH + x;
                if (pixels_[index] == previous_[index]) continue;
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
        }
        // Compare actual pixels, including clipping and the visible slice of long menus.
        if (right < 0 && !force_full) return ESP_OK;
    }
    epd4_window_t window{};
    const bool changed = right >= 0 && epd4_window(left, top, right - left + 1, bottom - top + 1, &window);
    const bool full = force_full || !valid_ || !epd_partial_ready() || partials_ >= 64 ||
                      (changed && epd4_window_area(&window) >= int(bytes / 3));
    // Probe only at startup/recovery. Repeated probes would reset a healthy panel before every update.
    if (!valid_ && !epd_probe()) return ESP_ERR_NOT_FOUND;
    const int timeouts = epd_busy_timeouts();
    if (full) {
        EPD_init(); PIC_display(pixels_); EPD_sleep();
    } else epd_partial(pixels_, left, top, right - left + 1, bottom - top + 1);
    if (epd_busy_timeouts() != timeouts) {
        valid_ = false;
        return ESP_ERR_TIMEOUT;
    }
    memcpy(previous_, pixels_, bytes);
    valid_ = true;
    partials_ = full ? 0 : partials_ + 1;
    ESP_LOGD("Paper4.display", "%s dirty=%dx%d window=%d partials=%u", full ? "full" : "partial",
             changed ? right - left + 1 : EPD_WIDTH, changed ? bottom - top + 1 : EPD_HEIGHT,
             changed ? epd4_window_area(&window) : int(bytes), partials_);
    return ESP_OK;
}
} // namespace paper4
