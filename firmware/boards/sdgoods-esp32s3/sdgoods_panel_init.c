// SDGOODS glass configuration; vendor sequence retains its Apache-2.0 notice.
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
typedef struct { uint8_t cmd; const void* data; size_t data_bytes; unsigned delay_ms; } st77916_lcd_init_cmd_t;
#include "st77916_vendor_init.inc"
esp_err_t sdgoods_panel_initialize(esp_lcd_panel_io_handle_t io) {
    const uint8_t madctl = 0x60, format = 0x55;
    esp_err_t err = esp_lcd_panel_io_tx_param(io, 0x02003600, &madctl, 1);
    if (err == ESP_OK) err = esp_lcd_panel_io_tx_param(io, 0x02003A00, &format, 1);
    for (size_t i = 0; err == ESP_OK && i < sizeof vendor_specific_init_vendor / sizeof vendor_specific_init_vendor[0]; ++i) {
        const st77916_lcd_init_cmd_t* cmd = &vendor_specific_init_vendor[i];
        err = esp_lcd_panel_io_tx_param(io, 0x02000000 | (cmd->cmd << 8), cmd->data, cmd->data_bytes);
        if (cmd->delay_ms) vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
    }
    return err;
}
