// Cst816Touch: CST816 capacitive touch controller over I2C, see cst816_touch.h.

#include "cst816_touch.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "cst816";

// CST816 register map (the handful that matters for a polled driver).
constexpr uint8_t kRegGesture      = 0x01;  // 0x01..0x06 read as one burst
constexpr uint8_t kRegChipId       = 0xA7;  // 0xB4 CST716, 0xB5 CST816S, 0xB6 CST816T, 0xB7 CST816D
constexpr uint8_t kRegFwVersion    = 0xA9;
constexpr uint8_t kRegDisAutoSleep = 0xFE;  // 1 = stay awake; without it the chip naps and polling reads zeros

constexpr int kTimeoutMs = 100;

const char* ChipName(uint8_t id) {
    switch (id) {
        case 0xB4: return "CST716";
        case 0xB5: return "CST816S";
        case 0xB6: return "CST816T";
        case 0xB7: return "CST816D";
        default:   return "unknown";
    }
}

esp_err_t WriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), kTimeoutMs);
}

esp_err_t ReadRegs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* out, size_t len) {
    return i2c_master_transmit_receive(dev, &reg, 1, out, len, kTimeoutMs);
}

// Nothing answered at the configured address — say what IS on the bus, so one flash settles whether
// the panel is wired, at a different address, or absent.
void ScanBus(i2c_master_bus_handle_t bus, uint8_t wanted) {
    ESP_LOGE(TAG, "no touch controller at 0x%02x — scanning the bus:", wanted);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; ++a) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            ESP_LOGE(TAG, "  device at 0x%02x", a);
            ++found;
        }
    }
    if (!found) ESP_LOGE(TAG, "  bus is empty — check SDA/SCL wiring and the panel's FPC");
}
}  // namespace

esp_err_t Cst816Touch::Init(const Cst816Config& cfg) {
    cfg_ = cfg;

    // Some modules wire RST to a GPIO, some tie it to the board reset. Pulse it when we have one.
    if (cfg_.pin_rst >= 0) {
        gpio_config_t rst = {};
        rst.pin_bit_mask = 1ULL << cfg_.pin_rst;
        rst.mode         = GPIO_MODE_OUTPUT;
        gpio_config(&rst);
        gpio_set_level(static_cast<gpio_num_t>(cfg_.pin_rst), 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(static_cast<gpio_num_t>(cfg_.pin_rst), 1);
        vTaskDelay(pdMS_TO_TICKS(50));  // CST816 needs ~50ms after reset before it answers
    }

    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t r = i2c_master_get_bus_handle(cfg_.i2c_port, &bus);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "I2C port %d is not initialised (%s) — the bus owner must come up first",
                 cfg_.i2c_port, esp_err_to_name(r));
        return r;
    }

    if (i2c_master_probe(bus, cfg_.addr, kTimeoutMs) != ESP_OK) {
        ScanBus(bus, cfg_.addr);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = cfg_.addr;
    dev_cfg.scl_speed_hz    = cfg_.scl_speed_hz;
    r = i2c_master_bus_add_device(bus, &dev_cfg, &dev_);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "bus_add_device: %s", esp_err_to_name(r));
        dev_ = nullptr;
        return r;
    }

    uint8_t id = 0, fw = 0;
    (void)ReadRegs(dev_, kRegChipId, &id, 1);
    (void)ReadRegs(dev_, kRegFwVersion, &fw, 1);

    // Polled driver, so the chip must not nap between reads.
    if (WriteReg(dev_, kRegDisAutoSleep, 0x01) != ESP_OK)
        ESP_LOGW(TAG, "could not disable auto-sleep — touches may stop responding when idle");

    ESP_LOGI(TAG, "touch ready: %s (id=0x%02x fw=0x%02x) at 0x%02x on I2C%d",
             ChipName(id), id, fw, cfg_.addr, cfg_.i2c_port);
    return ESP_OK;
}

// Native controller coordinates -> logical screen coordinates. Swap first, then mirror inside the output size
void Cst816Touch::MapPoint(uint16_t raw_x, uint16_t raw_y) {
    int32_t x = raw_x;
    int32_t y = raw_y;
    if (cfg_.swap_xy) {
        const int32_t t = x;
        x = y;
        y = t;
    }
    if (cfg_.mirror_x) x = cfg_.out_width  - 1 - x;
    if (cfg_.mirror_y) y = cfg_.out_height - 1 - y;

    // Clamp: the panel's active area can overhang the glass by a pixel or two at the edges.
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > cfg_.out_width  - 1) x = cfg_.out_width  - 1;
    if (y > cfg_.out_height - 1) y = cfg_.out_height - 1;

    if (cfg_.log_raw) ESP_LOGI(TAG, "touch raw=(%u,%u) -> (%ld,%ld)", raw_x, raw_y,
                               static_cast<long>(x), static_cast<long>(y));
    x_ = x;
    y_ = y;
}

esp_err_t Cst816Touch::Poll() {
    if (!dev_) return ESP_ERR_INVALID_STATE;

    uint8_t b[6] = {};  // 0x01 gesture, 0x02 finger count, 0x03..0x06 X/Y
    const esp_err_t r = ReadRegs(dev_, kRegGesture, b, sizeof(b));
    if (r != ESP_OK) {
        pressed_ = false;  // a bus hiccup reads as "not touched" rather than a stuck press
        return r;
    }

    pressed_prev_ = pressed_;
    pressed_      = (b[1] != 0);
    if (pressed_) {
        // 12-bit coordinates: the high nibble lives in the low 4 bits of the H byte.
        const uint16_t raw_x = static_cast<uint16_t>(((b[2] & 0x0F) << 8) | b[3]);
        const uint16_t raw_y = static_cast<uint16_t>(((b[4] & 0x0F) << 8) | b[5]);
        MapPoint(raw_x, raw_y);
    }
    if (pressed_ && !pressed_prev_) edge_ = true;
    return ESP_OK;
}

bool Cst816Touch::TakePressEdge() {
    const bool e = edge_;
    edge_ = false;
    return e;
}
