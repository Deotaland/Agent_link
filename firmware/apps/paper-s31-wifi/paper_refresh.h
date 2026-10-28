#pragma once

#include <cstdint>
#include <cstring>

namespace paper {
struct Page {
    char lines[14][96]{};
    bool Same(const Page& other) const { return std::memcmp(lines, other.lines, sizeof lines) == 0; }
};

enum class Refresh { kSame, kWait, kDraw };

class RefreshGate {
public:
    static constexpr int64_t kSettleUs = 3000000;
    static constexpr int64_t kAutomaticUs = 180000000;
    static constexpr int64_t kImportantUs = 10000000;
    static constexpr int64_t kRetryUs = 60000000;

    Refresh Observe(const Page& page, bool important, int64_t now) {
        if (have_displayed_ && page.Same(displayed_)) {
            have_pending_ = false;
            return Refresh::kSame;
        }
        if (!have_pending_ || !page.Same(pending_)) {
            pending_ = page;
            have_pending_ = true;
            changed_at_ = now;
        }
        if (now - changed_at_ < kSettleUs || now < retry_at_) return Refresh::kWait;
        if (have_displayed_ && now - completed_at_ < (important ? kImportantUs : kAutomaticUs))
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
