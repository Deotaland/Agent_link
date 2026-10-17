#pragma once
#include "cst820_frame.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

class Cst820Touch {
public:
    esp_err_t Init(i2c_port_t port, int width, int height);
    esp_err_t Poll(cst820::Point* point);
    bool Ready() const { return dev_ != nullptr; }
private:
    i2c_master_dev_handle_t dev_ = nullptr;
    int width_ = 0, height_ = 0;
};
