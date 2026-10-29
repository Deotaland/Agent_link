#pragma once
#include <cstdint>

namespace epaper {
enum class Press { kNone, kTap, kHold };
enum class Action { kUp, kDown, kCenter, kMenu, kBack };
constexpr bool InputAllowed(bool provisioning, Action) { return !provisioning; }
// Direction keys act on debounced press and repeat while held. BOOT acts once
// on press. Center distinguishes a tap from a hold without a second release action.
class Key {
public:
    Press Poll(bool down, int64_t now, bool on_press = false, bool repeat = true) {
        if (!initialized_) { initialized_ = true; raw_ = stable_ = down; armed_ = !down; changed_ = now; }
        if (raw_ != down) { raw_ = down; changed_ = now; }
        if (stable_ != raw_ && now - changed_ >= 30000) {
            stable_ = raw_;
            if (stable_) {
                pressed_ = now; next_repeat_ = now + 400000; emitted_ = false;
                if (armed_ && on_press) return Press::kTap;
            } else {
                if (!armed_) { armed_ = true; return Press::kNone; }
                if (!on_press && !emitted_) return now - pressed_ >= 1000000 ? Press::kHold : Press::kTap;
            }
        }
        if (stable_ && raw_ && armed_) {
            if (on_press && repeat && now >= next_repeat_) { next_repeat_ = now + 150000; return Press::kTap; }
            if (!on_press && !emitted_ && now - pressed_ >= 1000000) { emitted_ = true; return Press::kHold; }
        }
        return Press::kNone;
    }
private:
    bool initialized_ = false, raw_ = false, stable_ = false, armed_ = true, emitted_ = false;
    int64_t changed_ = 0, pressed_ = 0, next_repeat_ = 0;
};
constexpr int Volume(int value, int direction) {
    const int next = value + direction * 5;
    return next < 0 ? 0 : next > 100 ? 100 : next;
}
// Menu input refers to the physically displayed page, not a page still being refreshed.
constexpr bool Current(uint32_t input, uint32_t current) { return input && input == current; }
// Navigation can accumulate while the same page is being refreshed; selecting
// still requires Current(), so the selected row is visible before confirmation.
constexpr bool NavigationCurrent(uint32_t input, uint32_t first, uint32_t current) {
    return input && input >= first && input <= current;
}
class PendingConfirm {
public:
    void Request(uint32_t view) { view_ = view; }
    void Cancel() { view_ = 0; }
    bool Pending() const { return view_ != 0; }
    bool Take(uint32_t displayed, uint32_t current) {
        if (!view_) return false;
        if (current != view_) { Cancel(); return false; }
        if (displayed != view_) return false;
        Cancel(); return true;
    }
private:
    uint32_t view_ = 0;
};
}
