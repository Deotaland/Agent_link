#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bitmap_index;
    uint16_t adv_w;
    uint8_t box_w, box_h;
    int8_t ofs_x, ofs_y;
} raster_glyph_t;

typedef struct {
    const uint8_t* bitmap;
    const raster_glyph_t* glyphs;
    const uint16_t* sparse;
    const uint8_t* kern_left;
    const uint8_t* kern_right;
    const int8_t* kern_values;
    uint16_t sparse_count;
    uint16_t glyph_count;
    uint8_t kern_right_count;
    uint8_t line_height;
    uint8_t baseline;
} raster_font_t;

extern const raster_font_t raster_montserrat_14;
extern const raster_font_t raster_montserrat_20;
extern const raster_font_t raster_montserrat_28;

#ifdef __cplusplus
}
#endif
