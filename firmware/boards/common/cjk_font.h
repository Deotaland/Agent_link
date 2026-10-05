#pragma once
// Chinese text for LVGL boards: a TrueType font kept in a flash partition and drawn by LVGL's
// TinyTTF straight from flash, so it costs no RAM beyond TinyTTF's glyph cache. Reusable in
// boards/common.
//
// The partition holds an image built by boards/rorolee-muse/font/make_cjk_font.py: a 32-byte
// header (magic "AGLKTTF1", version, length, CRC-32) followed by the TTF. Needs LV_USE_TINY_TTF;
// without it this always answers nullptr.

#include <cstdint>

#include "lvgl.h"

// The font at `px` pixels from partition `label`, or nullptr when the partition holds no valid
// image (the board then shows Latin text only). Call from the LVGL task; the font keeps its flash
// mapping for as long as it lives.
lv_font_t* LoadCjkFont(const char* label, int32_t px, uint32_t cache_glyphs);
