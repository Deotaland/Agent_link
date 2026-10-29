#pragma once
#include <cstddef>
#include <cstdint>

namespace epaper {
constexpr int kNativeWidth = 800, kNativeHeight = 480;
constexpr size_t kNativeFrameBytes = kNativeWidth * kNativeHeight / 8;

// Match Waveshare's integrated menu: the partial waveform always consumes a
// complete native framebuffer at origin (0, 0). Logical rotation is already in
// the pixels; no dirty-window offsets are applied to controller addressing.
template <class Command>
bool QueuePartialFrame(const uint8_t* frame, Command command) {
    const uint8_t sensor = 0x80, border = 0x80, refresh = 0xff;
    const uint8_t x[] = {0, 0, uint8_t((kNativeWidth - 8) & 0xff), uint8_t((kNativeWidth - 8) >> 8)};
    const uint8_t y[] = {uint8_t((kNativeHeight - 1) & 0xff), uint8_t((kNativeHeight - 1) >> 8), 0, 0};
    const uint8_t origin[] = {0, 0};
    return frame && command(0x18, &sensor, 1) && command(0x3c, &border, 1) &&
        command(0x44, x, sizeof x) && command(0x45, y, sizeof y) &&
        command(0x4e, origin, sizeof origin) && command(0x4f, origin, sizeof origin) &&
        command(0x24, frame, kNativeFrameBytes) && command(0x22, &refresh, 1) &&
        command(0x20, nullptr, 0);
}
}
