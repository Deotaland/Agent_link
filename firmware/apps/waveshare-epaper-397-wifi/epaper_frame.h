#pragma once
#include <cstdint>

namespace epaper {
constexpr int kPanelWidth = 800, kPanelHeight = 480;
constexpr int kUiWidth = kPanelHeight, kUiHeight = kPanelWidth;
constexpr int kStride = kPanelWidth / 8;
struct Rect { int x0, y0, x1, y1; }; // native pixels, end exclusive

constexpr bool WrapGlyph(int x, int left, int max_x, int advance, int right_edge) {
    return x != left && (x + advance > max_x || right_edge > max_x);
}
constexpr bool InTextBox(int x, int y, int left, int top, int right, int bottom) {
    return x >= left && x < right && y >= top && y < bottom;
}

inline void BlackPixel(uint8_t* frame, int x, int y) {
    if (x < 0 || x >= kUiWidth || y < 0 || y >= kUiHeight) return;
    const int native_x = y, native_y = kPanelHeight - 1 - x;
    frame[native_y * kStride + native_x / 8] &= static_cast<uint8_t>(~(0x80u >> (native_x % 8)));
}
inline void ClearBox(uint8_t* frame, int x0, int y0, int x1, int y1) {
    for (int x = x0; x < x1; ++x) for (int y = y0; y < y1; ++y) {
        if (x < 0 || x >= kUiWidth || y < 0 || y >= kUiHeight) continue;
        const int native_x = y, native_y = kPanelHeight - 1 - x;
        frame[native_y * kStride + native_x / 8] |= static_cast<uint8_t>(0x80u >> (native_x % 8));
    }
}

inline bool ChangedRect(const uint8_t* frame, const uint8_t* previous, Rect& rect) {
    int left = kStride, right = -1, top = kPanelHeight, bottom = -1;
    for (int y = 0; y < kPanelHeight; ++y) {
        for (int x = 0; x < kStride; ++x) {
            if (frame[y * kStride + x] == previous[y * kStride + x]) continue;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    if (right < 0) return false;
    rect = {left * 8, top, (right + 1) * 8, bottom + 1};
    return true;
}
}
