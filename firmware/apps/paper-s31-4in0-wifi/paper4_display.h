#pragma once
#include "esp_err.h"
#include "paper4_state.h"

namespace paper4 {
// Called only on the display worker. GPIO/SPI/probe/refresh never run on SDK callbacks.
class Display {
public:
    Display() = default;
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    ~Display();
    esp_err_t Draw(const Frame& frame, bool force_full = false);
private:
    uint8_t* pixels_ = nullptr;
    uint8_t* previous_ = nullptr;
    bool valid_ = false;
    unsigned partials_ = 0;
};
}
