// Co5300Panel: CO5300 466x466 round AMOLED reusable driver implementation, see co5300_panel.h.
//
// Split of responsibilities:
//   - Low-level bring-up (esp_lcd_co5300's headers and config macros are C-only) -> co5300_hal.c (compiled as C).
//   - This C++ file only wraps it for boards: it calls the hal to bring up the chain, then uses the
//     generic esp_lcd API for fill/blit/brightness. Those generic APIs (draw_bitmap / io_tx_param / disp_on_off) are
//     plain functions usable from C++ and common across chips, so this file needs no per-target gating
//     (where the hal returns "not supported", panel_ stays null, which no-ops safely).
#include "co5300_panel.h"
#include "co5300_hal.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "co5300";

bool ColorTransDone(esp_lcd_panel_io_handle_t /*io*/, esp_lcd_panel_io_event_data_t* /*edata*/,
                    void* user_ctx) {
    auto* self = static_cast<Co5300Panel*>(user_ctx);
    return self ? self->NotifyDoneFromIsr() : false;
}
}  // namespace

Co5300Panel::~Co5300Panel() {
    for (auto*& s : stripe_) { if (s) { heap_caps_free(s); s = nullptr; } }
    if (fb_)    { heap_caps_free(fb_); fb_ = nullptr; }
    if (panel_) { esp_lcd_panel_del(static_cast<esp_lcd_panel_handle_t>(panel_)); panel_ = nullptr; }
    if (io_)    { esp_lcd_panel_io_del(static_cast<esp_lcd_panel_io_handle_t>(io_)); io_ = nullptr; }
    if (done_sem_) { vSemaphoreDelete(static_cast<SemaphoreHandle_t>(done_sem_)); done_sem_ = nullptr; }
    // Freeing dsi_bus_/ldo_ needs MIPI-only headers; this board is a resident singleton whose destructor almost never runs, so we skip deleting them one by one.
}

esp_err_t Co5300Panel::Init(const Co5300Config& cfg) {
    cfg_ = cfg;
    if (cfg_.bus == Co5300Bus::kQspi) return InitQspi();

    // Warn loudly when the reset pin is unset (placeholder -1): the CO5300 datasheet requires an active-low hardware reset to initialize correctly.
    if (cfg_.rst_gpio < 0) {
        ESP_LOGW(TAG, "reset pin not configured (placeholder %d): the CO5300 needs an active-low "
                      "hardware reset, so the panel may not initialize; set DISPLAY_RST_PIN to a real "
                      "GPIO in config.h and reflash", cfg_.rst_gpio);
    }

    // Hand the low-level bring-up to the C side (isolates esp_lcd_co5300's C-only macros, see co5300_hal.c).
    co5300_hal_handles_t h = {};
    const esp_err_t err = co5300_hal_init(cfg_.rst_gpio, cfg_.pwr_en_gpio, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "co5300_hal_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ldo_ = h.ldo; dsi_bus_ = h.dsi_bus; io_ = h.io; panel_ = h.panel;

    (void)SetBrightness(0xFF);   // AMOLED brightness goes through DBI 0x51; set it to max so the self-test is visible
    ESP_LOGI(TAG, "CO5300 %ux%u MIPI-DSI ready (rst=%d)", cfg_.width, cfg_.height, cfg_.rst_gpio);
    return ESP_OK;
}

esp_err_t Co5300Panel::InitQspi() {
    cfg_.stripe_rows = std::max<uint16_t>(2, static_cast<uint16_t>(cfg_.stripe_rows & ~1u));
    done_sem_ = xSemaphoreCreateBinary();
    if (!done_sem_) return ESP_ERR_NO_MEM;

    const size_t stripe_bytes = static_cast<size_t>(cfg_.width) * cfg_.stripe_rows * 2u;
    co5300_qspi_config_t q = {};
    q.spi_host           = cfg_.spi_host;
    q.pin_sclk           = cfg_.pin_sclk;
    q.pin_d0             = cfg_.pin_d0;
    q.pin_d1             = cfg_.pin_d1;
    q.pin_d2             = cfg_.pin_d2;
    q.pin_d3             = cfg_.pin_d3;
    q.pin_cs             = cfg_.pin_cs;
    q.pclk_hz            = cfg_.pclk_hz;
    q.max_transfer_bytes = stripe_bytes;   // one stripe per pixel transfer
    q.rst_gpio           = cfg_.rst_gpio;
    q.x_gap              = cfg_.x_gap;
    q.y_gap              = cfg_.y_gap;
    q.bgr                = cfg_.bgr;
    co5300_hal_handles_t h = {};
    esp_err_t err = co5300_hal_init_qspi(&q, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "co5300_hal_init_qspi failed: %s", esp_err_to_name(err));
        return err;
    }
    io_ = h.io;
    panel_ = h.panel;

    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = ColorTransDone;
    err = esp_lcd_panel_io_register_event_callbacks(static_cast<esp_lcd_panel_io_handle_t>(io_), &cbs, this);
    if (err == ESP_OK) err = EnsureStripes(stripe_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QSPI setup: %s", esp_err_to_name(err));
        return err;
    }

    // RAM holds noise after power-up: clear it before the display goes on.
    if (FillSolid(0x0000) != ESP_OK) ESP_LOGW(TAG, "initial clear failed");
    err = esp_lcd_panel_disp_on_off(static_cast<esp_lcd_panel_handle_t>(panel_), true);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "CO5300 %ux%u QSPI ready (%lu MHz, gap %u,%u)", cfg_.width, cfg_.height,
             static_cast<unsigned long>(cfg_.pclk_hz / 1000000), cfg_.x_gap, cfg_.y_gap);
    return ESP_OK;
}

