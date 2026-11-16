#pragma once
#include <cstdint>

namespace sugarat_power {
// Release-only power gestures. The key held during cold boot/wake is ignored
// until it has been released, and contact bounce cannot create another action.
class PowerButton {
   public:
    enum class Event { kNone, kShort, kLong };
    Event Update(bool down, int64_t now) {
        if (down != raw_) {
            raw_ = down;
            changed_at_ = now;
        }
        if (now - changed_at_ < 40000)
            return Event::kNone;
        if (down == stable_) {
            if (!down)
                armed_ = true;
            return Event::kNone;
        }
        stable_ = down;
        if (down) {
            tracking_ = armed_;
            pressed_at_ = changed_at_;
            return Event::kNone;
        }
        armed_ = true;
        if (!tracking_)
            return Event::kNone;
        tracking_ = false;
        return changed_at_ - pressed_at_ >= 1200000 ? Event::kLong : Event::kShort;
    }

   private:
    bool raw_ = false, stable_ = false, armed_ = false, tracking_ = false;
    int64_t changed_at_ = 0, pressed_at_ = 0;
};
}  // namespace sugarat_power
