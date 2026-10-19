#pragma once

#include <atomic>
#include <mutex>

#include "agent_link.h"
#include "paper_audio.h"
#include "voice_playback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

class PaperWifiAudio {
public:
    esp_err_t Init();
    bool Ready() const { return ready_; }
    void Play(const uint8_t* pcm, size_t bytes);
    void End() { finished_.store(true); }
    void Reset() { generation_.fetch_add(1); }
    void SetContinuous(bool enabled) { continuous_.store(enabled); }
    void Listen(bool enabled, uint32_t max_ms);
    void Stop() { continuous_.store(false); asr_.store(false); Reset(); }

private:
    static void PlayEntry(void* ctx) { static_cast<PaperWifiAudio*>(ctx)->PlayLoop(); }
    static void MicEntry(void* ctx) { static_cast<PaperWifiAudio*>(ctx)->MicLoop(); }
    void PlayLoop();
    void MicLoop();

    StreamBufferHandle_t buffer_ = nullptr;
    TaskHandle_t play_task_ = nullptr;
    TaskHandle_t mic_task_ = nullptr;
    bool ready_ = false;
    std::mutex writer_;
    std::atomic<uint32_t> generation_{0}, cleared_{0};
    std::atomic<bool> finished_{false}, continuous_{false}, asr_{false};
    std::atomic<int64_t> asr_until_{0};
    kc_audio::SpeakerEchoGuard echo_;
};
