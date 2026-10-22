#pragma once

namespace agentlink::realtime {

// Each attempt owns a fresh grant, transport and audio queues. The wait callback
// must observe cancellation and return false when the user leaves the call.
template <typename Attempt, typename Wait>
void RecoverTransport(Attempt attempt, Wait wait) {
    constexpr unsigned max_retries = 2;
    for (unsigned retries = 0;; ++retries) {
        if (!attempt(retries < max_retries) || retries == max_retries) return;
        if (!wait(retries + 1, 2000U << retries)) return;
    }
}

}  // namespace agentlink::realtime
