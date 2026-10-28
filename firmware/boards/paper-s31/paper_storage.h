#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t paper_sd_mount(void);
bool paper_sd_ready(void);
// Local civil time in the device timezone, backed by PCF85063 when available.
esp_err_t paper_rtc_read(struct tm* time);
esp_err_t paper_rtc_write(const struct tm* time);
#ifdef __cplusplus
}
#endif
