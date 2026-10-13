#pragma once
// The screen of the AI Passport, drawn with LVGL on the ST7789 240x320 panel.
//
// Same shape as the Muse boards' UI: what it shows follows from the link status
// (agent_link_status_t, identical wording on every transport) and the talk cycle the board
// drives - listening while OK is held, thinking until the agent answers, then the answer.
// Nothing in here knows which transport is running.
//
// The C3 has no PSRAM. One 240 x 16 stripe uses 7.5 KB instead of a full framebuffer.
//
// Threading: LVGL is touched only by the render task Start() creates. Every Set*() may be called
// from any task; they store the value and bump a revision the render loop picks up.

#include <cstdint>

#include "agent_link.h"
#include "esp_err.h"
#include "st7789_lcd.h"

namespace voice {

class Ui {
public:
    static Ui& Instance();

    // Build the LVGL display on an initialised panel and start the render task.
    esp_err_t Start(St7789Lcd* panel);

    void SetStatus(const agent_link_status_t& st);

    // The talk cycle, driven by the board's button task.
    void SetListening(bool on);
    void SetLevel(int percent);                     // mic level 0-100 while listening
    void SetThinking();                             // released: waiting for the answer
    void SetAnswer(const char* utf8);               // what the agent sent to the screen

    // A short line over the bottom of the screen for a moment: volume, errors.
    void Toast(const char* text, uint32_t ms);

private:
    Ui() = default;
};

}  // namespace voice
