#pragma once

#include <cstdint>
#include <cstring>

namespace paper {
struct Page {
    char lines[14][96]{};
    bool Same(const Page& other) const { return std::memcmp(lines, other.lines, sizeof lines) == 0; }
};

enum class Refresh { kSame, kWait, kDraw };

// A failed boot probe must not disable the desktop forever or reset a missing panel in a tight loop.
class ProbeRetry {
public:
    static constexpr unsigned kAttempts = 3;
    static constexpr int64_t kIntervalUs = 5000000;
    explicit ProbeRetry(int64_t now) : retry_at_(now + kIntervalUs) {}
    bool Due(int64_t now) const { return attempts_ < kAttempts && now >= retry_at_; }
    void Attempted(int64_t now) { ++attempts_; retry_at_ = now + kIntervalUs; }
    bool Exhausted() const { return attempts_ >= kAttempts; }
private:
    unsigned attempts_ = 0;
    int64_t retry_at_;
};

class RefreshGate {
public:
    static constexpr int64_t kSettleUs = 3000000;
    static constexpr int64_t kImportantUs = 10000000;
    static constexpr int64_t kRetryUs = 60000000;

    Refresh Observe(const Page& page, bool important, int64_t now) {
        if (have_displayed_ && page.Same(displayed_)) {
            have_pending_ = false;
            return Refresh::kSame;
        }
        // Clock, battery and transient status changes ride along with the next useful frame.
        if (have_displayed_ && !important) return Refresh::kWait;
        if (!have_pending_ || !page.Same(pending_)) {
            pending_ = page;
            have_pending_ = true;
            changed_at_ = now;
        }
        if (now - changed_at_ < kSettleUs || now < retry_at_) return Refresh::kWait;
        if (have_displayed_ && now - completed_at_ < kImportantUs)
            return Refresh::kWait;
        return Refresh::kDraw;
    }
    void Complete(const Page& page, int64_t now) {
        displayed_ = page;
        have_displayed_ = true;
        have_pending_ = false;
        completed_at_ = now;
        retry_at_ = 0;
    }
    void Failed(int64_t now) { retry_at_ = now + kRetryUs; }

private:
    Page displayed_, pending_;
    bool have_displayed_ = false, have_pending_ = false;
    int64_t changed_at_ = 0, completed_at_ = 0, retry_at_ = 0;
};
}
