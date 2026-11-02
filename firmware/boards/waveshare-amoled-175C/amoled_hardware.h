#pragma once
#include "co5300_panel.h"
#include "amoled_audio.h"
#include "cst9217_touch.h"

class AmoledHardware {
public:
    void Init(int volume);
    void InitDisplayAndTouch();  // Offline diagnostics: do not initialize audio.
    esp_err_t SetSpeakerEnabled(bool enabled);
    void Battery(int* percent, bool* charging);
    bool ButtonDown() const;
    Co5300Panel panel_;
    AmoledAudio codec_;
    Cst9217Touch touch_;
    bool codec_ok_ = false, power_ok_ = false, bus_ok_ = false, button_ok_ = false;
private:
    i2c_master_dev_handle_t pmu_ = nullptr;
};
