#pragma once
#include <cstdint>

namespace paper {
enum class Press { None, Tap, Hold };

// Emit one hold at one second, without a second tap on release. Ignore a key held at boot.
class Button {
public:
    Press Poll(bool down, int64_t now) {
        if (!initialized_) {
            initialized_ = true; raw_ = stable_ = down; armed_ = !down; changed_ = now;
        }
        if (down != raw_) { raw_ = down; changed_ = now; }
        if (stable_ != raw_ && now - changed_ >= 30000) {
            stable_ = raw_;
            if (stable_) { pressed_ = now; held_ = false; }
            else {
                if (!armed_) { armed_ = true; return Press::None; }
                if (!held_) return now - pressed_ >= 1000000 ? Press::Hold : Press::Tap;
            }
        }
        if (armed_ && stable_ && raw_ && !held_ && now - pressed_ >= 1000000) {
            held_ = true; return Press::Hold;
        }
        return Press::None;
    }
private:
    bool initialized_ = false, raw_ = false, stable_ = false, armed_ = true, held_ = false;
    int64_t changed_ = 0, pressed_ = 0;
};

class PendingConfirm {
public:
    void Request(uint32_t view) { view_ = view; }
    void Cancel() { view_ = 0; }
    bool Take(uint32_t displayed, uint32_t current) {
        if (!view_) return false;
        if (view_ != current) { Cancel(); return false; }
        if (displayed < view_) return false;
        Cancel(); return true;
    }
private:
    uint32_t view_ = 0;
};
}
