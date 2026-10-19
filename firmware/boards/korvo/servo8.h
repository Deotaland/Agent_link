#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#include <cstdint>

class KorvoCloudServo8 {
public:
    static constexpr uint8_t kChannels = 8;
    static constexpr uint8_t kAll = 0xff;
    enum class Mode : uint8_t { Released, Position180, Position360, Continuous };
    struct ChannelState {
        Mode mode;
        int16_t value;
    };
    struct Status {
        uint8_t last_id;
        Mode last_mode;
        int16_t last_value;
        ChannelState channels[kChannels];
    };
    struct Command {
        uint8_t channel;
        Mode mode;
        int16_t value;
        bool read_only = false;
    };
    static bool Valid(uint8_t channel, Mode mode, int value);
    esp_err_t Init();
    esp_err_t SetAngle(uint8_t channel, uint8_t angle);
    esp_err_t Write(uint8_t channel, Mode mode, int16_t value);
    esp_err_t ReadStatus(Status& status);

private:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
};
