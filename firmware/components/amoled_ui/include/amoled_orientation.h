#pragma once

#include <cstddef>
#include <cstdint>

namespace amoled_ui {

struct DisplayRect { int x, y, w, h; };
struct DisplayPoint { int x, y; };

// Logical UI -> panel; touch uses the inverse after the board's sensor mirrors.
constexpr DisplayRect Clockwise90Rect(DisplayRect rect, int panel_width) {
    return {panel_width - rect.y - rect.h, rect.x, rect.h, rect.w};
}

constexpr DisplayPoint Clockwise90Touch(int x, int y, int panel_width) {
    return {y, panel_width - 1 - x};
}

// Copy RGB565 pairs, optionally combining wire byte order with the rotation.
inline bool RotateRgb565Clockwise90(const uint8_t* source, uint8_t* target,
                                    int width, int height, size_t target_bytes, bool swap_bytes = false) {
    if (!source || !target || source == target || width <= 0 || height <= 0 ||
        static_cast<size_t>(width) > target_bytes / 2 / static_cast<size_t>(height)) return false;
    // Tiles keep both row-major images in cache during the transposed copy.
    for (int tile_y = 0; tile_y < height; tile_y += 16) {
        for (int tile_x = 0; tile_x < width; tile_x += 16) {
            for (int y = tile_y; y < height && y < tile_y + 16; ++y) {
                for (int x = tile_x; x < width && x < tile_x + 16; ++x) {
                    const size_t from = (static_cast<size_t>(y) * width + x) * 2;
                    const size_t to = (static_cast<size_t>(x) * height + height - 1 - y) * 2;
                    target[to] = source[from + (swap_bytes ? 1 : 0)];
                    target[to + 1] = source[from + (swap_bytes ? 0 : 1)];
                }
            }
        }
    }
    return true;
}

}  // namespace amoled_ui
