#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

constexpr int GPIO_NUM_NC = -1, I2C_NUM_0 = 0;
constexpr int GPIO_NUM_0 = 0, GPIO_NUM_4 = 4, GPIO_NUM_5 = 5, GPIO_NUM_6 = 6, GPIO_NUM_7 = 7;
constexpr int GPIO_NUM_8 = 8, GPIO_NUM_9 = 9, GPIO_NUM_10 = 10, GPIO_NUM_11 = 11, GPIO_NUM_12 = 12;
constexpr int GPIO_NUM_14 = 14, GPIO_NUM_15 = 15, GPIO_NUM_38 = 38, GPIO_NUM_39 = 39, GPIO_NUM_40 = 40;
constexpr int GPIO_NUM_42 = 42, GPIO_NUM_45 = 45, GPIO_NUM_46 = 46;
struct EsCodecConfig {
    int i2c_port, pin_sda, pin_scl, pin_mclk, pin_bclk, pin_ws, pin_din, pin_dout, pin_pa_en;
    uint8_t es7210_addr, es8311_addr;
    int sample_rate, mic_gain, out_volume;
};
extern EsCodecConfig observed;
extern int init_error, read_error, read_calls, observed_volume;
class EsCodec {
public:
    int Init(const EsCodecConfig& config) { observed = config; ready_ = init_error == ESP_OK; return init_error; }
    int StartMic() { if (!ready_) return ESP_ERR_INVALID_STATE; mic_ = true; return ESP_OK; }
    int StopMic() { mic_ = false; return ESP_OK; }
    bool MicOn() const { return mic_; }
    int ReadPcm(int16_t* data, size_t frames, size_t* got) {
        *got = 0;
        if (!mic_ || !data) return ESP_ERR_INVALID_STATE;
        ++read_calls;
        if (read_error) return read_error;
        for (size_t i = 0; i < frames; ++i) data[i] = i % 2 ? 1000 : -2000;
        *got = frames; return ESP_OK;
    }
    int WritePcm(const int16_t*, size_t) { return ready_ ? ESP_OK : ESP_ERR_INVALID_STATE; }
    int SetVolume(uint8_t value) { observed_volume = value; return ESP_OK; }
private:
    bool ready_ = false, mic_ = false;
};
