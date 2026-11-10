#include "paper_touch.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static paper_touch_info_t s_touch;
static std::vector<uint8_t> register_bytes, coordinates, written;
static bool read_error = false, write_error = false, device_error = false;
using i2c_master_dev_handle_t = void*;
static i2c_master_dev_handle_t dev(uint8_t address) {
    assert(address == s_touch.address);
    return device_error ? nullptr : reinterpret_cast<void*>(1);
}
static esp_err_t i2c_master_transmit(void*, const uint8_t* data, size_t bytes, int) {
    written.assign(data,data+bytes); return write_error ? ESP_ERR_TIMEOUT : ESP_OK;
}
#define I2C_TIMEOUT_MS 100
// INSERT_WRITE_REG16

static esp_err_t i2c_read_reg16(uint8_t address, uint16_t reg, uint8_t* data, size_t bytes) {
    assert(address == s_touch.address && (reg == 0x814e || reg == 0x814f));
    if (read_error) return ESP_ERR_TIMEOUT;
    const auto& input = reg == 0x814e ? register_bytes : coordinates;
    assert(input.size() >= bytes); std::memcpy(data,input.data(),bytes); return ESP_OK;
}
static esp_err_t i2c_read_regs(uint8_t address, uint8_t reg, uint8_t* data, size_t bytes) {
    assert(address == s_touch.address && reg == 2);
    if (read_error) return ESP_ERR_TIMEOUT;
    assert(register_bytes.size() >= bytes); std::memcpy(data,register_bytes.data(),bytes); return ESP_OK;
}
// INSERT_TOUCH_READ

int main() {
    paper_touch_sample_t sample{};
    assert(paper_touch_read(nullptr) == ESP_ERR_INVALID_ARG);
    assert(paper_touch_read(&sample) == ESP_ERR_INVALID_STATE);
    s_touch = {PAPER_TOUCH_GT911,0x5d,528,792};
    register_bytes = {0};
    assert(paper_touch_read(&sample) == ESP_OK && !sample.fresh && written.empty());
    register_bytes = {0x81}; coordinates = {0,0x34,0x02,0x78,0x03,0,0,0};
    assert(paper_touch_read(&sample) == ESP_OK && sample.fresh && sample.down && sample.x == 0x234 && sample.y == 0x378);
    assert(written == std::vector<uint8_t>({0x81,0x4e,0}));
    register_bytes = {0x80}; assert(paper_touch_read(&sample) == ESP_OK && sample.fresh && !sample.down);
    write_error = true; register_bytes = {0x81}; assert(paper_touch_read(&sample) == ESP_ERR_TIMEOUT && !sample.fresh);
    write_error = false; read_error = true; assert(paper_touch_read(&sample) == ESP_ERR_TIMEOUT && !sample.fresh);
    read_error = false; device_error = true; assert(paper_touch_read(&sample) == ESP_ERR_INVALID_STATE);
    device_error = false;
    s_touch = {PAPER_TOUCH_FT6X36,0x38,0,0}; register_bytes = {1,2,0x34,3,0x78};
    assert(paper_touch_read(&sample) == ESP_OK && sample.down && sample.x == 0x234 && sample.y == 0x378);
    register_bytes[0] = 15; assert(paper_touch_read(&sample) == ESP_OK && !sample.down);
    s_touch = {PAPER_TOUCH_CST816,0x15,0,0}; read_error = true;
    assert(paper_touch_read(&sample) == ESP_OK && sample.fresh && !sample.down);
    std::puts("PASS: production GT911/FT6x36/CST816 parsing, fresh/release flags, 16-bit buffer ACK and I2C error handling");
}
