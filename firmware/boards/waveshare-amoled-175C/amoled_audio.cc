#include "amoled_audio.h"

#include <algorithm>
#include <cstring>
#include <new>
#include "config.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

namespace {
constexpr const char* TAG = "Amoled175.codec";
constexpr size_t kPlayFrames = 256;
static_assert(AUDIO_SAMPLE_RATE == 16000 && AUDIO_HARDWARE_SAMPLE_RATE == 16000);
// C-board BSP: standard I2S in both directions, shared GPIO16 MCLK.
// See Waveshare-ESP32-components commit 23a6012, bsp/esp32_s3_touch_amoled_1_75c.
// Its microphone codec selects the two front inputs (MIC1/MIC2), not TDM.
constexpr uint16_t kConnectedMics = 0x03;

// codec_dev 1.6.2 ignores data_if set_fmt/enable failures in open(). Remember
// them here so a codec-register success cannot hide an I2S configuration error.
struct CheckedData {
    audio_codec_data_if_t api{};
    const audio_codec_data_if_t* driver = nullptr;
    int error = 0;
    static CheckedData* Self(const audio_codec_data_if_t* h) {
        return reinterpret_cast<CheckedData*>(const_cast<audio_codec_data_if_t*>(h));
    }
    void Init(const audio_codec_data_if_t* d) {
        driver = d;
        api.open = [](const audio_codec_data_if_t* h, void* c, int n) { auto* s = Self(h); return s->driver->open(s->driver, c, n); };
        api.is_open = [](const audio_codec_data_if_t* h) { auto* s = Self(h); return s->driver->is_open(s->driver); };
        api.set_fmt = [](const audio_codec_data_if_t* h, esp_codec_dev_type_t t, esp_codec_dev_sample_info_t* f) {
            auto* s = Self(h); const int r = s->driver->set_fmt(s->driver, t, f);
            if (r) { s->error = r; ESP_LOGE(TAG, "I2S set_fmt type=%d failed: %d", t, r); }
            return r;
        };
        api.enable = [](const audio_codec_data_if_t* h, esp_codec_dev_type_t t, bool on) {
            auto* s = Self(h); const int r = s->driver->enable(s->driver, t, on);
            if (r) { s->error = r; ESP_LOGE(TAG, "I2S enable type=%d on=%d failed: %d", t, on, r); }
            return r;
        };
        api.read = [](const audio_codec_data_if_t* h, uint8_t* p, int n) { auto* s = Self(h); return s->driver->read(s->driver, p, n); };
        api.write = [](const audio_codec_data_if_t* h, uint8_t* p, int n) { auto* s = Self(h); return s->driver->write(s->driver, p, n); };
        api.close = [](const audio_codec_data_if_t* h) { auto* s = Self(h); return s->driver->close(s->driver); };
    }
};
}

struct AmoledAudio::State {
    i2s_chan_handle_t tx = nullptr, rx = nullptr;
    const audio_codec_ctrl_if_t* out_ctrl = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl = nullptr;
    const audio_codec_if_t* out_codec = nullptr;
    const audio_codec_if_t* in_codec = nullptr;
    esp_codec_dev_handle_t output = nullptr, input = nullptr;
    CheckedData checked;
    // Worker-owned buffers stay off task stacks; capture and playback do not
    // share scratch memory. No rate conversion is needed at the SDK boundary.
    int16_t interleaved[kHardwareFrames * 2]{};
    int16_t stereo[kPlayFrames * 2]{};

