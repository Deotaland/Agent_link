#pragma once

#include <cstddef>
#include <cstdint>

#include "raster_font.h"

namespace raster {

struct Rect { int x, y, w, h; };

uint32_t NextCodepoint(const char*& text, const char* end);
uint16_t GlyphId(const raster_font_t& font, uint32_t codepoint);
void CopyText(char* out, size_t capacity, const char* text);
int TextWidth(const raster_font_t& font, const char* text, const char* end);

class TextLayout {
public:
    static constexpr unsigned kMaxLines = 128;
    void Wrap(const raster_font_t& font, const char* text, int width);
    unsigned Count() const { return count_; }
    uint16_t Start(unsigned line) const { return starts_[line]; }
    uint16_t End(unsigned line) const { return ends_[line]; }
private:
    uint16_t starts_[kMaxLines] = {};
    uint16_t ends_[kMaxLines] = {};
    unsigned count_ = 0;
};

// Pixels stay in the panel's byte order; only the current stripe occupies RAM.
class Canvas {
public:
    Canvas(uint8_t* pixels, int width, int y, int rows);
    void Clip(Rect rect);
    void Fill(uint32_t color);
    void Box(Rect rect, uint32_t color, int radius = 0);
    void Text(const raster_font_t& font, int x, int y, const char* text,
              const char* end, uint32_t color);
private:
    void Pixel(int x, int y, uint32_t color, unsigned alpha = 15);
    uint8_t* pixels_;
    int width_, y_, rows_;
    Rect clip_;
};

}  // namespace raster
