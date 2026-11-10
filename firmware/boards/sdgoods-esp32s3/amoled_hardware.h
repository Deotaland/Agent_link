#pragma once
#include "round_display.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"

class BadgeDisplay : public RoundDisplay {
public:
    esp_err_t Init(uint16_t rows);
    bool Ready() const override { return ready_; }
    esp_err_t DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels) override;
    esp_err_t SetBrightness(uint8_t level) override;
    esp_err_t Recover(bool* recovered) override;
private:
    esp_err_t InitializePanel();
    esp_err_t Command(uint8_t cmd, const void* data = nullptr, size_t bytes = 0);
    esp_lcd_panel_io_handle_t io_ = nullptr;
    size_t capacity_ = 0;
    uint8_t brightness_ = 0;
    bool ready_ = false, backlight_ok_ = false, reset_pending_ = false;
};
class BadgeTouch : public RoundTouch {
public:
    esp_err_t Init(i2c_master_bus_handle_t bus);
    bool Ready() const override { return device_ != nullptr; }
    esp_err_t Poll(cst9217::Point* point) override;
private:
    i2c_master_dev_handle_t device_ = nullptr;
    cst9217::Point last_;
    int64_t next_poll_ = 0;
    unsigned slow_ = 0;
};
// The shared round Factory controller uses this hardware facade.
class AmoledHardware {
public:
    void InitDisplayAndTouch(uint16_t stripe_rows = 12);
    void Battery(int* percent, bool* charging) { *percent = -1; *charging = false; }
    bool ButtonDown() const;
    BadgeDisplay panel_;
    BadgeTouch touch_;
    bool power_ok_ = false, bus_ok_ = false, button_ok_ = false;
};
