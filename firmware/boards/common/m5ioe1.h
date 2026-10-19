#pragma once
// M5IOE1: M5Stack's I2C IO expander (14 GPIO, 4 PWM channels). M5Stack boards put power rails,
// resets and amp enables on it. Reusable driver in boards/common.
//
// Attaches to an I2C bus another driver already created. Pins are IO1..IO14, as the schematics
// name them (PYG1..PYG14). A mutex keeps read-modify-writes from different tasks apart.
#include <cstdint>
#include <mutex>

#include "driver/i2c_master.h"
#include "esp_err.h"

class M5Ioe1 {
public:
    M5Ioe1() = default;
    M5Ioe1(const M5Ioe1&) = delete;
    M5Ioe1& operator=(const M5Ioe1&) = delete;

    // Wakes the chip, checks it answers and turns its I2C idle sleep off.
    esp_err_t Init(i2c_port_t port, uint8_t addr = 0x4F, uint32_t scl_hz = 100000);
    bool Ready() const { return dev_ != nullptr; }

    // Push-pull output. The level is latched before the pin turns into an output.
    esp_err_t SetOutput(int io, bool level);
    esp_err_t Write(int io, bool level);
    esp_err_t SetInput(int io, bool pull_up);
    esp_err_t Read(int io, bool* level);

    // PWM channel 1-4 drives IO9, IO8, IO11, IO10; the pin must be an output. One frequency for all.
    esp_err_t SetPwmFrequency(uint16_t hz);
    esp_err_t SetPwm(int channel, uint16_t duty12, bool on);   // duty 0-4095

private:
    esp_err_t Read16(uint8_t reg, uint16_t* v);
    esp_err_t Write16(uint8_t reg, uint16_t v);
    esp_err_t Update16(uint8_t reg, uint16_t mask, uint16_t bits);

    i2c_master_dev_handle_t dev_ = nullptr;
    std::mutex              mtx_;
};
