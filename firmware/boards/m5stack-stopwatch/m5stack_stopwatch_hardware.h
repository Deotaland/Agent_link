#pragma once

#include "co5300_panel.h"
#include "es8311_audio.h"
#include "m5ioe1.h"
#include "m5pm1.h"

class M5StopWatchHardware {
public:
    void Init(int volume);

    M5Pm1 pm1_;
    M5Ioe1 ioe_;
    Co5300Panel panel_;
    Es8311Codec codec_;
    bool i2c_ok_ = false;
    bool pm1_ok_ = false;
    bool ioe_ok_ = false;
    bool codec_ok_ = false;

private:
    void InitI2c();
    void InitPower();
    void InitExpander();
    void InitDisplay();
    void InitAudio(int volume);
};
