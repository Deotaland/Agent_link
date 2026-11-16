#pragma once
#include "amoled_hardware.h"
// Runs before the LVGL display owner starts. Never formats appdata.
void PlaySugaratBootAnimation(AmoledHardware& hardware);
// One-shot RTC marker: only an explicit sleep followed by EXT0 skips playback.
