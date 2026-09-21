#pragma once
// WavRecorder: streams mic PCM16 to a .wav file, optionally compressing it 4:1 with IMA-ADPCM.
//
// ADPCM is the default here. It is the same format agent_link already decodes on the downlink
// (src/adpcm.h), any desktop player opens it, and a 10-minute recording drops from 19MB to 4.8MB.
//
// A WAV header carries the total length, which is unknown while still recording, so Start() writes
// a placeholder header and Stop() seeks back and patches in the real sizes. A recording cut short
// by a power loss therefore leaves a file whose header says zero length — the audio is on the card
// but most players will not open it. Stopping properly is what makes the file valid.

#include <cstdint>
#include <cstdio>

#include "adpcm_encoder.h"
#include "esp_err.h"

class WavRecorder {
public:
    enum class Format {
        kPcm16,   // fmt_tag 0x0001, 32KB/s at 16kHz mono
        kAdpcm,   // fmt_tag 0x0011, IMA-ADPCM in 256-byte blocks, ~8KB/s
    };

    WavRecorder() = default;
    ~WavRecorder() { Stop(); }
    WavRecorder(const WavRecorder&) = delete;
    WavRecorder& operator=(const WavRecorder&) = delete;

    // Create `path` and write a placeholder header. Mono only.
    esp_err_t Start(const char* path, uint32_t sample_rate, Format fmt = Format::kAdpcm);

    // Append PCM16 (always PCM16 in, whatever the file format is). Returns ESP_FAIL on a short
    // write (card full / pulled out), after which the caller should Stop() — what has been written
    // so far is still patched up and playable.
    esp_err_t Write(const void* pcm, size_t bytes);

    // Flush, patch the header with the real sizes, and close. Safe to call when not recording.
    esp_err_t Stop();

    bool        Active()     const { return f_ != nullptr; }
    uint32_t    Samples()    const { return samples_; }
    uint32_t    DurationMs() const;
    const char* Path()       const { return path_; }

    // Duration of an existing file on disk, worked out from its size. Used by the recordings
    // listing, which would otherwise have to open and parse every file.
    static uint32_t DurationMsFromSize(uint32_t file_bytes, uint32_t sample_rate, Format fmt);

    // Bytes of header this format writes, i.e. where the audio data starts.
    static uint32_t HeaderBytes(Format fmt);

private:
    esp_err_t WriteHeader();

    FILE*    f_           = nullptr;
    char     path_[128]   = {};
    Format   fmt_         = Format::kAdpcm;
    uint32_t sample_rate_ = 16000;
    uint32_t data_bytes_  = 0;   // bytes of audio actually written (post-compression)
    uint32_t samples_     = 0;   // PCM samples fed in — what the fact chunk reports

    adpcm_enc::Encoder enc_;
};
