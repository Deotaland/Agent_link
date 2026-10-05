#pragma once
// Gc9d01Panel: the GC9D01 160x160 round TFT of the WOWNNY board (the rorolee PCB with a different
// screen). Reusable driver in boards/common.
//
// esp_lcd_panel_io_spi with DMA, SPI mode 0, and the BOE glass's own init sequence, which the
// controller's default sequence does not light. The sequence stops at SLPOUT: GRAM is cleared to
// black before DISPON and the backlight comes on last, so the power-up noise in GRAM never shows.
//
// Draws are synchronous: DrawBitmap returns once the pixels are on the wire, and the source may
// live in PSRAM. Rows are staged through two internal DMA buffers, each reused only after the panel
// IO reports its previous transfer done.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

struct Gc9d01Config {
    spi_host_device_t spi_host;
    int      pin_sck;
    int      pin_mosi;
    int      pin_cs;
    int      pin_dc;
    int      pin_rst;
    int      pin_backlight         = -1;      // -1: none, always lit
    bool     backlight_active_high = false;
    uint16_t width                 = 160;
    uint16_t height                = 160;
    uint32_t pclk_hz               = 20 * 1000 * 1000;
    // Height of one full-width stripe. Two of them live in internal DMA RAM (width*rows*2 bytes
    // each), so this trades internal RAM against the number of transfers per blit.
    uint16_t stripe_rows           = 10;
};

class Gc9d01Panel {
public:
    Gc9d01Panel() = default;
    ~Gc9d01Panel();

    Gc9d01Panel(const Gc9d01Panel&) = delete;
    Gc9d01Panel& operator=(const Gc9d01Panel&) = delete;

    // Bring up the SPI bus and the panel: on return the screen is on, black, with the backlight lit.
    esp_err_t Init(const Gc9d01Config& cfg);

    // Fill the whole screen with one RGB565 color.
    esp_err_t FillSolid(uint16_t rgb565_color);

    // Blit a rectangle of big-endian RGB565 pixels (row-major, w*h*2 bytes) at (x, y).
    esp_err_t DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* rgb565_be);

    // The backlight is on/off only: this board drives its enable line from a plain GPIO.
    esp_err_t SetBacklight(bool on);

    uint16_t Width()  const { return cfg_.width; }
    uint16_t Height() const { return cfg_.height; }
    bool     Ready()  const { return panel_ != nullptr; }

    // The panel IO's transfer-done callback lands here; not for callers.
    bool NotifyDoneFromIsr();

private:
    bool      WaitDone(uint32_t target, uint32_t timeout_ms);
    esp_err_t EnsureStripes(size_t bytes);

    Gc9d01Config              cfg_        = {};
    esp_lcd_panel_io_handle_t io_         = nullptr;
    esp_lcd_panel_handle_t    panel_      = nullptr;
    void*                     done_sem_   = nullptr;              // SemaphoreHandle_t
    uint8_t*                  stripe_[2]  = {nullptr, nullptr};   // internal DMA SRAM
    size_t                    stripe_cap_ = 0;                    // bytes in EACH stripe
    std::atomic<uint32_t>     done_count_{0};                     // transfers finished so far
};
