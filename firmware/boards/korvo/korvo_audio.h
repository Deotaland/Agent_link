#pragma once
// KorvoAudio: everything the Korvo does with sound, behind one object.
//
//   Agent -> device : PlayPcm() queues TTS; a task drains it into the ES8311 (the SDK calls us from
//                     the BLE host task, which must never block on I2S).
//   device -> Agent : StartAsr()/StopAsr()         — the L2CAP ASR channel (PSM 0x0081, 0x52/0x53),
//                                                    which the App transcribes live.
//                     StartCommand()/StopCommand() — the GATT voice channel (0x40 VoiceChunk on
//                                                    Notify 0xFFA1), where the App takes the audio
//                                                    and decides what to do with it itself.
//   device -> card  : StartRecording()/StopRecording() write the same mic to a WAV on the TF card.
//
// One mic, one reader task: the three uplinks are NOT exclusive. A frame is read once and fanned
// out to whichever are running, so recording a conversation while the App transcribes it costs
// nothing extra.
//
// Start/stop are requests, not actions: they set a desired-state flag that the mic task reconciles
// on its next pass. That keeps codec I2C/I2S work off the BLE host task and off the LVGL task, and
// makes a toggle idempotent — the last writer wins instead of two callers racing.

#include <atomic>
#include <cstdint>

#include "driver/gpio.h"
#include "agent_link_stream.h"
#include "driver/i2c_types.h"
#include "es_codec.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "sd_card.h"
#include "wav_recorder.h"

class KorvoAudio {
public:
    struct Config {
        // Codec. i2c_port must already have a bus on it — EsCodec attaches rather than creating one
        // (the camera's SCCB owns GPIO17/18 here).
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

        // TF card (1-line SDMMC). Recording is simply unavailable if no card is present.
        gpio_num_t  sd_clk, sd_cmd, sd_d0;
        const char* sd_mount;       // e.g. "/sdcard"
        const char* rec_dir;        // e.g. "/sdcard/records" — created if missing
        const char* msg_dir;        // e.g. "/sdcard/records/messages" — created if missing
        WavRecorder::Format rec_format;
    };

    esp_err_t Init(const Config& cfg);

    bool Ready()   const { return codec_ok_; }
    bool SdReady() const { return sd_.Ready(); }
    uint64_t SdFreeMb() const { return sd_.FreeMb(); }

    // Agent -> device TTS (PCM16 16k mono). Non-blocking; drops with a warning if the buffer is
    // full, which only happens when the link outruns real time by more than the buffer holds.
    void PlayPcm(const uint8_t* pcm16, size_t bytes);

    // Uplink to the App's ASR channel. Needs the link READY and the App's L2CAP CoC open.
    void StartAsr();
    void StopAsr();
    bool AsrActive() const { return asr_on_.load(std::memory_order_acquire); }

    // Uplink over AGENT_STREAM_VOICE instead: the SDK slices PCM into 0x40 VoiceChunk
    // notifications (session + sequence + <=205B of PCM each) and the App relays the speech on to
    // the Agent. No L2CAP channel involved, so this works as soon as the link is READY.
    void StartCommand();
    void StopCommand();
    bool CommandActive() const { return cmd_on_.load(std::memory_order_acquire); }

    // Uplink to a WAV file on the card.
    void StartRecording();
    void StopRecording();
    bool     RecordingActive() const { return rec_on_.load(std::memory_order_acquire); }
    uint32_t RecordedMs()      const { return rec_ms_.load(std::memory_order_acquire); }
    // Path of the recording in progress, or the last one finished. Empty before the first.
    const char* LastFile() const { return last_file_; }

private:
    esp_err_t OpenAudioStream();
    static void PlayTaskEntry(void* arg);
    static void MicTaskEntry(void* arg);
    void PlayLoop();
    void MicLoop();
    bool OpenRecordingFile();

    Config       cfg_ = {};
    EsCodec      codec_;
    SdCard       sd_;
    WavRecorder  wav_;
    bool         codec_ok_ = false;

    StreamBufferHandle_t play_buf_ = nullptr;

    // Open while the corresponding uplink is running; both may be open at once.
    agent_stream_handle_t audio_stream_ = nullptr;   // bulk mic audio for the App
    agent_stream_handle_t voice_stream_ = nullptr;   // speech relayed on to the Agent

    // Desired state, set by any task; reconciled by the mic task.
    std::atomic<bool> want_asr_{false};
    std::atomic<bool> want_cmd_{false};
    std::atomic<bool> want_rec_{false};
    // Actual state, owned by the mic task, published for the UI.
    std::atomic<bool>     asr_on_{false};
    std::atomic<bool>     cmd_on_{false};
    std::atomic<bool>     rec_on_{false};
    std::atomic<uint32_t> rec_ms_{0};

    char last_file_[128] = {};
    uint32_t rec_seq_ = 0;
};
