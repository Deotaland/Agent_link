#include "servo8.h"

#include "hardware_config.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "kc_servo8";
constexpr int kTimeoutMs = 100;
}

esp_err_t KorvoCloudServo8::Init() {
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = static_cast<i2c_port_t>(SERVO_I2C_PORT);
    bus.sda_io_num = SERVO_I2C_SDA;
    bus.scl_io_num = SERVO_I2C_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    // The module has 4.7k pull-ups on its host side.
    bus.flags.enable_internal_pullup = false;
    esp_err_t err = i2c_new_master_bus(&bus, &bus_);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = SERVO_I2C_ADDR;
    dev.scl_speed_hz = SERVO_I2C_FREQ_HZ;
    err = i2c_master_bus_add_device(bus_, &dev, &dev_);
    if (err != ESP_OK) {
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
        return err;
    }

    err = i2c_master_probe(bus_, SERVO_I2C_ADDR, kTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "servo module at 0x%02x did not answer yet: %s",
                 SERVO_I2C_ADDR, esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "servo module ready at 0x%02x on I2C%d", SERVO_I2C_ADDR, SERVO_I2C_PORT);
    }
    return ESP_OK;
}

esp_err_t KorvoCloudServo8::SetAngle(uint8_t channel, uint8_t angle) {
    return Write(channel, Mode::Position180, angle);
}

bool KorvoCloudServo8::Valid(uint8_t channel, Mode mode, int value) {
    if (channel != kAll && (channel < 1 || channel > kChannels)) return false;
    switch (mode) {
    case Mode::Released: return value == 0;
    case Mode::Position180: return value >= 0 && value <= 180;
    case Mode::Position360: return value >= 0 && value <= 360;
    case Mode::Continuous: return value >= -100 && value <= 100;
    default: return false;
    }
}

esp_err_t KorvoCloudServo8::Write(uint8_t channel, Mode mode, int16_t value) {
    if (!Valid(channel, mode, value)) return ESP_ERR_INVALID_ARG;
    if (!dev_) return ESP_ERR_INVALID_STATE;
    const uint16_t raw = static_cast<uint16_t>(value);
    const uint8_t frame[4] = {
        channel == kAll ? kAll : static_cast<uint8_t>(channel - 1),
        static_cast<uint8_t>(mode), static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw)};
    return i2c_master_transmit(dev_, frame, sizeof(frame), kTimeoutMs);
}

esp_err_t KorvoCloudServo8::ReadStatus(Status& status) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    uint8_t raw[4 + kChannels * 3];
    const esp_err_t err = i2c_master_receive(dev_, raw, sizeof(raw), kTimeoutMs);
    if (err != ESP_OK) return err;
    const auto decode = [](const uint8_t* bytes) -> int16_t {
        const uint16_t value = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
        return value <= 0x7fff ? static_cast<int16_t>(value) :
            static_cast<int16_t>(static_cast<int32_t>(value) - 65536);
    };
    Status next = {};
    next.last_id = raw[0];
    next.last_mode = static_cast<Mode>(raw[1]);
    next.last_value = decode(raw + 2);
    if ((next.last_id != kAll && next.last_id >= kChannels) ||
        !Valid(kAll, next.last_mode, next.last_value)) return ESP_ERR_INVALID_RESPONSE;
    for (uint8_t i = 0; i < kChannels; ++i) {
        next.channels[i] = {static_cast<Mode>(raw[4 + i * 3]), decode(raw + 5 + i * 3)};
        if (!Valid(i + 1, next.channels[i].mode, next.channels[i].value))
            return ESP_ERR_INVALID_RESPONSE;
    }
    status = next;
    return ESP_OK;
}
