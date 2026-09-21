// WavRecorder: PCM16 -> .wav on disk. See wav_recorder.h.

#include "wav_recorder.h"

#include <cerrno>
#include <cstring>

#include "esp_log.h"

namespace {
constexpr const char* TAG = "wav";
constexpr uint16_t kBitsPerSample = 16;
constexpr size_t   kHeaderBytes   = 44;

void Put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);         p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);   p[3] = static_cast<uint8_t>(v >> 24);
}
void Put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);         p[1] = static_cast<uint8_t>(v >> 8);
}
}  // namespace

// Canonical 44-byte RIFF/WAVE PCM header. Called twice: once with 0 at Start(), once with the
// real length at Stop().
esp_err_t WavRecorder::WriteHeader(uint32_t data_bytes) {
    const uint32_t byte_rate   = sample_rate_ * channels_ * (kBitsPerSample / 8);
    const uint16_t block_align = static_cast<uint16_t>(channels_ * (kBitsPerSample / 8));

    uint8_t h[kHeaderBytes] = {};
    memcpy(h + 0,  "RIFF", 4);
    Put32(h + 4,  36 + data_bytes);        // RIFF chunk size = everything after this field
    memcpy(h + 8,  "WAVEfmt ", 8);
    Put32(h + 16, 16);                     // fmt chunk size (PCM)
    Put16(h + 20, 1);                      // audio format: 1 = PCM
    Put16(h + 22, channels_);
    Put32(h + 24, sample_rate_);
    Put32(h + 28, byte_rate);
    Put16(h + 32, block_align);
    Put16(h + 34, kBitsPerSample);
    memcpy(h + 36, "data", 4);
    Put32(h + 40, data_bytes);

    if (fseek(f_, 0, SEEK_SET) != 0) return ESP_FAIL;
    if (fwrite(h, 1, sizeof(h), f_) != sizeof(h)) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t WavRecorder::Start(const char* path, uint32_t sample_rate, uint16_t channels) {
    if (f_) Stop();
    if (!path || !*path) return ESP_ERR_INVALID_ARG;

    f_ = fopen(path, "wb");
    if (!f_) {
        ESP_LOGE(TAG, "cannot create %s (errno %d)", path, errno);
        return ESP_FAIL;
    }
    snprintf(path_, sizeof(path_), "%s", path);
    sample_rate_ = sample_rate;
    channels_    = channels;
    data_bytes_  = 0;

    if (WriteHeader(0) != ESP_OK) {         // placeholder, patched by Stop()
        fclose(f_); f_ = nullptr;
        ESP_LOGE(TAG, "header write failed on %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "recording to %s (%luHz, %u ch)", path_,
             static_cast<unsigned long>(sample_rate_), channels_);
    return ESP_OK;
}

esp_err_t WavRecorder::Write(const void* pcm, size_t bytes) {
    if (!f_) return ESP_ERR_INVALID_STATE;
    if (!pcm || bytes == 0) return ESP_OK;
    if (fwrite(pcm, 1, bytes, f_) != bytes) {
        ESP_LOGE(TAG, "short write — card full or removed");
        return ESP_FAIL;
    }
    data_bytes_ += static_cast<uint32_t>(bytes);
    return ESP_OK;
}

esp_err_t WavRecorder::Stop() {
    if (!f_) return ESP_OK;
    const esp_err_t r = WriteHeader(data_bytes_);   // seek back and patch the real sizes
    fclose(f_);
    f_ = nullptr;
    ESP_LOGI(TAG, "saved %s: %lu bytes (%lu ms)%s", path_,
             static_cast<unsigned long>(data_bytes_), static_cast<unsigned long>(DurationMs()),
             r == ESP_OK ? "" : " — HEADER PATCH FAILED, file may not open");
    return r;
}

uint32_t WavRecorder::DurationMs() const {
    const uint32_t bytes_per_sec = sample_rate_ * channels_ * (kBitsPerSample / 8);
    if (!bytes_per_sec) return 0;
    return static_cast<uint32_t>((static_cast<uint64_t>(data_bytes_) * 1000ull) / bytes_per_sec);
}
