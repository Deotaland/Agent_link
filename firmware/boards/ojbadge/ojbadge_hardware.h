#pragma once
#include "bq27220.h"
#include "cst816_touch.h"
#include "gc9a01_panel.h"

class OjBadgeHardware {
public:
    esp_err_t Init();
    Gc9a01Panel panel;
    Cst816Touch touch;
    Bq27220 battery;
    bool button_ready = false;
};
