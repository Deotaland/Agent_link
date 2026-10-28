#include "paper_storage.h"
#include "board_pins.h"
#include "i2c_bus.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static sdmmc_card_t* s_card;
bool paper_sd_ready(void) { return s_card != NULL; }
esp_err_t paper_sd_mount(void) {
    if (s_card) return ESP_OK;
    gpio_set_level(SD_CTRL_GPIO, SD_PWR_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = SDMMC_CLK_GPIO; slot.cmd = SDMMC_CMD_GPIO;
    slot.d0 = SDMMC_D0_GPIO; slot.d1 = SDMMC_D1_GPIO;
    slot.d2 = SDMMC_D2_GPIO; slot.d3 = SDMMC_D3_GPIO;
    slot.cd = SDMMC_SLOT_NO_CD; slot.wp = SDMMC_SLOT_NO_WP;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16 * 1024,
    };
    esp_err_t result = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &s_card);
    if (result != ESP_OK) { s_card = NULL; gpio_set_level(SD_CTRL_GPIO, SD_PWR_OFF_LEVEL); }
    return result;
}
static unsigned bcd(unsigned n) { return (n >> 4) * 10 + (n & 15); }
static uint8_t encode(unsigned n) { return (n / 10) * 16 + n % 10; }
esp_err_t paper_rtc_read(struct tm* time) {
    if (!time) return ESP_ERR_INVALID_ARG;
    uint8_t r[7];
    esp_err_t result = i2c_read_regs(PCF85063_I2C_ADDR, 0x04, r, sizeof r);
    if (result != ESP_OK) return result;
    if (r[0] & 0x80) return ESP_ERR_INVALID_STATE; // oscillator-stop: time is untrusted
    *time = (struct tm){ .tm_sec = bcd(r[0] & 0x7f), .tm_min = bcd(r[1] & 0x7f),
        .tm_hour = bcd(r[2] & 0x3f), .tm_mday = bcd(r[3] & 0x3f), .tm_wday = r[4] & 7,
        .tm_mon = bcd(r[5] & 0x1f) - 1, .tm_year = bcd(r[6]) + 100, .tm_isdst = -1 };
    if (time->tm_sec > 59 || time->tm_min > 59 || time->tm_hour > 23 || time->tm_mday < 1 ||
        time->tm_mday > 31 || time->tm_mon < 0 || time->tm_mon > 11) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}
esp_err_t paper_rtc_write(const struct tm* time) {
    if (!time || time->tm_year < 100 || time->tm_year > 199 || time->tm_mon < 0 || time->tm_mon > 11 ||
        time->tm_mday < 1 || time->tm_mday > 31 || time->tm_hour < 0 || time->tm_hour > 23 ||
        time->tm_min < 0 || time->tm_min > 59 || time->tm_sec < 0 || time->tm_sec > 59) return ESP_ERR_INVALID_ARG;
    uint8_t r[] = { encode(time->tm_sec), encode(time->tm_min), encode(time->tm_hour),
        encode(time->tm_mday), (uint8_t)time->tm_wday, encode(time->tm_mon + 1), encode(time->tm_year - 100) };
    return i2c_write_regs(PCF85063_I2C_ADDR, 0x04, r, sizeof r);
}
