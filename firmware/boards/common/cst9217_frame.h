#pragma once
#include <cstddef>
#include <cstdint>

namespace cst9217 {
struct Point { bool pressed = false; uint16_t x = 0, y = 0; };
// CST9217 D000 report: status, X high, Y high, XY low, pressure,
// contact count, AB marker. Only the first contact drives the UI pointer.
inline bool Decode(const uint8_t* frame, size_t size, int width, int height, Point* point) {
    if (!point) return false;
    *point = {};
    if (!frame || size < 7 || width <= 0 || height <= 0 || frame[6] != 0xAB) return false;
    const unsigned count = frame[5] & 0x7F;
    if (count > 2) return false;
    if (!count || (frame[0] & 0x0F) != 0x06) return true;
    const unsigned x = (frame[1] << 4) | (frame[3] >> 4);
    const unsigned y = (frame[2] << 4) | (frame[3] & 0x0F);
    if (x >= static_cast<unsigned>(width) || y >= static_cast<unsigned>(height)) return false;
    *point = {true, static_cast<uint16_t>(x), static_cast<uint16_t>(y)};
    return true;
}
}  // namespace cst9217
