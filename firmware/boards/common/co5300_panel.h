#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// Co5300Panel: reusable driver for the CO5300 466x466 round AMOLED, in boards/common. Two buses:
//
//   - MIPI-DSI (ESP32-P4): LDO (2.5V for the D-PHY) -> DSI bus -> DBI command port -> CO5300 panel.
//     The board only supplies the reset pin + resolution; the official esp_lcd_co5300 component
//     handles lane count / bitrate / init sequence.
//   - QSPI (ESP32-S3, e.g. the M5Stack StopWatch): esp_lcd_co5300 over a quad SPI bus. Draws go
//     through two internal DMA stripes, so the source may live in PSRAM. The glass sits inside the
//     controller's 480x480 RAM at x_gap/y_gap, and the controller only takes windows that start on
//     an even column and row and have an even width and height: keep every draw aligned that way.
//
// Note: this header deliberately includes no MIPI headers; handles are stored as void* (cast to the
//   real types in the .cc). That keeps it chip-independent, so it compiles fine even though
//   boards/common is GLOB-compiled for every board; the real bring-up lives in co5300_hal.c.
// ═══════════════════════════════════════════════════════════════════════════════
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/spi_master.h"
#include "esp_err.h"

enum class Co5300Bus : uint8_t {
    kMipiDsi,   // ESP32-P4
    kQspi,
};

struct Co5300Config {
    Co5300Bus bus         = Co5300Bus::kMipiDsi;
    int      rst_gpio     = -1;   // reset GPIO (active low); -1 = none, or the board resets the panel itself
    int      pwr_en_gpio  = -1;   // MIPI: panel power-enable GPIO (active high, e.g. VCI_EN); -1 = none
    uint16_t width        = 466;
    uint16_t height       = 466;

    // QSPI only
    spi_host_device_t spi_host = SPI2_HOST;
    int      pin_sclk     = -1;
    int      pin_d0       = -1;
    int      pin_d1       = -1;
    int      pin_d2       = -1;
    int      pin_d3       = -1;
    int      pin_cs       = -1;
    uint32_t pclk_hz      = 40 * 1000 * 1000;
    uint16_t x_gap        = 0;    // first visible column / row in the controller's RAM
    uint16_t y_gap        = 0;
    bool     bgr          = false;
    // Rows per DMA stripe, even. Two stripes of width*rows*2 bytes live in internal RAM.
    uint16_t stripe_rows  = 12;
};

class Co5300Panel {
public:
    Co5300Panel() = default;
    ~Co5300Panel();

    Co5300Panel(const Co5300Panel&) = delete;
    Co5300Panel& operator=(const Co5300Panel&) = delete;

    // Bring the panel up and turn it on. MIPI: lit at full brightness. QSPI: black, at brightness 0
    // until SetBrightness().
    esp_err_t Init(const Co5300Config& cfg);

    // Fill the whole screen with one RGB565 color. Valid only after Init() succeeds.
    esp_err_t FillSolid(uint16_t rgb565_color);

    // Blit w*h RGB565 pixels at (x, y), row-major. QSPI: big-endian bytes, any memory, returns once
    // the pixels are on the wire. MIPI: native byte order, copied into the framebuffer.
    esp_err_t DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels);

    // Screen brightness 0-255 (write 0x51). An AMOLED has no backlight panel; brightness is controlled inside the CO5300.
    esp_err_t SetBrightness(uint8_t level);

    esp_err_t DisplayOn();
    esp_err_t DisplayOff();

    uint16_t Width()  const { return cfg_.width; }
    uint16_t Height() const { return cfg_.height; }
    bool     Ready()  const { return panel_ != nullptr; }

    // The QSPI transfer-done callback lands here; not for callers.
    bool NotifyDoneFromIsr();

private:
    esp_err_t InitQspi();
    esp_err_t EnsureStripes(size_t bytes);
    bool      WaitDone(uint32_t target, uint32_t timeout_ms);
    int       Cmd(uint8_t cmd) const;

    Co5300Config cfg_       = {};
    void*        ldo_       = nullptr;  // esp_ldo_channel_handle_t
    void*        dsi_bus_   = nullptr;  // esp_lcd_dsi_bus_handle_t
    void*        io_        = nullptr;  // esp_lcd_panel_io_handle_t (DBI command port on MIPI)
    void*        panel_     = nullptr;  // esp_lcd_panel_handle_t (CO5300, includes DPI on MIPI)
    uint8_t*     fb_        = nullptr;  // MIPI: full-screen framebuffer (PSRAM)
    size_t       fb_bytes_  = 0;

    // QSPI
    void*                 done_sem_   = nullptr;              // SemaphoreHandle_t
    uint8_t*              stripe_[2]  = {nullptr, nullptr};   // internal DMA RAM
    size_t                stripe_cap_ = 0;                    // bytes in each stripe
    std::atomic<uint32_t> done_count_{0};                     // transfers finished so far
};
