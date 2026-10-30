#include "cst9217_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t Cst9217Touch::Init(i2c_port_t port, gpio_num_t reset, gpio_num_t interrupt, int width, int height,
                           bool mirror_x, bool mirror_y) {
    if (dev_ || width <= 0 || height <= 0 || reset < 0 || interrupt < 0) return ESP_ERR_INVALID_ARG;
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_master_get_bus_handle(port, &bus);
    if (err != ESP_OK) return err;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << reset; cfg.mode = GPIO_MODE_OUTPUT;
    if ((err = gpio_config(&cfg)) != ESP_OK) return err;
    gpio_set_level(reset, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(reset, 1); vTaskDelay(pdMS_TO_TICKS(50));
    cfg.pin_bit_mask = 1ULL << interrupt; cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    if ((err = gpio_config(&cfg)) != ESP_OK) return err;
    if ((err = i2c_master_probe(bus, 0x5A, 40)) != ESP_OK) return err;
    i2c_device_config_t device = {};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = 0x5A; device.scl_speed_hz = 400000;
    if ((err = i2c_master_bus_add_device(bus, &device, &dev_)) != ESP_OK) { dev_ = nullptr; return err; }
    width_ = width; height_ = height;
    mirror_x_ = mirror_x; mirror_y_ = mirror_y;
    return ESP_OK;
}

esp_err_t Cst9217Touch::Read(uint16_t reg, uint8_t* data, size_t size) {
    const uint8_t address[] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg)};
    // CST9217 needs a STOP and a short delay before the separate read.
    esp_err_t err = i2c_master_transmit(dev_, address, sizeof address, 20);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);
    return i2c_master_receive(dev_, data, size, 20);
}

esp_err_t Cst9217Touch::Poll(cst9217::Point* point) {
    uint8_t raw[10] = {};
    return Poll(point, raw);
}

esp_err_t Cst9217Touch::Poll(cst9217::Point* point, uint8_t (&raw)[10]) {
    if (!point) return ESP_ERR_INVALID_ARG;
    *point = {};
    for (auto& byte : raw) byte = 0;
    if (!dev_) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = Read(0xD000, raw, sizeof raw);
    if (err != ESP_OK) return err;
    if (!cst9217::Decode(raw, sizeof raw, width_, height_, point)) return ESP_ERR_INVALID_RESPONSE;
    // Pixel coordinates are zero-based: 466 pixels span 0..465. Transform
    // validated contacts only, so release/error reports remain unpressed.
    if (point->pressed) {
        if (mirror_x_) point->x = static_cast<uint16_t>(width_ - 1 - point->x);
        if (mirror_y_) point->y = static_cast<uint16_t>(height_ - 1 - point->y);
    }
    return ESP_OK;
}
