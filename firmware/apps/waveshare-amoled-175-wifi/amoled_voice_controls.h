#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <atomic>

namespace amoled_voice {
// Ignore a key held across boot; produce one event per debounced press.
class ButtonDebounce {
public:
    bool Update(bool down, int64_t now) {
        if (down != raw_) { raw_ = down; changed_at_ = now; }
        if (now - changed_at_ < 40000) return false;
        if (!down) { armed_ = true; stable_down_ = false; return false; }
        if (stable_down_) return false;
        stable_down_ = true;
        if (!armed_) return false;
        armed_ = false;
        return true;
    }
private:
    bool raw_ = false, stable_down_ = false, armed_ = false;
    int64_t changed_at_ = 0;
};

enum class Swipe { kNone, kLeft, kRight, kHome };
class SwipeTracker {
public:
    Swipe Update(bool down, int x, int y, uint32_t view, int64_t now, bool blocked = false) {
        if (!down) { tracking_ = false; return Swipe::kNone; }
        if (!tracking_) {
            tracking_ = true; consumed_ = blocked; x_ = x; y_ = y; view_ = view; at_ = now;
            return Swipe::kNone;
        }
        const int dx = x - x_, dy = std::abs(y - y_);
        if (view != view_ || now - at_ > 1200000) consumed_ = true;
        if (!consumed_ && y_ >= 400 && y_ <= 438 && x_ >= 150 && x_ <= 316 &&
            y_ - y >= 55 && y_ - y > std::abs(dx) * 2) {
            consumed_ = true; return Swipe::kHome;
        }
        // A vertical contact in the content area belongs to native scrolling.
        if (dy > 55) consumed_ = true;
        if (consumed_ || std::abs(dx) < 70 || std::abs(dx) < dy * 2) return Swipe::kNone;
        consumed_ = true;
        return dx > 0 ? Swipe::kRight : Swipe::kLeft;
    }
private:
    bool tracking_ = false, consumed_ = false;
    int x_ = 0, y_ = 0;
    uint32_t view_ = 0;
    int64_t at_ = 0;
};

// The last drag value wins, but a release survives later drag samples. No NVS or
// codec work runs on the LVGL task; the controller drains this bounded mailbox.
class VolumeMailbox {
public:
    void Submit(int value, bool commit) {
        value = value < 0 ? 0 : value > 100 ? 100 : value;
        uint32_t old = pending_.load(), next;
        do {
            next = (old & 0x1FF00u) | 0x80u | static_cast<uint32_t>(value);
            if (commit) next = (next & 0xFFu) | 0x10000u | (static_cast<uint32_t>(value) << 8);
        } while (!pending_.compare_exchange_weak(old, next));
    }
    bool Take(int& value, int& saved) {
        const uint32_t data = pending_.exchange(0);
        value = static_cast<int>(data & 0x7Fu);
        saved = data & 0x10000u ? static_cast<int>((data >> 8) & 0x7Fu) : -1;
        return data & 0x80u;
    }
private:
    std::atomic<uint32_t> pending_{0};
};
inline bool FollowReply(int remaining, bool touching) { return remaining <= 24 && !touching; }
inline int WindowScroll(int previous_y, size_t previous_offset, size_t offset, int row_height) {
    const int64_t y = previous_y + (static_cast<int64_t>(previous_offset) - static_cast<int64_t>(offset)) * row_height;
    return y < 0 ? 0 : static_cast<int>(y);
}
}  // namespace amoled_voice
