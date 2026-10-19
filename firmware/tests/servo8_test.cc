#include "servo8.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

static std::vector<uint8_t> sent;
static uint8_t response[28] = {};
static int bus_error = ESP_OK;
static int read_error = ESP_OK;
static int write_error = ESP_OK;
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* c, i2c_master_bus_handle_t* b) {
    assert(c->sda_io_num == 19 && c->scl_io_num == 20);
    *b = reinterpret_cast<void*>(1); return bus_error;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t* c,
                                   i2c_master_dev_handle_t* d) {
    assert(c->device_address == 0x10 && c->scl_speed_hz == 100000);
    *d = reinterpret_cast<void*>(2); return ESP_OK;
}
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t) { return ESP_OK; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t, uint16_t, int) { return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t* p, size_t n, int) {
    assert(n == 4); sent.assign(p, p + n); return write_error;
}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t, uint8_t* p, size_t n, int) {
    assert(n == sizeof response); memcpy(p, response, n); return read_error;
}

int main() {
    using Servo = KorvoCloudServo8;
    using Mode = Servo::Mode;
    Servo servo;
    assert(servo.Write(1, Mode::Position180, 90) == ESP_ERR_INVALID_STATE);
    bus_error = ESP_FAIL;
    assert(servo.Init() == ESP_FAIL);
    bus_error = ESP_OK;
    assert(servo.Init() == ESP_OK);
    assert(servo.SetAngle(1, 90) == ESP_OK);
    assert((sent == std::vector<uint8_t>{0, 1, 0, 90}));
    assert(servo.Write(8, Mode::Position360, 360) == ESP_OK);
    assert((sent == std::vector<uint8_t>{7, 2, 1, 104}));
    assert(servo.Write(3, Mode::Continuous, -100) == ESP_OK);
    assert((sent == std::vector<uint8_t>{2, 3, 255, 156}));
    assert(servo.Write(255, Mode::Released, 0) == ESP_OK);
    assert((sent == std::vector<uint8_t>{255, 0, 0, 0}));
    assert(servo.Write(255, Mode::Continuous, 0) == ESP_OK);
    assert((sent == std::vector<uint8_t>{255, 3, 0, 0}));
    const auto before = sent;
    for (auto cmd : {Servo::Command{0, Mode::Position180, 90},
                    {9, Mode::Position180, 90}, {1, Mode::Position180, 181},
                    {1, Mode::Position360, 361}, {1, Mode::Continuous, -101},
                    {1, Mode::Continuous, 101}, {1, Mode::Released, 1},
                    {1, static_cast<Mode>(4), 0}}) {
        assert(servo.Write(cmd.channel, cmd.mode, cmd.value) == ESP_ERR_INVALID_ARG);
        assert(sent == before);
    }
    write_error = ESP_FAIL;
    assert(servo.SetAngle(1, 90) == ESP_FAIL);
    response[0] = 255; response[1] = 3; response[2] = 255; response[3] = 156;
    response[4] = 2; response[5] = 1; response[6] = 104;
    response[25] = 3; response[26] = 255; response[27] = 216;
    Servo::Status status = {};
    assert(servo.ReadStatus(status) == ESP_OK);
    assert(status.last_id == 255 && status.last_value == -100);
    assert(status.channels[0].mode == Mode::Position360 && status.channels[0].value == 360);
    assert(status.channels[7].mode == Mode::Continuous && status.channels[7].value == -40);
    read_error = ESP_FAIL;
    assert(servo.ReadStatus(status) == ESP_FAIL);
    assert(status.channels[0].value == 360);
    read_error = ESP_OK;
    response[4] = 9;
    assert(servo.ReadStatus(status) == ESP_ERR_INVALID_RESPONSE);
    assert(status.channels[0].value == 360);
    response[4] = 1;
    assert(servo.ReadStatus(status) == ESP_ERR_INVALID_RESPONSE);
    puts("Servo8 protocol tests passed");
}
