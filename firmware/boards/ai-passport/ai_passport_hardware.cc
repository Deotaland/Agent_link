#include "ai_passport_hardware.h"
#include "config.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

#define TAG "Passport.hardware"
namespace {
constexpr St7789LcdConfig::VendorCmd kVendorCmds[] = {
    {0xB2, 0, 5, {0x05, 0x05, 0x00, 0x33, 0x33}},   // PORCTRL, frame rate porch
    {0xB7, 0, 1, {0x35}},                          // GCTRL, gate control
    {0xBB, 0, 1, {0x21}},                          // VCOMS
    {0xC0, 0, 1, {0x2C}},                          // LCMCTRL
    {0xC2, 0, 1, {0x01}},                          // VDVVRHEN
    {0xC3, 0, 1, {0x0B}},                          // VRHS
    {0xC4, 0, 1, {0x20}},                          // VDVSET
    {0xC6, 0, 1, {0x0F}},                          // FRCTRL2, 60 Hz inversion
    {0xD0, 0, 2, {0xA7, 0xA1}},                    // PWCTRL1
    {0xD0, 0, 2, {0xA4, 0xA1}},                    // PWCTRL1 again, overrides the line above
    {0xD6, 0, 1, {0xA1}},
    {0xE0, 0, 14, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D,   // PVGAMCTRL
                   0x43, 0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}},
    {0xE1, 10, 14, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, // NVGAMCTRL
                    0x44, 0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}},
};
void InitDisplay(AiPassportHardware& hardware) {
    St7789LcdConfig c = {};
    c.spi_host      = DISPLAY_SPI_HOST;
    c.pin_sck       = DISPLAY_SCK_PIN;
    c.pin_mosi      = DISPLAY_MOSI_PIN;
    c.pin_cs        = DISPLAY_CS_PIN;
    c.pin_dc        = DISPLAY_DC_PIN;
    c.pin_rst       = DISPLAY_RST_PIN;
    c.pin_bl        = DISPLAY_BL_PIN;
    c.width         = DISPLAY_WIDTH;
    c.height        = DISPLAY_HEIGHT;
    c.pclk_hz       = DISPLAY_SPI_CLK_HZ;
    c.spi_mode      = DISPLAY_SPI_MODE;
    c.invert_color  = DISPLAY_INVERT_COLOR;
    c.stripe_rows   = 4;
    c.single_buffer = true;
#ifdef DISPLAY_HAS_VENDOR_CMDS
    c.vendor_cmds      = kVendorCmds;
    c.vendor_cmd_count = sizeof(kVendorCmds) / sizeof(kVendorCmds[0]);
#endif
    if (hardware.lcd.Init(c) != ESP_OK) {
        ESP_LOGW(TAG, "LCD init failed — continuing without display");
        return;
    }
    hardware.display_ready = true;
    (void)hardware.lcd.FillSolid(rgb565::kBlack);

}
void InitCodec(AiPassportHardware& hardware) {
    Es8311Config c = {};
    c.i2c_port    = AUDIO_I2C_PORT;
    c.pin_sda     = AUDIO_I2C_SDA;
    c.pin_scl     = AUDIO_I2C_SCL;
    c.pin_mclk    = AUDIO_I2S_MCLK;
    c.pin_bclk    = AUDIO_I2S_BCLK;
    c.pin_ws      = AUDIO_I2S_WS;
    c.pin_din     = AUDIO_I2S_DIN;
    c.pin_dout    = AUDIO_I2S_DOUT;
    c.pin_pa_en   = AUDIO_PA_EN;
    c.es8311_addr = ES8311_ADDR;
    c.sample_rate = AUDIO_SAMPLE_RATE;
    c.mic_gain    = 30;
    c.out_volume  = 70;   // the UP/DOWN task adjusts from here
    c.use_default_volume_curve = true;
    // Capture the microphone without the DAC reference.
    c.no_dac_ref  = true;
    // 5.0 V, not 0. With pa_voltage = 0 the driver substitutes 5.0 anyway, but saying it here
    // keeps the volume curve honest if that default ever changes.
    c.pa_voltage  = 5.0f;
    if (hardware.codec.Init(c) != ESP_OK) {
        ESP_LOGE(TAG, "codec init failed");
        return;
    }
    (void)hardware.codec.SetMute(true);

    hardware.audio_ready = true;
}

}  // namespace

void AiPassportHardware::Init() {
    InitDisplay(*this);
    InitCodec(*this);
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = BUTTON_ADC_UNIT;
    if (adc_oneshot_new_unit(&ucfg, &adc_) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit create failed");
        return;
    }
    adc_oneshot_chan_cfg_t ch = {};
    ch.atten = ADC_ATTEN_DB_12;
    ch.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(adc_, BUTTON_ADC_CHANNEL, &ch) != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed");
        return;
    }
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id  = BUTTON_ADC_UNIT;
    cal.chan     = BUTTON_ADC_CHANNEL;
    cal.atten    = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &calibration_) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration failed — using raw thresholds");
    }
    keys_ready = true;
}

PassportKey AiPassportHardware::ReadKey() {
    if (!keys_ready) return PassportKey::None;
    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc_, BUTTON_ADC_CHANNEL, &raw) != ESP_OK) return PassportKey::None;
    if (!calibration_ || adc_cali_raw_to_voltage(calibration_, raw, &mv) != ESP_OK)
        mv = raw * 3100 / 4095;
    if (mv < 0) return PassportKey::None;
    if (mv < BUTTON_MV_UP_MAX) return PassportKey::Up;
    if (mv < BUTTON_MV_DOWN_MAX) return PassportKey::Down;
    if (mv < BUTTON_MV_OK_MAX) return PassportKey::Ok;
    return PassportKey::None;
}

