#pragma once
#include <cstdint>

namespace watch_wifi {
enum class TouchAction { kCall, kSessions, kSettings, kRow, kBack, kPrevious, kNext };
struct Input { TouchAction action; int row = 0; uint32_t view = 0; int64_t at = 0; };
inline bool CurrentInput(const Input& input, uint32_t view, int64_t now) {
    return input.view == view && now >= input.at && now - input.at < 500000;
}
// Poll at 20 ms. A new level must persist for two samples; hold repeats volume only.
class VolumeKey {
public:
    bool Poll(bool pressed, int64_t now) {
        if (pressed == down_) stable_ = 0;
        else if (++stable_ >= 2) {
            stable_ = 0; down_ = pressed;
            repeat_at_ = now + 500000;
            if (down_) return true;
        }
        if (down_ && pressed && now >= repeat_at_) {
            repeat_at_ = now + 150000;
            return true;
        }
        return false;
    }
    bool Down() const { return down_; }
private:
    bool down_ = false;
    int stable_ = 0;
    int64_t repeat_at_ = 0;
};
inline int AdjustVolume(int volume, int direction) {
    const int next = volume + direction * 5;
    return next < 0 ? 0 : next > 100 ? 100 : next;
}
}  // namespace watch_wifi
