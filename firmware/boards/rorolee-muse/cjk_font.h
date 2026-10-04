#pragma once
// Chinese text for rorolee-muse: a TrueType font kept in a flash partition (CJK_FONT_PARTITION in
// config.h) and drawn by LVGL's TinyTTF straight from flash. The image there is built by
// font/make_cjk_font.py: a 32-byte header (magic "AGLKTTF1", version, length, CRC-32) and the TTF.

#include <cstdint>

#include "lvgl.h"

namespace voice {

// The font at `px` pixels, or nullptr when the partition holds no valid image (then the screen
// shows Latin text only). Call from the LVGL task; the mapping stays for the life of the font.
lv_font_t* LoadCjkFont(int32_t px);

}  // namespace voice
