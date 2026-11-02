#include "amoled_hardware.h"
#include "config.h"
#include "esp_log.h"

namespace { constexpr const char* TAG = "Amoled175.hw"; }

void AmoledHardware::InitDisplayAndTouch() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = SYS_I2C_PORT;
    bus_cfg.sda_io_num = SYS_I2C_SDA;
    bus_cfg.scl_io_num = SYS_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    const esp_err_t bus_error = i2c_new_master_bus(&bus_cfg, &bus);
    if (bus_error != ESP_OK) { ESP_LOGE(TAG, "I2C: %s", esp_err_to_name(bus_error)); return; }
    bus_ok_ = true;

    // The board's AXP2101 default rails power the screen/codecs. Preserve its
    // regulator and charging settings; telemetry must not reprogram the battery.
    i2c_device_config_t pmu = {};
    pmu.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    pmu.device_address = 0x34;
    pmu.scl_speed_hz = 400000;
    uint8_t id = 0, reg = 0x03;
    power_ok_ = i2c_master_bus_add_device(bus, &pmu, &pmu_) == ESP_OK &&
        i2c_master_transmit_receive(pmu_, &reg, 1, &id, 1, 40) == ESP_OK && (id & 0xCF) == 0x4A;
    if (!power_ok_) ESP_LOGW(TAG, "AXP2101 telemetry unavailable");

    Co5300Config display;
    display.bus = Co5300Bus::kQspi;
    display.rst_gpio = DISPLAY_RST_PIN;
    display.pin_sclk = DISPLAY_SCLK_PIN;
    display.pin_cs = DISPLAY_CS_PIN;
    display.pin_d0 = DISPLAY_D0_PIN; display.pin_d1 = DISPLAY_D1_PIN;
    display.pin_d2 = DISPLAY_D2_PIN; display.pin_d3 = DISPLAY_D3_PIN;
    display.pclk_hz = DISPLAY_QSPI_CLK_HZ;
    display.x_gap = DISPLAY_X_GAP;
    display.waveshare_175 = true;
    const esp_err_t panel_error = panel_.Init(display);
    if (panel_error != ESP_OK) ESP_LOGE(TAG, "display: %s", esp_err_to_name(panel_error));
    if (touch_.Init(SYS_I2C_PORT, TOUCH_RST_PIN, TOUCH_INT_PIN, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                    TOUCH_MIRROR_X, TOUCH_MIRROR_Y) != ESP_OK)
        ESP_LOGE(TAG, "CST9217 unavailable");
}

void AmoledHardware::Init(int volume) {
    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << CALL_BUTTON_PIN;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    button_ok_ = gpio_config(&button) == ESP_OK;
    ESP_LOGI(TAG, "BOOT GPIO0 active-low input: %s", button_ok_ ? "ready" : "unavailable");
    InitDisplayAndTouch();
    if (!bus_ok_) return;

    // This board owns PA gating. Keep codec-dev from independently driving it.
    gpio_config_t amp = {};
    amp.pin_bit_mask = 1ULL << AUDIO_PA_PIN;
    amp.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&amp) != ESP_OK || SetSpeakerEnabled(false) != ESP_OK) return;
    codec_ok_ = codec_.Init(volume) == ESP_OK;
    if (!codec_ok_) ESP_LOGE(TAG, "audio unavailable");
}

bool AmoledHardware::ButtonDown() const {
    return button_ok_ && gpio_get_level(CALL_BUTTON_PIN) == 0;
}

esp_err_t AmoledHardware::SetSpeakerEnabled(bool enabled) {
    return gpio_set_level(AUDIO_PA_PIN, enabled ? 1 : 0);
}

void AmoledHardware::Battery(int* percent, bool* charging) {
    *percent = -1; *charging = false;
    if (!power_ok_) return;
    uint8_t state[2] = {}, reg = 0x00, level = 0;
    if (i2c_master_transmit_receive(pmu_, &reg, 1, state, sizeof state, 40) != ESP_OK || !(state[0] & 0x08)) return;
    reg = 0xA4;
    if (i2c_master_transmit_receive(pmu_, &reg, 1, &level, 1, 40) != ESP_OK || level > 100) return;
    *percent = level;
    *charging = (state[1] >> 5) == 1;
}
