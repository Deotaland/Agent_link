#include "passport_wifi_audio.h"

#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"

namespace { constexpr const char* TAG = "Passport.audio"; }

esp_err_t PassportWifiAudio::Init(AiPassportHardware* hardware) {
    if (!hardware || !hardware->audio_ready) return ESP_ERR_INVALID_STATE;
    hardware_ = hardware;
    buffer_ = xStreamBufferCreate(kBufferBytes, 1);
    if (!buffer_) return ESP_ERR_NO_MEM;
    if (xTaskCreate(PlayEntry, "passport_play", 4096, this, 5, &play_task_) != pdPASS ||
        xTaskCreate(MicEntry, "passport_mic", 4096, this, 5, &mic_task_) != pdPASS) {
        if (play_task_) vTaskDelete(play_task_);
        vStreamBufferDelete(buffer_); buffer_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    agent_link_playback_set_buffer_level_callback(&BufferLevel, this);
    agent_link_playback_set_buffer_ms(kBufferBytes / 32);
    ready_ = true;
    return ESP_OK;
}

size_t PassportWifiAudio::BufferLevel(void* context) {
    const auto* audio = static_cast<const PassportWifiAudio*>(context);
    if (audio->generation_.load() != audio->cleared_.load()) return kBufferBytes;
    return xStreamBufferBytesAvailable(audio->buffer_);
}

void PassportWifiAudio::Play(const uint8_t* pcm, size_t bytes) {
    if (!ready_ || !pcm || !bytes || bytes % 2) return;
    std::lock_guard<std::mutex> lock(writer_);
    if (generation_.load() != cleared_.load()) return;
    finished_.store(false);
    // The SDK paces delivery to the advertised buffer capacity. Never block a callback.
    if (bytes > xStreamBufferSpacesAvailable(buffer_) ||
        xStreamBufferSend(buffer_, pcm, bytes, 0) != bytes)
        ESP_LOGW(TAG, "speaker buffer full, dropped %u bytes", static_cast<unsigned>(bytes));
}

void PassportWifiAudio::PlayLoop() {
    int16_t pcm[256];
    kc_audio::PlaybackGate gate(kc_audio::PrebufferBytes(kBufferBytes, 100),
                                kc_audio::PrebufferBytes(kBufferBytes, 150));
    bool unmuted = false;
    for (;;) {
        const uint32_t generation = generation_.load();
        if (generation != cleared_.load()) {
            std::lock_guard<std::mutex> lock(writer_);
            while (xStreamBufferReceive(buffer_, pcm, sizeof pcm, 0)) {}
            finished_.store(false); gate.Reset(); cleared_.store(generation);
        }
        const size_t available = xStreamBufferBytesAvailable(buffer_);
        if (!gate.Ready(available, finished_.load(), esp_timer_get_time() / 1000)) {
            if (unmuted && echo_.Drained(esp_timer_get_time())) {
                (void)hardware_->codec.SetMute(true); unmuted = false;
            }
            if (finished_.load() && !available) gate.Reset();
            // At 100 Hz, 5 ms rounds to zero: the high-priority task must block
            // for at least one tick so startup and the idle task can run.
            vTaskDelay(pdMS_TO_TICKS(5) > 0 ? pdMS_TO_TICKS(5) : 1); continue;
        }
        const size_t bytes = xStreamBufferReceive(buffer_, pcm, sizeof pcm, 0);
        if (generation != generation_.load() || bytes < 2) continue;
        if (!unmuted) { (void)hardware_->codec.SetMute(false); unmuted = true; }
        echo_.OnWrite(esp_timer_get_time(), bytes / 2);
        if (hardware_->codec.WritePcm(pcm, bytes / 2) != ESP_OK)
            ESP_LOGW(TAG, "speaker write failed");
    }
}

void PassportWifiAudio::MicLoop() {
    int16_t pcm[320];
    agent_stream_handle_t stream = nullptr;
    for (;;) {
        const bool ready = agent_link_state() == AGENT_STATE_READY;
        if (!ready) enabled_.store(false);
        const bool wanted = ready && enabled_.load();
        if (stream && !wanted) {
            // EndCall/transport already cancel the grant. An old mic handle must
            // not cancel a newer grant if the user reconnects while this task wakes.
            (void)agent_link_stream_close(stream, true); stream = nullptr;
        }
        if (!wanted) {
            if (hardware_->codec.MicOn()) (void)hardware_->codec.StopMic();
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        if (!stream && agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &stream) != ESP_OK) {
            enabled_.store(false); vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        if (!hardware_->codec.MicOn() && hardware_->codec.StartMic() != ESP_OK) {
            enabled_.store(false); continue;
        }
        size_t got = 0;
        if (hardware_->codec.ReadPcm(pcm, 320, &got) != ESP_OK || !got) {
            vTaskDelay(1); continue;
        }
        // Half duplex: keep server endpointing alive without feeding its own speech back.
        if (echo_.Muted(esp_timer_get_time())) memset(pcm, 0, got * sizeof(int16_t));
        const esp_err_t result = agent_link_stream_write(stream, pcm, got * sizeof(int16_t));
        if (result != ESP_OK && result != ESP_ERR_NO_MEM) enabled_.store(false);
    }
}
