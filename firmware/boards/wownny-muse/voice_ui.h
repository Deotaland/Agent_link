#pragma once
// LVGL UI for wownny-muse on the GC9D01 round TFT.
//
// Driven by the link status and by the board's talk cycle (listening -> thinking -> speaking).
// No text outside the settings menu: ring, battery, avatar and a small indicator.
//
// All LVGL calls run on the render task created by Start(). The setters can be called from any
// task; they only store values for the render loop.

#include <cstdint>

#include "agent_link.h"
#include "esp_err.h"
#include "gc9d01_panel.h"

namespace voice {

enum class Tone { kInfo, kMuted, kOk, kError };

// One menu screen. `list` selects the list layout (above / label+value / below), otherwise
// `lines` are shown. Colours are 0xRRGGBB; null strings are not shown.
struct MenuView {
    const char* heading;
    uint32_t    heading_color;
    bool        list;
    const char* above;
    const char* label;
    const char* value;
    uint32_t    value_color;
    const char* below;
    const char* lines[4];
    uint32_t    line_colors[4];
};

class Ui {
public:
    static Ui& Instance();

    esp_err_t Start(Gc9d01Panel* panel);

    void SetStatus(const agent_link_status_t& st);
    void SetBattery(int percent, bool charging);   // percent < 0: unknown

    void SetListening(bool on);
    void SetLevel(int percent);                     // mic level 0-100
    void SetThinking();
    void SetAnswer(const char* utf8);               // text-only answer: speak for its reading time
    void EndTurn();                                 // turn failed, back to idle

    // Spoken answer being played by the board.
    void SpeechStart();
    void SpeechLevel(int percent);                  // playback level 0-100
    void SpeechEnd();

    // Short notice in the indicator area: an LV_SYMBOL, optionally with a number. ~48 px wide.
    void Toast(const char* text, uint32_t ms, Tone tone = Tone::kInfo);

    void ShowMenu(const MenuView& view);
    void HideMenu();

private:
    Ui() = default;
};

}  // namespace voice
