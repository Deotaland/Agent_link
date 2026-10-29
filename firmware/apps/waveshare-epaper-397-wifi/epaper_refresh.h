#pragma once
#include <cstdint>
#include "epaper_frame.h"

namespace epaper {
enum class Scene { kIdle, kSetup, kConnecting, kActivation, kBlocked, kOnline };
// Screen updates are serialized by the display worker, not by a fixed refresh
// interval. Failed transfers invalidate base RAM and retry with a full image.
class RefreshPolicy {
public:
    bool Ready(int64_t now) const { return now >= retry_at_; }
    bool Full(Scene scene, int page, const Rect& changed) const {
        const int area = (changed.x1 - changed.x0) * (changed.y1 - changed.y0);
        return !valid_ || partials_ >= 64 || scene != scene_ || page != page_ ||
               area >= kPanelWidth * kPanelHeight / 3;
    }
    void Success(bool full, Scene scene, int page) {
        valid_ = true; failures_ = 0; retry_at_ = 0;
        scene_ = scene; page_ = page;
        partials_ = full ? 0 : partials_ + 1;
    }
    void Failure(int64_t now) {
        valid_ = false;
        if (failures_ < 4) ++failures_;
        retry_at_ = now + (500000LL << (failures_ - 1));
    }
private:
    bool valid_ = false;
    Scene scene_ = Scene::kIdle;
    int page_ = 0;
    unsigned partials_ = 0, failures_ = 0;
    int64_t retry_at_ = 0;
};
}