    ~State() {
        if (input) esp_codec_dev_delete(input);
        if (output) esp_codec_dev_delete(output);
        if (in_codec) audio_codec_delete_codec_if(in_codec);
        if (out_codec) audio_codec_delete_codec_if(out_codec);
        if (in_ctrl) audio_codec_delete_ctrl_if(in_ctrl);
        if (out_ctrl) audio_codec_delete_ctrl_if(out_ctrl);
        if (checked.driver) audio_codec_delete_data_if(checked.driver);
        if (rx) { (void)i2s_channel_disable(rx); (void)i2s_del_channel(rx); }
        if (tx) { (void)i2s_channel_disable(tx); (void)i2s_del_channel(tx); }
    }
    esp_err_t Open(esp_codec_dev_handle_t device, uint8_t channels, uint16_t mask) {
        esp_codec_dev_sample_info_t fs{};
        fs.sample_rate = AUDIO_HARDWARE_SAMPLE_RATE;
        fs.bits_per_sample = 16; fs.channel = channels; fs.channel_mask = mask;
        checked.error = 0;
        const int r = esp_codec_dev_open(device, &fs);
        if (r || checked.error) {
            ESP_LOGE(TAG, "codec open ch=%u mask=0x%x failed: codec=%d data=%d", channels, mask, r, checked.error);
            (void)esp_codec_dev_close(device);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    esp_err_t Init(int volume) {
        i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        ch.dma_desc_num = 6; ch.dma_frame_num = 240; ch.auto_clear_after_cb = true;
        ESP_RETURN_ON_ERROR(i2s_new_channel(&ch, &tx, &rx), TAG, "create I2S");
        i2s_std_config_t std{};
        std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_HARDWARE_SAMPLE_RATE);
        std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        std.gpio_cfg.mclk = AUDIO_I2S_MCLK; std.gpio_cfg.bclk = AUDIO_I2S_BCLK;
        std.gpio_cfg.ws = AUDIO_I2S_WS; std.gpio_cfg.dout = AUDIO_I2S_DOUT;
        std.gpio_cfg.din = I2S_GPIO_UNUSED;
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "STD TX init");
        i2s_std_config_t rx_cfg = std;
        rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
        rx_cfg.gpio_cfg.din = AUDIO_I2S_DIN;
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &rx_cfg), TAG, "STD RX init");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "TX enable");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "RX enable");
        audio_codec_i2s_cfg_t data{};
        data.port = I2S_NUM_0; data.rx_handle = rx; data.tx_handle = tx;
        checked.Init(audio_codec_new_i2s_data(&data));
        ESP_RETURN_ON_FALSE(checked.driver, ESP_ERR_NO_MEM, TAG, "I2S data interface");
        i2c_master_bus_handle_t bus = nullptr;
        ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(SYS_I2C_PORT, &bus), TAG, "shared I2C bus");
        audio_codec_i2c_cfg_t ctrl{};
        ctrl.port = SYS_I2C_PORT; ctrl.bus_handle = bus; ctrl.addr = 0x18 << 1;
        out_ctrl = audio_codec_new_i2c_ctrl(&ctrl);
        ctrl.addr = 0x40 << 1;
        in_ctrl = audio_codec_new_i2c_ctrl(&ctrl);
        ESP_RETURN_ON_FALSE(out_ctrl && in_ctrl, ESP_ERR_NO_MEM, TAG, "codec control");
        es8311_codec_cfg_t dac{};
        dac.ctrl_if = out_ctrl; dac.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
        dac.pa_pin = GPIO_NUM_NC; dac.use_mclk = true;  // Board owns PA gating.
        dac.hw_gain.pa_voltage = 3.3f; dac.hw_gain.codec_dac_voltage = 3.3f;
        out_codec = es8311_codec_new(&dac);
        es7210_codec_cfg_t adc{};
        adc.ctrl_if = in_ctrl; adc.mic_selected = kConnectedMics;
        in_codec = es7210_codec_new(&adc);
        ESP_RETURN_ON_FALSE(out_codec && in_codec, ESP_FAIL, TAG, "codec init");
        esp_codec_dev_cfg_t device{};
        device.dev_type = ESP_CODEC_DEV_TYPE_OUT; device.codec_if = out_codec; device.data_if = &checked.api;
        output = esp_codec_dev_new(&device);
        device.dev_type = ESP_CODEC_DEV_TYPE_IN; device.codec_if = in_codec;
        input = esp_codec_dev_new(&device);
        ESP_RETURN_ON_FALSE(output && input, ESP_ERR_NO_MEM, TAG, "codec devices");
        ESP_RETURN_ON_ERROR(Open(output, 2, 0), TAG, "open stereo output");
        ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_mute(output, false) == 0 &&
                            esp_codec_dev_set_out_vol(output, volume) == 0, ESP_FAIL, TAG, "output volume/mute");
        ESP_LOGI(TAG, "hardware=ESP32-S3-Touch-AMOLED-1.75C: MCLK=GPIO16, STD TX/RX 16kHz PCM16 stereo, MIC1/2 gain=%ddB; SDK PCM16 mono 16kHz -> Opus", AUDIO_MIC_GAIN);
        return ESP_OK;
    }
};

