/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileContributor: 2026 DEOTALAND LIMITED
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NOTICE: This file is a modified version of Espressif's esp_lcd_sh8501 panel driver.
 * Changes by DEOTALAND LIMITED, for the factory short-code ("LK") SH8501 AMOLED panels:
 *   - the vendor init sequence is replaced with the factory short code (standard user
 *     commands only; power and gamma come from the panel's OTP);
 *   - the driver owns its own SPI device (8-bit commands, blocking transfers) instead of
 *     going through esp_lcd_panel_io;
 *   - symbols are renamed to the sh8501_lk_* namespace so both drivers can coexist.
 */

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include "esp_lcd_sh8501_lk.h"

#define SH8501_LK_SPI_CHUNK_BYTES   (4092)

static const char *TAG = "sh8501_lk";

static const sh8501_lk_lcd_init_cmd_t lk_init_cmds_default[] = {
    {0x11, (uint8_t[]){0}, 0, 60},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0x77}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x44, (uint8_t[]){0x01, 0x27}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 60},
    {0x29, (uint8_t[]){0}, 0, 120},
    {0x39, (uint8_t[]){0}, 0, 0},
};

typedef struct {
    esp_lcd_panel_t base;
    spi_device_handle_t spi;
    spi_host_device_t spi_host;
    int reset_gpio_num;
    int dc_gpio_num;
    int cs_gpio_num;
    uint32_t pclk_hz;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
    const sh8501_lk_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int reset_level : 1;
    } flags;
} sh8501_lk_panel_t;

static esp_err_t panel_sh8501_lk_del(esp_lcd_panel_t *panel);
static esp_err_t panel_sh8501_lk_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_sh8501_lk_init(esp_lcd_panel_t *panel);
static esp_err_t panel_sh8501_lk_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                             const void *color_data);
static esp_err_t panel_sh8501_lk_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_sh8501_lk_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_sh8501_lk_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_sh8501_lk_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_sh8501_lk_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

static esp_err_t lk_spi_write_comm(sh8501_lk_panel_t *lk, uint8_t cmd)
{
    gpio_set_level(lk->dc_gpio_num, 0);
    spi_transaction_t trans = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    return spi_device_polling_transmit(lk->spi, &trans);
}

static esp_err_t lk_spi_write_data(sh8501_lk_panel_t *lk, const void *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_OK;
    }
    gpio_set_level(lk->dc_gpio_num, 1);
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(lk->spi, &trans);
}

static esp_err_t lk_spi_tx_locked(sh8501_lk_panel_t *lk, uint8_t cmd, const void *data, size_t data_len)
{
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(lk->spi, portMAX_DELAY), TAG, "acquire bus failed");

    esp_err_t ret = lk_spi_write_comm(lk, cmd);
    if (ret == ESP_OK && data && data_len > 0) {
        ret = lk_spi_write_data(lk, data, data_len);
    }

    spi_device_release_bus(lk->spi);
    return ret;
}

static esp_err_t lk_spi_write_pixels_locked(sh8501_lk_panel_t *lk, const void *data, size_t len)
{
    esp_err_t ret = ESP_OK;
    const uint8_t *ptr = data;

    gpio_set_level(lk->dc_gpio_num, 1);

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > SH8501_LK_SPI_CHUNK_BYTES) {
            chunk = SH8501_LK_SPI_CHUNK_BYTES;
        }
        const bool last = (offset + chunk >= len);
        spi_transaction_t trans = {
            .length = chunk * 8,
            .tx_buffer = ptr + offset,
            .flags = last ? 0 : SPI_TRANS_CS_KEEP_ACTIVE,
        };
        ret = spi_device_polling_transmit(lk->spi, &trans);
        if (ret != ESP_OK) {
            break;
        }
        offset += chunk;
    }

    return ret;
}

