// Codec + playback + mic fan-out. See korvo_cloud_audio.h.

#include "korvo_cloud_audio.h"

#include <cstring>

#include "agent_link.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "kc_audio";
constexpr size_t kPlayChunkSamples = 256;
constexpr size_t kMicBufSamples    = 640;   // 20ms at 32kHz; more than the 16kHz frame needs
}  // namespace

esp_err_t KorvoCloudAudio::Init(const Config& cfg) {
    cfg_ = cfg;

    EsCodecConfig cc = {};
    cc.i2c_port    = cfg_.i2c_port;
    cc.pin_sda     = GPIO_NUM_NC;   // attach to the bus the camera's SCCB already made on this port
    cc.pin_scl     = GPIO_NUM_NC;
    cc.pin_mclk    = cfg_.pin_mclk;
    cc.pin_bclk    = cfg_.pin_bclk;
    cc.pin_ws      = cfg_.pin_ws;
    cc.pin_din     = cfg_.pin_din;
    cc.pin_dout    = cfg_.pin_dout;
    cc.pin_pa_en   = cfg_.pin_pa_en;
    cc.es7210_addr = cfg_.es7210_addr;
    cc.es8311_addr = cfg_.es8311_addr;
    cc.sample_rate = cfg_.sample_rate;
    cc.mic_gain    = cfg_.mic_gain;
    cc.out_volume  = cfg_.out_volume;
    if (codec_.Init(cc) != ESP_OK) {
        ESP_LOGE(TAG, "codec init failed - no audio on this boot");
        return ESP_FAIL;
    }
    codec_ok_ = true;

    play_buf_ = xStreamBufferCreateWithCaps(cfg_.play_buf_bytes, /*trigger=*/1, MALLOC_CAP_SPIRAM);
    if (!play_buf_) {
        play_buf_ = xStreamBufferCreate(16 * 1024, /*trigger=*/1);
        if (!play_buf_) { ESP_LOGE(TAG, "play buffer alloc failed"); return ESP_ERR_NO_MEM; }
        ESP_LOGW(TAG, "no PSRAM - play buffer is 16KB (0.5s); long replies will stutter");
        agent_link_playback_set_buffer_ms(16 * 1024 / 32);
    } else {
        // Tell the SDK what we hold so it throttles the sender before this overflows.
        // 32 bytes per ms at PCM16/16kHz/mono.
        agent_link_playback_set_buffer_ms(static_cast<uint32_t>(cfg_.play_buf_bytes / 32));
    }

    xTaskCreate(&KorvoCloudAudio::PlayTaskEntry, "spk_play", 4096, this, 5, nullptr);
    xTaskCreate(&KorvoCloudAudio::MicTaskEntry,  "mic_up",   4096, this, 5, nullptr);
    ESP_LOGI(TAG, "audio ready (%d Hz, play buffer %uKB)", cfg_.sample_rate,
             static_cast<unsigned>(cfg_.play_buf_bytes / 1024));
    return ESP_OK;
}

void KorvoCloudAudio::PlayPcm(const uint8_t* pcm16, size_t bytes) {
    if (!play_buf_ || !pcm16 || bytes == 0) return;
    const size_t sent = xStreamBufferSend(play_buf_, pcm16, bytes, 0);   // 0 = never block the caller
    if (sent < bytes) {
        ESP_LOGW(TAG, "play buffer full - dropped %u/%u bytes",
                 static_cast<unsigned>(bytes - sent), static_cast<unsigned>(bytes));
    }
}

void KorvoCloudAudio::PlayTaskEntry(void* arg) { static_cast<KorvoCloudAudio*>(arg)->PlayLoop(); }

void KorvoCloudAudio::PlayLoop() {
    int16_t buf[kPlayChunkSamples];
    while (true) {
        const size_t n = xStreamBufferReceive(play_buf_, buf, sizeof(buf), portMAX_DELAY);
        if (n >= sizeof(int16_t)) (void)codec_.WritePcm(buf, n / sizeof(int16_t));
    }
}

// The label says where the audio came from, not what to do with it.
esp_err_t KorvoCloudAudio::OpenAudioStream() {
    agent_stream_opts_t o = {};
    o.name = "mic";
    return agent_link_stream_open(AGENT_STREAM_AUDIO, &o, &audio_stream_);
}

void KorvoCloudAudio::StartAsr() { want_asr_.store(true,  std::memory_order_release); }
void KorvoCloudAudio::StopAsr()  { want_asr_.store(false, std::memory_order_release); }

