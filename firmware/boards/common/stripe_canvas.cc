#include "stripe_canvas.h"

#include <algorithm>
#include <cstring>

namespace raster {
namespace {

int Advance(const raster_font_t& font, uint16_t id, uint16_t next) {
    if (!id) return font.line_height / 2;
    int width = font.glyphs[id].adv_w;
    const unsigned left = font.kern_left[id], right = font.kern_right[next];
    if (left && right) width += font.kern_values[(left - 1) * font.kern_right_count + right - 1];
    return (width + 8) >> 4;
}

uint16_t Color565(uint32_t color) {
    return static_cast<uint16_t>(((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x001f));
}

}  // namespace

uint32_t NextCodepoint(const char*& text, const char* end) {
    if (text >= end) return 0;
    const auto first = static_cast<uint8_t>(*text++);
    if (first < 0x80) return first;
    unsigned count;
    uint32_t value, minimum;
    if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 0x1f; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 0x0f; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 0x10000; }
    else return 0xfffd;
    if (static_cast<size_t>(end - text) < count) return 0xfffd;
    for (unsigned i = 0; i < count; ++i) {
        const auto byte = static_cast<uint8_t>(text[i]);
        if ((byte & 0xc0) != 0x80) return 0xfffd;
        value = (value << 6) | (byte & 0x3f);
    }
    text += count;
    return value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ? 0xfffd : value;
}

uint16_t GlyphId(const raster_font_t& font, uint32_t codepoint) {
    if (codepoint >= 32 && codepoint < 127) return static_cast<uint16_t>(codepoint - 31);
    if (codepoint < 176 || codepoint - 176 > UINT16_MAX) return 0;
    const uint16_t relative = static_cast<uint16_t>(codepoint - 176);
    const auto* found = std::lower_bound(font.sparse, font.sparse + font.sparse_count, relative);
    if (found == font.sparse + font.sparse_count || *found != relative) return 0;
    return static_cast<uint16_t>(96 + found - font.sparse);
}

int TextWidth(const raster_font_t& font, const char* text, const char* end) {
    int width = 0;
    while (text < end) {
        const uint16_t id = GlyphId(font, NextCodepoint(text, end));
        const char* peek = text;
        width += Advance(font, id, GlyphId(font, NextCodepoint(peek, end)));
    }
    return width;
}

void TextLayout::Wrap(const raster_font_t& font, const char* text, int width) {
    count_ = 0;
    if (!text || width <= 0) return;
    const char* end = text + std::min<size_t>(std::strlen(text), UINT16_MAX);
    const char* line = text;
    while (line < end && count_ < kMaxLines) {
        const char* p = line;
        const char* break_end = nullptr;
        const char* break_next = nullptr;
        while (p < end) {
            const char* before = p;
            const uint32_t codepoint = NextCodepoint(p, end);
            if (codepoint == '\n') {
                starts_[count_] = static_cast<uint16_t>(line - text);
                ends_[count_++] = static_cast<uint16_t>(before - text);
                line = p;
                break;
            }
            if (codepoint == ' ') { break_end = before; break_next = p; }
            if (TextWidth(font, line, p) > width && before != line) {
                const char* stop = break_end && break_end > line ? break_end : before;
                const char* next = break_end && break_end > line ? break_next : before;
                starts_[count_] = static_cast<uint16_t>(line - text);
                ends_[count_++] = static_cast<uint16_t>(stop - text);
                line = next;
                while (line < end && *line == ' ') ++line;
                break;
            }
            if (p == end) {
                starts_[count_] = static_cast<uint16_t>(line - text);
                ends_[count_++] = static_cast<uint16_t>(end - text);
                line = end;
            }
        }
    }
}

Canvas::Canvas(uint8_t* pixels, int width, int y, int rows)
    : pixels_(pixels), width_(width), y_(y), rows_(rows), clip_{0, y, width, rows} {}

void Canvas::Clip(Rect rect) { clip_ = rect; }

void Canvas::Pixel(int x, int y, uint32_t color, unsigned alpha) {
    if (x < 0 || x >= width_ || y < y_ || y >= y_ + rows_ ||
        x < clip_.x || x >= clip_.x + clip_.w || y < clip_.y || y >= clip_.y + clip_.h || !alpha) return;
    uint8_t* out = pixels_ + ((y - y_) * width_ + x) * 2;
    uint16_t value = Color565(color);
    if (alpha < 15) {
        const uint16_t old = static_cast<uint16_t>((out[0] << 8) | out[1]);
        const unsigned inverse = 15 - alpha;
        const unsigned r = (((value >> 11) * alpha + (old >> 11) * inverse + 7) / 15);
        const unsigned g = ((((value >> 5) & 63) * alpha + ((old >> 5) & 63) * inverse + 7) / 15);
        const unsigned b = (((value & 31) * alpha + (old & 31) * inverse + 7) / 15);
        value = static_cast<uint16_t>((r << 11) | (g << 5) | b);
    }
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}

void Canvas::Fill(uint32_t color) {
    const uint16_t value = Color565(color);
    for (int i = 0; i < width_ * rows_; ++i) {
        pixels_[i * 2] = static_cast<uint8_t>(value >> 8);
        pixels_[i * 2 + 1] = static_cast<uint8_t>(value);
    }
}

void Canvas::Box(Rect rect, uint32_t color, int radius) {
    if (rect.w <= 0 || rect.h <= 0) return;
    const int r = std::max(0, std::min(radius, std::min(rect.w, rect.h) / 2));
    const int top = std::max({rect.y, y_, clip_.y}), bottom = std::min({rect.y + rect.h, y_ + rows_, clip_.y + clip_.h});
    const int left = std::max({rect.x, 0, clip_.x}), right = std::min({rect.x + rect.w, width_, clip_.x + clip_.w});
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            unsigned coverage = 15;
            if (r && (x < rect.x + r || x >= rect.x + rect.w - r) &&
                (y < rect.y + r || y >= rect.y + rect.h - r)) {
                const int cx = 4 * (x < rect.x + r ? rect.x + r : rect.x + rect.w - r);
                const int cy = 4 * (y < rect.y + r ? rect.y + r : rect.y + rect.h - r);
                unsigned inside = 0;
                for (int sy = 1; sy <= 3; sy += 2) for (int sx = 1; sx <= 3; sx += 2) {
                    const int dx = 4 * x + sx - cx, dy = 4 * y + sy - cy;
                    inside += dx * dx + dy * dy <= 16 * r * r;
                }
                coverage = (inside * 15 + 2) / 4;
            }
            Pixel(x, y, color, coverage);
        }
    }
}

