#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kc_audio {

// Keep enough room below the transport's 75% pacing watermark to start playback.
constexpr size_t PrebufferBytes(size_t capacity) {
    const size_t limit = capacity * 3 / 4;
    const size_t safe = limit > 2048 ? limit - 2048 : capacity / 2;
    return safe < 192000 ? safe : 192000;  // Six seconds of PCM16 mono at 16 kHz.
}

class PlaybackGate {
public:
    explicit PlaybackGate(size_t prebuffer_bytes) : threshold_(prebuffer_bytes) {}
    void Reset() { playing_ = false; empty_since_ = -1; underruns_ = 0; }
    bool Ready(size_t available, bool finished, int64_t now_ms) {
        if (!playing_) {
            if (available < threshold_ && !(finished && available)) return false;
            playing_ = true;
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

private:
    size_t threshold_;
    bool playing_ = false;
    int64_t empty_since_ = -1;
    unsigned underruns_ = 0;
};

}  // namespace kc_audio
