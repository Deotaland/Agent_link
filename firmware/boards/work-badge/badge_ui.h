#pragma once
// The badge screen, drawn with LVGL on the SH8501 AMOLED.
//
// Everything visible lives here: the staff card, the link/battery strip, and the full-screen
// takeover shown while a firmware upgrade is running.
//
// Threading contract: LVGL is not thread-safe and is touched from exactly one task (the render
// loop started by Start()). Every Set*() below may be called from any task - they only store a
// value and bump a counter; the render loop notices and redraws. That is why there is no lock
// anywhere in this class, and why no caller ever blocks on the display.

#include <cstdint>

#include "agent_link_ota.h"
#include "esp_err.h"
#include "sh8501_lk_panel.h"

namespace badge {

class Ui {
public:
    static Ui& Instance();

    // Build the LVGL display on top of an already-initialised panel and start the render task.
    esp_err_t Start(Sh8501LkPanel* panel);

    void SetBattery(int percent, bool charging);   // percent < 0 = unknown / no gauge
    void SetLinkState(bool connected);

    // Transient line under the card (what the Agent sends via on_show_text). Empty clears it.
    void SetNote(const char* utf8);

    // Firmware upgrade progress: swaps in the upgrade screen and drives its bar.
    void SetOtaStatus(const agent_ota_status_t* st);

private:
    Ui() = default;
};

}  // namespace badge
