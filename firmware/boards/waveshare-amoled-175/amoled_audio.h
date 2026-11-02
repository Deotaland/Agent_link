#pragma once

#include <array>
#include "es_codec.h"
#include "pcm_levels.h"

// Non-C board: reuse the shared ES8311 STD TX / ES7210 four-slot TDM RX driver.
// Only MIC1 (serialized slot 0) is delivered to the SDK as 16 kHz PCM16 mono.
class AmoledAudio {
public:
    static constexpr size_t kCaptureCapacity = 320;
    esp_err_t Init(int volume);
    esp_err_t StartMic() { return codec_.StartMic(); }
    esp_err_t StopMic() { return codec_.StopMic(); }
    bool MicOn() const { return codec_.MicOn(); }
    esp_err_t ReadPcm(int16_t* dest, size_t capacity, size_t* got);
    esp_err_t WritePcm(const int16_t* data, size_t frames) { return codec_.WritePcm(data, frames); }
    esp_err_t SetVolume(uint8_t volume) { return codec_.SetVolume(volume); }
    const std::array<kc_audio::PcmLevels, 2>& ChannelLevels() const { return channels_; }
private:
    EsCodec codec_;
    std::array<kc_audio::PcmLevels, 2> channels_{};
};
