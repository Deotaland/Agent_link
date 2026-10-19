#include "m5stack_stopwatch_hardware.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "StopWatch.hw"

void M5StopWatchHardware::Init(int volume) {
    InitI2c();
    InitPower();
    InitExpander();
    InitDisplay();
    InitAudio(volume);
}

void M5StopWatchHardware::InitI2c() {
    i2c_master_bus_config_t bus = {};
    bus.i2c_port          = SYS_I2C_PORT;
    bus.sda_io_num        = SYS_I2C_SDA;
    bus.scl_io_num        = SYS_I2C_SCL;
    bus.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t handle = nullptr;
    i2c_ok_ = i2c_new_master_bus(&bus, &handle) == ESP_OK;
    if (!i2c_ok_) ESP_LOGE(TAG, "I2C bus failed: no screen, audio or battery");
}

void M5StopWatchHardware::InitPower() {
    if (!i2c_ok_) return;
    if (pm1_.Init(SYS_I2C_PORT, PM1_ADDR) != ESP_OK) {
        ESP_LOGW(TAG, "PMIC not found - battery shown as unknown");
        return;
    }
    pm1_ok_ = true;
    // ESP32 rail, RTC/IMU rail and charger on, as M5's own firmware leaves them.
    (void)pm1_.EnablePower(M5Pm1::kPwrCharge | M5Pm1::kPwrDcdc | M5Pm1::kPwrLdo | M5Pm1::kPwrLed);
    (void)pm1_.SetGpioInput(PM1_CHG_STAT_GPIO);
}

void M5StopWatchHardware::InitExpander() {
    if (!i2c_ok_) return;
    if (ioe_.Init(SYS_I2C_PORT, IOE_ADDR) != ESP_OK &&
        ioe_.Init(SYS_I2C_PORT, 0x6F) != ESP_OK) {
        ESP_LOGE(TAG, "IO expander not found - no screen power, no audio");
        return;
    }

    // Match the official power/reset sequence; stop instead of advertising unusable hardware.
    const auto output = [this](int pin, bool level) { return ioe_.SetOutput(pin, level) == ESP_OK; };
    if (!output(IOE_MUX_CTR, true) || !output(IOE_AUDIO_EN, false) ||
        !output(IOE_SPK_EN, false) || !output(IOE_PANEL_PWR, true) ||
        !output(IOE_TP_RST, true) || !output(IOE_PANEL_RST, true)) {
        ESP_LOGE(TAG, "IO expander power setup failed"); return;
    }
    bool powered = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (ioe_.Read(IOE_PANEL_PWR, &powered) == ESP_OK && powered) break;
        (void)ioe_.Write(IOE_PANEL_PWR, true);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    if (!powered) { ESP_LOGE(TAG, "panel supply did not turn on"); return; }
    if (ioe_.Write(IOE_PANEL_RST, false) != ESP_OK ||
        ioe_.Write(IOE_TP_RST, false) != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (ioe_.Write(IOE_PANEL_RST, true) != ESP_OK ||
        ioe_.Write(IOE_TP_RST, true) != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(50));  // official CST820 reset settling time
    ioe_ok_ = true;

}

void M5StopWatchHardware::InitDisplay() {
    if (!ioe_ok_) return;
    Co5300Config c = {};
    c.bus         = Co5300Bus::kQspi;
    c.rst_gpio    = -1;   // reset through the IO expander, above
    c.width       = DISPLAY_WIDTH;
    c.height      = DISPLAY_HEIGHT;
    c.spi_host    = DISPLAY_SPI_HOST;
    c.pin_sclk    = DISPLAY_SCLK_PIN;
    c.pin_d0      = DISPLAY_D0_PIN;
    c.pin_d1      = DISPLAY_D1_PIN;
    c.pin_d2      = DISPLAY_D2_PIN;
    c.pin_d3      = DISPLAY_D3_PIN;
    c.pin_cs      = DISPLAY_CS_PIN;
    c.pclk_hz     = DISPLAY_QSPI_CLK_HZ;
    c.x_gap       = DISPLAY_X_GAP;
    c.y_gap       = DISPLAY_Y_GAP;
    c.bgr         = DISPLAY_BGR;
    c.stripe_rows = DISPLAY_STRIPE_ROWS;
    if (panel_.Init(c) != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    (void)panel_.SetBrightness(DISPLAY_BRIGHTNESS);
}

void M5StopWatchHardware::InitAudio(int volume) {
    if (!ioe_ok_) return;   // the codec's supply is on the expander
    if (ioe_.Write(IOE_AUDIO_EN, true) != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(20));  // supply up before the codec is spoken to

    Es8311Config c = {};
    c.i2c_port    = SYS_I2C_PORT;
    c.pin_sda     = GPIO_NUM_NC;   // the board's bus
    c.pin_scl     = GPIO_NUM_NC;
    c.pin_mclk    = AUDIO_I2S_MCLK;
    c.pin_bclk    = AUDIO_I2S_BCLK;
    c.pin_ws      = AUDIO_I2S_WS;
    c.pin_din     = AUDIO_I2S_DIN;
    c.pin_dout    = AUDIO_I2S_DOUT;
    c.pin_pa_en   = GPIO_NUM_NC;   // the amp is on the expander, switched by the play task
    c.es8311_addr = ES8311_ADDR;
    c.i2s_port    = AUDIO_I2S_PORT;
    c.sample_rate = AUDIO_SAMPLE_RATE;
    c.mic_gain    = AUDIO_MIC_GAIN;
    c.out_volume  = volume;
#if CONFIG_BOARD_TYPE_M5STACK_STOPWATCH_WIFI
    c.use_default_volume_curve = true;  // official StopWatch audio uses codec-dev's -50..0 dB curve
#endif
    if (codec_.Init(c) != ESP_OK) {
        ESP_LOGE(TAG, "codec init failed");
        return;
    }
    codec_ok_ = true;
}


esp_err_t M5StopWatchHardware::SetSpeakerEnabled(bool enabled) {
    if (!ioe_ok_) return ESP_ERR_INVALID_STATE;
    // Official firmware drives GPIO14 and IOE10. On revisions with the optional
    // link populated they share a net: release GPIO14 before changing IOE10.
    constexpr gpio_num_t pin = GPIO_NUM_14;
    esp_err_t error = gpio_set_direction(pin, GPIO_MODE_INPUT);
    if (error == ESP_OK) error = ioe_.Write(IOE_SPK_EN, enabled);
    if (error == ESP_OK) error = gpio_set_level(pin, enabled);
    if (error == ESP_OK) error = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    return error;
}
