#include "korvo_hardware.h"
#include "hardware_config.h"
#include "esp_log.h"
#include "esp_check.h"

namespace {
constexpr const char* TAG = "korvo.hw";
static St7789LcdConfig MakeLcdConfig() {
    St7789LcdConfig c = {};
    c.spi_host     = DISPLAY_SPI_HOST;
    c.pin_sck      = DISPLAY_SCK_PIN;
    c.pin_mosi     = DISPLAY_MOSI_PIN;
    c.pin_cs       = DISPLAY_CS_PIN;
    c.pin_dc       = DISPLAY_DC_PIN;
    c.pin_rst      = DISPLAY_RST_PIN;
    c.pin_bl       = DISPLAY_BL_PIN;
    c.width        = DISPLAY_WIDTH;
    c.height       = DISPLAY_HEIGHT;
    c.pclk_hz      = DISPLAY_SPI_CLK_HZ;
    c.spi_mode     = DISPLAY_SPI_MODE;
    c.invert_color = true;            // ST7789 default
    c.swap_xy      = DISPLAY_SWAP_XY; // rotate 90° CCW in the panel's MADCTL — see config.h
    c.mirror_x     = DISPLAY_MIRROR_X;
    c.mirror_y     = DISPLAY_MIRROR_Y;
    c.gap_x        = DISPLAY_GAP_X;   // 280-row panel sits 20 into the 320 GRAM; swap_xy puts that on x
    c.gap_y        = DISPLAY_GAP_Y;
    c.stripe_rows  = DISPLAY_STRIPE_ROWS;  // halved from the default to leave BLE its internal RAM
    return c;
}

static Gc2145Config MakeCameraConfig() {
    Gc2145Config c = {};
    c.pin_pwdn     = CAM_PIN_PWDN;
    c.pin_reset    = CAM_PIN_RESET;
    c.pin_xclk     = CAM_PIN_XCLK;
    c.pin_sccb_sda = CAM_PIN_SIOD;
    c.pin_sccb_scl = CAM_PIN_SIOC;
    c.pin_d7       = CAM_PIN_D7;
    c.pin_d6       = CAM_PIN_D6;
    c.pin_d5       = CAM_PIN_D5;
    c.pin_d4       = CAM_PIN_D4;
    c.pin_d3       = CAM_PIN_D3;
    c.pin_d2       = CAM_PIN_D2;
    c.pin_d1       = CAM_PIN_D1;
    c.pin_d0       = CAM_PIN_D0;
    c.pin_vsync    = CAM_PIN_VSYNC;
    c.pin_href     = CAM_PIN_HREF;
    c.pin_pclk     = CAM_PIN_PCLK;
    c.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
    c.pixel_format = PIXFORMAT_RGB565;  // straight to the LCD, no decode
    c.frame_size   = FRAMESIZE_240X240; // 1:1 with the panel's short edge
    c.fb_count     = 2;                 // double-buffer in PSRAM
    c.mask_top_rows        = CAMERA_MASK_TOP_ROWS;
    c.ae_lock_after_frames = CAMERA_AE_LOCK_FRAMES;
    // Left off on purpose: a snapshot's JPEG encode needs the sensor's original byte order, so
    // the swap (when the panel needs one) happens in the preview loop, after SendSnapshot().
    c.byte_swap = false;
    return c;
}

}  // namespace

esp_err_t KorvoHardware::Init() {
    ESP_RETURN_ON_ERROR(lcd.Init(MakeLcdConfig()), TAG, "LCD init failed");
    // The camera creates the shared I2C bus before touch and audio attach.
    esp_err_t result = camera.Init(MakeCameraConfig());
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed");
        lcd.FillSolid(rgb565::kRed);
        return result;
    }
    Cst816Config tc = {};
    tc.i2c_port = TOUCH_I2C_PORT;
    tc.addr = TOUCH_I2C_ADDR;
    tc.pin_rst = TOUCH_RST_PIN;
    tc.swap_xy = TOUCH_SWAP_XY;
    tc.mirror_x = TOUCH_MIRROR_X;
    tc.mirror_y = TOUCH_MIRROR_Y;
    tc.out_width = DISPLAY_WIDTH;
    tc.out_height = DISPLAY_HEIGHT;
    tc.log_raw = TOUCH_LOG_RAW;
    touch_ready = touch.Init(tc) == ESP_OK;
    if (!touch_ready) ESP_LOGW(TAG, "touch unavailable");
    EsCodecConfig cc = {};
    cc.i2c_port    = static_cast<i2c_port_t>(AUDIO_I2C_PORT);
    cc.pin_sda     = GPIO_NUM_NC;   // attach to the bus the camera's SCCB already made on this port
    cc.pin_scl     = GPIO_NUM_NC;
    cc.pin_mclk    = AUDIO_I2S_MCLK;
    cc.pin_bclk    = AUDIO_I2S_BCLK;
    cc.pin_ws      = AUDIO_I2S_WS;
    cc.pin_din     = AUDIO_I2S_DIN;
    cc.pin_dout    = AUDIO_I2S_DOUT;
    cc.pin_pa_en   = AUDIO_PA_EN;
    cc.es7210_addr = ES7210_ADDR;
    cc.es8311_addr = ES8311_ADDR;
    cc.sample_rate = AUDIO_SAMPLE_RATE;
    cc.mic_gain    = AUDIO_MIC_GAIN;
    cc.out_volume  = AUDIO_OUT_VOLUME;

    if (codec.Init(cc) != ESP_OK) ESP_LOGE(TAG, "codec init failed - audio unavailable");
    return ESP_OK;
}
