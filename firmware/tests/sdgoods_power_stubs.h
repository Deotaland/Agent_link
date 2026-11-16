#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <set>

#define RTC_DATA_ATTR
#define CONFIG_BOOTLOADER_RESERVE_RTC_MEM 1
#define ESP_ERROR_CHECK(call) assert((call) == 0)
using gpio_num_t = int;
constexpr int GPIO_NUM_3=3, GPIO_NUM_5=5, GPIO_NUM_6=6, GPIO_NUM_7=7;
constexpr int GPIO_NUM_9=9, GPIO_NUM_12=12, GPIO_NUM_13=13;
constexpr int GPIO_MODE_INPUT=1, GPIO_MODE_OUTPUT=2, GPIO_PULLUP_ENABLE=1;
enum esp_reset_reason_t {ESP_RST_POWERON, ESP_RST_DEEPSLEEP, ESP_RST_SW, ESP_RST_PANIC,
    ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_USB, ESP_RST_EXT, ESP_RST_BROWNOUT};
constexpr int ESP_SLEEP_WAKEUP_UNDEFINED=0, ESP_SLEEP_WAKEUP_EXT0=2;
struct gpio_config_t {
    uint64_t pin_bit_mask;
    int mode, pull_up_en;
};
struct StandbySignal {};
namespace power_fixture {
inline esp_reset_reason_t reason=ESP_RST_POWERON;
inline int wake_cause=ESP_SLEEP_WAKEUP_UNDEFINED;
inline int64_t now=0, release_at=-1;
inline bool down=false, bootloader_hold=false, deep_hold=false, key_pullup=false;
inline int wake_pin=-1, wake_level=-1;
inline std::map<int,int> levels, directions;
inline std::set<int> holds;
inline void Reset(esp_reset_reason_t reset, bool pressed=false, int64_t release=-1) {
    reason=reset; wake_cause=ESP_SLEEP_WAKEUP_UNDEFINED; now=0;
    down=pressed; release_at=release; bootloader_hold=deep_hold=key_pullup=false;
    wake_pin=wake_level=-1; levels.clear(); directions.clear(); holds.clear();
}
}
inline esp_reset_reason_t esp_reset_reason(){return power_fixture::reason;}
inline int esp_sleep_get_wakeup_cause(){return power_fixture::wake_cause;}
inline int64_t esp_timer_get_time(){return power_fixture::now;}
inline int64_t pdMS_TO_TICKS(int64_t ms){return ms;}
inline void vTaskDelay(int64_t ms){power_fixture::now+=ms*1000;}
inline int gpio_get_level(int pin){
    assert(pin==6);
    return !(power_fixture::down &&
        (power_fixture::release_at<0 || power_fixture::now<power_fixture::release_at));
}
inline int gpio_config(const gpio_config_t* cfg){
    assert(cfg->pin_bit_mask==(1ULL<<6) && cfg->mode==GPIO_MODE_INPUT && cfg->pull_up_en);
    return 0;
}
inline int gpio_set_direction(int pin,int mode){power_fixture::directions[pin]=mode;return 0;}
inline int gpio_set_level(int pin,int level){power_fixture::levels[pin]=level;return 0;}
inline int gpio_hold_dis(int pin){power_fixture::holds.erase(pin);return 0;}
inline int gpio_hold_en(int pin){power_fixture::holds.insert(pin);return 0;}
inline void gpio_deep_sleep_hold_dis(){power_fixture::deep_hold=false;}
inline void gpio_deep_sleep_hold_en(){power_fixture::deep_hold=true;}
inline int rtc_gpio_deinit(int pin){assert(pin==6);return 0;}
inline int rtc_gpio_pullup_en(int pin){assert(pin==6);power_fixture::key_pullup=true;return 0;}
inline int rtc_gpio_pulldown_dis(int pin){assert(pin==6);return 0;}
inline int esp_sleep_enable_ext0_wakeup(int pin,int level){
    power_fixture::wake_pin=pin;power_fixture::wake_level=level;return 0;
}
inline bool bootloader_common_get_rtc_retain_mem_factory_reset_state(){
    const bool held=power_fixture::bootloader_hold;power_fixture::bootloader_hold=false;return held;
}
[[noreturn]] inline void esp_deep_sleep_start(){throw StandbySignal{};}
