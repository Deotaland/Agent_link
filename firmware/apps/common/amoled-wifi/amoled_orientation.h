#pragma once

#include <cstddef>
#include <cstdint>

namespace amoled_voice {

struct DisplayRect { int x, y, w, h; };
struct DisplayPoint { int x, y; };

// Logical UI -> panel; touch uses the inverse after the board's sensor mirrors.
constexpr DisplayRect Clockwise90Rect(DisplayRect rect, int panel_width) {
    return {panel_width - rect.y - rect.h, rect.x, rect.h, rect.w};
}

constexpr DisplayPoint Clockwise90Touch(int x, int y, int panel_width) {
    return {y, panel_width - 1 - x};
}

// Copy byte pairs unchanged: RGB565 byte order is handled by the flush callback.
inline bool RotateRgb565Clockwise90(const uint8_t* source, uint8_t* target,
                                    int width, int height, size_t target_bytes) {
    if (!source || !target || source == target || width <= 0 || height <= 0 ||
        static_cast<size_t>(width) > target_bytes / 2 / static_cast<size_t>(height)) return false;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t from = (static_cast<size_t>(y) * width + x) * 2;
            const size_t to = (static_cast<size_t>(x) * height + height - 1 - y) * 2;
            target[to] = source[from];
            target[to + 1] = source[from + 1];
        }
    }
    return true;
}

}  // namespace amoled_voice
