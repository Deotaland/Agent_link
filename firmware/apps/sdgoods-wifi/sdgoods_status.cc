#include "amoled_wifi_application.h"

// Keep the installed application's identity/profile while adding realtime voice.
// The shared controller's restricted policy exposes Launcher exit only through
// the pull-down system menu; the physical key starts/ends the selected call.
class SdgoodsWifiApplication final : public WaveshareAmoledWifi {
public:
    SdgoodsWifiApplication() : WaveshareAmoledWifi(true) {}
};

DECLARE_APPLICATION(SdgoodsWifiApplication);
