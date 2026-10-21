#include "epaper_hardware.h"

#include <algorithm>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace { constexpr const char* TAG = "WS397.hw"; }

esp_err_t EpaperHardware::Read(uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(pmic_, &reg, 1, value, 1, 100);
}
esp_err_t EpaperHardware::Update(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t old = 0;
    ESP_RETURN_ON_ERROR(Read(reg, &old), TAG, "PMIC read");
    const uint8_t data[] = {reg, static_cast<uint8_t>((old & ~mask) | (value & mask))};
    return i2c_master_transmit(pmic_, data, sizeof data, 100);
}

esp_err_t EpaperHardware::Init(int volume) {
    gpio_config_t outputs{};
    outputs.pin_bit_mask = (1ULL << AUDIO_PA) | (1ULL << EPD_RST) | (1ULL << EPD_DC);
    outputs.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "outputs");
    (void)Speaker(false);
    gpio_config_t keys{};
    keys.pin_bit_mask = (1ULL << KEY_UP) | (1ULL << KEY_CENTER) | (1ULL << KEY_DOWN) | (1ULL << KEY_BOOT);
    keys.mode = GPIO_MODE_INPUT;
    keys.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&keys), TAG, "keys");
    gpio_config_t busy{};
    busy.pin_bit_mask = 1ULL << EPD_BUSY;
    busy.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&busy), TAG, "BUSY");

    i2c_master_bus_config_t bus{};
    bus.i2c_port = SYS_I2C_PORT;
    bus.sda_io_num = SYS_I2C_SDA; bus.scl_io_num = SYS_I2C_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT; bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &bus_), TAG, "I2C");
    i2c_device_config_t pmic{};
    pmic.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    pmic.device_address = PMIC_ADDR; pmic.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_, &pmic, &pmic_), TAG, "PMIC");
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(Read(0x03, &id), TAG, "PMIC identity");
    // The official TG28 BSP uses the AXP2101-compatible register map.
    ESP_RETURN_ON_FALSE(id == 0x4a, ESP_ERR_NOT_SUPPORTED, TAG, "unknown PMIC 0x%02x", id);
    // ALDO1: amplifier supply, ALDO2: codec/mic, ALDO3: display, all 3.3 V.
    // Preserve DC1, charging parameters and the independent PWR key behavior.
    for (uint8_t reg : {0x92, 0x93, 0x94})
        ESP_RETURN_ON_ERROR(Update(reg, 0x1f, 28), TAG, "3.3 V rail");
    ESP_RETURN_ON_ERROR(Update(0x90, 0x07, 0x07), TAG, "enable rails");
    ESP_RETURN_ON_ERROR(Update(0x68, 0x01, 0x01), TAG, "battery detection");
    vTaskDelay(pdMS_TO_TICKS(100));

    Es8311Config audio{};
    audio.i2c_port = SYS_I2C_PORT;
    audio.pin_sda = audio.pin_scl = GPIO_NUM_NC;
    audio.pin_mclk = AUDIO_I2S_MCLK; audio.pin_bclk = AUDIO_I2S_BCLK;
    audio.pin_ws = AUDIO_I2S_WS; audio.pin_din = AUDIO_I2S_DIN; audio.pin_dout = AUDIO_I2S_DOUT;
    audio.pin_pa_en = GPIO_NUM_NC;  // PA is gated by the playback worker.
    audio.es8311_addr = ES8311_ADDR; audio.i2s_port = I2S_NUM_0;
    audio.sample_rate = AUDIO_SAMPLE_RATE; audio.mic_gain = 30; audio.out_volume = volume;
    audio.pa_voltage = 3.3f; audio.use_default_volume_curve = true;
    audio_ok_ = codec.Init(audio) == ESP_OK;
    if (!audio_ok_) ESP_LOGE(TAG, "ES8311 unavailable");

    spi_bus_config_t spi{};
    spi.mosi_io_num = EPD_MOSI; spi.miso_io_num = -1; spi.sclk_io_num = EPD_SCLK;
    spi.quadwp_io_num = -1; spi.quadhd_io_num = -1; spi.max_transfer_sz = 4096;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_SPI_HOST, &spi, SPI_DMA_CH_AUTO), TAG, "SPI bus");
    spi_device_interface_config_t device{};
    device.clock_speed_hz = 20000000; device.spics_io_num = EPD_CS; device.queue_size = 1;
    return spi_bus_add_device(EPD_SPI_HOST, &device, &spi_);
}