void Canvas::Text(const raster_font_t& font, int x, int y, const char* text, const char* end, uint32_t color) {
    if (y >= y_ + rows_ || y + font.line_height < y_) return;
    while (text < end) {
        const uint16_t id = GlyphId(font, NextCodepoint(text, end));
        const char* peek = text;
        const uint16_t next = GlyphId(font, NextCodepoint(peek, end));
        if (id) {
            const auto& glyph = font.glyphs[id];
            const int gx = x + glyph.ofs_x;
            const int gy = y + font.line_height - font.baseline - glyph.box_h - glyph.ofs_y;
            const uint8_t* bitmap = font.bitmap + glyph.bitmap_index;
            for (int row = std::max(0, y_ - gy); row < glyph.box_h && gy + row < y_ + rows_; ++row) {
                for (int column = 0; column < glyph.box_w; ++column) {
                    const unsigned index = row * glyph.box_w + column;
                    const unsigned coverage = (bitmap[index / 2] >> (index & 1 ? 0 : 4)) & 15;
                    Pixel(gx + column, gy + row, color, coverage);
                }
            }
        } else {
            const int w = font.line_height / 2 - 2, h = font.line_height - 5;
            Box({x + 1, y + 2, w, 1}, color);
            Box({x + 1, y + h + 1, w, 1}, color);
            Box({x + 1, y + 2, 1, h}, color);
            Box({x + w, y + 2, 1, h}, color);
        }
        x += Advance(font, id, next);
    }
}

}  // namespace raster
