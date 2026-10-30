#pragma once
#include <cstdint>
#include "cst9217_frame.h"

namespace amoled_touch_test {
constexpr int kCenter = 233, kContentInset = 25, kContentSize = 416;
constexpr int kRingInset = 7, kRingSize = 452, kRingWidth = 10;
constexpr int kTargetRadius = 22;
struct Target { int x, y; const char* name; };
constexpr Target kTargets[]{{233, 252, "中心"}, {233, 194, "上方"}, {311, 252, "右侧"},
                            {233, 316, "下方"}, {155, 252, "左侧"}};
constexpr unsigned kTargetCount = sizeof kTargets / sizeof kTargets[0];
constexpr bool InCircle(int x, int y) {
    return (x - kCenter) * (x - kCenter) + (y - kCenter) * (y - kCenter) <= 208 * 208;
}
constexpr bool TargetsFit() {
    for (const auto& t : kTargets)
        if (!InCircle(t.x - kTargetRadius, t.y - kTargetRadius) ||
            !InCircle(t.x + kTargetRadius, t.y - kTargetRadius) ||
            !InCircle(t.x - kTargetRadius, t.y + kTargetRadius) ||
            !InCircle(t.x + kTargetRadius, t.y + kTargetRadius)) return false;
    return true;
}
static_assert(TargetsFit(), "Touch targets must remain inside the circular content mask");

enum class Read { kValid, kInvalidReport, kBusError };
enum class Event { kNone, kDown, kMove, kUp };
// Cancel a click after a data gap, then require an observed release before
// accepting another UI press. Recovery while still held cannot click Clear.
class PointerGate {
public:
    bool Update(bool stale, bool pressed, bool valid) {
        const bool cancel = stale && !stale_;
        if (cancel) blocked_ = true;
        if (valid && !pressed) blocked_ = false;
        stale_ = stale;
        return cancel;
    }
    bool Down(bool pressed) const { return pressed && !stale_ && !blocked_; }
private:
    bool stale_ = true, blocked_ = false;
};
class Monitor {
public:
    Event Observe(Read result, const cst9217::Point& point, int64_t now_ms) {
        if (result == Read::kInvalidReport) { ++invalid_; return Event::kNone; }
        if (result == Read::kBusError) { ++bus_errors_; return Event::kNone; }
        ++valid_; seen_ = true; valid_at_ = now_ms;
        if (!point.pressed) {
            if (!pressed_) return Event::kNone;
            pressed_ = false; ++ups_; return Event::kUp;
        }
        const bool moved = point.x != x_ || point.y != y_;
        x_ = point.x; y_ = point.y;
        if (pressed_) { if (moved) { ++moves_; return Event::kMove; } return Event::kNone; }
        pressed_ = true; ++downs_;
        if (next_ < kTargetCount) {
            const auto& t = kTargets[next_];
            const int dx = x_ - t.x, dy = y_ - t.y;
            if (dx * dx + dy * dy <= kTargetRadius * kTargetRadius) ++next_;
        }
        return Event::kDown;
    }
    bool Stale(int64_t now_ms) const { return !seen_ || now_ms - valid_at_ >= 250; }
    bool Pressed() const { return pressed_; }
    unsigned X() const { return x_; }
    unsigned Y() const { return y_; }
    uint32_t Downs() const { return downs_; }
    uint32_t Ups() const { return ups_; }
    uint32_t Moves() const { return moves_; }
    uint32_t Valid() const { return valid_; }
    uint32_t Invalid() const { return invalid_; }
    uint32_t BusErrors() const { return bus_errors_; }
    unsigned NextTarget() const { return next_; }
    bool Complete() const { return next_ == kTargetCount; }
    void Reset() { *this = Monitor{}; }
private:
    bool pressed_ = false, seen_ = false;
    uint16_t x_ = 0, y_ = 0;
    unsigned next_ = 0;
    int64_t valid_at_ = 0;
    uint32_t downs_ = 0, ups_ = 0, moves_ = 0, valid_ = 0, invalid_ = 0, bus_errors_ = 0;
};
}  // namespace amoled_touch_test
