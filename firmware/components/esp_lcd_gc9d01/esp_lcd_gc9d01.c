/*
 * SPDX-FileCopyrightText: 2024 RoRoLee
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_log.h"

#include "esp_lcd_gc9d01.h"

static const char *TAG = "gc9d01";

static esp_err_t panel_gc9d01_del(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9d01_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9d01_init(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9d01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_gc9d01_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_gc9d01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_gc9d01_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_gc9d01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_gc9d01_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;  // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val;  // save current value of LCD_CMD_COLMOD register
    const gc9d01_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int reset_level: 1;
    } flags;
} gc9d01_panel_t;


/* GC9D01-specific initialization command sequence */
static const gc9d01_lcd_init_cmd_t vendor_specific_init_default[] = {
    // Inter-register enable 1
    {0xFE, (uint8_t []){0x00}, 0, 0},
    // Inter-register enable 2
    {0xEF, (uint8_t []){0x00}, 0, 0},
    // Power control
    {0xB0, (uint8_t []){0xC0}, 1, 0},
    {0xB2, (uint8_t []){0x2F}, 1, 0},
    {0xB3, (uint8_t []){0x03}, 1, 0},
    {0xB6, (uint8_t []){0x19}, 1, 0},
    {0xB7, (uint8_t []){0x01}, 1, 0},
    {0xAC, (uint8_t []){0xCB}, 1, 0},
    {0xAB, (uint8_t []){0x0E}, 1, 0},
    // Gamma settings
    {0x3A, (uint8_t []){0x05}, 1, 0},  // Pixel format: RGB565
    {0xB4, (uint8_t []){0x04}, 1, 0},
    {0xA8, (uint8_t []){0x19}, 1, 0},
    // Positive gamma
    {0x60, (uint8_t []){0x38, 0x09, 0x6D, 0x67, 0x2F, 0x5C}, 6, 0},
    {0x61, (uint8_t []){0x38, 0x09, 0x6D, 0x67, 0x2F, 0x5C}, 6, 0},
    {0x62, (uint8_t []){0x38, 0x0B, 0x70, 0x6A, 0x30, 0x5C}, 6, 0},
    {0x63, (uint8_t []){0x38, 0x0D, 0x72, 0x6C, 0x31, 0x5C}, 6, 0},
    {0x64, (uint8_t []){0x38, 0x11, 0x74, 0x6E, 0x32, 0x5C}, 6, 0},
    // Negative gamma
    {0x65, (uint8_t []){0x28, 0x09, 0x5D, 0x57, 0x1F, 0x4C}, 6, 0},
    {0x66, (uint8_t []){0x28, 0x09, 0x5D, 0x57, 0x1F, 0x4C}, 6, 0},
    {0x67, (uint8_t []){0x20, 0x0B, 0x60, 0x5A, 0x20, 0x4C}, 6, 0},
    {0x68, (uint8_t []){0x28, 0x0D, 0x62, 0x5C, 0x21, 0x4C}, 6, 0},
    {0x69, (uint8_t []){0x28, 0x11, 0x64, 0x5E, 0x22, 0x4C}, 6, 0},
    // Additional power/display settings
    {0x80, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x01}, 1, 0},
    {0x7E, (uint8_t []){0x20}, 1, 0},
    {0xBE, (uint8_t []){0x11}, 1, 0},
    {0xBC, (uint8_t []){0x40}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    // Display inversion on
    {LCD_CMD_INVON, (uint8_t []){0x00}, 0, 0},
    // Sleep out
    {LCD_CMD_SLPOUT, (uint8_t []){0x00}, 0, 120},
    // Display on
    {LCD_CMD_DISPON, (uint8_t []){0x00}, 0, 20},
};

