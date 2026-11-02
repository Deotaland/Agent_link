#pragma once
#include "esp_err.h"
typedef struct fake_panel* esp_lcd_panel_handle_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void*);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t, int, int);
#ifdef __cplusplus
}
#endif
