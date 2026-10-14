#pragma once
// The screen of the AI Passport, drawn directly on the ST7789 240x320 panel.
//
// Same shape as the Muse boards' UI: what it shows follows from the link status
// (agent_link_status_t, identical wording on every transport) and the talk cycle the board
// drives: Listening, Thinking, Speaking, then Ready. Answer text does not change the session.
// Nothing in here knows which transport is running.
//
// The C3 has no PSRAM. The renderer shares one 240 x 4 DMA stripe with the panel.
//
// Threading: drawing belongs to the render task Start() creates. Every Set*() may be called
// from any task; they store the value and bump a revision the render loop picks up.

#include <cstdint>

#include "agent_link.h"
#include "esp_err.h"
#include "st7789_lcd.h"
#include "voice_session.h"

namespace voice {

class Ui {
public:
    static Ui& Instance();

    // Start the render task on an initialised panel.
    esp_err_t Start(St7789Lcd* panel);

    void SetStatus(const agent_link_status_t& st);

    // Acknowledge only after the requested session screen has been flushed.
    void SetSession(uint32_t token);
    bool SessionRendered(uint32_t token) const;
    void SetLevel(int percent);                     // mic level 0-100 while listening
    void SetAnswer(const char* utf8);               // what the agent sent to the screen

    // A short line over the bottom of the screen for a moment: volume, errors.
    void Toast(const char* text, uint32_t ms);

private:
    Ui() = default;
};

}  // namespace voice
