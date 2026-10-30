#pragma once

#include <atomic>
#include <mutex>

#include "agent_link.h"
#include "amoled_hardware.h"
#include "amoled_pcm_diagnostics.h"
#include "voice_playback.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

class AmoledVoiceAudio {
public:
    esp_err_t Init(AmoledHardware* hardware);
    bool Ready() const { return ready_; }
    bool Fault() const { return fault_.load(); }
    void ClearFault() { fault_.store(false); }
    bool Speaking() const { return queued_.load() || echo_.Muted(esp_timer_get_time()); }
    void Play(const uint8_t* pcm, size_t bytes);
    void End() { finished_.store(true); }
    void Reset() { generation_.fetch_add(1); }
    void SetContinuous(bool enabled) { continuous_.store(enabled); }
    void Listen(bool enabled, uint32_t max_ms);
    void Stop() { continuous_.store(false); asr_.store(false); Reset(); }
    void ReportDiagnostics();  // Controller worker only; no logging from capture.

private:
    static void PlayEntry(void* ctx) { static_cast<AmoledVoiceAudio*>(ctx)->PlayLoop(); }
    static void MicEntry(void* ctx) { static_cast<AmoledVoiceAudio*>(ctx)->MicLoop(); }
    void PlayLoop();
    void MicLoop();
    void BeginDiagnostics();
    void PublishDiagnostics(bool final);

    AmoledHardware* hardware_ = nullptr;
    StreamBufferHandle_t buffer_ = nullptr;
    TaskHandle_t play_task_ = nullptr;
    TaskHandle_t mic_task_ = nullptr;
    bool ready_ = false;
    std::mutex writer_;
    std::atomic<uint32_t> generation_{0}, cleared_{0};
    std::atomic<bool> finished_{false}, continuous_{false}, asr_{false};
    std::atomic<bool> fault_{false};
    std::atomic<size_t> queued_{0};
    std::atomic<int64_t> asr_until_{0};
    kc_audio::SpeakerEchoGuard echo_;
    // Capture owns the accumulator. Only the copied snapshot crosses tasks.
    amoled_voice::MicDiagnosticWindow mic_window_, diagnostic_snapshot_;
    portMUX_TYPE diagnostic_lock_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t diagnostic_sequence_ = 0, diagnostic_capture_ = 0;
    uint32_t diagnostic_reported_ = 0;  // Controller worker only.
    bool diagnostic_active_ = false;  // Microphone worker only.
};
