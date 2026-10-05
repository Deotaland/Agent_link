#include "gc9d01_panel.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9d01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "gc9d01";

// The BOE 160x160 glass, as the production firmware brings it up. Its registers (0x80-0x8F, 0x6E,
// the 0xF0-0xF3 gamma) are nothing like the driver's built-in default sequence, so the whole table
// replaces it through vendor_config. Do not reorder or "tidy":
//   - no 0x21 INVON: this glass is not inverted (MADCTL 0x00, RGB order);
//   - no trailing 0x2C RAMWR: draw_bitmap issues CASET/RASET/RAMWR itself on every blit;
//   - no 0x29 DISPON: lit here, the panel would show GRAM's power-up noise until the first clear.
//     The table ends at SLPOUT and Init() clears GRAM to black before turning the display on.
//   - the driver sends MADCTL and COLMOD before this table, which sets both again: the two
//     "command has been used and will be overwritten" warnings at boot are expected.
const gc9d01_lcd_init_cmd_t kBoeInit[] = {
    {0xFE, (uint8_t[]){0x00}, 0, 0},   // inter-register enable 1
    {0xEF, (uint8_t[]){0x00}, 0, 0},   // inter-register enable 2

    {0x80, (uint8_t[]){0xFF}, 1, 0},
    {0x81, (uint8_t[]){0xFF}, 1, 0},
    {0x82, (uint8_t[]){0xFF}, 1, 0},
    {0x83, (uint8_t[]){0xFF}, 1, 0},
    {0x84, (uint8_t[]){0xFF}, 1, 0},
    {0x85, (uint8_t[]){0xFF}, 1, 0},
    {0x86, (uint8_t[]){0xFF}, 1, 0},
    {0x87, (uint8_t[]){0xFF}, 1, 0},
    {0x88, (uint8_t[]){0xFF}, 1, 0},
    {0x89, (uint8_t[]){0xFF}, 1, 0},
    {0x8A, (uint8_t[]){0xFF}, 1, 0},
    {0x8B, (uint8_t[]){0xFF}, 1, 0},
    {0x8C, (uint8_t[]){0xFF}, 1, 0},
    {0x8D, (uint8_t[]){0xFF}, 1, 0},
    {0x8E, (uint8_t[]){0xFF}, 1, 0},
    {0x8F, (uint8_t[]){0xFF}, 1, 0},

    {0x3A, (uint8_t[]){0x05}, 1, 0},   // pixel format: RGB565
    {0xEC, (uint8_t[]){0x01}, 1, 0},

    {0x74, (uint8_t[]){0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00}, 7, 0},

    {0x98, (uint8_t[]){0x3E}, 1, 0},
    {0x99, (uint8_t[]){0x3E}, 1, 0},

    {0xB5, (uint8_t[]){0x0D, 0x0D}, 2, 0},

    {0x60, (uint8_t[]){0x38, 0x0F, 0x79, 0x67}, 4, 0},
    {0x61, (uint8_t[]){0x38, 0x11, 0x79, 0x67}, 4, 0},
    {0x64, (uint8_t[]){0x38, 0x17, 0x71, 0x5F, 0x79, 0x67}, 6, 0},
    {0x65, (uint8_t[]){0x38, 0x13, 0x71, 0x5B, 0x79, 0x67}, 6, 0},

    {0x6A, (uint8_t[]){0x00, 0x00}, 2, 0},

    {0x6C, (uint8_t[]){0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50}, 7, 0},

    {0x6E, (uint8_t[]){0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0F, 0x0F,
                       0x0D, 0x0D, 0x0B, 0x0B, 0x09, 0x09, 0x00, 0x00,
                       0x00, 0x00, 0x0A, 0x0A, 0x0C, 0x0C, 0x0E, 0x0E,
                       0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04}, 32, 0},

    {0xBF, (uint8_t[]){0x01}, 1, 0},
    {0xF9, (uint8_t[]){0x40}, 1, 0},

    {0x9B, (uint8_t[]){0x3B}, 1, 0},
    {0x93, (uint8_t[]){0x33, 0x7F, 0x00}, 3, 0},

    {0x7E, (uint8_t[]){0x30}, 1, 0},

    {0x70, (uint8_t[]){0x0D, 0x02, 0x08, 0x0D, 0x02, 0x08}, 6, 0},
    {0x71, (uint8_t[]){0x0D, 0x02, 0x08}, 3, 0},

    {0x91, (uint8_t[]){0x0E, 0x09}, 2, 0},

    {0xC3, (uint8_t[]){0x18}, 1, 0},
    {0xC4, (uint8_t[]){0x18}, 1, 0},
    {0xC9, (uint8_t[]){0x3C}, 1, 0},

    {0xF0, (uint8_t[]){0x13, 0x15, 0x04, 0x05, 0x01, 0x38}, 6, 0},
    {0xF2, (uint8_t[]){0x13, 0x15, 0x04, 0x05, 0x01, 0x34}, 6, 0},
    {0xF1, (uint8_t[]){0x4B, 0xB8, 0x7B, 0x34, 0x35, 0xEF}, 6, 0},
    {0xF3, (uint8_t[]){0x47, 0xB4, 0x72, 0x34, 0x35, 0xDA}, 6, 0},

    {0x36, (uint8_t[]){0x00}, 1, 0},   // MADCTL: RGB order, no mirroring

    {0x11, (uint8_t[]){0x00}, 0, 200}, // Sleep Out, then 200 ms
};

