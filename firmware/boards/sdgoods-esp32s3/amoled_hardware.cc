#include "amoled_hardware.h"
#include "config.h"
#include "co5300_qspi_io.h"
#include "sdgoods_touch_frame.h"
#include "sdgoods_power.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include <algorithm>
#include <cstring>

extern "C" esp_err_t sdgoods_panel_initialize(esp_lcd_panel_io_handle_t io);

namespace {
constexpr const char* TAG = "badge.hw";
esp_err_t Output(gpio_num_t pin, int level) {
    gpio_config_t cfg{}; cfg.pin_bit_mask = 1ULL << pin; cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "GPIO");
    return gpio_set_level(pin, level);
}
bool InterruptService() {
    static const auto error = gpio_install_isr_service(0);
    return error == ESP_OK || error == ESP_ERR_INVALID_STATE;
}
}  // namespace
void BadgeDisplay::OnTe(void* context) {
    auto* self = static_cast<BadgeDisplay*>(context);
    self->te_edges_.fetch_add(1, std::memory_order_relaxed);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(self->te_, &wake);
    if (wake)
        portYIELD_FROM_ISR();
}
void BadgeDisplay::AlignFrame(uint32_t pixels) {
    if (!te_ || te_strikes_ >= 4 || pixels < DISPLAY_WIDTH * DISPLAY_HEIGHT * 4 / 5)
        return;
    const int64_t now = esp_timer_get_time();
    const bool still = !last_big_frame_ || now - last_big_frame_ >= 150000;
    last_big_frame_ = now;
    if (!still)
        return;  // Keep continuous carousel gestures flowing.
    (void)xSemaphoreTake(te_, 0);
    if (xSemaphoreTake(te_, pdMS_TO_TICKS(40)) == pdTRUE)
        te_strikes_ = 0;
    else {
        te_timeouts_.fetch_add(1);
        if (++te_strikes_ == 4)
            ESP_LOGW(TAG, "No TE signal; continuing without frame synchronization");
    }
}
esp_err_t BadgeDisplay::Command(uint8_t cmd, const void* data, size_t bytes) {
    return esp_lcd_panel_io_tx_param(io_, (0x02u << 24) | (static_cast<unsigned>(cmd) << 8), data, bytes);
}
esp_err_t BadgeDisplay::InitializePanel() {
    ESP_RETURN_ON_ERROR(Output(DISPLAY_RST_PIN, 0), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(DISPLAY_RST_PIN, 1), TAG, "reset release");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(sdgoods_panel_initialize(io_), TAG, "panel commands");
    const uint8_t vertical = 0;
    return Command(0x35, &vertical, 1);
}
esp_err_t BadgeDisplay::Init(uint16_t rows) {
    // A small permanent internal DMA stripe isolates rendering from TLS/flash fragmentation.
    rows = std::clamp<uint16_t>(rows & ~1u, 2, 12);
    capacity_ = DISPLAY_WIDTH * rows * 2;
    ESP_RETURN_ON_ERROR(Output(DISPLAY_BACKLIGHT_PIN, 0), TAG, "backlight off");
    ESP_RETURN_ON_ERROR(Output(DISPLAY_POWER_PIN, DISPLAY_POWER_ON_LEVEL), TAG, "LCD supply");
    vTaskDelay(pdMS_TO_TICKS(50));
    spi_bus_config_t bus{};
    bus.sclk_io_num = DISPLAY_SCLK_PIN; bus.data0_io_num = DISPLAY_D0_PIN;
    bus.data1_io_num = DISPLAY_D1_PIN; bus.data2_io_num = DISPLAY_D2_PIN;
    bus.data3_io_num = DISPLAY_D3_PIN; bus.data4_io_num = -1;
    bus.data5_io_num = -1; bus.data6_io_num = -1; bus.data7_io_num = -1;
    bus.max_transfer_sz = capacity_;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "QSPI bus");
    // ST77916 and CO5300 use the same 02/32 + 24-bit address QSPI framing.
    ESP_RETURN_ON_ERROR(co5300_new_qspi_io(SPI2_HOST, DISPLAY_CS_PIN, DISPLAY_QSPI_CLK_HZ,
                                         capacity_, &io_), TAG, "QSPI IO");
    ESP_RETURN_ON_ERROR(InitializePanel(), TAG, "panel");
    ledc_timer_config_t timer{}; timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT; timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 20000; timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "PWM timer");
    ledc_channel_config_t pwm{}; pwm.gpio_num = DISPLAY_BACKLIGHT_PIN;
    pwm.speed_mode = LEDC_LOW_SPEED_MODE; pwm.channel = LEDC_CHANNEL_0;
    pwm.timer_sel = LEDC_TIMER_0; pwm.duty = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&pwm), TAG, "PWM channel");
    backlight_ok_ = true;
    te_ = xSemaphoreCreateBinary();
    if (te_) {
        gpio_config_t interrupt{};
        interrupt.pin_bit_mask = 1ULL << DISPLAY_TE_PIN;
        interrupt.mode = GPIO_MODE_INPUT;
        interrupt.pull_down_en = GPIO_PULLDOWN_ENABLE;
        interrupt.intr_type = GPIO_INTR_POSEDGE;
        if (gpio_config(&interrupt) != ESP_OK || !InterruptService() ||
            gpio_isr_handler_add(DISPLAY_TE_PIN, OnTe, this) != ESP_OK) {
            vSemaphoreDelete(te_);
            te_ = nullptr;
            ESP_LOGW(TAG, "TE interrupt unavailable; software refresh remains enabled");
        }
    }
    ready_ = true;
    // Never illuminate uninitialized GRAM. This temporary black stripe is PSRAM-owned.
    auto* black = heap_caps_calloc(1, capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!black) { ready_ = false; return ESP_ERR_NO_MEM; }
    esp_err_t err = ESP_OK;
    for (int y = 0; y < DISPLAY_HEIGHT && err == ESP_OK; y += rows)
        err = DrawBitmap(0, y, DISPLAY_WIDTH, std::min<int>(rows, DISPLAY_HEIGHT - y), black);
    heap_caps_free(black);
    ready_ = err == ESP_OK;
    ESP_LOGI(TAG, "ST77916 360x360 QSPI 80MHz, %uB internal DMA stripe", unsigned(capacity_));
    return err;
}
esp_err_t BadgeDisplay::DrawBitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels) {
    if (!ready_) return ESP_ERR_INVALID_STATE;
    if (!pixels || !w || !h || x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) return ESP_ERR_INVALID_ARG;
    AlignFrame(uint32_t(w) * h);
    const auto* source = static_cast<const uint8_t*>(pixels);
    const size_t rows = capacity_ / (w * 2);
    for (size_t row = 0; row < h; row += rows) {
        const unsigned n = std::min<size_t>(rows, h - row);
        const uint8_t columns[]{uint8_t(x >> 8), uint8_t(x), uint8_t((x+w-1) >> 8), uint8_t(x+w-1)};
        const unsigned top = y + row, bottom = top + n - 1;
        const uint8_t lines[]{uint8_t(top >> 8), uint8_t(top), uint8_t(bottom >> 8), uint8_t(bottom)};
        ESP_RETURN_ON_ERROR(Command(0x2A, columns, sizeof columns), TAG, "columns");
        ESP_RETURN_ON_ERROR(Command(0x2B, lines, sizeof lines), TAG, "rows");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io_, (0x32u << 24) | (0x2Cu << 8),
                             source + row * w * 2, n * w * 2), TAG, "pixels");
    }
    return ESP_OK;
}
esp_err_t BadgeDisplay::SetBrightness(uint8_t level) {
    brightness_ = level;
    if (!backlight_ok_) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level * 1023u / 255u), TAG, "PWM duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
