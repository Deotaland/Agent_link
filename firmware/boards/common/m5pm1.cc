#include "m5pm1.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "m5pm1";

constexpr uint8_t kRegId       = 0x00;   // 2 bytes, 0x2050
constexpr uint8_t kRegPwrCfg   = 0x06;
constexpr uint8_t kRegI2cCfg   = 0x09;   // [3:0] idle sleep in seconds, 0 = never
constexpr uint8_t kRegWdtCnt   = 0x0A;   // seconds, 0 = off
constexpr uint8_t kRegSysCmd   = 0x0C;
constexpr uint8_t kRegGpioMode = 0x10;   // 1 = output
constexpr uint8_t kRegGpioIn   = 0x12;
constexpr uint8_t kRegGpioFn0  = 0x16;   // 2 bits per pin, G0..G3; 00 = GPIO
constexpr uint8_t kRegGpioFn1  = 0x17;   // G4
constexpr uint8_t kRegVbat     = 0x22;   // mV, low byte first
constexpr uint8_t kRegVin      = 0x24;

constexpr uint16_t kDeviceId = 0x2050;
constexpr uint8_t  kCmdShutdown = 0xA1;
constexpr int      kTimeoutMs = 50;
// The chip is a small MCU; M5's own driver leaves it this long after every transaction.
constexpr uint32_t kGapUs = 500;
}  // namespace

esp_err_t M5Pm1::Init(i2c_port_t port, uint8_t addr, uint32_t scl_hz) {
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t r = i2c_master_get_bus_handle(port, &bus);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "I2C port %d has no bus yet", static_cast<int>(port));
        return r;
    }

    // Asleep after an idle spell, a START wakes it.
    bool found = false;
    for (int i = 0; i < 20 && !found; ++i) {
        found = i2c_master_probe(bus, addr, kTimeoutMs) == ESP_OK;
        if (!found) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!found) {
        ESP_LOGE(TAG, "no M5PM1 at 0x%02x", addr);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address  = addr;
    dc.scl_speed_hz    = scl_hz;
    r = i2c_master_bus_add_device(bus, &dc, &dev_);
    if (r != ESP_OK) {
        dev_ = nullptr;
        return r;
    }

    uint8_t id[2] = {};
    r = ReadRegs(kRegId, id, sizeof id);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "M5PM1 at 0x%02x does not answer reads", addr);
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
        return r;
    }
    const uint16_t got = static_cast<uint16_t>(id[0] | (id[1] << 8));
    if (got != kDeviceId) ESP_LOGW(TAG, "device id %04x, expected %04x", got, kDeviceId);

    // A previous firmware may have left either running.
    if (WriteReg(kRegI2cCfg, 0x00) != ESP_OK) ESP_LOGW(TAG, "could not turn I2C sleep off");
    if (WriteReg(kRegWdtCnt, 0x00) != ESP_OK) ESP_LOGW(TAG, "could not stop the watchdog");

    ESP_LOGI(TAG, "M5PM1 at 0x%02x", addr);
    return ESP_OK;
}

esp_err_t M5Pm1::ReadRegs(uint8_t reg, uint8_t* out, size_t len) {
    const esp_err_t r = i2c_master_transmit_receive(dev_, &reg, 1, out, len, kTimeoutMs);
    esp_rom_delay_us(kGapUs);
    return r;
}

esp_err_t M5Pm1::WriteReg(uint8_t reg, uint8_t val) {
    const uint8_t b[2] = {reg, val};
    const esp_err_t r = i2c_master_transmit(dev_, b, sizeof b, kTimeoutMs);
    esp_rom_delay_us(kGapUs);
    return r;
}

esp_err_t M5Pm1::UpdateReg(uint8_t reg, uint8_t mask, uint8_t bits) {
    uint8_t v = 0;
    const esp_err_t r = ReadRegs(reg, &v, 1);
    if (r != ESP_OK) return r;
    const uint8_t nv = static_cast<uint8_t>((v & ~mask) | (bits & mask));
    return nv == v ? ESP_OK : WriteReg(reg, nv);
}

esp_err_t M5Pm1::EnablePower(uint8_t bits) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    return UpdateReg(kRegPwrCfg, bits, bits);
}

esp_err_t M5Pm1::BatteryMv(uint16_t* mv) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = {};
    const esp_err_t r = ReadRegs(kRegVbat, b, sizeof b);
    if (r == ESP_OK && mv) *mv = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return r;
}

esp_err_t M5Pm1::VinMv(uint16_t* mv) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = {};
    const esp_err_t r = ReadRegs(kRegVin, b, sizeof b);
    if (r == ESP_OK && mv) *mv = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return r;
}

esp_err_t M5Pm1::SetGpioInput(int pin) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (pin < 0 || pin > 4) return ESP_ERR_INVALID_ARG;
    const uint8_t fn_reg = pin < 4 ? kRegGpioFn0 : kRegGpioFn1;
    const uint8_t shift  = static_cast<uint8_t>((pin < 4 ? pin : 0) * 2);
    esp_err_t r = UpdateReg(fn_reg, static_cast<uint8_t>(0x03 << shift), 0);
    if (r == ESP_OK) r = UpdateReg(kRegGpioMode, static_cast<uint8_t>(1u << pin), 0);
    return r;
}

esp_err_t M5Pm1::ReadGpio(int pin, bool* level) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (pin < 0 || pin > 4 || !level) return ESP_ERR_INVALID_ARG;
    uint8_t v = 0;
    const esp_err_t r = ReadRegs(kRegGpioIn, &v, 1);
    if (r == ESP_OK) *level = (v & (1u << pin)) != 0;
    return r;
}

esp_err_t M5Pm1::PowerOff() {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    return WriteReg(kRegSysCmd, kCmdShutdown);
}
