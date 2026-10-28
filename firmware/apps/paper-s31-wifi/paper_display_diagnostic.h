#pragma once

#include <cstdio>
#include "paper_refresh.h"

namespace paper {
// Deliberately excludes clocks, battery, activation codes and connection state.
// A white background must erase the old English page and its color strip.
inline Page DisplayDiagnosticPage() {
    Page page;
    const char* lines[] = {
        "S31 DISPLAY TEST", "单次全刷诊断", "",
        "WHITE BACKGROUND", "BLACK TEXT ONLY", "",
        "0123456789", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "",
        "ONE FRAME ONLY", "NO MENU / NO TIMER", "", "HOLD UNTIL REBOOT",
    };
    for (unsigned i = 0; i < 14; ++i)
        std::snprintf(page.lines[i], sizeof page.lines[i], "%s", lines[i]);
    return page;
}
}