esp_err_t BadgeDisplay::Recover(bool* recovered) {
    if (!recovered || !io_) return ESP_ERR_INVALID_ARG;
    *recovered = false;
    bool reset = false;
    ESP_RETURN_ON_ERROR(co5300_qspi_io_recover(io_, &reset), TAG, "recover IO");
    reset_pending_ |= reset;
    if (reset_pending_) {
        ESP_RETURN_ON_ERROR(InitializePanel(), TAG, "recover panel");
        ESP_RETURN_ON_ERROR(SetBrightness(brightness_), TAG, "recover brightness");
        reset_pending_ = false;
        te_strikes_ = 0;
        *recovered = true;
    }
    return ESP_OK;
}
esp_err_t BadgeTouch::Init(i2c_master_bus_handle_t bus) {
    ESP_RETURN_ON_ERROR(Output(TOUCH_RST_PIN, 0), TAG, "touch reset");
    vTaskDelay(pdMS_TO_TICKS(11)); gpio_set_level(TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, 0x15, 90), TAG, "CST816 probe");
    i2c_device_config_t cfg{}; cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = 0x15; cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &device_), TAG, "CST816 device");
    gpio_config_t interrupt{};
    interrupt.pin_bit_mask = 1ULL << TOUCH_INT_PIN;
    interrupt.mode = GPIO_MODE_INPUT;
    interrupt.pull_up_en = GPIO_PULLUP_ENABLE;
    interrupt.intr_type = GPIO_INTR_NEGEDGE;
    interrupt_ok_ = gpio_config(&interrupt) == ESP_OK && InterruptService() &&
                    gpio_isr_handler_add(TOUCH_INT_PIN, OnInterrupt, this) == ESP_OK;
    return ESP_OK;
}
void BadgeTouch::OnInterrupt(void* context) {
    static_cast<BadgeTouch*>(context)->interrupt_pending_.store(true, std::memory_order_relaxed);
}
esp_err_t BadgeTouch::Poll(cst9217::Point* point) {
    if (!point || !device_) return ESP_ERR_INVALID_STATE;
    const int64_t now = esp_timer_get_time();
    const bool interrupt = interrupt_pending_.exchange(false);
    if (now < next_poll_) {
        if (interrupt) interrupt_pending_.store(true);
        *point = last_;
        return ESP_OK;
    }
    if (interrupt_ok_ && !interrupt && !last_.pressed && now < next_idle_poll_) {
        *point = last_;
        return ESP_OK;
    }
    const uint8_t reg = 0x02; uint8_t raw[5]{};
    const esp_err_t err = i2c_master_transmit_receive(device_, &reg, 1, raw, sizeof raw, 90);
    const int64_t end = esp_timer_get_time();
    slow_ = err != ESP_OK || end - now > 20000 ? std::min(slow_ + 1, 15u) : 0;
    next_poll_ = end + 12000 + slow_ * 20000;
    next_idle_poll_ = end + 100000;
    last_ = {};
    if (err == ESP_OK && !sdgoods::DecodeTouch(raw, sizeof raw, &last_)) {
        *point = {}; return ESP_ERR_INVALID_RESPONSE;
    }
    *point = last_;
    return err;
}
void AmoledHardware::InitDisplayAndTouch(uint16_t rows) {
    // Cold/USB power must qualify the key before any display or audio startup.
    (void)sdgoods_power_startup(true);
    for (auto pin :
         {DISPLAY_POWER_PIN, DISPLAY_BACKLIGHT_PIN, DISPLAY_RST_PIN, TOUCH_RST_PIN, AUDIO_PA_PIN})
        gpio_hold_dis(pin);
    ESP_ERROR_CHECK(Output(AUDIO_PA_PIN, 0));
    (void)rtc_gpio_deinit(CALL_BUTTON_PIN); // EXT0 wake leaves the key in the RTC mux.
    gpio_config_t key{}; key.pin_bit_mask = 1ULL << CALL_BUTTON_PIN;
    key.mode = GPIO_MODE_INPUT; key.pull_up_en = GPIO_PULLUP_ENABLE;
    button_ok_ = gpio_config(&key) == ESP_OK;
    const esp_err_t display = panel_.Init(rows);
    if (display != ESP_OK) ESP_LOGE(TAG, "LCD init: %s", esp_err_to_name(display));
    i2c_master_bus_config_t cfg{}; cfg.i2c_port = SYS_I2C_PORT;
    cfg.sda_io_num = SYS_I2C_SDA; cfg.scl_io_num = SYS_I2C_SCL;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT; cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    bus_ok_ = i2c_new_master_bus(&cfg, &bus) == ESP_OK;
    if (!bus_ok_ || touch_.Init(bus) != ESP_OK) ESP_LOGE(TAG, "Touch unavailable");
    sensors_.Init(bus);
    power_ok_ = sensors_.BatteryReady();
}
void AmoledHardware::Init(int volume) {
    InitDisplayAndTouch();
    codec_ok_ = codec_.Init(volume) == ESP_OK;
    if (!codec_ok_) ESP_LOGE(TAG, "Voice audio initialization failed");
}
void AmoledHardware::Battery(int* percent, bool* charging) {
    sensors_.Poll(sensors_.MotionEnabled());
    *percent = sensors_.Read().percent;
    *charging = false;  // No documented charger-status input. Factory explicitly displays unknown.
}
esp_err_t AmoledHardware::EnterSleep(bool power_off) {
    if (ButtonDown() || !audio_.StopAndWait())
        return ESP_ERR_INVALID_STATE;
    sensors_.Suspend();
    ESP_RETURN_ON_ERROR(panel_.SetBrightness(0), TAG, "sleep backlight");
    ESP_RETURN_ON_ERROR(Output(DISPLAY_POWER_PIN, 1), TAG, "LCD off");
    ESP_RETURN_ON_ERROR(Output(DISPLAY_RST_PIN, 0), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(Output(TOUCH_RST_PIN, 0), TAG, "touch off");
    ESP_RETURN_ON_ERROR(Output(AUDIO_PA_PIN, 0), TAG, "amplifier off");
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(CALL_BUTTON_PIN, 0), TAG, "key wake");
    rtc_gpio_pullup_en(CALL_BUTTON_PIN);
    rtc_gpio_pulldown_dis(CALL_BUTTON_PIN);
    sdgoods_power_set_sleep_resume(!power_off);
    ESP_RETURN_ON_ERROR(gpio_set_level(BATTERY_LATCH_PIN, power_off ? 0 : 1), TAG, "battery latch");
    for (auto pin : {BATTERY_LATCH_PIN, DISPLAY_POWER_PIN, DISPLAY_BACKLIGHT_PIN, DISPLAY_RST_PIN,
                     TOUCH_RST_PIN, AUDIO_PA_PIN})
        gpio_hold_en(pin);
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
    return ESP_FAIL;
}
bool AmoledHardware::ButtonDown() const { return button_ok_ && !gpio_get_level(CALL_BUTTON_PIN); }