esp_err_t EpaperHardware::Battery(int* percent, bool* charging) {
    *percent = -1; *charging = false;
    if (!pmic_) return ESP_ERR_INVALID_STATE;
    uint8_t present = 0, state = 0, value = 0;
    ESP_RETURN_ON_ERROR(Read(0x00, &present), TAG, "battery presence");
    if (!(present & 0x08)) return ESP_OK;
    ESP_RETURN_ON_ERROR(Read(0x01, &state), TAG, "charge state");
    ESP_RETURN_ON_ERROR(Read(0xa4, &value), TAG, "battery percent");
    if (value <= 100) *percent = value;
    *charging = (state >> 5) == 1;
    return ESP_OK;
}

esp_err_t EpaperHardware::Send(bool data, const uint8_t* bytes, size_t length) {
    gpio_set_level(EPD_DC, data);
    while (length) {
        const size_t chunk = std::min(length, size_t(4096));
        spi_transaction_t transaction{};
        transaction.length = chunk * 8; transaction.tx_buffer = bytes;
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(spi_, &transaction), TAG, "SPI write");
        bytes += chunk; length -= chunk;
    }
    return ESP_OK;
}
esp_err_t EpaperHardware::Command(uint8_t command, const uint8_t* data, size_t length) {
    ESP_RETURN_ON_ERROR(Send(false, &command, 1), TAG, "command");
    return length ? Send(true, data, length) : ESP_OK;
}
esp_err_t EpaperHardware::Wait() {
    vTaskDelay(pdMS_TO_TICKS(100));
    const int64_t deadline = esp_timer_get_time() + 20000000;
    while (gpio_get_level(EPD_BUSY)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}
esp_err_t EpaperHardware::Display(const uint8_t* frame, bool full) {
    if (!spi_ || !frame) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(Update(0x90, 0x04, 0x04), TAG, "display power");
    // Native full/fast monochrome sequence from Waveshare's 3.97-inch BSP.
    gpio_set_level(EPD_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_RST, 0); vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(EPD_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(Wait(), TAG, "reset BUSY");
    ESP_RETURN_ON_ERROR(Command(0x12), TAG, "soft reset");
    ESP_RETURN_ON_ERROR(Wait(), TAG, "soft reset BUSY");
    const uint8_t booster[] = {0xae, 0xc7, 0xc3, 0xc0, 0x80};
    const uint8_t driver[] = {0xdf, 0x01, 0x02};
    const uint8_t x[] = {0x00, 0x00, 0x1f, 0x03};
    const uint8_t y[] = {0xdf, 0x01, 0x00, 0x00}, zero[] = {0, 0};
    const uint8_t one = 1, sensor = 0x80, fast = 0x6a;
    ESP_RETURN_ON_ERROR(Command(0x0c, booster, sizeof booster), TAG, "booster");
    ESP_RETURN_ON_ERROR(Command(0x01, driver, sizeof driver), TAG, "driver");
    ESP_RETURN_ON_ERROR(Command(0x11, &one, 1), TAG, "entry mode");
    ESP_RETURN_ON_ERROR(Command(0x44, x, sizeof x), TAG, "X range");
    ESP_RETURN_ON_ERROR(Command(0x45, y, sizeof y), TAG, "Y range");
    ESP_RETURN_ON_ERROR(Command(0x4e, zero, sizeof zero), TAG, "X cursor");
    ESP_RETURN_ON_ERROR(Command(0x4f, zero, sizeof zero), TAG, "Y cursor");
    ESP_RETURN_ON_ERROR(Command(0x3c, &one, 1), TAG, "border");
    ESP_RETURN_ON_ERROR(Command(0x18, &sensor, 1), TAG, "temperature");
    if (!full) ESP_RETURN_ON_ERROR(Command(0x1a, &fast, 1), TAG, "fast waveform");
    ESP_RETURN_ON_ERROR(Command(0x24, frame, EPD_WIDTH * EPD_HEIGHT / 8), TAG, "black RAM");
    ESP_RETURN_ON_ERROR(Command(0x26, frame, EPD_WIDTH * EPD_HEIGHT / 8), TAG, "base RAM");
    const uint8_t refresh = full ? 0xf7 : 0xd7;
    ESP_RETURN_ON_ERROR(Command(0x22, &refresh, 1), TAG, "refresh mode");
    ESP_RETURN_ON_ERROR(Command(0x20), TAG, "refresh");
    ESP_RETURN_ON_ERROR(Wait(), TAG, "refresh BUSY");
    ESP_RETURN_ON_ERROR(Command(0x10, &one, 1), TAG, "deep sleep");
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RST, 0); gpio_set_level(EPD_DC, 0);
    return Update(0x90, 0x04, 0);
}
