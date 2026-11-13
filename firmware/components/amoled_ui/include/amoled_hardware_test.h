#pragma once

namespace amoled_ui {
// Snapshot data only; board Factory owns the test and LVGL owns its presentation.
struct HardwareTest {
    enum class Kind { kNone, kTouch, kLevel };
    Kind kind = Kind::kNone;
    int x = -1, y = -1, target_x = 180, target_y = 180, completed = 0;
    float tilt_x = 0, tilt_y = 0;
    bool valid = false, centered = false, passed = false;
};
}  // namespace amoled_ui
