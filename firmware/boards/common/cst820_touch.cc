#include "cst820_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t Cst820Touch::Init(i2c_port_t port, int width, int height) {
    if (dev_) return ESP_ERR_INVALID_STATE;
    if (width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t error = i2c_master_get_bus_handle(port, &bus);
    if (error != ESP_OK) return error;
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = 0x15;
    config.scl_speed_hz = 100000;
    error = i2c_master_bus_add_device(bus, &config, &dev_);
    if (error != ESP_OK) { dev_ = nullptr; return error; }
    // The expander already reset the touch IC. Allow bounded boot retries.
    uint8_t id = 0, version = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const uint8_t chip = 0xA7, firmware = 0xA9;
        error = i2c_master_transmit_receive(dev_, &chip, 1, &id, 1, 30);
        if (error == ESP_OK) error = i2c_master_transmit_receive(dev_, &firmware, 1, &version, 1, 30);
        if (error == ESP_OK && id && version && id != 0xFF && version != 0xFF) break;
        error = ESP_ERR_INVALID_RESPONSE;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (error != ESP_OK) {
        i2c_master_bus_rm_device(dev_); dev_ = nullptr;
        ESP_LOGE("cst820", "touch identification failed");
        return error;
    }
    width_ = width; height_ = height;
    ESP_LOGI("cst820", "touch ready: chip %02x, firmware %02x", id, version);
    return ESP_OK;
}

esp_err_t Cst820Touch::Poll(cst820::Point* point) {
    if (!point) return ESP_ERR_INVALID_ARG;
    *point = {};
    if (!dev_) return ESP_ERR_INVALID_STATE;
    const uint8_t reg = 0x00;
    uint8_t frame[7] = {};
    const esp_err_t error = i2c_master_transmit_receive(dev_, &reg, 1, frame, sizeof frame, 30);
    if (error != ESP_OK) return error;
    return cst820::Decode(frame, sizeof frame, width_, height_, point) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
