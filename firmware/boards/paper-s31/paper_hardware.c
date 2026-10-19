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
#include "font8x16.h"
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

static void battery_text(char *text, size_t size)
{
    int16_t mv, ma, soc;
    if (!i2c_bus_probe(BQ27220_I2C_ADDR)) {
        snprintf(text, size, "Battery: not connected");
    } else if (!battery_word(0x08, &mv) || !battery_word(0x0C, &ma) || !battery_word(0x2C, &soc)) {
        snprintf(text, size, "Battery: read failed");
    } else if (soc < 0 || soc > 100 || mv < 2500 || mv > 4500) {
        snprintf(text, size, "Battery: invalid reading");
    } else {
        snprintf(text, size, "Battery: %d%% %dmV %dmA", soc, mv, ma);
    }
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

void paper_sensor_log(void)
{
    static const uint8_t addresses[] = {0x10, 0x18, 0x19, 0x51, 0x55};
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        ESP_LOGI(TAG, "I2C 0x%02x: %s", addresses[i], i2c_bus_probe(addresses[i]) ? "present" : "absent");
    }
    char battery[80];
    battery_text(battery, sizeof(battery));
    ESP_LOGI(TAG, "%s", battery);
    uint8_t rtc[7];
    if (i2c_bus_probe(PCF85063_I2C_ADDR) &&
        i2c_read_regs(PCF85063_I2C_ADDR, 0x04, rtc, sizeof(rtc)) == ESP_OK) {
        if (rtc[0] & 0x80) {
            ESP_LOGW(TAG, "RTC oscillator stopped; time is not valid");
        } else {
            ESP_LOGI(TAG, "RTC BCD: 20%02x-%02x-%02x %02x:%02x:%02x",
                     rtc[6], rtc[5] & 0x1f, rtc[3] & 0x3f, rtc[2] & 0x3f, rtc[1] & 0x7f, rtc[0] & 0x7f);
        }
    }
    uint8_t imu = i2c_bus_probe(0x19) ? 0x19 : (i2c_bus_probe(0x18) ? 0x18 : 0);
    if (imu) {
        uint8_t who;
        if (i2c_read_regs(imu, 0x0f, &who, 1) == ESP_OK) {
            ESP_LOGI(TAG, "SC7A20 WHO_AM_I=0x%02x (expected 0x11)", who);
        }
    }
}

static void draw_text(uint8_t *frame, int x, int y, const char *text)
{
    for (; *text && y + FONT_H * 2 <= EPD_HEIGHT; ++text, x += FONT_W * 2) {
        if (x + FONT_W * 2 > EPD_WIDTH) break;
        unsigned c = (unsigned char)*text;
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        for (int row = 0; row < FONT_H; ++row) {
            for (int col = 0; col < FONT_W; ++col) {
                if (!(font8x16[c - FONT_FIRST][row] & (0x80 >> col))) continue;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        frame[(y + row * 2 + dy) * EPD_WIDTH + x + col * 2 + dx] = 0;
                    }
                }
            }
        }
    }
}

esp_err_t paper_display_status(void)
{
    ESP_RETURN_ON_FALSE(epd_probe(), ESP_ERR_NOT_FOUND, TAG, "Display not responding");
    uint8_t *frame = heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(frame, ESP_ERR_NO_MEM, TAG, "Display requires PSRAM");
    memset(frame, 0xff, EPD_WIDTH * EPD_HEIGHT);
    draw_text(frame, 16, 20, "Agent Link / PAPER-S31");
    draw_text(frame, 16, 60, "3.6in 400x600 SIX COLOR");
    draw_text(frame, 16, 100, "Offline board demo");
    static const uint8_t colors[] = {0x00, 0xff, 0xfc, 0xe0, 0x03, 0x1c};
    for (int y = 160; y < 280; ++y) {
        for (int x = 8; x < EPD_WIDTH - 8; ++x) {
            frame[y * EPD_WIDTH + x] = colors[(x - 8) * 6 / (EPD_WIDTH - 16)];
        }
    }
    char battery[80];
    battery_text(battery, sizeof(battery));
    char first[24];
    snprintf(first, sizeof(first), "%.23s", battery);
    draw_text(frame, 16, 300, first);
    if (strlen(battery) > 23) draw_text(frame, 16, 340, battery + 23);
    ESP_LOGI(TAG, "%s", battery);
    draw_text(frame, 16, 400, "BOOT: next demo");
    draw_text(frame, 16, 440, "1 Speaker tone");
    draw_text(frame, 16, 480, "2 Record + playback");
    draw_text(frame, 16, 520, "3 Motor + LED");
    draw_text(frame, 16, 560, "4 Sensors + screen");
    int before = epd_busy_timeouts();
    EPD_init();
    PIC_display(frame);
    EPD_sleep();
    free(frame);
    return epd_busy_timeouts() == before ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t paper_display_lines(const char *const *lines, size_t count)
{
    if (!lines || count > 14) return ESP_ERR_INVALID_ARG;
    uint8_t *frame = heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(frame, ESP_ERR_NO_MEM, TAG, "Display requires PSRAM");
    if (!epd_probe()) { free(frame); return ESP_ERR_NOT_FOUND; }
    memset(frame, 0xff, EPD_WIDTH * EPD_HEIGHT);
    for (size_t i = 0; i < count; ++i) {
        if (lines[i]) draw_text(frame, 16, 12 + i * 38, lines[i]);
    }
    static const uint8_t colors[] = {0x00, 0xff, 0xfc, 0xe0, 0x03, 0x1c};
    for (int y = 560; y < 592; ++y) {
        for (int x = 8; x < EPD_WIDTH - 8; ++x)
            frame[y * EPD_WIDTH + x] = colors[(x - 8) * 6 / (EPD_WIDTH - 16)];
    }
    int before = epd_busy_timeouts();
    EPD_init();
    PIC_display(frame);
    EPD_sleep();
    free(frame);
    return epd_busy_timeouts() == before ? ESP_OK : ESP_ERR_TIMEOUT;
}
