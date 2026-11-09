#pragma once
#include <cstdint>
#include <memory>

namespace agent_system {
constexpr uint32_t kCoverEdge = 132;
constexpr uint32_t kCoverHeaderBytes = 64;
constexpr uint32_t kCoverPixelBytes = kCoverEdge * kCoverEdge * 3;
constexpr uint32_t kCoverBytes = kCoverHeaderBytes + kCoverPixelBytes;
#ifndef AGENT_SYSTEM_COVER_RESERVE
#define AGENT_SYSTEM_COVER_RESERVE 0xE000
#endif
#ifndef AGENT_SYSTEM_APP_MAX_BYTES
#define AGENT_SYSTEM_APP_MAX_BYTES (0x300000 - AGENT_SYSTEM_COVER_RESERVE)
#endif
constexpr uint32_t kCoverReserveBytes = AGENT_SYSTEM_COVER_RESERVE;
constexpr uint32_t kAppImageMaxBytes = AGENT_SYSTEM_APP_MAX_BYTES;
constexpr const char* kCoverFormat = "alcover-rgb565a8-le-v1";
// Little-endian fields and RGB565 pixels. A8 is a separate plane, not interleaved.
// Independent Agent Link format; SDGOODS' swapped RGB565 bytes are not compatible.
struct CoverHeader {
    char magic[8];            // ALCOVER1, no NUL
    uint8_t version, format;  // 1, 1
    uint16_t width, height, reserved;
    uint32_t data_len, crc32;  // CRC-32/ISO-HDLC of both pixel planes, not the header.
    char app_id[40];           // Must equal Descriptor.app_id, NUL terminated.
};
static_assert(sizeof(CoverHeader) == kCoverHeaderBytes);
struct CoverPixels {
    std::shared_ptr<const uint8_t> blob;  // Header followed by RGB565LE plane and A8 plane.
    const uint8_t* Data() const {
        return blob.get() + kCoverHeaderBytes;
    }
};
struct Cover {
    int slot = -1;
    uint32_t revision = 0;
};
// Immutable snapshots are retained by the renderer while the controller changes slots.
std::shared_ptr<const CoverPixels> AcquireCover(const Cover& cover);
}  // namespace agent_system