bool ColorTransDone(esp_lcd_panel_io_handle_t /*io*/, esp_lcd_panel_io_event_data_t* /*edata*/,
                    void* user_ctx) {
    auto* self = static_cast<Gc9d01Panel*>(user_ctx);
    return self ? self->NotifyDoneFromIsr() : false;
}
}  // namespace

Gc9d01Panel::~Gc9d01Panel() {
    for (auto*& s : stripe_) { if (s) { heap_caps_free(s); s = nullptr; } }
    if (panel_)    { esp_lcd_panel_del(panel_); panel_ = nullptr; }
    if (io_)       { esp_lcd_panel_io_del(io_); io_ = nullptr; }
    if (done_sem_) { vSemaphoreDelete(static_cast<SemaphoreHandle_t>(done_sem_)); done_sem_ = nullptr; }
}

bool Gc9d01Panel::NotifyDoneFromIsr() {
    done_count_.fetch_add(1, std::memory_order_release);
    auto sem = static_cast<SemaphoreHandle_t>(done_sem_);
    if (!sem) return false;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &woken);
    return woken == pdTRUE;
}

bool Gc9d01Panel::WaitDone(uint32_t target, uint32_t timeout_ms) {
    auto sem = static_cast<SemaphoreHandle_t>(done_sem_);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (done_count_.load(std::memory_order_acquire) < target) {
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return false;
        (void)xSemaphoreTake(sem, deadline - now);
    }
    return true;
}

esp_err_t Gc9d01Panel::Init(const Gc9d01Config& cfg) {
    cfg_ = cfg;

    done_sem_ = xSemaphoreCreateBinary();
    if (!done_sem_) return ESP_ERR_NO_MEM;

    // A pad hold left by a previous run (the production firmware holds these through sleep)
    // would swallow the reset pulse and the backlight level.
    (void)gpio_hold_dis(static_cast<gpio_num_t>(cfg_.pin_rst));
    (void)gpio_hold_dis(static_cast<gpio_num_t>(cfg_.pin_cs));
    if (cfg_.pin_backlight >= 0) {
        (void)gpio_hold_dis(static_cast<gpio_num_t>(cfg_.pin_backlight));
        gpio_config_t bl = {};
        bl.pin_bit_mask = 1ULL << cfg_.pin_backlight;
        bl.mode         = GPIO_MODE_OUTPUT;
        ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
        (void)SetBacklight(false);   // dark until GRAM holds black
    }

    const size_t stripe_bytes = static_cast<size_t>(cfg_.width) * cfg_.stripe_rows * 2u;
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = cfg_.pin_sck;
    bus.mosi_io_num     = cfg_.pin_mosi;
    bus.miso_io_num     = -1;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = static_cast<int>(stripe_bytes);   // one stripe per color transfer
    esp_err_t ret = spi_bus_initialize(cfg_.spi_host, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = already initialized
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num       = cfg_.pin_dc;
    io_cfg.cs_gpio_num       = cfg_.pin_cs;
    io_cfg.pclk_hz           = cfg_.pclk_hz;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    io_cfg.spi_mode          = 0;
    // One transfer in flight, and every stripe buffer is reused only after its own done callback:
    // the production firmware found the queue's implicit blocking alone let a buffer be refilled
    // while DMA was still reading it (stray colors in flat areas).
    io_cfg.trans_queue_depth = 1;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(cfg_.spi_host), &io_cfg, &io_),
        TAG, "new_panel_io_spi");

    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = ColorTransDone;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io_, &cbs, this), TAG, "io callbacks");

    gc9d01_vendor_config_t vendor = {};
    vendor.init_cmds      = kBoeInit;
    vendor.init_cmds_size = sizeof(kBoeInit) / sizeof(kBoeInit[0]);

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = cfg_.pin_rst;
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;   // matches MADCTL 0x00 in the table
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config  = &vendor;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9d01(io_, &panel_cfg, &panel_), TAG, "new_panel_gc9d01");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_),  TAG, "init");

    ESP_RETURN_ON_ERROR(EnsureStripes(stripe_bytes), TAG, "stripes");
    if (FillSolid(0x0000) != ESP_OK) ESP_LOGW(TAG, "initial clear failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_, true), TAG, "disp on");
    (void)SetBacklight(true);

    ESP_LOGI(TAG, "GC9D01 panel ready (%ux%u @ %luMHz, BOE sequence)", cfg_.width, cfg_.height,
             static_cast<unsigned long>(cfg_.pclk_hz / 1000000));
    return ESP_OK;
}

