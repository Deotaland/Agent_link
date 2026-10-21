#pragma once
#include <cstdint>

namespace epaper {
enum class Press { kNone, kTap, kHold };
// A release produces exactly one action; chatter and boot-held buttons cannot select a menu item.
class Key {
public:
    Press Poll(bool down, int64_t now) {
        if (!initialized_) { initialized_ = true; raw_ = stable_ = down; armed_ = !down; changed_ = now; }
        if (raw_ != down) { raw_ = down; changed_ = now; }
        if (stable_ == raw_ || now - changed_ < 30000) return Press::kNone;
        stable_ = raw_;
        if (stable_) { pressed_ = now; return Press::kNone; }
        if (!armed_) { armed_ = true; return Press::kNone; }
        return now - pressed_ >= 1000000 ? Press::kHold : Press::kTap;
    }
private:
    bool initialized_ = false, raw_ = false, stable_ = false, armed_ = true;
    int64_t changed_ = 0, pressed_ = 0;
};
constexpr int Volume(int value, int direction) {
    const int next = value + direction * 5;
    return next < 0 ? 0 : next > 100 ? 100 : next;
}
// Menu input refers to the physically displayed page, not a page still being refreshed.
constexpr bool Current(uint32_t input, uint32_t current) { return input && input == current; }
}
