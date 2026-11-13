#pragma once
#include "lvgl.h"
#include <algorithm>

namespace amoled_ui {
// Fixed text regions center their actual line block. Content-sized paragraphs
// keep their natural height so scrolling text is never padded or clipped.
inline void CenterLabelVertically(lv_obj_t* label) {
    const int32_t height = lv_obj_get_style_height(label, LV_PART_MAIN);
    const int32_t width = lv_obj_get_style_width(label, LV_PART_MAIN);
    if (height <= 0 || height == LV_SIZE_CONTENT || LV_COORD_IS_PCT(height) ||
        width <= 0 || width == LV_SIZE_CONTENT || LV_COORD_IS_PCT(width))
        return;
    const auto* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int32_t available_width = std::max<int32_t>(
        1, width - lv_obj_get_style_pad_left(label, 0) - lv_obj_get_style_pad_right(label, 0));
    lv_point_t size{};
    lv_text_get_size(&size, lv_label_get_text(label), font,
                     lv_obj_get_style_text_letter_space(label, 0),
                     lv_obj_get_style_text_line_space(label, 0),
                     lv_label_get_long_mode(label) == LV_LABEL_LONG_WRAP
                         ? available_width : LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    const int32_t padding = std::max<int32_t>(0, (height - size.y) / 2);
    lv_obj_set_style_pad_top(label, padding, 0);
    lv_obj_set_style_pad_bottom(label, padding, 0);
}
}  // namespace amoled_ui
