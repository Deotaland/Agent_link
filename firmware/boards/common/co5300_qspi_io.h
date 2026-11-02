#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

// Synchronous CO5300 IO: opcode (8 bits), address (24 bits), then optional
// serial parameters or quad pixels in one CS assertion. No polling/bus lock.
esp_err_t co5300_new_qspi_io(spi_host_device_t host, int cs, uint32_t clock_hz,
                            size_t max_bytes, esp_lcd_panel_io_handle_t *out);

// Clear a fault only after the outstanding descriptor has been returned by SPI.
// Nonblocking when DMA is still pending. The caller must then reinitialize the panel.
esp_err_t co5300_qspi_io_recover(esp_lcd_panel_io_handle_t io, bool *recovered);

#ifdef __cplusplus
}
#endif
