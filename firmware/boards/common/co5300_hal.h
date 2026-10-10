#pragma once
// C interface for the CO5300 low-level bring-up (esp_lcd_co5300's headers and config macros are
// C-only), called from the C++ Co5300Panel. Two buses: MIPI-DSI and QSPI.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Handles returned after bring-up
typedef struct {
    void *ldo;      // esp_ldo_channel_handle_t (MIPI only)
    void *dsi_bus;  // esp_lcd_dsi_bus_handle_t (MIPI only)
    void *io;       // esp_lcd_panel_io_handle_t (DBI command port on MIPI, QSPI device otherwise)
    void *panel;    // esp_lcd_panel_handle_t (CO5300, includes DPI on MIPI)
} co5300_hal_handles_t;

// Bring up the full LDO -> DSI -> DBI -> DPI -> CO5300 chain and turn the panel on. rst_gpio < 0 means no reset pin (placeholder).
// pwr_en_gpio: panel power-enable pin (active high, e.g. VCI_EN), < 0 = none; driven high with a settle delay before reset/init.
// Returns ESP_OK and fills out on success; returns an error code on failure (out contents undefined).
esp_err_t co5300_hal_init(int rst_gpio, int pwr_en_gpio, co5300_hal_handles_t *out);

typedef struct {
    int      spi_host;            // spi_host_device_t
    int      pin_sclk;
    int      pin_d0, pin_d1, pin_d2, pin_d3;
    int      pin_cs;
    uint32_t pclk_hz;
    size_t   max_transfer_bytes;  // largest single pixel transfer
    int      rst_gpio;            // < 0: software reset only
    int      x_gap, y_gap;        // where the glass starts in the controller's RAM
    bool     bgr;
} co5300_qspi_config_t;

// QSPI bus + panel IO + CO5300, reset and initialised up to sleep-out. The display stays off and
// its RAM is not cleared: the caller clears it, then turns the display on.
esp_err_t co5300_hal_init_qspi(const co5300_qspi_config_t *cfg, co5300_hal_handles_t *out);

#ifdef __cplusplus
}
#endif
