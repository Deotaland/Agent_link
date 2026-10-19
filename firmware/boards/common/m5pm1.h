#pragma once
// M5PM1: M5Stack's power management IC (charger, 3.3 V rails, 5 V boost, five GPIO) on the
// StopWatch and other M5Stack boards. Reusable driver in boards/common.
//
// Attaches to an I2C bus another driver already created. Its watchdog and I2C idle sleep survive
// an ESP32 reset (the chip is always powered), so Init() turns both off.
#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

class M5Pm1 {
public:
    // PWR_CFG bits. On the StopWatch the DCDC feeds the ESP32 and the LDO the RTC and IMU.
    static constexpr uint8_t kPwrCharge = 1u << 0;
    static constexpr uint8_t kPwrDcdc   = 1u << 1;
    static constexpr uint8_t kPwrLdo    = 1u << 2;
    static constexpr uint8_t kPwrBoost  = 1u << 3;   // 5 V out on the Grove port
    static constexpr uint8_t kPwrLed    = 1u << 4;   // LED_EN idle level high

    M5Pm1() = default;
    M5Pm1(const M5Pm1&) = delete;
    M5Pm1& operator=(const M5Pm1&) = delete;

    esp_err_t Init(i2c_port_t port, uint8_t addr = 0x6E, uint32_t scl_hz = 100000);
    bool Ready() const { return dev_ != nullptr; }

    // Sets the given PWR_CFG bits, leaving the others as they are.
    esp_err_t EnablePower(uint8_t bits);

    esp_err_t BatteryMv(uint16_t* mv);   // battery voltage
    esp_err_t VinMv(uint16_t* mv);       // USB input voltage

    // PM1 G0..G4 as plain inputs (a charger's status line, say).
    esp_err_t SetGpioInput(int pin);
    esp_err_t ReadGpio(int pin, bool* level);

    esp_err_t PowerOff();

private:
    esp_err_t ReadRegs(uint8_t reg, uint8_t* out, size_t len);
    esp_err_t WriteReg(uint8_t reg, uint8_t val);
    esp_err_t UpdateReg(uint8_t reg, uint8_t mask, uint8_t bits);

    i2c_master_dev_handle_t dev_ = nullptr;
};
