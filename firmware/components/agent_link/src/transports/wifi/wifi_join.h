#pragma once

#include <stdint.h>

namespace agentlink::wifi {

class JoinWindow {
public:
    static constexpr int64_t kTimeoutMs = 20000;
    static constexpr int64_t kRetryMs = 2000;

    void Begin(int64_t now_ms) {
        deadline_ = now_ms + kTimeoutMs;
        retry_at_ = 0;
        attempts_ = 1;
        waiting_ip_ = false;
    }
    void Stop() { deadline_ = retry_at_ = 0; waiting_ip_ = false; }
    bool Active() const { return deadline_ != 0; }
    bool Expired(int64_t now_ms) const { return Active() && now_ms >= deadline_; }
    void Failed(int64_t now_ms) {
        if (!Active()) return;
        waiting_ip_ = false;
        retry_at_ = now_ms + kRetryMs;
    }
    void Associated() { waiting_ip_ = true; retry_at_ = 0; }
    bool WaitingIp() const { return waiting_ip_; }
    bool TakeRetry(int64_t now_ms) {
        if (!Active() || Expired(now_ms) || !retry_at_ || now_ms < retry_at_) return false;
        retry_at_ = 0;
        ++attempts_;
        return true;
    }
    unsigned Attempts() const { return attempts_; }
    unsigned RemainingSeconds(int64_t now_ms) const {
        return !Active() || Expired(now_ms) ? 0 : static_cast<unsigned>((deadline_ - now_ms + 999) / 1000);
    }

private:
    int64_t deadline_ = 0;
    int64_t retry_at_ = 0;
    unsigned attempts_ = 0;
    bool waiting_ip_ = false;
};

}  // namespace agentlink::wifi
