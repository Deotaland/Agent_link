#pragma once
#include "round_display.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "sdgoods_sensors.h"
#include "sdgoods_audio.h"
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class BadgeDisplay : public RoundDisplay {
public:
    esp_err_t Init(uint16_t rows);
    bool Ready() const override { return ready_; }
    esp_err_t DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels) override;
    esp_err_t SetBrightness(uint8_t level) override;
    esp_err_t Recover(bool* recovered) override;
    unsigned TeEdges() const {
        return te_edges_.load();
    }
    unsigned TeTimeouts() const {
        return te_timeouts_.load();
    }

private:
 static void OnTe(void* context);
 void AlignFrame(uint32_t pixels);
 esp_err_t InitializePanel();
 esp_err_t Command(uint8_t cmd, const void* data = nullptr, size_t bytes = 0);
 esp_lcd_panel_io_handle_t io_ = nullptr;
 size_t capacity_ = 0;
 uint8_t brightness_ = 0;
 bool ready_ = false, backlight_ok_ = false, reset_pending_ = false;
 SemaphoreHandle_t te_ = nullptr;
 std::atomic<unsigned> te_edges_{0}, te_timeouts_{0};
 unsigned te_strikes_ = 0;
 int64_t last_big_frame_ = 0;
};
class BadgeTouch : public RoundTouch {
public:
    esp_err_t Init(i2c_master_bus_handle_t bus);
    bool Ready() const override { return device_ != nullptr; }
    esp_err_t Poll(cst9217::Point* point) override;
private:
 static void OnInterrupt(void* context);
 i2c_master_dev_handle_t device_ = nullptr;
 cst9217::Point last_;
    int64_t next_poll_ = 0, next_idle_poll_ = 0;
 unsigned slow_ = 0;
 bool interrupt_ok_ = false;
 std::atomic<bool> interrupt_pending_{true};
};
using AmoledAudio = sdgoods::Audio;

// The shared round Factory and voice controllers use this hardware facade.
class AmoledHardware {
public:
    void Init(int volume);
    esp_err_t SetSpeakerEnabled(bool enabled) { return codec_.SetSpeakerEnabled(enabled); }
    void InitDisplayAndTouch(uint16_t stripe_rows = 12);
    void Battery(int* percent, bool* charging);
    esp_err_t EnterSleep(bool power_off);
    bool ButtonDown() const;
    BadgeDisplay panel_;
    BadgeTouch touch_;
    sdgoods::Sensors sensors_;
    sdgoods::Audio audio_;
    AmoledAudio codec_;
    bool codec_ok_ = false;
    bool power_ok_ = false, bus_ok_ = false, button_ok_ = false;
};
