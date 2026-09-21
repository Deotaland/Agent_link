#pragma once
// WavRecorder: streams PCM16 to a .wav file on a mounted filesystem.
//
// A WAV header carries the total length, which is unknown while still recording, so Start() writes
// a placeholder header and Stop() seeks back and patches in the real sizes. That means a recording
// cut short by a power loss leaves a file whose header says zero length -- the audio is on the card
// but most players will not open it. Stopping properly is what makes the file valid.

#include <cstdint>
#include <cstdio>

#include "esp_err.h"

class WavRecorder {
public:
    WavRecorder() = default;
    ~WavRecorder() { Stop(); }
    WavRecorder(const WavRecorder&) = delete;
    WavRecorder& operator=(const WavRecorder&) = delete;

    // Create `path` and write a placeholder header. 16-bit samples only.
    esp_err_t Start(const char* path, uint32_t sample_rate, uint16_t channels = 1);

    // Append PCM16. Returns ESP_FAIL on a short write (card full / pulled out), after which the
    // caller should Stop() -- what has been written so far is still patched up and playable.
    esp_err_t Write(const void* pcm, size_t bytes);

    // Patch the header with the real sizes and close. Safe to call when not recording.
    esp_err_t Stop();

    bool        Active()     const { return f_ != nullptr; }
    uint32_t    DataBytes()  const { return data_bytes_; }
    uint32_t    DurationMs() const;
    const char* Path()       const { return path_; }

private:
    esp_err_t WriteHeader(uint32_t data_bytes);

    FILE*    f_           = nullptr;
    char     path_[128]   = {};
    uint32_t sample_rate_ = 16000;
    uint16_t channels_    = 1;
    uint32_t data_bytes_  = 0;
};