esp_err_t AmoledAudio::Init(int volume) {
    if (state_) return ESP_ERR_INVALID_STATE;
    auto* state = new (std::nothrow) State;
    if (!state) return ESP_ERR_NO_MEM;
    const esp_err_t r = state->Init(volume);
    if (r != ESP_OK) { delete state; return r; }
    state_ = state;
    return ESP_OK;
}

esp_err_t AmoledAudio::StartMic() {
    if (!state_) return ESP_ERR_INVALID_STATE;
    if (mic_on_) return ESP_OK;
    ESP_RETURN_ON_ERROR(state_->Open(state_->input, 2, 0), TAG, "open standard stereo input");
    if (esp_codec_dev_set_in_channel_gain(state_->input, kConnectedMics, AUDIO_MIC_GAIN) != 0 ||
        esp_codec_dev_set_in_mute(state_->input, false) != 0) {
        (void)esp_codec_dev_close(state_->input); return ESP_FAIL;
    }
    // Verify the physical ADC state instead of relying only on Open OK logs.
    constexpr int regs[] = {0x00, 0x01, 0x02, 0x06, 0x07, 0x08, 0x11, 0x12, 0x14, 0x15, 0x43, 0x44, 0x45, 0x46, 0x4b, 0x4c};
    for (int reg : regs) {
        int value = 0;
        if (esp_codec_dev_read_reg(state_->input, reg, &value) != 0) {
            ESP_LOGE(TAG, "ES7210 register read failed: 0x%02x", reg);
            (void)esp_codec_dev_close(state_->input); return ESP_FAIL;
        }
        ESP_LOGI(TAG, "ES7210 reg 0x%02x=0x%02x", reg, value);
    }
    channels_ = {}; mic_on_ = true;
    return ESP_OK;
}

esp_err_t AmoledAudio::StopMic() {
    if (!state_ || !mic_on_) return ESP_OK;
    mic_on_ = false;
    return esp_codec_dev_close(state_->input) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t AmoledAudio::ReadPcm(int16_t* dest, size_t capacity, size_t* got) {
    if (got) *got = 0;
    if (!state_ || !mic_on_ || !dest) return ESP_ERR_INVALID_STATE;
    if (capacity < kHardwareFrames) return ESP_ERR_INVALID_SIZE;
    size_t bytes = 0;
    // Check the real DMA length: never report an incomplete read as a valid
    // frame, or rely on codec_dev's synthetic reconfiguration silence.
    const esp_err_t r = i2s_channel_read(state_->rx, state_->interleaved,
                                       sizeof state_->interleaved, &bytes, 1000);
    if (r != ESP_OK) return r;
    if (bytes != sizeof state_->interleaved) return ESP_ERR_INVALID_SIZE;
    for (size_t channel = 0; channel < 2; ++channel) {
        channels_[channel] = {};
        channels_[channel].Add(state_->interleaved + channel, kHardwareFrames, 2);
    }
    // Standard I2S left/right carry MIC1/MIC2. Use MIC1 for the mono uplink;
    // keep MIC2 visible in diagnostics. Analog echo-reference MIC3 is excluded.
    for (size_t i = 0; i < kHardwareFrames; ++i) dest[i] = state_->interleaved[i * 2];
    if (got) *got = kHardwareFrames;
    return ESP_OK;
}

esp_err_t AmoledAudio::WritePcm(const int16_t* data, size_t frames) {
    if (!state_ || !data) return ESP_ERR_INVALID_STATE;
    while (frames) {
        const size_t chunk = std::min(frames, kPlayFrames);
        for (size_t i = 0; i < chunk; ++i)
            state_->stereo[i * 2] = state_->stereo[i * 2 + 1] = data[i];
        size_t written = 0;
        const size_t bytes = chunk * 2 * sizeof(int16_t);
        const esp_err_t r = i2s_channel_write(state_->tx, state_->stereo, bytes, &written, 1000);
        if (r != ESP_OK) return r;
        if (written != bytes) return ESP_ERR_INVALID_SIZE;
        data += chunk; frames -= chunk;
    }
    return ESP_OK;
}

esp_err_t AmoledAudio::SetVolume(uint8_t volume) {
    return state_ && esp_codec_dev_set_out_vol(state_->output, std::min<int>(volume, 100)) == 0 ? ESP_OK : ESP_FAIL;
}
