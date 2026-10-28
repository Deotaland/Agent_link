#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t paper_hardware_init(void);
esp_err_t paper_display_lines(const char *const *lines, size_t count);
esp_err_t paper_battery_read(int *percent, bool *charging);
void paper_vibrate(unsigned duration_ms);
void paper_motor(bool on);
void paper_led(bool on);

#ifdef __cplusplus
}
#endif
