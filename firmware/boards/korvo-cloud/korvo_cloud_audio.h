#pragma once
// KorvoCloudAudio: everything this board does with sound, behind one object.
//
//   platform -> device : PlayPcm() queues TTS; a task drains it into the ES8311. The SDK calls us
//                        from the transport's own task, which must never block on I2S.
//   device -> platform : StartAsr()/StopAsr()         - bulk mic audio on AGENT_STREAM_AUDIO
//                        StartCommand()/StopCommand() - one utterance on AGENT_STREAM_VOICE
//
// One mic, one reader task, and the two uplinks are not exclusive - a frame is read once and
// fanned out to whichever are running.
//
// Start/stop are requests, not actions: they set a desired-state flag that the mic task reconciles
// on its next pass. That keeps codec I2C/I2S work off the SDK's task and off the LVGL task, and
// makes a toggle idempotent - the last writer wins instead of two callers racing.

#include <atomic>
#include <cstdint>

#include "agent_link_stream.h"
#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "es_codec.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

class KorvoCloudAudio {
public:
    struct Config {
        i2c_port_t i2c_port;
        gpio_num_t pin_mclk, pin_bclk, pin_ws;
        gpio_num_t pin_din;        // ES7210 SDOUT -> ESP
        gpio_num_t pin_dout;       // ESP -> ES8311 DSDIN
        gpio_num_t pin_pa_en;
        uint8_t    es7210_addr, es8311_addr;
        int        sample_rate;
        int        mic_gain;
        int        out_volume;
        size_t     play_buf_bytes;  // TTS jitter buffer; PSRAM, falls back to a small internal one
    };

    esp_err_t Init(const Config& cfg);

    bool Ready() const { return codec_ok_; }

    // Downlink TTS (PCM16 16k mono). Non-blocking; drops with a warning if the buffer is full,
    // which only happens when the link outruns real time by more than the buffer holds.
    void PlayPcm(const uint8_t* pcm16, size_t bytes);

    // Bulk mic uplink. Needs the link READY.
    void StartAsr();
    void StopAsr();
    bool AsrActive() const { return asr_on_.load(std::memory_order_acquire); }

    // One utterance on the voice channel instead; the SDK opens the session lazily on the first
    // frame, so this needs nothing but a live link.
    void StartCommand();
    void StopCommand();
    bool CommandActive() const { return cmd_on_.load(std::memory_order_acquire); }

private:
    esp_err_t OpenAudioStream();
    static void PlayTaskEntry(void* arg);
    static void MicTaskEntry(void* arg);
    void PlayLoop();
    void MicLoop();

    Config  cfg_ = {};
    EsCodec codec_;
    bool    codec_ok_ = false;

    StreamBufferHandle_t play_buf_ = nullptr;

    // Open while the corresponding uplink is running; both may be open at once.
    agent_stream_handle_t audio_stream_ = nullptr;
    agent_stream_handle_t voice_stream_ = nullptr;

    // Desired state, set by any task; reconciled by the mic task.
    std::atomic<bool> want_asr_{false};
    std::atomic<bool> want_cmd_{false};
    // Actual state, owned by the mic task, published for the UI.
    std::atomic<bool> asr_on_{false};
    std::atomic<bool> cmd_on_{false};
};
