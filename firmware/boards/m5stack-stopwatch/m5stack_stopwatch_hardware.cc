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
    if (ioe_.Init(SYS_I2C_PORT, IOE_ADDR) != ESP_OK) {
        ESP_LOGE(TAG, "IO expander not found - no screen power, no audio");
        return;
    }
    ioe_ok_ = true;
    // The levels M5's own firmware sets at boot; audio power and the amp come on later.
    (void)ioe_.SetOutput(IOE_MUX_CTR, true);
    (void)ioe_.SetOutput(IOE_AUDIO_EN, false);
    (void)ioe_.SetOutput(IOE_SPK_EN, false);
    (void)ioe_.SetOutput(IOE_PANEL_PWR, true);
    (void)ioe_.SetOutput(IOE_TP_RST, true);
    (void)ioe_.SetOutput(IOE_PANEL_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    // Reset the panel and the touch controller together.
    (void)ioe_.Write(IOE_PANEL_RST, false);
    (void)ioe_.Write(IOE_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    (void)ioe_.Write(IOE_PANEL_RST, true);
    (void)ioe_.Write(IOE_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(20));
}

void M5StopWatchHardware::InitDisplay() {
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
    (void)ioe_.Write(IOE_AUDIO_EN, true);
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
    if (codec_.Init(c) != ESP_OK) {
        ESP_LOGE(TAG, "codec init failed");
        return;
    }
    codec_ok_ = true;
}
