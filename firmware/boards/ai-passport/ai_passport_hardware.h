#pragma once

#include "es8311_audio.h"
#include "st7789_lcd.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"

enum class PassportKey { None, Up, Down, Ok };

class AiPassportHardware {
public:
    void Init();
    PassportKey ReadKey();
    St7789Lcd lcd;
    Es8311Codec codec;
    bool display_ready = false, audio_ready = false, keys_ready = false;
private:
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
};
