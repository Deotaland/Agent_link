#pragma once

#include <stdbool.h>
// Include the selected panel's epd.h before this geometry helper.
// Controller source coordinates are byte-aligned; gates are interleaved across two strips.
typedef struct {
    int h0, h1, g0, g1; // inclusive controller coordinates
} epd4_window_t;

static inline int epd4_gate_of_col(int x)
{
#if EPD_ROTATION == 270
    x = EPD_WIDTH - 1 - x;
#endif
    return x < EPD_4IN0_LEFT_COLS ? 2 * x : 2 * (x - EPD_4IN0_LEFT_COLS) + 1;
}

static inline int epd4_col_of_gate(int g)
{
    int x = g % 2 == 0 ? g / 2 : EPD_4IN0_LEFT_COLS + g / 2;
    if (x >= EPD_WIDTH) return -1;
#if EPD_ROTATION == 270
    x = EPD_WIDTH - 1 - x;
#endif
    return x;
}

static inline int epd4_row_of_source(int source)
{
#if EPD_ROTATION == 270
    return EPD_HEIGHT - 1 - source;
#else
    return source;
#endif
}

static inline bool epd4_window(int x, int y, int w, int h, epd4_window_t* window)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= EPD_WIDTH || y >= EPD_HEIGHT) return false;
    if (w > EPD_WIDTH - x) w = EPD_WIDTH - x;
    if (h > EPD_HEIGHT - y) h = EPD_HEIGHT - y;
    if (w <= 0 || h <= 0) return false;
#if EPD_ROTATION == 270
    window->h0 = (EPD_HEIGHT - y - h) & ~7;
    window->h1 = (EPD_HEIGHT - 1 - y) | 7;
#else
    window->h0 = y & ~7;
    window->h1 = (y + h - 1) | 7;
#endif
    window->g0 = EPD_4IN0_GATES;
    window->g1 = -1;
    for (int column = x; column < x + w; ++column) {
        const int gate = epd4_gate_of_col(column);
        if (gate < window->g0) window->g0 = gate;
        if (gate > window->g1) window->g1 = gate;
    }
    return true;
}

static inline int epd4_window_area(const epd4_window_t* window)
{
    int columns = 0;
    for (int gate = window->g0; gate <= window->g1; ++gate)
        columns += epd4_col_of_gate(gate) >= 0;
    return columns * (window->h1 - window->h0 + 1);
}
