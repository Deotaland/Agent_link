#pragma once
#include <cstddef>
#include <cstdint>

// CST820 register 0x00..0x06, as used by M5StopWatch-UserDemo.
namespace cst820 {
struct Point { bool pressed = false; uint16_t x = 0, y = 0; };
inline bool Decode(const uint8_t* frame, size_t size, int width, int height, Point* point) {
    if (!point) return false;
    *point = {};
    if (!frame || size != 7 || frame[2] > 1 || width <= 0 || height <= 0) return false;
    if (!frame[2]) return true;  // coordinates/event can be stale when no finger is present
    const unsigned event = frame[3] >> 6;
    if (event == 3) return false;
    if (event == 1) return true;
    const unsigned x = ((frame[3] & 0x0F) << 8) | frame[4];
    const unsigned y = ((frame[5] & 0x0F) << 8) | frame[6];
    if (x >= static_cast<unsigned>(width) || y >= static_cast<unsigned>(height)) return false;
    *point = {true, static_cast<uint16_t>(x), static_cast<uint16_t>(y)};
    return true;
}
}  // namespace cst820
