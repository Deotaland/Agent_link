// WavRecorder: PCM16 in, PCM or IMA-ADPCM .wav out. See wav_recorder.h.

#include "wav_recorder.h"

#include <cerrno>
#include <cstring>

#include "esp_log.h"

namespace {
constexpr const char* TAG = "wav";

// PCM: 12 RIFF + 8+16 fmt + 8 data.
// ADPCM: 12 RIFF + 8+20 fmt (cbSize=2 carries wSamplesPerBlock) + 8+4 fact + 8 data.
constexpr uint32_t kPcmHeaderBytes   = 44;
constexpr uint32_t kAdpcmHeaderBytes = 60;

void Put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);         p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);   p[3] = static_cast<uint8_t>(v >> 24);
}
void Put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);         p[1] = static_cast<uint8_t>(v >> 8);
}
}  // namespace

uint32_t WavRecorder::HeaderBytes(Format fmt) {
    return fmt == Format::kAdpcm ? kAdpcmHeaderBytes : kPcmHeaderBytes;
}

uint32_t WavRecorder::DurationMsFromSize(uint32_t file_bytes, uint32_t sample_rate, Format fmt) {
    if (!sample_rate) return 0;
    const uint32_t hdr = HeaderBytes(fmt);
    if (file_bytes <= hdr) return 0;
    const uint32_t data = file_bytes - hdr;

    uint64_t samples;
    if (fmt == Format::kAdpcm) {
        samples = static_cast<uint64_t>(data / adpcm_enc::kBlockBytes) * adpcm_enc::kSamplesPerBlock;
    } else {
        samples = data / 2u;
    }
    return static_cast<uint32_t>((samples * 1000ull) / sample_rate);
}

