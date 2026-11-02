#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t;
typedef esp_lcd_panel_io_t* esp_lcd_panel_io_handle_t;
typedef struct { bool (*on_color_trans_done)(void*, void*, void*); } esp_lcd_panel_io_callbacks_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t, int, const void*, size_t);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t);
#ifdef __cplusplus
}
#endif
