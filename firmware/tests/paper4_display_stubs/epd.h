#pragma once
#include <cstdint>
constexpr int EPD_WIDTH = 528, EPD_HEIGHT = 792, EPD_COLORS = 2;
#define EPD_ROTATION 270
#define EPD_4IN0_LEFT_COLS 300
#define EPD_4IN0_GATES 600
bool epd_probe();
int epd_busy_timeouts();
void EPD_init();
void PIC_display(const unsigned char* data);
void EPD_sleep();
void epd_partial(const unsigned char* data, int x, int y, int w, int h);
bool epd_partial_ready();
