#pragma once

#include "config.h"
#include "es8311_audio.h"

class EpaperHardware {
public:
    esp_err_t Init(int volume);
    esp_err_t Speaker(bool enabled) { return gpio_set_level(AUDIO_PA, enabled); }
    esp_err_t Battery(int* percent, bool* charging);
    esp_err_t Display(const uint8_t* frame, bool full);
    bool DisplayReady() const { return spi_ != nullptr; }
    bool AudioReady() const { return audio_ok_; }
    Es8311Codec codec;
private:
    esp_err_t Read(uint8_t reg, uint8_t* value);
    esp_err_t Update(uint8_t reg, uint8_t mask, uint8_t value);
    esp_err_t Send(bool data, const uint8_t* bytes, size_t length);
    esp_err_t Command(uint8_t command, const uint8_t* data = nullptr, size_t length = 0);
    esp_err_t Wait();
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t pmic_ = nullptr;
    spi_device_handle_t spi_ = nullptr;
    bool audio_ok_ = false;
};
