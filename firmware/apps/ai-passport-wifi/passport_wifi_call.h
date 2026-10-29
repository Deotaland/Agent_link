#pragma once

#include <cstdint>

namespace passport_wifi {

// The SDK owns transport retries. Leaving READY pauses capture, not the call.
class CallState {
public:
    enum class Phase { Idle, Starting, Active, Recovering, Closing };
    enum class Action { None, StartMic, Recovering, Closing, Resumed, MicStopped, TransportEnded, StartTimeout };
    void Start(int64_t now) { phase_ = Phase::Starting; started_at_ = now; }
    void Stop() { phase_ = Phase::Idle; }
    bool Calling() const { return phase_ != Phase::Idle; }
    bool Active() const { return phase_ == Phase::Active; }
    bool Recovering() const { return phase_ == Phase::Recovering; }
    bool Closing() const { return phase_ == Phase::Closing; }
    int64_t StartedAt() const { return started_at_; }
    Action Poll(bool ready, bool running, bool recovering, bool mic_enabled, int64_t now) {
        if (!Calling()) return Action::None;
        if (!running) return Action::TransportEnded;
        if (phase_ != Phase::Starting && !mic_enabled) return Action::MicStopped;
        if (ready) {
            const Phase before = phase_;
            phase_ = Phase::Active;
            return before == Phase::Starting ? Action::StartMic :
                   before == Phase::Recovering ? Action::Resumed : Action::None;
        }
        // The initial grant has a deadline; an established call has no age limit.
        if (phase_ == Phase::Starting)
            return now - started_at_ >= 30000000 ? Action::StartTimeout : Action::None;
        const Phase next = recovering ? Phase::Recovering : Phase::Closing;
        if (phase_ != next) {
            phase_ = next;
            return recovering ? Action::Recovering : Action::Closing;
        }
        return Action::None;
    }
private:
    Phase phase_ = Phase::Idle;
    int64_t started_at_ = 0;
};

}  // namespace passport_wifi