esp_err_t esp_lcd_new_panel_gc9d01(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    esp_err_t ret = ESP_OK;
    gc9d01_panel_t *gc9d01 = NULL;
    gc9d01 = calloc(1, sizeof(gc9d01_panel_t));
    ESP_GOTO_ON_FALSE(gc9d01, ESP_ERR_NO_MEM, err, TAG, "no mem for gc9d01 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        gc9d01->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        gc9d01->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color element order");
        break;
    }

    uint8_t fb_bits_per_pixel = 0;
    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        gc9d01->colmod_val = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        gc9d01->colmod_val = 0x66;
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    gc9d01->io = io;
    gc9d01->reset_gpio_num = panel_dev_config->reset_gpio_num;
    gc9d01->fb_bits_per_pixel = fb_bits_per_pixel;
    gc9d01_vendor_config_t *vendor_config = (gc9d01_vendor_config_t *)panel_dev_config->vendor_config;
    if (vendor_config) {
        gc9d01->init_cmds = vendor_config->init_cmds;
        gc9d01->init_cmds_size = vendor_config->init_cmds_size;
    }
    gc9d01->flags.reset_level = panel_dev_config->flags.reset_active_high;
    gc9d01->base.del = panel_gc9d01_del;
    gc9d01->base.reset = panel_gc9d01_reset;
    gc9d01->base.init = panel_gc9d01_init;
    gc9d01->base.draw_bitmap = panel_gc9d01_draw_bitmap;
    gc9d01->base.invert_color = panel_gc9d01_invert_color;
    gc9d01->base.set_gap = panel_gc9d01_set_gap;
    gc9d01->base.mirror = panel_gc9d01_mirror;
    gc9d01->base.swap_xy = panel_gc9d01_swap_xy;
    gc9d01->base.disp_on_off = panel_gc9d01_disp_on_off;
    *ret_panel = &(gc9d01->base);
    ESP_LOGD(TAG, "new gc9d01 panel @%p", gc9d01);

    return ESP_OK;

err:
    if (gc9d01) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(gc9d01);
    }
    return ret;
}

static esp_err_t panel_gc9d01_del(esp_lcd_panel_t *panel)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);

    if (gc9d01->reset_gpio_num >= 0) {
        gpio_reset_pin(gc9d01->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del gc9d01 panel @%p", gc9d01);
    free(gc9d01);
    return ESP_OK;
}

static esp_err_t panel_gc9d01_reset(esp_lcd_panel_t *panel)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9d01->io;

    // Perform hardware reset
    if (gc9d01->reset_gpio_num >= 0) {
        gpio_set_level(gc9d01->reset_gpio_num, gc9d01->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9d01->reset_gpio_num, !gc9d01->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_gc9d01_init(esp_lcd_panel_t *panel)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9d01->io;
    const gc9d01_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    // Send MADCTL
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9d01->madctl_val,
    }, 1), TAG, "send command failed");
    // Send COLMOD (pixel format)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        gc9d01->colmod_val,
    }, 1), TAG, "send command failed");

    // Vendor specific initialization
    if (gc9d01->init_cmds) {
        init_cmds = gc9d01->init_cmds;
        init_cmds_size = gc9d01->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(gc9d01_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        switch (init_cmds[i].cmd) {
        case LCD_CMD_MADCTL:
            is_cmd_overwritten = true;
            gc9d01->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        case LCD_CMD_COLMOD:
            is_cmd_overwritten = true;
            gc9d01->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        default:
            is_cmd_overwritten = false;
            break;
        }

        if (is_cmd_overwritten) {
            ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                     init_cmds[i].cmd);
        }

        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes),
                            TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_gc9d01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = gc9d01->io;

    x_start += gc9d01->x_gap;
    x_end += gc9d01->x_gap;
    y_start += gc9d01->y_gap;
    y_end += gc9d01->y_gap;

    // Set column address (0x2A)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");

    // Set row address (0x2B)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");

    // Write memory (0x2C)
    size_t len = (x_end - x_start) * (y_end - y_start) * gc9d01->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "send color data failed");

    return ESP_OK;
}

static esp_err_t panel_gc9d01_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9d01->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9d01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9d01->io;

    if (mirror_x) {
        gc9d01->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        gc9d01->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        gc9d01->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        gc9d01->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9d01->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9d01_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9d01->io;

    if (swap_axes) {
        gc9d01->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        gc9d01->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9d01->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9d01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);
    gc9d01->x_gap = x_gap;
    gc9d01->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_gc9d01_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    gc9d01_panel_t *gc9d01 = __containerof(panel, gc9d01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9d01->io;
    int command = 0;

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
