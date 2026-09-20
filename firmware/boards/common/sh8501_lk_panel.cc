#include "sh8501_lk_panel.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8501_lk.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "sh8501_lk";

// Rows per transfer. 120 x 20 x 2 = 4800 B.
// the LK driver splits anything over its own 4092-byte chunk across two transactions with CS held, which is fine).
constexpr uint16_t kStripeRows = 20;

// Factory short code
// Standard user commands only - power, boost and gamma come from the panel's OTP, which is why
// this table only works on production panels. Do not reorder or "tidy":
//   - 0x29 DISPON must stay inside the sequence; this panel will not accept draw_bitmap before it.
//   - 0x36 MADCTL = 0x00 is this board's orientation (board_test used 0xC0, which is 180 degrees
//     off here). If the picture comes out upside down, that is the byte to flip.
//   - no trailing RAMWR: the driver issues 0x2A/0x2B/0x2C itself on every blit.
const sh8501_lk_lcd_init_cmd_t kInitShortLk[] = {
    {0x11, NULL, 0, 60},                                // Sleep Out
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0x77}, 4, 0},  // CASET 0..119
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},  // RASET 0..239
    {0x44, (uint8_t[]){0x01, 0x27}, 2, 0},              // Tearing Effect scan line
    {0x35, (uint8_t[]){0x00}, 1, 0},                    // Tearing Effect ON
    {0x3A, (uint8_t[]){0x55}, 1, 0},                    // Pixel format RGB565
    {0x36, (uint8_t[]){0x00}, 1, 0},                    // MADCTL (orientation)
    {0x51, (uint8_t[]){0xFF}, 1, 60},                   // Brightness max
    {0x29, NULL, 0, 120},                               // Display ON
    {0x39, NULL, 0, 0},                                 // Idle Mode OFF
};
}  // namespace

Sh8501LkPanel::~Sh8501LkPanel() {
    if (stripe_) { heap_caps_free(stripe_); stripe_ = nullptr; }
    if (panel_)  { esp_lcd_panel_del(panel_); panel_ = nullptr; }
}

esp_err_t Sh8501LkPanel::Init(const Sh8501LkConfig& cfg) {
    cfg_ = cfg;

    // Defensive: clear any RST/CS pad hold left from a previous run (light sleep / abnormal
    // reset), or the reset pulse never reaches the panel.
    (void)gpio_hold_dis(static_cast<gpio_num_t>(cfg_.pin_rst));
    (void)gpio_hold_dis(static_cast<gpio_num_t>(cfg_.pin_cs));

    // The LK driver only adds a device, so the bus is ours to set up. max_transfer_sz covers one
    // stripe, which also keeps it above the driver's 4092-byte chunk.
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = cfg_.pin_sck;
    bus.mosi_io_num     = cfg_.pin_mosi;
    bus.miso_io_num     = -1;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = static_cast<int>(cfg_.width) * kStripeRows * 2;
    esp_err_t ret = spi_bus_initialize(cfg_.spi_host, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = already initialized
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    sh8501_lk_vendor_config_t vendor = {};
    vendor.spi_host       = cfg_.spi_host;
    vendor.cs_gpio_num    = cfg_.pin_cs;
    vendor.dc_gpio_num    = cfg_.pin_dc;
    vendor.pclk_hz        = cfg_.pclk_hz;
    vendor.init_cmds      = kInitShortLk;
    vendor.init_cmds_size = sizeof(kInitShortLk) / sizeof(kInitShortLk[0]);

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = cfg_.pin_rst;
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.flags.reset_active_high = 0;   // reset is active low on this board
    panel_cfg.vendor_config  = &vendor;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8501_lk(&panel_cfg, &panel_), TAG, "new_panel_lk");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_),  TAG, "init");

    // The short code ends with DISPON at full brightness, so the panel is now lit showing
    // whatever random bytes GRAM powered up with. Drive the brightness to 0 (a plain 0x51 write,
    // the same trick the full vendor sequence uses), clear to black behind that, then bring it
    // back. The ~120ms between the table's own DISPON and this first 0x51 is inherited from the
    // validated table and is the one flash of noise left
    (void)SetBrightness(0);
    (void)FillSolid(rgb565::kBlack);
    (void)SetBrightness(0xFF);

    ESP_LOGI(TAG, "SH8501 LK panel ready (%ux%u @ %luMHz, factory short code, %u cmds)",
             cfg_.width, cfg_.height, static_cast<unsigned long>(cfg_.pclk_hz / 1000000),
             static_cast<unsigned>(vendor.init_cmds_size));
    return ESP_OK;
}

