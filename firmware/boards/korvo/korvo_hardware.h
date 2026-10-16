#pragma once

#include "cst816_touch.h"
#include "es_codec.h"
#include "gc2145_camera.h"
#include "st7789_lcd.h"

class KorvoHardware {
public:
    esp_err_t Init();

    St7789Lcd lcd;
    Gc2145Camera camera;
    Cst816Touch touch;
    EsCodec codec;
    bool touch_ready = false;
};
