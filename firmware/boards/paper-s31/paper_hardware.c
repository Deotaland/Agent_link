#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "board_pins.h"
#include "epd.h"
#include "i2c_bus.h"
#include "paper_hardware.h"

static const char *TAG = "paper.hardware";

esp_err_t paper_hardware_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << MOTOR_GPIO) | (1ULL << TOP_LED_EN_GPIO) |
                        (1ULL << CAM_PWR_EN_GPIO) | (1ULL << SD_CTRL_GPIO) |
                        (1ULL << PA_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    // Preload inactive levels before switching the power pins to outputs.
    gpio_set_level(CAM_PWR_EN_GPIO, CAM_PWR_OFF_LEVEL);
    gpio_set_level(SD_CTRL_GPIO, SD_PWR_OFF_LEVEL);
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "Output GPIO");
    gpio_set_level(CAM_PWR_EN_GPIO, CAM_PWR_OFF_LEVEL);
    gpio_set_level(SD_CTRL_GPIO, SD_PWR_OFF_LEVEL);
    gpio_set_level(PA_CTRL_GPIO, !PA_ON_LEVEL);
    paper_vibrate(0);
    paper_led(false);
    gpio_config_t keys = {
        .pin_bit_mask = (1ULL << BOOT_GPIO) | (1ULL << KEY1_GPIO) |
                        (1ULL << KEY2_GPIO) | (1ULL << KEY3_GPIO),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&keys), TAG, "Keys");
    gpio_config_t power = { .pin_bit_mask = 1ULL << POWER_SWITCH_GPIO, .mode = GPIO_MODE_INPUT };
    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "Power key");
    return i2c_bus_init();
}

void paper_vibrate(unsigned duration_ms)
{
    if (duration_ms > 2000) duration_ms = 2000;
    gpio_set_level(MOTOR_GPIO, duration_ms ? MOTOR_ON_LEVEL : !MOTOR_ON_LEVEL);
    if (duration_ms) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        gpio_set_level(MOTOR_GPIO, !MOTOR_ON_LEVEL);
    }
}

void paper_motor(bool on)
{
    gpio_set_level(MOTOR_GPIO, on ? MOTOR_ON_LEVEL : !MOTOR_ON_LEVEL);
}

void paper_led(bool on)
{
    gpio_set_level(TOP_LED_EN_GPIO, on);
}

static bool battery_word(uint8_t reg, int16_t *value)
{
    uint8_t bytes[2];
    if (i2c_read_regs(BQ27220_I2C_ADDR, reg, bytes, sizeof(bytes)) != ESP_OK) return false;
    *value = (int16_t)(bytes[0] | (bytes[1] << 8));
    return true;
}

esp_err_t paper_battery_read(int *percent, bool *charging)
{
    if (!percent || !charging) return ESP_ERR_INVALID_ARG;
    *percent = -1;
    *charging = false;
    int16_t mv, ma, soc;
    if (!i2c_bus_probe(BQ27220_I2C_ADDR)) return ESP_ERR_NOT_FOUND;
    if (!battery_word(0x08, &mv) || !battery_word(0x0C, &ma) || !battery_word(0x2c, &soc)) return ESP_FAIL;
    if (soc < 0 || soc > 100 || mv < 2500 || mv > 4500) return ESP_ERR_INVALID_RESPONSE;
    *percent = soc;
    *charging = ma > 0;
    return ESP_OK;
}