static esp_err_t lk_dm_block_write(sh8501_lk_panel_t *lk, uint16_t x_start, uint16_t x_end,
                                   uint16_t y_start, uint16_t y_end,
                                   const void *pixels, size_t pixel_bytes)
{
    const uint8_t col[4] = {
        (uint8_t)(x_start >> 8), (uint8_t)(x_start & 0xFF),
        (uint8_t)(x_end >> 8), (uint8_t)(x_end & 0xFF),
    };
    const uint8_t row[4] = {
        (uint8_t)(y_start >> 8), (uint8_t)(y_start & 0xFF),
        (uint8_t)(y_end >> 8), (uint8_t)(y_end & 0xFF),
    };

    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(lk->spi, portMAX_DELAY), TAG, "acquire bus failed");

    esp_err_t ret = ESP_OK;
    if ((ret = lk_spi_write_comm(lk, 0x2A)) != ESP_OK) {
        goto out;
    }
    if ((ret = lk_spi_write_data(lk, col, sizeof(col))) != ESP_OK) {
        goto out;
    }
    if ((ret = lk_spi_write_comm(lk, 0x2B)) != ESP_OK) {
        goto out;
    }
    if ((ret = lk_spi_write_data(lk, row, sizeof(row))) != ESP_OK) {
        goto out;
    }
    if ((ret = lk_spi_write_comm(lk, 0x2C)) != ESP_OK) {
        goto out;
    }
    if ((ret = lk_spi_write_comm(lk, 0x2C)) != ESP_OK) {
        goto out;
    }
    ret = lk_spi_write_pixels_locked(lk, pixels, pixel_bytes);

out:
    spi_device_release_bus(lk->spi);
    return ret;
}

esp_err_t esp_lcd_new_panel_sh8501_lk(const esp_lcd_panel_dev_config_t *panel_dev_config,
                                      esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(panel_dev_config->vendor_config, ESP_ERR_INVALID_ARG, TAG, "vendor_config required");

    const sh8501_lk_vendor_config_t *vendor = (const sh8501_lk_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor->cs_gpio_num >= 0 && vendor->dc_gpio_num >= 0, ESP_ERR_INVALID_ARG, TAG, "invalid gpio");

    esp_err_t ret = ESP_OK;
    sh8501_lk_panel_t *lk = calloc(1, sizeof(sh8501_lk_panel_t));
    ESP_GOTO_ON_FALSE(lk, ESP_ERR_NO_MEM, err, TAG, "no mem");

    if (panel_dev_config->reset_gpio_num >= 0) {
        const gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "RST gpio config failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        lk->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        lk->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color order");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        lk->colmod_val = 0x55;
        lk->fb_bits_per_pixel = 16;
        break;
    case 18:
        lk->colmod_val = 0x66;
        lk->fb_bits_per_pixel = 24;
        break;
    case 24:
        lk->colmod_val = 0x77;
        lk->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported bpp");
        break;
    }

    lk->spi_host = vendor->spi_host;
    lk->cs_gpio_num = vendor->cs_gpio_num;
    lk->dc_gpio_num = vendor->dc_gpio_num;
    lk->pclk_hz = vendor->pclk_hz ? vendor->pclk_hz : (10 * 1000 * 1000);
    lk->reset_gpio_num = panel_dev_config->reset_gpio_num;
    lk->flags.reset_level = panel_dev_config->flags.reset_active_high;
    lk->init_cmds = vendor->init_cmds;
    lk->init_cmds_size = vendor->init_cmds_size;

    const gpio_config_t dc_cfg = {
        .pin_bit_mask = 1ULL << lk->dc_gpio_num,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&dc_cfg), err, TAG, "DC gpio config failed");
    gpio_set_level(lk->dc_gpio_num, 1);

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = lk->pclk_hz,
        .mode = 0,
        .spics_io_num = lk->cs_gpio_num,
        .queue_size = 1,
    };
    ESP_GOTO_ON_ERROR(spi_bus_add_device(lk->spi_host, &devcfg, &lk->spi), err, TAG, "add SPI device failed");

    lk->base.del = panel_sh8501_lk_del;
    lk->base.reset = panel_sh8501_lk_reset;
    lk->base.init = panel_sh8501_lk_init;
    lk->base.draw_bitmap = panel_sh8501_lk_draw_bitmap;
    lk->base.invert_color = panel_sh8501_lk_invert_color;
    lk->base.set_gap = panel_sh8501_lk_set_gap;
    lk->base.mirror = panel_sh8501_lk_mirror;
    lk->base.swap_xy = panel_sh8501_lk_swap_xy;
    lk->base.disp_on_off = panel_sh8501_lk_disp_on_off;

    *ret_panel = &lk->base;
    ESP_LOGI(TAG, "new SH8501A SPI4 (LK) panel");
    return ESP_OK;

