#pragma once

#include <stddef.h>
#include <stdint.h>
#include <atomic>

namespace kc_audio {

// Keep the mic live, but exclude speaker output and its tail from continuous uploads.
class SpeakerEchoGuard {
public:
    void OnWrite(int64_t now_us, size_t samples) {
        dma_until_.store(now_us + static_cast<int64_t>(samples) * 1000000 / 16000 + 100000);
    }
    bool Drained(int64_t now_us) const { return now_us >= dma_until_.load(); }
    bool Muted(int64_t now_us) const {
        const int64_t until = dma_until_.load();
        return until && now_us < until + 200000;
    }
private:
    std::atomic<int64_t> dma_until_{0};
};

// PCM16 mono at 16 kHz is 32 bytes per millisecond. Use a longer cushion only
// after a network gap, while keeping both thresholds reachable in the fallback buffer.
constexpr size_t PrebufferBytes(size_t capacity, size_t target_ms) {
    const size_t target = target_ms * 32;
    return target < capacity / 2 ? target : capacity / 2;
}

class PlaybackGate {
public:
    PlaybackGate(size_t start_bytes, size_t resume_bytes)
        : start_bytes_(start_bytes), resume_bytes_(resume_bytes) {}
    void Reset() { playing_ = false; started_ = false; empty_since_ = -1; underruns_ = 0; }
    bool Ready(size_t available, bool finished, int64_t now_ms) {
        if (!playing_) {
            const size_t threshold = started_ ? resume_bytes_ : start_bytes_;
            if (available < threshold && !(finished && available)) return false;
            playing_ = true;
            started_ = true;
        }
        if (available) { empty_since_ = -1; return true; }
        if (finished) { playing_ = false; empty_since_ = -1; return false; }
        if (empty_since_ < 0) empty_since_ = now_ms;
        if (now_ms - empty_since_ >= 80) {
            playing_ = false;
            empty_since_ = -1;
            ++underruns_;
        }
        return false;
    }
    unsigned Underruns() const { return underruns_; }
    bool Playing() const { return playing_; }

private:
    size_t start_bytes_;
    size_t resume_bytes_;
    bool playing_ = false;
    bool started_ = false;
    int64_t empty_since_ = -1;
    unsigned underruns_ = 0;
};

}  // namespace kc_audio
