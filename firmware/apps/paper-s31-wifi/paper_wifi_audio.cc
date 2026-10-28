#include "paper_wifi_audio.h"

#include <cstring>
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"

namespace { constexpr const char* TAG = "Paper.audio"; }

esp_err_t PaperWifiAudio::Init() {
    if (paper_audio_init() != ESP_OK) return ESP_FAIL;
    buffer_ = xStreamBufferCreateWithCaps(AUDIO_PLAY_BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!buffer_) return ESP_ERR_NO_MEM;
    if (xTaskCreate(PlayEntry, "paper_wifi_play", 6144, this, 5, &play_task_) != pdPASS ||
        xTaskCreate(MicEntry, "paper_wifi_mic", 6144, this, 5, &mic_task_) != pdPASS) {
        if (play_task_) vTaskDelete(play_task_);
        vStreamBufferDeleteWithCaps(buffer_); buffer_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    agent_link_playback_set_buffer_ms(AUDIO_PLAY_BUF_BYTES / 32);
    ready_ = true;
    return ESP_OK;
}

void PaperWifiAudio::Play(const uint8_t* pcm, size_t bytes) {
    if (!ready_ || !pcm || !bytes) return;
    std::lock_guard<std::mutex> guard(writer_);
    if (generation_.load() != cleared_.load()) return;
    finished_.store(false);
    const size_t sent = xStreamBufferSend(buffer_, pcm, bytes, 0);
    if (sent < bytes) ESP_LOGW(TAG, "speaker buffer full, dropped %u bytes", static_cast<unsigned>(bytes - sent));
}

void PaperWifiAudio::Listen(bool enabled, uint32_t max_ms) {
    asr_until_.store(enabled && max_ms ? esp_timer_get_time() + static_cast<int64_t>(max_ms) * 1000 : 0);
    asr_.store(enabled);
}

bool PaperWifiAudio::PlayLocal(const uint8_t* pcm, size_t bytes,
                              const std::atomic<uint32_t>& generation, uint32_t expected) {
    if (!ready_) return false;
    size_t offset = 0;
    while (offset < bytes && generation.load() == expected) {
        if (generation_.load() != cleared_.load()) { vTaskDelay(1); continue; }
        size_t sent;
        { std::lock_guard<std::mutex> guard(writer_);
          sent = xStreamBufferSend(buffer_, pcm + offset, bytes - offset, 0); }
        offset += sent;
        if (!sent) vTaskDelay(pdMS_TO_TICKS(10));
    }
    return offset == bytes;
}

void PaperWifiAudio::PlayLoop() {
    int16_t pcm[256];
    kc_audio::PlaybackGate gate(kc_audio::PrebufferBytes(AUDIO_PLAY_BUF_BYTES, 500),
                                kc_audio::PrebufferBytes(AUDIO_PLAY_BUF_BYTES, 1000));
    bool amp = false;
    while (true) {
        const uint32_t generation = generation_.load();
        if (generation != cleared_.load()) {
            std::lock_guard<std::mutex> guard(writer_);
            while (xStreamBufferReceive(buffer_, pcm, sizeof pcm, 0)) {}
            finished_.store(false);
            gate.Reset();
            cleared_.store(generation);
        }
        const size_t available = xStreamBufferBytesAvailable(buffer_);
        if (!gate.Ready(available, finished_.load(), esp_timer_get_time() / 1000)) {
            if (amp && echo_.Drained(esp_timer_get_time())) {
                if (paper_audio_speaker_stop() == ESP_OK) amp = false;
            }
            if (finished_.load() && !available) gate.Reset();
            vTaskDelay(1);
            continue;
        }
        const size_t bytes = xStreamBufferReceive(buffer_, pcm, sizeof pcm, 0);
        if (generation != generation_.load() || bytes < sizeof(int16_t)) continue;
        if (!amp) {
            if (paper_audio_speaker_start() != ESP_OK) {
                ESP_LOGE(TAG, "speaker enable failed");
                vTaskDelay(pdMS_TO_TICKS(100)); continue;
            }
            amp = true;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (generation != generation_.load()) continue;
        echo_.OnWrite(esp_timer_get_time(), bytes / sizeof(int16_t));
        if (paper_audio_write_mono(pcm, bytes / sizeof(int16_t)) != ESP_OK)
            ESP_LOGW(TAG, "speaker write failed");
    }
}

void PaperWifiAudio::MicLoop() {
    int16_t pcm[AUDIO_SAMPLE_RATE / 50];
    agent_stream_handle_t voice = nullptr, audio = nullptr;
    bool voice_on = false, audio_on = false;
    while (true) {
        const bool ready = agent_link_state() == AGENT_STATE_READY;
        const int64_t until = asr_until_.load();
        if (until && esp_timer_get_time() >= until) asr_.store(false);
        const bool want_voice = ready && continuous_.load();
        const bool want_audio = ready && asr_.load();
        if (voice_on && !want_voice) {
            agent_link_stream_close(voice, ready); voice = nullptr; voice_on = false;
        }
        if (audio_on && !want_audio) {
            agent_link_stream_close(audio, ready); audio = nullptr; audio_on = false;
        }
        if (!ready) { continuous_.store(false); asr_.store(false); }
        if (!voice_on && want_voice) {
            const esp_err_t error = agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &voice);
            voice_on = error == ESP_OK;
            if (!voice_on) continuous_.store(false);
        }
        if (!audio_on && want_audio) {
            agent_stream_opts_t opts = {}; opts.name = "mic";
            audio_on = agent_link_stream_open(AGENT_STREAM_AUDIO, &opts, &audio) == ESP_OK;
            if (!audio_on) asr_.store(false);
        }
        if (!voice_on && !audio_on) {
            if (paper_audio_mic_active()) (void)paper_audio_mic_stop();
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        if (!paper_audio_mic_active() && paper_audio_mic_start() != ESP_OK) {
            continuous_.store(false); asr_.store(false);
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        size_t got = 0;
        if (paper_audio_read_mono(pcm, AUDIO_SAMPLE_RATE / 50, &got) != ESP_OK || !got) {
            vTaskDelay(1); continue;
        }
        const size_t bytes = got * sizeof(int16_t);
        if (audio_on && agent_link_stream_write(audio, pcm, bytes) != ESP_OK) asr_.store(false);
        if (voice_on) {
            if (echo_.Muted(esp_timer_get_time())) memset(pcm, 0, bytes);
            const esp_err_t error = agent_link_stream_write(voice, pcm, bytes);
            if (error != ESP_OK && error != ESP_ERR_NO_MEM) continuous_.store(false);
        }
    }
}
