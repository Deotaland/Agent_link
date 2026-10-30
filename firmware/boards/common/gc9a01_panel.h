#pragma once
#include "driver/spi_master.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct Gc9a01Config {
    spi_host_device_t host = SPI2_HOST;
    int mosi = -1, sclk = -1, cs = -1, dc = -1, reset = -1, backlight = -1;
    int width = 240, height = 240;
    bool bgr = true, invert = true, mirror_x = false, mirror_y = false;
};

class Gc9a01Panel {
public:
    esp_err_t Init(const Gc9a01Config& config);
    esp_err_t Draw(int x1, int y1, int x2, int y2, const void* pixels);
    esp_err_t Backlight(bool on);
private:
    static bool Done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*);
    Gc9a01Config config_{};
    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    SemaphoreHandle_t done_ = nullptr;
    bool pending_ = false;
};