esp_err_t Sh8501LkPanel::EnsureStripe(size_t bytes) {
    if (stripe_ && stripe_cap_ >= bytes) return ESP_OK;
    if (stripe_) heap_caps_free(stripe_);
    // Must be internal DMA-capable SRAM: the SPI master transmits straight out of this buffer.
    stripe_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!stripe_) { stripe_cap_ = 0; return ESP_ERR_NO_MEM; }
    stripe_cap_ = bytes;
    return ESP_OK;
}

esp_err_t Sh8501LkPanel::FillSolid(uint16_t color) {
    if (!panel_) return ESP_ERR_INVALID_STATE;

    const size_t stripe_bytes = static_cast<size_t>(cfg_.width) * kStripeRows * 2u;
    ESP_RETURN_ON_ERROR(EnsureStripe(stripe_bytes), TAG, "stripe alloc");

    // RGB565 big-endian fill: [hi, lo, hi, lo, ...]
    const uint8_t hi = static_cast<uint8_t>(color >> 8);
    const uint8_t lo = static_cast<uint8_t>(color & 0xFF);
    for (size_t i = 0; i < stripe_bytes; i += 2u) { stripe_[i] = hi; stripe_[i + 1u] = lo; }

    const uint16_t w = cfg_.width;
    const uint16_t h = cfg_.height;
    for (uint16_t row = 0; row < h; row = static_cast<uint16_t>(row + kStripeRows)) {
        const uint16_t rows_this = std::min<uint16_t>(kStripeRows, static_cast<uint16_t>(h - row));
        // Blocking driver: the transfer is done when this returns, so one buffer is enough.
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(panel_, 0, row, w, static_cast<uint16_t>(row + rows_this),
                                      stripe_),
            TAG, "draw");
    }
    return ESP_OK;
}

esp_err_t Sh8501LkPanel::DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                    const void* rgb565_be) {
    if (!panel_ || !rgb565_be) return ESP_ERR_INVALID_STATE;
    if (w == 0 || h == 0) return ESP_OK;
    if (x + w > cfg_.width || y + h > cfg_.height) return ESP_ERR_INVALID_ARG;

    const size_t row_bytes = static_cast<size_t>(w) * 2u;
    const size_t stripe_bytes = static_cast<size_t>(cfg_.width) * kStripeRows * 2u;
    ESP_RETURN_ON_ERROR(EnsureStripe(stripe_bytes), TAG, "stripe alloc");

    size_t rows_per_chunk = stripe_cap_ / row_bytes;
    if (rows_per_chunk == 0) return ESP_ERR_INVALID_SIZE;   // one row wider than the staging buffer
    if (rows_per_chunk > h) rows_per_chunk = h;

    const uint8_t* src = static_cast<const uint8_t*>(rgb565_be);
    for (uint16_t row = 0; row < h; row += static_cast<uint16_t>(rows_per_chunk)) {
        const uint16_t rows_this =
            std::min<uint16_t>(static_cast<uint16_t>(rows_per_chunk), static_cast<uint16_t>(h - row));
        memcpy(stripe_, src + static_cast<size_t>(row) * row_bytes,
               static_cast<size_t>(rows_this) * row_bytes);
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(panel_, x, static_cast<uint16_t>(y + row),
                                      static_cast<uint16_t>(x + w),
                                      static_cast<uint16_t>(y + row + rows_this), stripe_),
            TAG, "draw");
    }
    return ESP_OK;
}

esp_err_t Sh8501LkPanel::SetBrightness(uint8_t level) {
    if (!panel_) return ESP_ERR_INVALID_STATE;
    const uint8_t percent = static_cast<uint8_t>(static_cast<unsigned>(level) * 100u / 255u);
    return esp_lcd_panel_sh8501_lk_set_brightness(panel_, percent);
}

esp_err_t Sh8501LkPanel::DisplayOn()  {
    return panel_ ? esp_lcd_panel_disp_on_off(panel_, true)  : ESP_ERR_INVALID_STATE;
}
esp_err_t Sh8501LkPanel::DisplayOff() {
    return panel_ ? esp_lcd_panel_disp_on_off(panel_, false) : ESP_ERR_INVALID_STATE;
}