void KorvoCloudAudio::StartCommand() { want_cmd_.store(true,  std::memory_order_release); }
void KorvoCloudAudio::StopCommand()  { want_cmd_.store(false, std::memory_order_release); }

void KorvoCloudAudio::MicTaskEntry(void* arg) { static_cast<KorvoCloudAudio*>(arg)->MicLoop(); }

void KorvoCloudAudio::MicLoop() {
    const size_t want_frame = static_cast<size_t>(cfg_.sample_rate) / 50;   // 20ms
    const size_t frame      = want_frame > kMicBufSamples ? kMicBufSamples : want_frame;
    int16_t buf[kMicBufSamples];

    while (true) {
        const bool want_asr = want_asr_.load(std::memory_order_acquire);
        const bool want_cmd = want_cmd_.load(std::memory_order_acquire);
        bool asr_on = asr_on_.load(std::memory_order_acquire);
        bool cmd_on = cmd_on_.load(std::memory_order_acquire);

        // Reconcile desired vs actual. Stops run first, so a stop+start in one pass cannot overlap.
        if (asr_on && !want_asr) {
            agent_link_stream_close(audio_stream_, true);
            asr_on = false;
            asr_on_.store(false, std::memory_order_release);
            ESP_LOGI(TAG, "ASR stream ended");
        }
        if (cmd_on && !want_cmd) {
            agent_link_stream_close(voice_stream_, true);
            cmd_on = false;
            cmd_on_.store(false, std::memory_order_release);
            ESP_LOGI(TAG, "voice command sent");
        }
        if (!asr_on && want_asr) {
            if (agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "asr requested but the link is not ready");
                want_asr_.store(false, std::memory_order_release);
            } else if (OpenAudioStream() != ESP_OK) {
                ESP_LOGW(TAG, "audio stream failed to open");
                want_asr_.store(false, std::memory_order_release);
            } else {
                asr_on = true;
                asr_on_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "ASR stream started");
            }
        }
        if (!cmd_on && want_cmd) {
            if (agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "voice command requested but the link is not ready");
                want_cmd_.store(false, std::memory_order_release);
            } else if (agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &voice_stream_) != ESP_OK) {
                ESP_LOGW(TAG, "voice stream failed to open");
                want_cmd_.store(false, std::memory_order_release);
            } else {
                cmd_on = true;
                cmd_on_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "voice stream started");
            }
        }

        const bool need_mic = asr_on || cmd_on;
        if (need_mic && !codec_.MicOn()) {
            if (codec_.StartMic() != ESP_OK) {
                ESP_LOGE(TAG, "mic start failed");
                want_asr_.store(false, std::memory_order_release);
                want_cmd_.store(false, std::memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        } else if (!need_mic && codec_.MicOn()) {
            (void)codec_.StopMic();
        }

        if (!need_mic) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        // One read, fanned out to whichever uplinks are live.
        size_t got = 0;
        if (codec_.ReadPcm(buf, frame, &got) != ESP_OK || got == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));   // read failed: don't spin
            continue;
        }
        const size_t bytes = got * sizeof(int16_t);

        if (asr_on) {
            if (agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "link dropped mid-stream, ending ASR");
                agent_link_stream_close(audio_stream_, false);
                asr_on_.store(false, std::memory_order_release);
                want_asr_.store(false, std::memory_order_release);
            } else if (agent_link_stream_write(audio_stream_, buf, bytes) == ESP_ERR_NO_MEM) {
                // Backpressure truncated us: end this segment rather than send a torn stream.
                ESP_LOGW(TAG, "ASR truncated (link can't keep up), stopping");
                agent_link_stream_close(audio_stream_, false);
                asr_on_.store(false, std::memory_order_release);
                want_asr_.store(false, std::memory_order_release);
            }
        }
        if (cmd_on) {
            if (agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "link dropped mid-command, closing the voice session");
                agent_link_stream_close(voice_stream_, false);
                cmd_on_.store(false, std::memory_order_release);
                want_cmd_.store(false, std::memory_order_release);
            } else {
                // ESP_ERR_NO_MEM here means the SDK's voice queue hit its cap and dropped this
                // frame to keep the head of the utterance. It logs that itself and the session
                // stays open on purpose, so - unlike the ASR path - do NOT tear the stream down.
                (void)agent_link_stream_write(voice_stream_, buf, bytes);
            }
        }
    }
}
