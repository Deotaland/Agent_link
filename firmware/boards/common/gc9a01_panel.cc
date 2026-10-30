#include "gc9a01_panel.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

bool Gc9a01Panel::Done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* context) {
    auto* self = static_cast<Gc9a01Panel*>(context);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(self->done_, &wake);
    return wake == pdTRUE;
}

esp_err_t Gc9a01Panel::Backlight(bool on) {
    if (config_.backlight < 0) return ESP_OK;
    return gpio_set_level(static_cast<gpio_num_t>(config_.backlight), on);
}

esp_err_t Gc9a01Panel::Init(const Gc9a01Config& config) {
    config_ = config;
    done_ = xSemaphoreCreateBinary();
    if (!done_) return ESP_ERR_NO_MEM;
    if (config.backlight >= 0) {
        gpio_config_t bl{};
        bl.pin_bit_mask = 1ULL << config.backlight;
        bl.mode = GPIO_MODE_OUTPUT;
        ESP_RETURN_ON_ERROR(gpio_config(&bl), "gc9a01", "backlight");
        ESP_RETURN_ON_ERROR(Backlight(false), "gc9a01", "backlight off");
    }
    spi_bus_config_t bus{};
    bus.mosi_io_num = config.mosi; bus.sclk_io_num = config.sclk;
    bus.miso_io_num = bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.max_transfer_sz = config.width * 40 * 2;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(config.host, &bus, SPI_DMA_CH_AUTO), "gc9a01", "SPI bus");
    esp_lcd_panel_io_spi_config_t io{};
    io.cs_gpio_num = config.cs; io.dc_gpio_num = config.dc;
    io.pclk_hz = 40000000; io.spi_mode = 0;
    io.trans_queue_depth = 1; io.lcd_cmd_bits = io.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(config.host), &io, &io_), "gc9a01", "SPI IO");
    esp_lcd_panel_io_callbacks_t callbacks{};
    callbacks.on_color_trans_done = Done;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io_, &callbacks, this), "gc9a01", "callback");
    esp_lcd_panel_dev_config_t panel{};
    panel.reset_gpio_num = config.reset;
    panel.rgb_ele_order = config.bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB;
    panel.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(io_, &panel, &panel_), "gc9a01", "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_), "gc9a01", "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_), "gc9a01", "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel_, config.invert), "gc9a01", "inversion");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel_, config.mirror_x, config.mirror_y), "gc9a01", "orientation");
    return esp_lcd_panel_disp_on_off(panel_, true);
}

esp_err_t Gc9a01Panel::Draw(int x1, int y1, int x2, int y2, const void* pixels) {
    if (!panel_) return ESP_ERR_INVALID_STATE;
    // On a timeout the previous DMA buffer is still owned by SPI. Wait before
    // accepting another transfer; callers treat a draw failure as fatal.
    if (pending_ && xSemaphoreTake(done_, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    pending_ = false;
    esp_err_t result = esp_lcd_panel_draw_bitmap(panel_, x1, y1, x2, y2, pixels);
    if (result != ESP_OK) return result;
    pending_ = true;
    if (xSemaphoreTake(done_, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    pending_ = false;
    return ESP_OK;
}
