#pragma once
// Cst816Touch: CST816S/T/D capacitive touch controller (I2C), in boards/common.
//
// Polled, not interrupt-driven: the chip's INT line is not broken out, and one 6-byte read
// per UI frame is cheap. Init() disables the controller's auto-sleep so polling keeps working
// without INT. Call Poll() once per frame, then read Pressed() / GetPoint() as often as you like.
//
// It attaches to an I2C bus somebody else already created rather than making its own — on boards
// where the touch panel shares SDA/SCL with another chip a second
// bus on the same pins would just fail. Pass the port number; the handle is looked up.
//
// Orientation: a panel driven rotated (ST7789 MADCTL, for instance) rotates what is displayed but
// NOT what the touch controller reports, so the same rotation has to be applied here in software.
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

struct Cst816Config {
    int      i2c_port      = 0;       // an ALREADY-INITIALISED I2C master bus to attach to
    uint8_t  addr          = 0x15;    // CST816 default
    uint32_t scl_speed_hz  = 400000;
    int      pin_rst       = -1;      // -1 = not wired (tied to the board reset)

    // Coordinate mapping, applied in this order: swap, then mirror within the OUTPUT size.
    // For a display rotated with swap_xy + mirror_y, touch wants swap_xy + mirror_x — the mirrors
    // trade places, because this transform runs the opposite way from the display's.
    bool     swap_xy   = false;
    bool     mirror_x  = false;
    bool     mirror_y  = false;
    uint16_t out_width  = 240;        // logical screen size the mapped coordinates land in
    uint16_t out_height = 280;

    // Log every press with both raw and mapped coordinates. Worth one flash when bringing a new
    // panel up: it says immediately whether the swap/mirror above are right.
    bool     log_raw = false;
};

class Cst816Touch {
public:
    Cst816Touch() = default;
    Cst816Touch(const Cst816Touch&) = delete;
    Cst816Touch& operator=(const Cst816Touch&) = delete;

    // Attach to the bus and identify the chip. If the configured address does not answer, the whole
    // bus is scanned and what was found is logged, so a wrong address or a dead panel says so once.
    esp_err_t Init(const Cst816Config& cfg);

    bool Ready() const { return dev_ != nullptr; }

    // One I2C transaction; refreshes what Pressed()/GetPoint()/TakePressEdge() report.
    esp_err_t Poll();

    bool Pressed() const { return pressed_; }
    void GetPoint(int32_t* x, int32_t* y) const { if (x) *x = x_; if (y) *y = y_; }

    // True once per press, consumed by the call — for "tap anywhere to go back" style handling.
    bool TakePressEdge();

private:
    void MapPoint(uint16_t raw_x, uint16_t raw_y);

    Cst816Config            cfg_  = {};
    i2c_master_dev_handle_t dev_  = nullptr;
    bool     pressed_      = false;
    bool     pressed_prev_ = false;
    bool     edge_         = false;
    int32_t  x_ = 0, y_ = 0;
};
