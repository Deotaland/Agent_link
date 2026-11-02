#include "amoled_audio.h"
#include "config.h"
#include "esp_log.h"

esp_err_t AmoledAudio::Init(int volume) {
    EsCodecConfig config{};
    config.i2c_port = SYS_I2C_PORT;
    config.pin_sda = config.pin_scl = GPIO_NUM_NC;  // Reuse the board's shared bus.
    config.pin_mclk = AUDIO_I2S_MCLK; config.pin_bclk = AUDIO_I2S_BCLK;
    config.pin_ws = AUDIO_I2S_WS; config.pin_dout = AUDIO_I2S_DOUT;
    config.pin_din = AUDIO_I2S_DIN;
    config.pin_pa_en = GPIO_NUM_NC;  // AmoledHardware gates the active-high GPIO46 PA.
    config.es7210_addr = 0x40; config.es8311_addr = 0x18;
    config.sample_rate = AUDIO_SAMPLE_RATE;
    config.mic_gain = AUDIO_MIC_GAIN; config.out_volume = volume;
    const esp_err_t result = codec_.Init(config);
    if (result == ESP_OK)
        ESP_LOGI("Amoled175.codec", "non-C 16MB board: MCLK=GPIO42, STD TX/TDM4 RX, MIC1 slot0, %dHz PCM16 mono", AUDIO_SAMPLE_RATE);
    return result;
}

esp_err_t AmoledAudio::ReadPcm(int16_t* dest, size_t capacity, size_t* got) {
    if (got) *got = 0;
    channels_ = {};
    if (capacity < kCaptureCapacity) return ESP_ERR_INVALID_SIZE;
    size_t frames = 0;
    const esp_err_t result = codec_.ReadPcm(dest, kCaptureCapacity, &frames);
    if (result != ESP_OK) return result;
    channels_[0].Add(dest, frames);
    // This driver exposes MIC1 mono, so the MIC2 diagnostic slot stays empty.
    if (got) *got = frames;
    return ESP_OK;
}