esp_err_t Co5300Panel::EnsureStripes(size_t bytes) {
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

bool Co5300Panel::NotifyDoneFromIsr() {
    done_count_.fetch_add(1, std::memory_order_release);
    auto sem = static_cast<SemaphoreHandle_t>(done_sem_);
    if (!sem) return false;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &woken);
    return woken == pdTRUE;
}

bool Co5300Panel::WaitDone(uint32_t target, uint32_t timeout_ms) {
    auto sem = static_cast<SemaphoreHandle_t>(done_sem_);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (done_count_.load(std::memory_order_acquire) < target) {
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return false;
        (void)xSemaphoreTake(sem, deadline - now);
    }
    return true;
}

// QSPI frames a command as opcode 0x02 with the command in the middle of a 24-bit address.
int Co5300Panel::Cmd(uint8_t cmd) const {
    return cfg_.bus == Co5300Bus::kQspi ? static_cast<int>((0x02u << 24) | (static_cast<uint32_t>(cmd) << 8))
                                        : cmd;
}

esp_err_t Co5300Panel::FillSolid(uint16_t color) {
    if (!panel_) return ESP_ERR_INVALID_STATE;
    auto* panel = static_cast<esp_lcd_panel_handle_t>(panel_);

    if (cfg_.bus == Co5300Bus::kQspi) {
        if (!stripe_[0]) return ESP_ERR_INVALID_STATE;
        const size_t   row_bytes = static_cast<size_t>(cfg_.width) * 2u;
        const uint16_t rows_per  = static_cast<uint16_t>((stripe_cap_ / row_bytes) & ~static_cast<size_t>(1));
        uint8_t* buf = stripe_[0];   // one solid stripe, sent again and again: no ping-pong needed
        const uint8_t hi = static_cast<uint8_t>(color >> 8);   // high byte first on the wire
        const uint8_t lo = static_cast<uint8_t>(color & 0xFF);
        for (size_t i = 0; i < rows_per * row_bytes; i += 2u) { buf[i] = hi; buf[i + 1u] = lo; }

        for (uint16_t row = 0; row < cfg_.height; row = static_cast<uint16_t>(row + rows_per)) {
            const uint16_t rows = std::min<uint16_t>(rows_per, static_cast<uint16_t>(cfg_.height - row));
            const uint32_t expect = done_count_.load(std::memory_order_acquire) + 1;
            const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, row, cfg_.width,
                                                            static_cast<uint16_t>(row + rows), buf);
            if (err != ESP_OK) return err;
            if (!WaitDone(expect, 500)) {
                ESP_LOGE(TAG, "fill: transfer timeout at row %u", static_cast<unsigned>(row));
                return ESP_ERR_TIMEOUT;
            }
        }
        return ESP_OK;
    }

    const size_t px    = static_cast<size_t>(cfg_.width) * cfg_.height;
    const size_t bytes = px * sizeof(uint16_t);            // 466*466*2 ~= 434KB
    if (!fb_ || fb_bytes_ < bytes) {
        if (fb_) heap_caps_free(fb_);
        // The full-screen framebuffer must live in PSRAM (434KB will not fit in internal SRAM) -> config.json needs CONFIG_SPIRAM=y.
        fb_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        if (!fb_) { fb_bytes_ = 0; return ESP_ERR_NO_MEM; }
        fb_bytes_ = bytes;
    }
    // The MIPI DPI RGB565 framebuffer is native-endian uint16 (unlike SH8501's hand-packed big-endian).
    // If self-test colors are wrong (e.g. red shows as blue), it is usually a byte-order / element-order issue; try rgb_ele_order first.
    uint16_t* p = reinterpret_cast<uint16_t*>(fb_);
    for (size_t i = 0; i < px; ++i) p[i] = color;
    return esp_lcd_panel_draw_bitmap(panel, 0, 0, cfg_.width, cfg_.height, fb_);
}

