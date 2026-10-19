#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "config.h"
#include "paper_audio.h"
#include "paper_hardware.h"

extern "C" void app_main(void)
{
    constexpr const char *tag = "paper.demo";
    ESP_LOGI(tag, "board = paper-s31, model = %s, fw = %s, offline",
             PAPER_MODEL, esp_app_get_description()->version);
    ESP_ERROR_CHECK(paper_hardware_init());
    paper_sensor_log();
    ESP_LOGI(tag, "Drawing six-color screen; wait until refresh finishes");
    esp_err_t result = paper_display_status();
    ESP_LOGI(tag, "Display: %s", esp_err_to_name(result));
    const gpio_num_t keys[] = {BOOT_GPIO, KEY1_GPIO, KEY2_GPIO, KEY3_GPIO, POWER_SWITCH_GPIO};
    const char *names[] = {"BOOT", "KEY1", "KEY2", "KEY3", "POWER"};
    int stable[5], previous[5], count[5] = {};
    for (int i = 0; i < 5; ++i) stable[i] = previous[i] = gpio_get_level(keys[i]);
    unsigned step = 0;
    ESP_LOGI(tag, "BOOT demos: speaker -> record/playback -> motor/LED -> sensors/screen");
    while (true) {
        bool run = false;
        for (int i = 0; i < 5; ++i) {
            int level = gpio_get_level(keys[i]);
            count[i] = level == previous[i] ? count[i] + 1 : 1;
            previous[i] = level;
            if (count[i] >= 3) {
                count[i] = 3;
                if (stable[i] != level) {
                    stable[i] = level;
                    ESP_LOGI(tag, "%s %s", names[i], level ? "released" : "pressed");
                    if (i == 0 && level == 0) run = true;
                }
            }
        }
        if (run) {
            result = ESP_OK;
            ESP_LOGI(tag, "Demo %u starting", step + 1);
            switch (step) {
            case 0: result = paper_audio_tone(); break;
            case 1: result = paper_audio_record_playback(); break;
            case 2:
                for (int i = 0; i < 3; ++i) {
                    paper_led(true);
                    paper_vibrate(200);
                    paper_led(false);
                    vTaskDelay(pdMS_TO_TICKS(300));
                }
                break;
            case 3:
                paper_sensor_log();
                result = paper_display_status();
                break;
            }
            ESP_LOGI(tag, "Demo %u: %s; check physical output", step + 1, esp_err_to_name(result));
            step = (step + 1) % 4;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
