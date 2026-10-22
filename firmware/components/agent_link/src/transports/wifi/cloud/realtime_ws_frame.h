#pragma once

#include <stddef.h>
#include <stdint.h>

namespace agentlink::realtime {

// Client text frame: FIN + mandatory fresh mask. Keep header and payload in
// one contiguous buffer so the TLS transport sees exactly one write.
inline size_t EncodeTextFrame(const char* payload, size_t bytes, const uint8_t mask[4],
                              uint8_t* frame, size_t capacity) {
    const size_t header = bytes < 126 ? 6 : 8;
    if (!payload || !frame || bytes > 65535 || capacity < header || bytes > capacity - header) return 0;
    frame[0] = 0x81;
    frame[1] = static_cast<uint8_t>(0x80 | (bytes < 126 ? bytes : 126));
    size_t offset = 2;
    if (bytes >= 126) {
        frame[offset++] = static_cast<uint8_t>(bytes >> 8);
        frame[offset++] = static_cast<uint8_t>(bytes);
    }
    for (size_t i = 0; i < 4; ++i) frame[offset++] = mask[i];
    for (size_t i = 0; i < bytes; ++i) frame[offset + i] = static_cast<uint8_t>(payload[i]) ^ mask[i % 4];
    return header + bytes;
}

template <typename Writer>
bool WriteTextFrame(const char* payload, size_t bytes, const uint8_t mask[4],
                    uint8_t* frame, size_t capacity, Writer write) {
    const size_t size = EncodeTextFrame(payload, bytes, mask, frame, capacity);
    // A short/failed write poisons this stream. Never replay any portion here.
    return size && write(frame, size) == static_cast<int>(size);
}

class OpusBatchGate {
public:
    static constexpr unsigned kFrames = 3;
    bool Ready(unsigned queued, int64_t now_ms) {
        if (!queued) { Reset(); return false; }
        if (since_ms_ < 0) since_ms_ = now_ms;
        return queued >= kFrames || now_ms - since_ms_ >= 40;
    }
    void Reset() { since_ms_ = -1; }
private:
    int64_t since_ms_ = -1;
};

class WriteStall {
public:
    void Observe(bool writable, int64_t now_ms) {
        if (writable) since_ms_ = -1;
        else if (since_ms_ < 0) since_ms_ = now_ms;
    }
    bool Blocked() const { return since_ms_ >= 0; }
    bool Expired(int64_t now_ms) const { return Blocked() && now_ms - since_ms_ >= 3000; }
private:
    int64_t since_ms_ = -1;
};

}  // namespace agentlink::realtime