esp_err_t Co5300Panel::DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels) {
    if (!panel_ || !pixels) return ESP_ERR_INVALID_STATE;
    if (w == 0 || h == 0) return ESP_OK;
    if (x + w > cfg_.width || y + h > cfg_.height) return ESP_ERR_INVALID_ARG;
    auto* panel = static_cast<esp_lcd_panel_handle_t>(panel_);
    if (cfg_.bus != Co5300Bus::kQspi) {
        return esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, pixels);
    }
    if (!stripe_[0]) return ESP_ERR_INVALID_STATE;

    const size_t row_bytes = static_cast<size_t>(w) * 2u;
    // A narrow rectangle packs more rows per stripe; keep the count even so every window starts on
    // an even row.
    size_t rows_per = (stripe_cap_ / row_bytes) & ~static_cast<size_t>(1);
    if (rows_per > h) rows_per = h;

    const uint8_t* src = static_cast<const uint8_t*>(pixels);
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
        const esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x, static_cast<uint16_t>(y + row),
                                                        static_cast<uint16_t>(x + w),
                                                        static_cast<uint16_t>(y + row + rows), stripe_[buf]);
        if (err != ESP_OK) return err;
        free_at[buf] = ++queued;
        buf ^= 1;
    }
    // Both stripes must be idle before the caller (or the next blit) touches anything.
    return WaitDone(queued, 500) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t Co5300Panel::SetBrightness(uint8_t level) {
    if (!io_) return ESP_ERR_INVALID_STATE;
    const uint8_t p[] = { level };
    return esp_lcd_panel_io_tx_param(static_cast<esp_lcd_panel_io_handle_t>(io_), Cmd(0x51), p, sizeof(p));
}

esp_err_t Co5300Panel::DisplayOn() {
    return panel_ ? esp_lcd_panel_disp_on_off(static_cast<esp_lcd_panel_handle_t>(panel_), true)
                  : ESP_ERR_INVALID_STATE;
}
esp_err_t Co5300Panel::DisplayOff() {
    return panel_ ? esp_lcd_panel_disp_on_off(static_cast<esp_lcd_panel_handle_t>(panel_), false)
                  : ESP_ERR_INVALID_STATE;
}
