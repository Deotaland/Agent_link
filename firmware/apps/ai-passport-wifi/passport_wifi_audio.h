#pragma once

#include <atomic>
#include <mutex>
#include "ai_passport_hardware.h"
#include "agent_link.h"
#include "voice_playback.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

class PassportWifiAudio {
public:
    esp_err_t Init(AiPassportHardware* hardware);
    bool Ready() const { return ready_; }
    // User/Agent intent, retained while the transport temporarily pauses capture.
    bool Enabled() const { return enabled_.load(); }
    void Play(const uint8_t* pcm, size_t bytes);
    void End() { finished_.store(true); }
    void Reset() { generation_.fetch_add(1); }
    void Enable(bool enabled) { enabled_.store(enabled); }
    void Stop() { enabled_.store(false); Reset(); }
    bool Speaking() const { return echo_.Muted(esp_timer_get_time()); }
private:
    static size_t BufferLevel(void* context);
    static void PlayEntry(void* ctx) { static_cast<PassportWifiAudio*>(ctx)->PlayLoop(); }
    static void MicEntry(void* ctx) { static_cast<PassportWifiAudio*>(ctx)->MicLoop(); }
    void PlayLoop();
    void MicLoop();
    static constexpr size_t kBufferBytes = 8192;
    AiPassportHardware* hardware_ = nullptr;
    StreamBufferHandle_t buffer_ = nullptr;
    TaskHandle_t play_task_ = nullptr, mic_task_ = nullptr;
    bool ready_ = false;
    std::mutex writer_;
    std::atomic<uint32_t> generation_{0}, cleared_{0};
    std::atomic<bool> finished_{false}, enabled_{false};
    kc_audio::SpeakerEchoGuard echo_;
};
