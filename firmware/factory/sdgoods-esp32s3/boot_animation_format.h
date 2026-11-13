#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sugarat_boot {
constexpr size_t kHeaderBytes = 48, kEntryBytes = 8, kMaxBytes = 6 * 1024 * 1024;
inline uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
// Little-endian, versioned data file. No ESP partition offsets or executable bytes.
class Movie {
   public:
    bool Open(const uint8_t* bytes, size_t length) {
        data_ = nullptr;
        if (!bytes || length < kHeaderBytes || length > kMaxBytes ||
            memcmp(bytes, "SGBV0001", 8) || U32(bytes + 8) != 360 || U32(bytes + 12) != 360)
            return false;
        fps = U32(bytes + 16); frames = U32(bytes + 20);
        pcm_bytes = U32(bytes + 24); rate = U32(bytes + 28);
        const size_t index = U32(bytes + 32), start = U32(bytes + 36);
        if (fps < 10 || fps > 30 || !frames || frames > fps * 10 || rate != 16000 ||
            pcm_bytes % 2 || pcm_bytes > 320000 ||
            index != kHeaderBytes + pcm_bytes || start != index + frames * kEntryBytes ||
            start > length || U32(bytes + 40) != length)
            return false;
        size_t previous = start;
        for (unsigned i = 0; i < frames; ++i) {
            const auto* entry = bytes + index + i * kEntryBytes;
            const size_t at = U32(entry), count = U32(entry + 4);
            if (at != previous || count < 4 || count > 256 * 1024 ||
                at > length || count > length - at || bytes[at] != 0xff || bytes[at + 1] != 0xd8)
                return false;
            previous = at + count;
        }
        if (previous != length || pcm_bytes / 2 > uint64_t(frames) * rate / fps)
            return false;
        data_ = bytes; index_ = index;
        return true;
    }
    const uint8_t* Frame(unsigned i, size_t* length) const {
        if (!data_ || !length || i >= frames) return nullptr;
        const auto* entry = data_ + index_ + i * kEntryBytes;
        *length = U32(entry + 4);
        return data_ + U32(entry);
    }
    const int16_t* Pcm() const { return reinterpret_cast<const int16_t*>(data_ + kHeaderBytes); }
    unsigned FrameAt(uint64_t elapsed_us) const {
        const uint64_t frame = elapsed_us * fps / 1000000;
        return frame < frames ? static_cast<unsigned>(frame) : frames - 1;
    }
    uint64_t DurationUs() const { return uint64_t(frames) * 1000000 / fps; }
    uint32_t fps = 0, frames = 0, pcm_bytes = 0, rate = 0;
   private:
    const uint8_t* data_ = nullptr;
    size_t index_ = 0;
};
}  // namespace sugarat_boot
