#pragma once
#include "lvgl.h"

// Resident font copies and TinyTTF caches. Init/use only on the LVGL task.
// ASCII keeps Montserrat; Chinese falls back to the shared flash-backed font.
class AmoledFonts {
public:
    void Init();
    const lv_font_t* Body() const { return &body_; }
    const lv_font_t* Small() const { return &small_; }
    const lv_font_t* Heading() const { return &heading_; }
    const lv_font_t* Title() const { return &title_; }
    bool Ready() const { return ready_; }
private:
    lv_font_t body_{}, small_{}, heading_{}, title_{};
    bool initialized_ = false, ready_ = false;
};
