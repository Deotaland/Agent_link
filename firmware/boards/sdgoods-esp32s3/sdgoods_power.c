#include "sdgoods_power.h"
#include "bootloader_common.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Board pins shared with config.h; this minimal module also runs in Recovery,
// which deliberately has no display, audio, network or application dependencies.
static const gpio_num_t key_pin = GPIO_NUM_6, latch_pin = GPIO_NUM_7;
static const gpio_num_t quiet_pins[] = {
    GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_9, GPIO_NUM_5, GPIO_NUM_3};
static const int quiet_levels[] = {1, 0, 0, 0, 0}; // LCD supply is active low.
static const uint32_t sleep_cookie = 0x5347574B;
RTC_DATA_ATTR static uint32_t sleep_resume = 0;
static bool resumed_from_sleep = false;

static void output(gpio_num_t pin, int level) {
    // Program the intended level before releasing a retained GPIO hold.
    ESP_ERROR_CHECK(gpio_set_level(pin, level));
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_hold_dis(pin));
}

static void standby(void) {
    sleep_resume = 0;
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(key_pin, 0));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(key_pin));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(key_pin));
    ESP_ERROR_CHECK(gpio_set_level(latch_pin, 0));
    ESP_ERROR_CHECK(gpio_hold_en(latch_pin));
    for (unsigned n = 0; n < sizeof quiet_pins / sizeof quiet_pins[0]; ++n)
        ESP_ERROR_CHECK(gpio_hold_en(quiet_pins[n]));
    gpio_deep_sleep_hold_en();
    // Battery power falls away through Q2. USB bypasses Q2 through D1, so
    // charging remains available while the CPU and display stay asleep.
    esp_deep_sleep_start();
}

bool sdgoods_power_startup(bool allow_sleep_resume) {
    const esp_reset_reason_t reason = esp_reset_reason();
    resumed_from_sleep = allow_sleep_resume && reason == ESP_RST_DEEPSLEEP &&
                         esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 &&
                         sleep_resume == sleep_cookie;
    sleep_resume = 0;
    ESP_ERROR_CHECK(rtc_gpio_deinit(key_pin));
    gpio_config_t key = {0};
    key.pin_bit_mask = 1ULL << key_pin;
    key.mode = GPIO_MODE_INPUT;
    key.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&key));
    gpio_deep_sleep_hold_dis();

    // Recovery routing, updates and crash restarts continue an already powered
    // session. Only initial power and off-state EXT0 wake need a long press.
    const bool powered_restart = reason == ESP_RST_SW || reason == ESP_RST_PANIC ||
                                 reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                                 reason == ESP_RST_WDT;
    const bool new_power = !resumed_from_sleep && !powered_restart;
    if (new_power) {
        for (unsigned n = 0; n < sizeof quiet_pins / sizeof quiet_pins[0]; ++n)
            output(quiet_pins[n], quiet_levels[n]);
        output(latch_pin, 0);
        bool held_by_bootloader = false;
#ifdef CONFIG_BOOTLOADER_RESERVE_RTC_MEM
        // The matching bootloader already checks a three-second hold on cold
        // power. Consume its proof instead of requiring another long press.
        held_by_bootloader = bootloader_common_get_rtc_retain_mem_factory_reset_state();
#endif
        if (!(held_by_bootloader && reason != ESP_RST_DEEPSLEEP && !gpio_get_level(key_pin))) {
            const int64_t start = esp_timer_get_time();
            while (!gpio_get_level(key_pin) && esp_timer_get_time() - start < 3000000)
                vTaskDelay(pdMS_TO_TICKS(10));
            if (gpio_get_level(key_pin))
                standby();
        }
    }
    output(latch_pin, 1);
    if (new_power && !allow_sleep_resume) {
        // Recovery must not carry the power-on hold across esp_restart(): the
        // bootloader would interpret it as another recovery request. Latch
        // power first, then require a debounced release before routing.
        int64_t released_at = -1;
        while (released_at < 0 || esp_timer_get_time() - released_at < 40000) {
            if (!gpio_get_level(key_pin))
                released_at = -1;
            else if (released_at < 0)
                released_at = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return new_power;
}

void sdgoods_power_set_sleep_resume(bool resume) {
    sleep_resume = resume ? sleep_cookie : 0;
}

bool sdgoods_power_take_sleep_resume(void) {
    const bool result = resumed_from_sleep;
    resumed_from_sleep = false;
    return result;
}
