#pragma once
// The screen of m5stack-stopwatch, drawn with LVGL on the 466x466 round CO5300 AMOLED.
//
// What it shows follows from two inputs: the link status (agent_link_status_t, the same words on
// every transport) and the talk cycle the board drives - listening while the key is held,
// thinking until the agent answers, then the answer. Nothing in here knows which transport is
// running.
//
// Threading: LVGL is touched only by the render task Start() creates. Every Set*() may be called
// from any task; they store the value and bump a revision the render loop picks up, so no caller
// ever waits on the display.

#include <cstdint>

#include "agent_link.h"
#include "co5300_panel.h"
#include "esp_err.h"

namespace watch {

class Ui {
public:
    static Ui& Instance();

    // Build the LVGL display on an initialised panel and start the render task.
    esp_err_t Start(Co5300Panel* panel);

    void SetStatus(const agent_link_status_t& st);
    void SetBattery(int percent, bool charging);   // percent < 0 = unknown

    // The talk cycle, driven by the board's key task.
    void SetListening(bool on);
    void SetLevel(int percent);                     // mic level 0-100 while listening
    void SetThinking();                             // released: waiting for the answer
    void SetAnswer(const char* utf8);               // what the agent sent to the screen

    // A short line near the bottom for a moment: volume, reset countdown, errors.
    void Toast(const char* text, uint32_t ms);

    // A key was pressed: back to full brightness.
    void Wake();

private:
    Ui() = default;
};

}  // namespace watch
