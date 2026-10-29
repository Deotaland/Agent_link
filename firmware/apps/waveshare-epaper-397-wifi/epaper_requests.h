#pragma once
#include <cstdint>

namespace epaper {
// A cancelled HTTP request may still finish. Its result must not replace a page
// the user has left, or become the result of a later operation.
class RequestState {
public:
    uint32_t Begin() {
        if (inflight_) return 0;
        if (++sequence_ == 0) ++sequence_;
        return active_ = inflight_ = sequence_;
    }
    void Cancel() { active_ = 0; }
    bool Busy() const { return active_ != 0; }
    bool Complete(uint32_t id) {
        if (!id || id != inflight_) return false;
        inflight_ = 0;
        const bool apply = active_ == id;
        active_ = 0;
        return apply;
    }
private:
    uint32_t sequence_ = 0, inflight_ = 0, active_ = 0;
};
}
