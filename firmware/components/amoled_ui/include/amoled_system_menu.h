#pragma once
#ifdef AGENT_SYSTEM_ENABLED
#include "amoled_ui.h"

// One controller/renderer shared by factory and the voice app.
class AmoledSystemMenu {
   public:
    using VolumeHandler = bool (*)(int, void*);
    void Open();
    void Close() {
        page_ = Page::kClosed;
    }
    bool Active() const {
        return page_ != Page::kClosed;
    }
    bool Handle(const amoled_ui::Input& input);
    void Render(amoled_ui::Screen& screen, uint32_t view, uint32_t navigation, int battery,
                bool charging);
    bool TakeExit() {
        const bool value = exit_;
        exit_ = false;
        return value;
    }
    bool TakeCoverRetry() {
        const bool value = cover_retry_;
        cover_retry_ = false;
        return value;
    }
    void SetCoverSummary(const char* text);
    void SetVolumeHandler(VolumeHandler handler, void* context) {
        volume_handler_ = handler;
        context_ = context;
    }

   private:
    enum class Page {
        kClosed,
        kRoot,
        kNetwork,
        kApp,
        kDevice,
        kService,
        kVolume,
        kLanguage,
        kCovers
    };
    Page page_ = Page::kClosed;
    bool exit_ = false;
    bool cover_retry_ = false;
    char cover_summary_[384]{};
    int volume_ = 70;
    VolumeHandler volume_handler_ = nullptr;
    void* context_ = nullptr;
};
#endif
