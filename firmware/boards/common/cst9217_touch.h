#pragma once
#include "cst9217_frame.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

class Cst9217Touch {
public:
    esp_err_t Init(i2c_port_t port, gpio_num_t reset, gpio_num_t interrupt, int width, int height,
                   bool mirror_x = false, bool mirror_y = false);
    esp_err_t Poll(cst9217::Point* point);
    // Diagnostic overload: returns the same unmodified D000 report that was
    // decoded for this sample. Raw bytes stay unchanged; Point uses the board's
    // configured orientation. No additional I2C read is performed.
    esp_err_t Poll(cst9217::Point* point, uint8_t (&raw)[10]);
    bool Ready() const { return dev_ != nullptr; }
private:
    esp_err_t Read(uint16_t reg, uint8_t* data, size_t size);
    i2c_master_dev_handle_t dev_ = nullptr;
    int width_ = 0, height_ = 0;
    bool mirror_x_ = false, mirror_y_ = false;
};
