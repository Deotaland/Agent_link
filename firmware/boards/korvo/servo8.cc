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
    if (!dev_ || channel < 1 || channel > 8 || angle > 180) return ESP_ERR_INVALID_ARG;
    // HC32L110 firmware v2: [channel 0..7 for SS1..SS8][angle][reserved].
    const uint8_t frame[3] = {static_cast<uint8_t>(channel - 1), angle, 0};
    return i2c_master_transmit(dev_, frame, sizeof(frame), kTimeoutMs);
}
