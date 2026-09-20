#pragma once
// Sh8501LkPanel: reusable driver for the 简码 (short-code / "LK") SH8501 AMOLED, in boards/common.
//
// It is a different bring-up from Sh8501Panel even though the controller is the same chip:
//   - the LK driver owns its SPI device (8-bit commands, blocking transfers) instead of going
//     through esp_lcd_panel_io, so this class initialises the SPI *bus* and hands the pins to the
//     driver through vendor_config;
//   - the init sequence is the short code: standard user commands only, with power and
//     gamma coming from the panel's OTP. A panel whose OTP was never burned stays black on this
//     path and needs Sh8501Panel (full vendor sequence) instead.
// See components/esp_lcd_sh8501_lk/README.md for the full comparison.
//
// Same interface as Sh8501Panel, so a board swaps one type name and nothing else. Draws are
// synchronous: DrawBitmap returns once the pixels are on the wire, and the source may live in
// PSRAM (pixels are staged through an internal DMA buffer).
#include <cstddef>
#include <cstdint>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "sh8501_panel.h"   // rgb565 color constants, shared with the non-LK panel

struct Sh8501LkConfig {
    spi_host_device_t spi_host;
    int      pin_sck;
    int      pin_mosi;
    int      pin_cs;
    int      pin_dc;
    int      pin_rst;
    uint16_t width;
    uint16_t height;
    uint32_t pclk_hz;
    // No spi_mode: the LK driver fixes the device at SPI mode 0 (this is the mode the factory
    // short code is validated at, and it is not configurable through vendor_config).
};

class Sh8501LkPanel {
public:
    Sh8501LkPanel() = default;
    ~Sh8501LkPanel();

    Sh8501LkPanel(const Sh8501LkPanel&) = delete;
    Sh8501LkPanel& operator=(const Sh8501LkPanel&) = delete;

    // Bring up the SPI bus + panel and light it: on return the screen is on, cleared to black,
    // and at full brightness (the clear happens with the backlight driven to 0 so the random
    // power-on GRAM contents are never visible).
    esp_err_t Init(const Sh8501LkConfig& cfg);

    // Fill the whole screen with one RGB565 color.
    esp_err_t FillSolid(uint16_t rgb565_color);

    // Blit a rectangle of big-endian RGB565 pixels (row-major, w*h*2 bytes). The source may be
    // anywhere, PSRAM included - pixels are copied through an internal DMA-capable staging
    // buffer a few rows at a time, so the caller may reuse it as soon as this returns.
    esp_err_t DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* rgb565_be);

    // Screen brightness 0-255 (the LK driver takes a percentage; this converts).
    esp_err_t SetBrightness(uint8_t level);

    esp_err_t DisplayOn();
    esp_err_t DisplayOff();

    uint16_t Width()  const { return cfg_.width; }
    uint16_t Height() const { return cfg_.height; }
    bool     Ready()  const { return panel_ != nullptr; }

private:
    esp_err_t EnsureStripe(size_t bytes);

    Sh8501LkConfig         cfg_        = {};
    esp_lcd_panel_handle_t panel_      = nullptr;
    uint8_t*               stripe_     = nullptr;   // internal DMA SRAM
    size_t                 stripe_cap_ = 0;
};