esp_err_t Gc9d01Panel::EnsureStripes(size_t bytes) {
    if (stripe_[0] && stripe_[1] && stripe_cap_ >= bytes) return ESP_OK;
    for (auto*& s : stripe_) { if (s) { heap_caps_free(s); s = nullptr; } }
    stripe_cap_ = 0;
    // Internal DMA RAM: the SPI master sends straight out of these.
    for (auto*& s : stripe_) {
        s = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(4, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!s) {
            for (auto*& t : stripe_) { if (t) { heap_caps_free(t); t = nullptr; } }
            return ESP_ERR_NO_MEM;
        }
    }
    stripe_cap_ = bytes;
    return ESP_OK;
}

esp_err_t Gc9d01Panel::FillSolid(uint16_t color) {
    if (!panel_ || !stripe_[0]) return ESP_ERR_INVALID_STATE;

    const size_t   row_bytes = static_cast<size_t>(cfg_.width) * 2u;
    const uint16_t rows_per  = static_cast<uint16_t>(stripe_cap_ / row_bytes);
    uint8_t* buf = stripe_[0];   // one solid stripe, sent again and again: no ping-pong needed
    const uint8_t hi = static_cast<uint8_t>(color >> 8);   // the panel latches the high byte first
    const uint8_t lo = static_cast<uint8_t>(color & 0xFF);
    for (size_t i = 0; i < rows_per * row_bytes; i += 2u) { buf[i] = hi; buf[i + 1u] = lo; }

    for (uint16_t row = 0; row < cfg_.height; row = static_cast<uint16_t>(row + rows_per)) {
        const uint16_t rows = std::min<uint16_t>(rows_per, static_cast<uint16_t>(cfg_.height - row));
        const uint32_t expect = done_count_.load(std::memory_order_acquire) + 1;
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(panel_, 0, row, cfg_.width, static_cast<uint16_t>(row + rows), buf),
            TAG, "fill");
        if (!WaitDone(expect, 500)) {
            ESP_LOGE(TAG, "fill: transfer timeout at row %u", static_cast<unsigned>(row));
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t Gc9d01Panel::DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* rgb565_be) {
    if (!panel_ || !rgb565_be || !stripe_[0]) return ESP_ERR_INVALID_STATE;
    if (w == 0 || h == 0) return ESP_OK;
    if (x + w > cfg_.width || y + h > cfg_.height) return ESP_ERR_INVALID_ARG;

    const size_t row_bytes = static_cast<size_t>(w) * 2u;
    size_t rows_per = stripe_cap_ / row_bytes;   // a narrow rectangle packs more rows per stripe
    if (rows_per > h) rows_per = h;

    const uint8_t* src = static_cast<const uint8_t*>(rgb565_be);
    // The done count at which each buffer's last transfer has finished; 0 = not used yet.
    uint32_t free_at[2] = {0, 0};
    uint32_t queued = done_count_.load(std::memory_order_acquire);
    int buf = 0;
    for (uint16_t row = 0; row < h; row = static_cast<uint16_t>(row + rows_per)) {
        const uint16_t rows = std::min<uint16_t>(static_cast<uint16_t>(rows_per), static_cast<uint16_t>(h - row));
        if (free_at[buf] && !WaitDone(free_at[buf], 500)) {
            ESP_LOGE(TAG, "blit: stripe %d still in flight at row %u", buf, static_cast<unsigned>(row));
            return ESP_ERR_TIMEOUT;
        }
        memcpy(stripe_[buf], src + static_cast<size_t>(row) * row_bytes, rows * row_bytes);
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(panel_, x, static_cast<uint16_t>(y + row), static_cast<uint16_t>(x + w),
                                      static_cast<uint16_t>(y + row + rows), stripe_[buf]),
            TAG, "blit");
        free_at[buf] = ++queued;
        buf ^= 1;
    }
    // Both stripes must be idle before the caller (or the next blit) touches anything.
    return WaitDone(queued, 500) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t Gc9d01Panel::SetBacklight(bool on) {
    if (cfg_.pin_backlight < 0) return ESP_OK;
    const int level = on == cfg_.backlight_active_high ? 1 : 0;
    return gpio_set_level(static_cast<gpio_num_t>(cfg_.pin_backlight), level);
}
