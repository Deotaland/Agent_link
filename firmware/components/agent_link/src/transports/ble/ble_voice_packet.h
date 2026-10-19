#pragma once

#include <cstddef>
#include <cstdint>

namespace agentlink::ble {

constexpr size_t kVoiceHeaderBytes = 15;
// A 512-byte ATT MTU leaves 509 bytes for the VoiceChunk frame.
constexpr size_t kVoiceMaxPcmBytes = 494;
constexpr size_t kVoiceMaxFrameBytes = kVoiceHeaderBytes + kVoiceMaxPcmBytes;
constexpr size_t kVoiceQueueBytes = 24 * 1024;
constexpr int kVoicePoolReserve = 12;

inline bool VoiceBuffersReady(int small_free, int large_free) {
    // One small ATT header and up to two large payload buffers per notification.
    return small_free >= kVoicePoolReserve + 1 && large_free >= kVoicePoolReserve + 2;
}

inline size_t VoicePacketBytes(size_t queued, uint16_t mtu, bool drain) {
    if (mtu < 23) return 0;
    size_t limit = mtu - 3 - kVoiceHeaderBytes;
    if (limit > kVoiceMaxPcmBytes) limit = kVoiceMaxPcmBytes;
    limit &= ~size_t(1);
    if (!drain && queued < limit) return 0;
    return (queued < limit ? queued : limit) & ~size_t(1);
}

}  // namespace agentlink::ble
