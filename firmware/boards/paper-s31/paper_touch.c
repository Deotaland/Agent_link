#include "paper_touch.h"
#include "board_pins.h"
#include "i2c_bus.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static paper_touch_info_t s_touch;

esp_err_t paper_touch_init(paper_touch_info_t* info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    memset(&s_touch, 0, sizeof s_touch);
    esp_err_t error = i2c_bus_init();
    if (error != ESP_OK) return error;
    gpio_set_level(EPD_PWR_EN_GPIO, EPD_PWR_ON_LEVEL);
    gpio_config_t power = { .pin_bit_mask = 1ULL << EPD_PWR_EN_GPIO, .mode = GPIO_MODE_OUTPUT };
    if ((error = gpio_config(&power)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_config_t reset = { .pin_bit_mask = (1ULL << TOUCH_RST_GPIO) | (1ULL << TOUCH_INT_GPIO), .mode = GPIO_MODE_OUTPUT };
    if ((error = gpio_config(&reset)) != ESP_OK) return error;
    gpio_set_level(TOUCH_INT_GPIO, 0);
    gpio_set_level(TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_config_t interrupt = { .pin_bit_mask = 1ULL << TOUCH_INT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    if ((error = gpio_config(&interrupt)) != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(250));
    const uint8_t goodix[] = {0x5d, 0x14};
    for (unsigned i = 0; i < sizeof goodix; ++i) {
        uint8_t id[4] = {0}, size[4] = {0};
        if (i2c_bus_probe(goodix[i]) && i2c_read_reg16(goodix[i], 0x8140, id, sizeof id) == ESP_OK &&
            id[0] >= '0' && id[0] <= '9' && i2c_read_reg16(goodix[i], 0x8048, size, sizeof size) == ESP_OK) {
            s_touch = (paper_touch_info_t){PAPER_TOUCH_GT911, goodix[i], size[0] | size[1] << 8, size[2] | size[3] << 8};
            break;
        }
    }
    if (!s_touch.type && i2c_bus_probe(0x38)) {
        uint8_t id = 0;
        if (i2c_read_regs(0x38, 0xa3, &id, 1) == ESP_OK && id != 0xff)
            s_touch = (paper_touch_info_t){PAPER_TOUCH_FT6X36, 0x38, 0, 0};
    }
    if (!s_touch.type && i2c_bus_probe(0x15)) {
        uint8_t id = 0;
        if (i2c_read_regs(0x15, 0xa7, &id, 1) == ESP_OK && id != 0xff)
            s_touch = (paper_touch_info_t){PAPER_TOUCH_CST816, 0x15, 0, 0};
    }
    *info = s_touch;
    if (!s_touch.type) return ESP_ERR_NOT_FOUND;
    ESP_LOGI("paper.touch", "controller=%u address=0x%02x range=%ux%u", (unsigned)s_touch.type,
             s_touch.address, s_touch.width, s_touch.height);
    return ESP_OK;
}

esp_err_t paper_touch_read(paper_touch_sample_t* sample)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    memset(sample, 0, sizeof(*sample));
    if (!s_touch.type) return ESP_ERR_INVALID_STATE;
    uint8_t data[8] = {0};
    unsigned count;
    esp_err_t error;
    if (s_touch.type == PAPER_TOUCH_GT911) {
        uint8_t status = 0;
        error = i2c_read_reg16(s_touch.address, 0x814e, &status, 1);
        if (error != ESP_OK) return error;
        if (!(status & 0x80)) return ESP_OK;
        count = status & 15;
        if (count > 5) return ESP_ERR_INVALID_RESPONSE;
        if (count && (error = i2c_read_reg16(s_touch.address, 0x814f, data, sizeof data)) != ESP_OK) return error;
        if ((error = i2c_write_reg16(s_touch.address, 0x814e, 0)) != ESP_OK) return error;
        sample->x = data[1] | data[2] << 8;
        sample->y = data[3] | data[4] << 8;
    } else {
        error = i2c_read_regs(s_touch.address, 0x02, data, 5);
        if (error != ESP_OK) {
            if (s_touch.type == PAPER_TOUCH_CST816) { sample->fresh = true; return ESP_OK; }
            return error;
        }
        count = data[0] & 15;
        if (count > 5) count = 0;
        sample->x = (data[1] & 15) << 8 | data[2];
        sample->y = (data[3] & 15) << 8 | data[4];
    }
    sample->fresh = true;
    sample->down = count != 0;
    return ESP_OK;
}
