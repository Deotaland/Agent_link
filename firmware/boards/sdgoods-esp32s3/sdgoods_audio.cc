#include "sdgoods_audio.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace sdgoods {
namespace {
constexpr size_t kRate = 16000, kRecordingFrames = kRate * 3, kBlock = 128;
constexpr const char* kTag = "button.audio";
}  // namespace
void Audio::Amplifier(bool enabled) {
    gpio_set_level(AUDIO_PA_PIN, enabled && !stop_.load() && volume_.load() > 0 ? 1 : 0);
}
void Audio::SetVolume(int percent) {
    volume_.store(std::clamp(percent, 0, 100));
    if (!volume_.load())
        Amplifier(false);
}
void Audio::Stop() {
    stop_.store(true);
    Amplifier(false);
}
bool Audio::StopAndWait() {
    Stop();
    for (unsigned n = 0; n < 100 && Busy(); ++n)
        vTaskDelay(pdMS_TO_TICKS(10));
    return !Busy();
}
esp_err_t Audio::Start(Test test) {
    bool idle = false;
    if (!busy_.compare_exchange_strong(idle, true))
        return ESP_ERR_INVALID_STATE;
    stop_.store(false);
    test_.store(test);
    error_.store(ESP_OK);
    stage_.store(Stage::kPreparing);
    if (!worker_ && xTaskCreate(Entry, "button_audio", 8192, this, 3, &worker_) != pdPASS) {
        error_.store(ESP_ERR_NO_MEM);
        stage_.store(Stage::kFailed);
        busy_.store(false);
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(worker_);
    return ESP_OK;
}
esp_err_t Audio::InitSpeaker() {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 240;
    esp_err_t error = i2s_new_channel(&channel, &tx_, nullptr);
    if (error != ESP_OK)
        return error;
    i2s_std_config_t standard{};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    standard.slot_cfg =
        I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    standard.gpio_cfg.mclk = AUDIO_SPK_MCLK;
    standard.gpio_cfg.bclk = AUDIO_SPK_BCLK;
    standard.gpio_cfg.ws = AUDIO_SPK_WS;
    standard.gpio_cfg.dout = AUDIO_SPK_DOUT;
    standard.gpio_cfg.din = GPIO_NUM_NC;
    error = i2s_channel_init_std_mode(tx_, &standard);
    if (error == ESP_OK)
        error = i2s_channel_enable(tx_);
    return error;
}
esp_err_t Audio::InitMicrophone() {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 256;
    esp_err_t error = i2s_new_channel(&channel, nullptr, &rx_);
    if (error != ESP_OK)
        return error;
    i2s_std_config_t standard{};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
    standard.slot_cfg =
        I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    standard.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    standard.gpio_cfg.mclk = GPIO_NUM_NC;
    standard.gpio_cfg.bclk = AUDIO_MIC_BCLK;
    standard.gpio_cfg.ws = AUDIO_MIC_WS;
    standard.gpio_cfg.dout = GPIO_NUM_NC;
    standard.gpio_cfg.din = AUDIO_MIC_DIN;
    error = i2s_channel_init_std_mode(rx_, &standard);
    if (error == ESP_OK)
        error = i2s_channel_enable(rx_);
    return error;
}
esp_err_t Audio::Capture(int16_t* pcm, size_t frames) {
    esp_err_t error = InitMicrophone();
    if (error != ESP_OK)
        return error;
    int32_t raw[kBlock]{};
    size_t offset = 0, warmup = kRate / 5;
    int32_t previous = 0, filtered = 0;
    while (!stop_.load() && offset < frames) {
        size_t bytes = 0;
        error = i2s_channel_read(rx_, raw, sizeof raw, &bytes, 100);
        if (error != ESP_OK || !bytes || bytes % sizeof(raw[0]))
            return error == ESP_OK ? ESP_FAIL : error;
        if (warmup) {
            warmup -= std::min(warmup, bytes / sizeof(raw[0]));
            continue;
        }
        stage_.store(Stage::kRecording);
        for (size_t i = 0; i < bytes / sizeof(raw[0]) && offset < frames; ++i) {
            const int32_t input = std::clamp<int32_t>(raw[i] >> 14, -32768, 32767);
            filtered = static_cast<int32_t>((int64_t(filtered + input - previous) * 32604) / 32768);
            previous = input;
            pcm[offset++] = static_cast<int16_t>(std::clamp<int32_t>(filtered, -32768, 32767));
        }
    }
    return ESP_OK;
}
esp_err_t Audio::Write(const int16_t* stereo, size_t frames) {
    size_t offset = 0;
    const size_t size = frames * sizeof(int16_t) * 2;
    while (offset < size && !stop_.load()) {
        size_t written = 0;
        const auto error = i2s_channel_write(tx_, reinterpret_cast<const uint8_t*>(stereo) + offset,
                                             size - offset, &written, 100);
        if (error != ESP_OK || !written || written > size - offset)
            return error == ESP_OK ? ESP_FAIL : error;
        offset += written;
    }
    return ESP_OK;
}
esp_err_t Audio::Play(const int16_t* pcm, size_t frames) {
    auto error = InitSpeaker();
    if (error != ESP_OK)
        return error;
    int16_t stereo[kBlock * 2]{};
    // Fill the DAC with zero before enabling the amplifier.
    for (unsigned n = 0; n < 12 && error == ESP_OK && !stop_.load(); ++n)
        error = Write(stereo, kBlock);
    if (error != ESP_OK || stop_.load())
        return error;
    stage_.store(Stage::kPlaying);
    Amplifier(true);
    for (size_t offset = 0; offset < frames && error == ESP_OK && !stop_.load(); offset += kBlock) {
        const size_t count = std::min(kBlock, frames - offset);
        const int volume = volume_.load();
        Amplifier(volume > 0);
        for (size_t n = 0; n < count; ++n) {
            const size_t position = offset + n;
            const int sample =
                pcm ? pcm[position] : int(6000 * std::sin(position * 6.28318530718 * 660 / kRate));
            const size_t fade = std::min({position, frames - position - 1, size_t(320)});
            stereo[n * 2] = stereo[n * 2 + 1] =
                static_cast<int16_t>(int64_t(sample) * volume * static_cast<int64_t>(fade) / 32000);
        }
        error = Write(stereo, count);
    }
    std::fill(std::begin(stereo), std::end(stereo), 0);
    for (unsigned n = 0; n < 13 && error == ESP_OK && !stop_.load(); ++n)
        error = Write(stereo, kBlock);
    return error;
}
void Audio::Release() {
    Amplifier(false);
    if (rx_) {
        (void)i2s_channel_disable(rx_);
        (void)i2s_del_channel(rx_);
        rx_ = nullptr;
    }
    if (tx_) {
        (void)i2s_channel_disable(tx_);
        (void)i2s_del_channel(tx_);
        tx_ = nullptr;
    }
}
void Audio::Entry(void* context) {
    static_cast<Audio*>(context)->Run();
}
void Audio::Run() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int16_t* pcm = nullptr;
        esp_err_t error = ESP_OK;
        if (test_.load() == Test::kRecordPlayback) {
            pcm = static_cast<int16_t*>(heap_caps_malloc(kRecordingFrames * sizeof(int16_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            error = pcm ? Capture(pcm, kRecordingFrames) : ESP_ERR_NO_MEM;
        }
        if (error == ESP_OK && !stop_.load())
            error = Play(pcm, pcm ? kRecordingFrames : kRate);
        Release();
        heap_caps_free(pcm);
        error_.store(error);
        stage_.store(error != ESP_OK ? Stage::kFailed
                     : stop_.load()  ? Stage::kStopped
                                     : Stage::kDone);
        if (error != ESP_OK)
            ESP_LOGW(kTag, "Audio self-test failed: %s", esp_err_to_name(error));
        busy_.store(false);
    }
}
}  // namespace sdgoods
