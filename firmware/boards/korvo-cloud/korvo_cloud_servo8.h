#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#include <cstdint>

class KorvoCloudServo8 {
public:
    esp_err_t Init();
    esp_err_t SetSs1Angle(uint8_t angle);

private:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
};
