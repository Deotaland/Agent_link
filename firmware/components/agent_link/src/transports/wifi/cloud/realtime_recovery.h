#pragma once

#include <cstdint>

namespace agentlink::realtime {

// Count consecutive recovery failures, with a bounded budget for each unstable
// period. READY alone is insufficient: both directions must keep making progress.
class RetryBudget {
public:
    static constexpr unsigned kMaxRetries = 2;
    static constexpr int64_t kStableMs = 120000;
    static constexpr int64_t kProgressGapMs = 30000;
    bool CanRetry() const { return used_ < kMaxRetries; }
    unsigned UseRetry() { return CanRetry() ? ++used_ : 0; }
    void BeginAttempt() { stable_since_ = last_tx_ = last_rx_ = -1; }
    bool Observe(bool ready, bool tx_progress, bool rx_progress, int64_t now_ms) {
        if (!ready) { BeginAttempt(); return false; }
        if ((last_tx_ >= 0 && now_ms - last_tx_ > kProgressGapMs) ||
            (last_rx_ >= 0 && now_ms - last_rx_ > kProgressGapMs)) stable_since_ = -1;
        if (tx_progress) last_tx_ = now_ms;
        if (rx_progress) last_rx_ = now_ms;
        if (last_tx_ < 0 || last_rx_ < 0 || now_ms - last_tx_ > kProgressGapMs ||
            now_ms - last_rx_ > kProgressGapMs) {
            stable_since_ = -1; return false;
        }
        if (stable_since_ < 0) stable_since_ = now_ms;
        if (used_ && now_ms - stable_since_ >= kStableMs) {
            used_ = 0; stable_since_ = now_ms; return true;
        }
        return false;
    }
private:
    unsigned used_ = 0;
    int64_t stable_since_ = -1, last_tx_ = -1, last_rx_ = -1;
};

// Each attempt owns a fresh grant, transport and audio queues. The wait callback
// must observe cancellation and return false when the user leaves the call.
template <typename Attempt, typename Wait>
void RecoverTransport(Attempt attempt, Wait wait) {
    RetryBudget budget;
    for (;;) {
        budget.BeginAttempt();
        if (!attempt(budget)) return;
        const unsigned retry = budget.UseRetry();
        if (!retry || !wait(retry, 2000U << (retry - 1))) return;
    }
}

}  // namespace agentlink::realtime