err:
    if (lk) {
        if (lk->spi) {
            spi_bus_remove_device(lk->spi);
        }
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(lk);
    }
    return ret;
}

static esp_err_t panel_sh8501_lk_del(esp_lcd_panel_t *panel)
{
    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);

    if (lk->spi) {
        spi_bus_remove_device(lk->spi);
        lk->spi = NULL;
    }
    if (lk->reset_gpio_num >= 0) {
        gpio_reset_pin(lk->reset_gpio_num);
    }
    gpio_reset_pin(lk->dc_gpio_num);
    free(lk);
    return ESP_OK;
}

static esp_err_t panel_sh8501_lk_reset(esp_lcd_panel_t *panel)
{
    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);

    if (lk->reset_gpio_num >= 0) {
        gpio_set_level(lk->reset_gpio_num, lk->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(lk->reset_gpio_num, !lk->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(150));
        return ESP_OK;
    }

    return lk_spi_tx_locked(lk, LCD_CMD_SWRESET, NULL, 0);
}

static esp_err_t panel_sh8501_lk_init(esp_lcd_panel_t *panel)
{
    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);

    const sh8501_lk_lcd_init_cmd_t *init_cmds = lk->init_cmds;
    uint16_t init_cmds_size = lk->init_cmds_size;
    if (!init_cmds || init_cmds_size == 0) {
        init_cmds = lk_init_cmds_default;
        init_cmds_size = sizeof(lk_init_cmds_default) / sizeof(sh8501_lk_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        switch (init_cmds[i].cmd) {
        case LCD_CMD_MADCTL:
            lk->madctl_val = ((const uint8_t *)init_cmds[i].data)[0];
            break;
        case LCD_CMD_COLMOD:
            lk->colmod_val = ((const uint8_t *)init_cmds[i].data)[0];
            break;
        default:
            break;
        }

        ESP_RETURN_ON_ERROR(lk_spi_tx_locked(lk, (uint8_t)init_cmds[i].cmd,
                                             init_cmds[i].data, init_cmds[i].data_bytes),
                            TAG, "init cmd 0x%02X failed", init_cmds[i].cmd);
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }

    ESP_LOGD(TAG, "init done");
    return ESP_OK;
}

static esp_err_t panel_sh8501_lk_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                             const void *color_data)
{
    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);
    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG, "invalid area");

    x_start += lk->x_gap;
    x_end += lk->x_gap;
    y_start += lk->y_gap;
    y_end += lk->y_gap;

    const size_t len = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) * lk->fb_bits_per_pixel / 8;
    return lk_dm_block_write(lk, (uint16_t)x_start, (uint16_t)(x_end - 1),
                           (uint16_t)y_start, (uint16_t)(y_end - 1), color_data, len);
}

static esp_err_t panel_sh8501_lk_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);
    const uint8_t cmd = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    return lk_spi_tx_locked(lk, cmd, NULL, 0);
}

static esp_err_t panel_sh8501_lk_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);

    if (mirror_y) {
        ESP_LOGE(TAG, "mirror_y not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (mirror_x) {
        lk->madctl_val |= BIT(6);
    } else {
        lk->madctl_val &= (uint8_t)~BIT(6);
    }

    return lk_spi_tx_locked(lk, LCD_CMD_MADCTL, &lk->madctl_val, 1);
}

static esp_err_t panel_sh8501_lk_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    (void)panel;
    (void)swap_axes;
    ESP_LOGE(TAG, "swap_xy not supported");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_sh8501_lk_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);
    lk->x_gap = x_gap;
    lk->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_sh8501_lk_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);
    const uint8_t cmd = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    return lk_spi_tx_locked(lk, cmd, NULL, 0);
}

esp_err_t esp_lcd_panel_sh8501_lk_set_brightness(esp_lcd_panel_handle_t panel, uint8_t brightness_percent)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "invalid panel");
    ESP_RETURN_ON_FALSE(brightness_percent <= 100, ESP_ERR_INVALID_ARG, TAG, "invalid percent");

    sh8501_lk_panel_t *lk = __containerof(panel, sh8501_lk_panel_t, base);
    const uint8_t level = (uint8_t)((brightness_percent * 255 + 50) / 100);
    return lk_spi_tx_locked(lk, 0x51, &level, 1);
}
