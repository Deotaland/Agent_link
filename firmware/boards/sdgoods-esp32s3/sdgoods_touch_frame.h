#pragma once
#include "cst9217_frame.h"
namespace sdgoods {
// Native 360px coordinates, matching vendor MADCTL=0x60: x=raw_y, y=359-raw_x.
inline bool DecodeTouch(const uint8_t* raw, size_t size, cst9217::Point* point) {
    if (!point) return false;
    *point = {};
    if (!raw || size < 5 || (raw[0] & 0x0F) > 1) return false;
    if (!(raw[0] & 0x0F)) return true;
    const unsigned x = ((raw[1] & 0x0F) << 8) | raw[2];
    const unsigned y = ((raw[3] & 0x0F) << 8) | raw[4];
    if (x >= 360 || y >= 360) return false;
    *point = {true, static_cast<uint16_t>(y), static_cast<uint16_t>(359 - x)};
    return true;
}
}
