#include "ojbadge_hardware.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

esp_err_t OjBadgeHardware::Init() {
    Gc9a01Config lcd{};
    lcd.mosi = LCD_MOSI; lcd.sclk = LCD_SCLK; lcd.cs = LCD_CS;
    lcd.dc = LCD_DC; lcd.reset = LCD_RESET; lcd.backlight = LCD_BACKLIGHT;
    lcd.mirror_y = true;  // Correct the vertically mirrored image on OJBadge.
    ESP_RETURN_ON_ERROR(panel.Init(lcd), "OJBadge", "display");

    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = static_cast<gpio_num_t>(TOUCH_SDA);
    bus.scl_io_num = static_cast<gpio_num_t>(TOUCH_SCL);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7; bus.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t handle = nullptr;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &handle), "OJBadge", "touch/gauge bus");
    Cst816Config tp{};
    tp.pin_rst = TOUCH_RESET; tp.out_width = tp.out_height = 240;
    esp_err_t result = touch.Init(tp);
    if (result != ESP_OK) ESP_LOGW("OJBadge", "touch unavailable: %s", esp_err_to_name(result));
    result = battery.Init(I2C_NUM_0);
    if (result != ESP_OK) ESP_LOGW("OJBadge", "battery gauge unavailable: %s", esp_err_to_name(result));
    gpio_config_t button{};
    button.pin_bit_mask = 1ULL << USER_BUTTON;
    button.mode = GPIO_MODE_INPUT; button.pull_up_en = GPIO_PULLUP_ENABLE;
    button_ready = gpio_config(&button) == ESP_OK;
    return ESP_OK;
}
