#include "m5ioe1.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "m5ioe1";

// 16-bit registers, low byte first; bit n-1 is IOn.
constexpr uint8_t kRegUid     = 0x00;
constexpr uint8_t kRegRev     = 0x02;
constexpr uint8_t kRegMode    = 0x03;   // 1 = output
constexpr uint8_t kRegOut     = 0x05;
constexpr uint8_t kRegIn      = 0x07;
constexpr uint8_t kRegPullUp  = 0x09;
constexpr uint8_t kRegPullDn  = 0x0B;
constexpr uint8_t kRegDrive   = 0x13;   // 1 = open drain
constexpr uint8_t kRegPwm1    = 0x1B;   // duty [11:0]; high byte bit 7 = enable, bit 6 = inverted
constexpr uint8_t kRegI2cCfg  = 0x23;   // [3:0] idle sleep in seconds, 0 = never
constexpr uint8_t kRegPwmFreq = 0x25;

constexpr int kTimeoutMs = 50;
// The chip is a small MCU; M5's own driver leaves it this long after every transaction.
constexpr uint32_t kGapUs = 500;

bool ValidIo(int io) { return io >= 1 && io <= 14; }
uint16_t Bit(int io) { return static_cast<uint16_t>(1u << (io - 1)); }
}  // namespace

esp_err_t M5Ioe1::Init(i2c_port_t port, uint8_t addr, uint32_t scl_hz) {
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t r = i2c_master_get_bus_handle(port, &bus);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "I2C port %d has no bus yet", static_cast<int>(port));
        return r;
    }

    // An idle chip may be asleep: a START wakes it, and it answers a moment later.
    bool found = false;
    for (int i = 0; i < 20 && !found; ++i) {
        found = i2c_master_probe(bus, addr, kTimeoutMs) == ESP_OK;
        if (!found) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!found) {
        ESP_LOGE(TAG, "no M5IOE1 at 0x%02x", addr);
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
    vTaskDelay(pdMS_TO_TICKS(10));

    uint16_t uid = 0;
    uint8_t  rev = 0;
    const uint8_t rev_reg = kRegRev;
    if (Read16(kRegUid, &uid) != ESP_OK ||
        i2c_master_transmit_receive(dev_, &rev_reg, 1, &rev, 1, kTimeoutMs) != ESP_OK) {
        ESP_LOGE(TAG, "M5IOE1 at 0x%02x does not answer reads", addr);
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
        return ESP_FAIL;
    }
    esp_rom_delay_us(kGapUs);

    // 100 kHz, wake on a falling edge, internal pull-ups on, never sleep.
    const uint8_t cfg[2] = {kRegI2cCfg, 0x00};
    r = i2c_master_transmit(dev_, cfg, sizeof cfg, kTimeoutMs);
    esp_rom_delay_us(kGapUs);
    if (r != ESP_OK) ESP_LOGW(TAG, "could not turn I2C sleep off: %s", esp_err_to_name(r));

    ESP_LOGI(TAG, "M5IOE1 at 0x%02x (uid %04x, fw %02x)", addr, uid, rev);
    return ESP_OK;
}

esp_err_t M5Ioe1::Read16(uint8_t reg, uint16_t* v) {
    uint8_t b[2] = {};
    const esp_err_t r = i2c_master_transmit_receive(dev_, &reg, 1, b, sizeof b, kTimeoutMs);
    esp_rom_delay_us(kGapUs);
    if (r == ESP_OK) *v = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return r;
}

esp_err_t M5Ioe1::Write16(uint8_t reg, uint16_t v) {
    const uint8_t b[3] = {reg, static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8)};
    const esp_err_t r = i2c_master_transmit(dev_, b, sizeof b, kTimeoutMs);
    esp_rom_delay_us(kGapUs);
    return r;
}

// Caller holds mtx_.
esp_err_t M5Ioe1::Update16(uint8_t reg, uint16_t mask, uint16_t bits) {
    uint16_t v = 0;
    esp_err_t r = Read16(reg, &v);
    if (r != ESP_OK) return r;
    const uint16_t nv = static_cast<uint16_t>((v & ~mask) | (bits & mask));
    return nv == v ? ESP_OK : Write16(reg, nv);
}

esp_err_t M5Ioe1::SetOutput(int io, bool level) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (!ValidIo(io)) return ESP_ERR_INVALID_ARG;
    const uint16_t b = Bit(io);
    std::lock_guard<std::mutex> lk(mtx_);
    esp_err_t r = Update16(kRegOut, b, level ? b : 0);
    if (r == ESP_OK) r = Update16(kRegDrive, b, 0);
    if (r == ESP_OK) r = Update16(kRegPullUp, b, 0);
    if (r == ESP_OK) r = Update16(kRegPullDn, b, 0);
    if (r == ESP_OK) r = Update16(kRegMode, b, b);
    if (r != ESP_OK) ESP_LOGE(TAG, "IO%d as output: %s", io, esp_err_to_name(r));
    return r;
}

esp_err_t M5Ioe1::Write(int io, bool level) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (!ValidIo(io)) return ESP_ERR_INVALID_ARG;
    const uint16_t b = Bit(io);
    std::lock_guard<std::mutex> lk(mtx_);
    const esp_err_t r = Update16(kRegOut, b, level ? b : 0);
    if (r != ESP_OK) ESP_LOGE(TAG, "IO%d write: %s", io, esp_err_to_name(r));
    return r;
}

esp_err_t M5Ioe1::SetInput(int io, bool pull_up) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (!ValidIo(io)) return ESP_ERR_INVALID_ARG;
    const uint16_t b = Bit(io);
    std::lock_guard<std::mutex> lk(mtx_);
    esp_err_t r = Update16(kRegMode, b, 0);
    if (r == ESP_OK) r = Update16(kRegPullDn, b, 0);
    if (r == ESP_OK) r = Update16(kRegPullUp, b, pull_up ? b : 0);
    return r;
}

esp_err_t M5Ioe1::Read(int io, bool* level) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (!ValidIo(io) || !level) return ESP_ERR_INVALID_ARG;
    uint16_t v = 0;
    std::lock_guard<std::mutex> lk(mtx_);
    const esp_err_t r = Read16(kRegIn, &v);
    if (r == ESP_OK) *level = (v & Bit(io)) != 0;
    return r;
}

esp_err_t M5Ioe1::SetPwmFrequency(uint16_t hz) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> lk(mtx_);
    return Write16(kRegPwmFreq, hz);
}

esp_err_t M5Ioe1::SetPwm(int channel, uint16_t duty12, bool on) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (channel < 1 || channel > 4) return ESP_ERR_INVALID_ARG;
    const uint16_t v = static_cast<uint16_t>((duty12 & 0x0FFF) | (on ? 0x8000 : 0));
    std::lock_guard<std::mutex> lk(mtx_);
    return Write16(static_cast<uint8_t>(kRegPwm1 + 2 * (channel - 1)), v);
}
