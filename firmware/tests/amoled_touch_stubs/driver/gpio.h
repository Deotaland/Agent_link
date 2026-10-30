#pragma once
#include <cstdint>
#include "esp_err.h"
using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1, GPIO_MODE_INPUT = 2, GPIO_PULLUP_ENABLE = 1;
struct gpio_config_t { uint64_t pin_bit_mask = 0; int mode = 0, pull_up_en = 0; };
esp_err_t gpio_config(const gpio_config_t*);
esp_err_t gpio_set_level(gpio_num_t, int);
