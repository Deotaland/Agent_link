#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "pcm_levels.h"

// C-board: standard I2S TX/RX, 16 kHz PCM16 stereo on the wires.
// The Agent Link application boundary is 16 kHz PCM16 mono in both directions.
class AmoledAudio {
public:
    static constexpr size_t kCaptureCapacity = 320;
    static constexpr size_t kHardwareFrames = 320;  // 20 ms at 16 kHz
    esp_err_t Init(int volume);
    esp_err_t StartMic();
    esp_err_t StopMic();
    bool MicOn() const { return mic_on_; }
    esp_err_t ReadPcm(int16_t* dest, size_t capacity, size_t* got);
    esp_err_t WritePcm(const int16_t* data, size_t frames);
    esp_err_t SetVolume(uint8_t volume);
    const std::array<kc_audio::PcmLevels, 2>& ChannelLevels() const { return channels_; }
private:
    struct State;
    State* state_ = nullptr;
    bool mic_on_ = false;
    std::array<kc_audio::PcmLevels, 2> channels_;
};
