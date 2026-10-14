#pragma once
#include <array>
#include <cstdint>
#include "esp_err.h"
class St7789Lcd {
public:
    bool Ready() const { return true; }
    uint16_t Width() const { return 240; }
    uint16_t Height() const { return 320; }
    size_t StripeCapacity() const { return 1920; }
    uint8_t* StripeBuffer() { return stripe.data() + 16; }
    esp_err_t FlushStripe(uint16_t y, uint16_t rows);
    std::array<uint8_t, 1952> stripe{};
    std::array<uint8_t, 240 * 320 * 2> frame{};
    unsigned flushes = 0;
    unsigned fail_at = 0, interrupt_at = 0;
};
