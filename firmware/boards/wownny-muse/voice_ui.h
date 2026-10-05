#pragma once
// The screen of wownny-muse, drawn with LVGL on the GC9D01 round TFT.
//
// What it shows follows from two inputs: the link status (agent_link_status_t, the same words on
// every transport) and the talk cycle the board drives - listening while the button is held,
// thinking until the agent answers, then the answer. Nothing in here knows which transport is
// running.
//
// Threading: LVGL is touched only by the render task Start() creates. Every Set*() may be called
// from any task; they store the value and bump a revision the render loop picks up, so no caller
// ever waits on the display.

#include <cstdint>

#include "agent_link.h"
#include "esp_err.h"
#include "gc9d01_panel.h"

namespace voice {

class Ui {
public:
    static Ui& Instance();

    // Build the LVGL display on an initialised panel and start the render task.
    esp_err_t Start(Gc9d01Panel* panel);

    void SetStatus(const agent_link_status_t& st);
    void SetBattery(int percent, bool charging);   // percent < 0 = unknown / no gauge

    // The talk cycle, driven by the board's button task.
    void SetListening(bool on);
    void SetLevel(int percent);                     // mic level 0-100 while listening
    void SetThinking();                             // released: waiting for the answer
    void SetAnswer(const char* utf8);               // what the agent sent to the screen

    // A short line over the lower part of the screen for a moment: volume, reset countdown, errors.
    void Toast(const char* text, uint32_t ms);

private:
    Ui() = default;
};

}  // namespace voice
