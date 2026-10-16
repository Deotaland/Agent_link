#pragma once
// KorvoCloudAudio: everything this board does with sound, behind one object.
//
//   platform -> device : PlayPcm() queues TTS; a task drains it into the ES8311. The SDK calls us
//                        from the transport's own task, which must never block on I2S.
//   device -> platform : StartAsr()/StopAsr()         - bulk mic audio on AGENT_STREAM_AUDIO
//                        StartCommand()/StopCommand() - one utterance on AGENT_STREAM_VOICE
//                        StartContinuous()/StopContinuous() - open mic for a realtime call
//
// One mic, one reader task, and the two uplinks are not exclusive - a frame is read once and
// fanned out to whichever are running.
//
// Start/stop are requests, not actions: they set a desired-state flag that the mic task reconciles
// on its next pass. That keeps codec I2C/I2S work off the SDK's task and off the LVGL task, and
// makes a toggle idempotent - the last writer wins instead of two callers racing.

#include <atomic>
#include <cstdint>
#include <mutex>

#include "agent_link_stream.h"
#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "es_codec.h"
#include "voice_playback.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

class KorvoCloudAudio {
public:
    struct Config {
        EsCodec* codec;  // initialized and owned by KorvoHardware
        int sample_rate;
        size_t     play_buf_bytes;  // TTS jitter buffer; PSRAM, falls back to a small internal one
    };

    esp_err_t Init(const Config& cfg);

    bool Ready() const { return codec_ok_; }

    // Downlink TTS (PCM16 16k mono). Non-blocking; drops with a warning if the buffer is full,
    // which only happens when the link outruns real time by more than the buffer holds.
    void PlayPcm(const uint8_t* pcm16, size_t bytes);
    void EndPlayback();
    void StopPlayback();

    // Bulk mic uplink. Needs the link READY.
    void StartAsr();
    void StopAsr();
    bool AsrActive() const { return asr_on_.load(std::memory_order_acquire); }

    // One utterance on the voice channel instead; the SDK opens the session lazily on the first
    // frame, so this needs nothing but a live link.
    void StartCommand();
    void StopCommand();
    void StartContinuous();
    void StopContinuous();
    bool CommandActive() const { return cmd_on_.load(std::memory_order_acquire); }

private:
    esp_err_t OpenAudioStream();
    static void PlayTaskEntry(void* arg);
    static void MicTaskEntry(void* arg);
    void PlayLoop();
    void MicLoop();

    Config  cfg_ = {};
    EsCodec* codec_ = nullptr;
    bool    codec_ok_ = false;

    StreamBufferHandle_t play_buf_ = nullptr;
    std::mutex play_mutex_;
    std::atomic<uint32_t> play_generation_{0};
    std::atomic<uint32_t> play_cleared_{0};
    std::atomic<bool> play_finished_{false};
    std::atomic<size_t> play_dropped_{0};
    std::atomic<unsigned> play_ends_{0};
    kc_audio::SpeakerEchoGuard echo_guard_;

    // Open while the corresponding uplink is running; both may be open at once.
    agent_stream_handle_t audio_stream_ = nullptr;
    agent_stream_handle_t voice_stream_ = nullptr;

    // Desired state, set by any task; reconciled by the mic task.
    std::atomic<bool> want_asr_{false};
    std::atomic<bool> want_cmd_{false};
    std::atomic<bool> want_continuous_{false};
    // Actual state, owned by the mic task, published for the UI.
    std::atomic<bool> asr_on_{false};
    std::atomic<bool> cmd_on_{false};
};