// Called twice: once with zeros at Start(), once with the real counts at Stop().
esp_err_t WavRecorder::WriteHeader() {
    uint8_t h[kAdpcmHeaderBytes] = {};
    size_t  n = 0;

    const bool adpcm = (fmt_ == Format::kAdpcm);
    const uint32_t hdr_bytes = HeaderBytes(fmt_);

    memcpy(h + 0, "RIFF", 4);
    Put32(h + 4, (hdr_bytes - 8) + data_bytes_);   // everything after this field
    memcpy(h + 8, "WAVE", 4);
    n = 12;

    memcpy(h + n, "fmt ", 4);                     n += 4;
    Put32(h + n, adpcm ? 20u : 16u);              n += 4;
    Put16(h + n, adpcm ? 0x0011 : 0x0001);        n += 2;   // IMA ADPCM / PCM
    Put16(h + n, 1);                              n += 2;   // mono
    Put32(h + n, sample_rate_);                   n += 4;
    if (adpcm) {
        // avg bytes/sec = rate * block_align / samples_per_block
        const uint32_t avg = static_cast<uint32_t>(
            (static_cast<uint64_t>(sample_rate_) * adpcm_enc::kBlockBytes) / adpcm_enc::kSamplesPerBlock);
        Put32(h + n, avg);                                        n += 4;
        Put16(h + n, static_cast<uint16_t>(adpcm_enc::kBlockBytes));      n += 2;  // nBlockAlign
        Put16(h + n, 4);                                          n += 2;  // bits per sample
        Put16(h + n, 2);                                          n += 2;  // cbSize
        Put16(h + n, static_cast<uint16_t>(adpcm_enc::kSamplesPerBlock)); n += 2;  // wSamplesPerBlock
        // fact: how many real samples, which the zero-padded tail of the last block would otherwise
        // overstate. Players use this to know where to stop.
        memcpy(h + n, "fact", 4);   n += 4;
        Put32(h + n, 4);            n += 4;
        Put32(h + n, samples_);     n += 4;
    } else {
        Put32(h + n, sample_rate_ * 2u);   n += 4;   // byte rate
        Put16(h + n, 2);                   n += 2;   // block align
        Put16(h + n, 16);                  n += 2;   // bits per sample
    }

    memcpy(h + n, "data", 4);   n += 4;
    Put32(h + n, data_bytes_);  n += 4;

    if (n != hdr_bytes) {   // the two must agree or every offset downstream is wrong
        ESP_LOGE(TAG, "header size mismatch: built %u, expected %u",
                 static_cast<unsigned>(n), static_cast<unsigned>(hdr_bytes));
        return ESP_FAIL;
    }
    if (fseek(f_, 0, SEEK_SET) != 0) return ESP_FAIL;
    if (fwrite(h, 1, n, f_) != n) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t WavRecorder::Start(const char* path, uint32_t sample_rate, Format fmt) {
    if (f_) Stop();
    if (!path || !*path) return ESP_ERR_INVALID_ARG;

    f_ = fopen(path, "wb");
    if (!f_) {
        ESP_LOGE(TAG, "cannot create %s (errno %d)", path, errno);
        return ESP_FAIL;
    }
    snprintf(path_, sizeof(path_), "%s", path);
    fmt_         = fmt;
    sample_rate_ = sample_rate;
    data_bytes_  = 0;
    samples_     = 0;
    enc_.Reset();

    if (WriteHeader() != ESP_OK) {          // placeholder, patched by Stop()
        fclose(f_); f_ = nullptr;
        ESP_LOGE(TAG, "header write failed on %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "recording to %s (%luHz, %s)", path_,
             static_cast<unsigned long>(sample_rate_), fmt == Format::kAdpcm ? "IMA-ADPCM" : "PCM16");
    return ESP_OK;
}

esp_err_t WavRecorder::Write(const void* pcm, size_t bytes) {
    if (!f_) return ESP_ERR_INVALID_STATE;
    if (!pcm || bytes < sizeof(int16_t)) return ESP_OK;

    const int16_t* s = static_cast<const int16_t*>(pcm);
    const size_t   n = bytes / sizeof(int16_t);
    samples_ += static_cast<uint32_t>(n);

    if (fmt_ == Format::kPcm16) {
        if (fwrite(pcm, 1, bytes, f_) != bytes) {
            ESP_LOGE(TAG, "short write — card full or removed");
            return ESP_FAIL;
        }
        data_bytes_ += static_cast<uint32_t>(bytes);
        return ESP_OK;
    }

    uint8_t block[adpcm_enc::kBlockBytes];
    for (size_t i = 0; i < n; ++i) {
        if (!enc_.Push(s[i], block)) continue;       // still filling this block
        if (fwrite(block, 1, sizeof(block), f_) != sizeof(block)) {
            ESP_LOGE(TAG, "short write — card full or removed");
            return ESP_FAIL;
        }
        data_bytes_ += static_cast<uint32_t>(sizeof(block));
    }
    return ESP_OK;
}

esp_err_t WavRecorder::Stop() {
    if (!f_) return ESP_OK;

    if (fmt_ == Format::kAdpcm) {
        uint8_t block[adpcm_enc::kBlockBytes];
        const size_t tail = enc_.Flush(block);       // don't drop the last partial block
        if (tail && fwrite(block, 1, tail, f_) == tail) data_bytes_ += static_cast<uint32_t>(tail);
    }

    const esp_err_t r = WriteHeader();               // seek back, patch the real counts
    fclose(f_);
    f_ = nullptr;
    ESP_LOGI(TAG, "saved %s: %lu bytes (%lu ms)%s", path_,
             static_cast<unsigned long>(data_bytes_), static_cast<unsigned long>(DurationMs()),
             r == ESP_OK ? "" : " — HEADER PATCH FAILED, file may not open");
    return r;
}

uint32_t WavRecorder::DurationMs() const {
    if (!sample_rate_) return 0;
    return static_cast<uint32_t>((static_cast<uint64_t>(samples_) * 1000ull) / sample_rate_);
}
