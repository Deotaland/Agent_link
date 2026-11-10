#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { PAPER_TOUCH_NONE, PAPER_TOUCH_GT911, PAPER_TOUCH_FT6X36, PAPER_TOUCH_CST816 } paper_touch_type_t;
typedef struct { paper_touch_type_t type; uint8_t address; uint16_t width, height; } paper_touch_info_t;
typedef struct { int x, y; bool down, fresh; } paper_touch_sample_t;
esp_err_t paper_touch_init(paper_touch_info_t* info);
esp_err_t paper_touch_read(paper_touch_sample_t* sample);
#ifdef __cplusplus
}
#endif
