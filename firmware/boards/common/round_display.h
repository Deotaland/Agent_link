#pragma once
#include <cstdint>
#include "esp_err.h"
#include "cst9217_frame.h"

// UI-facing contracts shared by round LCD and AMOLED hardware.
class RoundDisplay {
public:
    virtual ~RoundDisplay() = default;
    virtual bool Ready() const = 0;
    virtual esp_err_t DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 const void* pixels) = 0;
    virtual esp_err_t SetBrightness(uint8_t level) = 0;
    virtual esp_err_t Recover(bool* recovered) = 0;
};
class RoundTouch {
public:
    virtual ~RoundTouch() = default;
    virtual bool Ready() const = 0;
    virtual esp_err_t Poll(cst9217::Point* point) = 0;
};
