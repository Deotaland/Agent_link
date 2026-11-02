#include "amoled_voice_audio.h"

#include <cstring>
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"

namespace {
constexpr const char* TAG = "Amoled175.audio";
constexpr uint32_t kMicStackBytes = 8192;
constexpr int64_t kDiagnosticIntervalUs = 5000000;
}

esp_err_t AmoledVoiceAudio::Init(AmoledHardware* hardware) {
    if (!hardware || !hardware->codec_ok_) return ESP_ERR_INVALID_STATE;
    hardware_ = hardware;
    if (hardware_->SetSpeakerEnabled(false) != ESP_OK) return ESP_FAIL;
    buffer_ = xStreamBufferCreateWithCaps(AUDIO_PLAY_BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!buffer_) return ESP_ERR_NO_MEM;
    if (xTaskCreate(PlayEntry, "amoled_play", 4096, this, 5, &play_task_) != pdPASS ||
        xTaskCreate(MicEntry, "amoled_mic", kMicStackBytes, this, 5, &mic_task_) != pdPASS) {
        if (play_task_) vTaskDelete(play_task_);
        vStreamBufferDeleteWithCaps(buffer_); buffer_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    agent_link_playback_set_buffer_ms(AUDIO_PLAY_BUF_BYTES / 32);
    ready_ = true;
    ESP_LOGI(TAG, "PCM diagnostics enabled: microphone stack=%luB, interval=5s, hardware=1.75C STD stereo 16kHz -> PCM16 mono %dHz, Opus transport unchanged",
             static_cast<unsigned long>(kMicStackBytes), AUDIO_SAMPLE_RATE);
    agent_link_playback_set_buffer_level_callback([](void* ctx) -> size_t {
        return static_cast<AmoledVoiceAudio*>(ctx)->queued_.load();
    }, this);
    return ESP_OK;
}

void AmoledVoiceAudio::Play(const uint8_t* pcm, size_t bytes) {
    if (!ready_ || !pcm || !bytes) return;
    std::lock_guard<std::mutex> guard(writer_);
    if (generation_.load() != cleared_.load()) return;
    finished_.store(false);
    const size_t sent = xStreamBufferSend(buffer_, pcm, bytes, 0);
    queued_.store(xStreamBufferBytesAvailable(buffer_));
    if (sent < bytes) fault_.store(true);
}

void AmoledVoiceAudio::Listen(bool enabled, uint32_t max_ms) {
    asr_until_.store(enabled && max_ms ? esp_timer_get_time() + static_cast<int64_t>(max_ms) * 1000 : 0);
    asr_.store(enabled);
}

void AmoledVoiceAudio::BeginDiagnostics() {
    mic_window_ = {};
    mic_window_.capture = ++diagnostic_capture_;
    mic_window_.started_us = esp_timer_get_time();
    diagnostic_active_ = true;
}

void AmoledVoiceAudio::PublishDiagnostics(bool final) {
    if (!diagnostic_active_) return;
    const int64_t now = esp_timer_get_time();
    if (mic_window_.raw.frames || mic_window_.read_errors) {
        mic_window_.ended_us = now;
        mic_window_.final = final;
        mic_window_.sequence = ++diagnostic_sequence_;
        mic_window_.mic_stack_min = uxTaskGetStackHighWaterMark(nullptr);
        mic_window_.play_stack_min = play_task_ ? uxTaskGetStackHighWaterMark(play_task_) : 0;
        taskENTER_CRITICAL(&diagnostic_lock_);
        diagnostic_snapshot_ = mic_window_;
        taskEXIT_CRITICAL(&diagnostic_lock_);
    }
    if (final) diagnostic_active_ = false;
    else {
        const uint32_t capture = mic_window_.capture;
        mic_window_ = {};
        mic_window_.capture = capture;
        mic_window_.started_us = now;
    }
}

void AmoledVoiceAudio::ReportDiagnostics() {
    amoled_voice::MicDiagnosticWindow snapshot;
    taskENTER_CRITICAL(&diagnostic_lock_);
    snapshot = diagnostic_snapshot_;
    taskEXIT_CRITICAL(&diagnostic_lock_);
    if (!snapshot.sequence || snapshot.sequence == diagnostic_reported_) return;
    const uint32_t skipped = snapshot.sequence - diagnostic_reported_ - 1;
    diagnostic_reported_ = snapshot.sequence;
    const auto& raw = snapshot.raw;
    const auto& up = snapshot.uplink;
    for (size_t channel = 0; channel < snapshot.channels.size(); ++channel) {
        const auto& levels = snapshot.channels[channel];
        ESP_LOGI(TAG, "PCM mic%u #%lu 16kHz: samples=%lu peak=%lu rms=%lu dc=%ld zero=%lu/%lu",
                 static_cast<unsigned>(channel + 1), static_cast<unsigned long>(snapshot.sequence),
                 static_cast<unsigned long>(levels.samples), static_cast<unsigned long>(levels.peak),
                 static_cast<unsigned long>(levels.Rms()), static_cast<long>(levels.Mean()),
                 static_cast<unsigned long>(levels.zero_frames), static_cast<unsigned long>(levels.frames));
    }
    ESP_LOGI(TAG, "PCM raw #%lu capture=%lu %lums: frames=%lu samples=%lu min=%ld max=%ld peak=%lu rms=%lu dc=%ld zero=%lu quiet=%lu clip=%lu",
             static_cast<unsigned long>(snapshot.sequence), static_cast<unsigned long>(snapshot.capture),
             static_cast<unsigned long>((snapshot.ended_us - snapshot.started_us) / 1000),
             static_cast<unsigned long>(raw.frames), static_cast<unsigned long>(raw.samples),
             static_cast<long>(raw.minimum), static_cast<long>(raw.maximum), static_cast<unsigned long>(raw.peak),
             static_cast<unsigned long>(raw.Rms()), static_cast<long>(raw.Mean()),
             static_cast<unsigned long>(raw.zero_frames), static_cast<unsigned long>(raw.quiet_frames),
             static_cast<unsigned long>(raw.clipped_samples));
    ESP_LOGI(TAG, "PCM uplink #%lu: frames=%lu samples=%lu min=%ld max=%ld peak=%lu rms=%lu dc=%ld zero=%lu quiet=%lu clip=%lu",
             static_cast<unsigned long>(snapshot.sequence), static_cast<unsigned long>(up.frames), static_cast<unsigned long>(up.samples),
             static_cast<long>(up.minimum), static_cast<long>(up.maximum), static_cast<unsigned long>(up.peak),
             static_cast<unsigned long>(up.Rms()), static_cast<long>(up.Mean()),
             static_cast<unsigned long>(up.zero_frames), static_cast<unsigned long>(up.quiet_frames),
             static_cast<unsigned long>(up.clipped_samples));
    ESP_LOGI(TAG, "PCM path #%lu: voice=%lu asr=%lu echo_muted=%lu voice_drop=%lu write_err=%lu read_err=%lu last_read=%ld last_write=%ld stack_mic=%luB stack_play=%luB final=%d skipped=%lu",
             static_cast<unsigned long>(snapshot.sequence), static_cast<unsigned long>(snapshot.voice_frames),
             static_cast<unsigned long>(snapshot.asr_frames), static_cast<unsigned long>(snapshot.echo_muted_frames),
             static_cast<unsigned long>(snapshot.voice_drops), static_cast<unsigned long>(snapshot.write_errors),
             static_cast<unsigned long>(snapshot.read_errors), static_cast<long>(snapshot.last_read_error),
             static_cast<long>(snapshot.last_write_error), static_cast<unsigned long>(snapshot.mic_stack_min),
             static_cast<unsigned long>(snapshot.play_stack_min), snapshot.final, static_cast<unsigned long>(skipped));
}

void AmoledVoiceAudio::PlayLoop() {
    int16_t pcm[256];
    kc_audio::PlaybackGate gate(kc_audio::PrebufferBytes(AUDIO_PLAY_BUF_BYTES, 500),
                                kc_audio::PrebufferBytes(AUDIO_PLAY_BUF_BYTES, 1000));
    bool amp = false;
    while (true) {
        const uint32_t generation = generation_.load();
        if (generation != cleared_.load()) {
            std::lock_guard<std::mutex> guard(writer_);
            while (xStreamBufferReceive(buffer_, pcm, sizeof pcm, 0)) {}
            queued_.store(0);
            finished_.store(false);
            gate.Reset();
            cleared_.store(generation);
        }
        const size_t available = xStreamBufferBytesAvailable(buffer_);
        queued_.store(available);
        if (!gate.Ready(available, finished_.load(), esp_timer_get_time() / 1000)) {
            if (amp && echo_.Drained(esp_timer_get_time())) {
                if (hardware_->SetSpeakerEnabled(false) == ESP_OK) amp = false;
            }
            if (finished_.load() && !available) gate.Reset();
            vTaskDelay(1);
            continue;
        }
        const size_t bytes = xStreamBufferReceive(buffer_, pcm, sizeof pcm, 0);
        queued_.store(xStreamBufferBytesAvailable(buffer_));
        if (generation != generation_.load() || bytes < sizeof(int16_t)) continue;
        if (!amp) {
            if (hardware_->SetSpeakerEnabled(true) != ESP_OK) {
                fault_.store(true);
                ESP_LOGE(TAG, "speaker enable failed");
                vTaskDelay(pdMS_TO_TICKS(100)); continue;
            }
            amp = true;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (generation != generation_.load()) continue;
        echo_.OnWrite(esp_timer_get_time(), bytes / sizeof(int16_t));
        if (hardware_->codec_.WritePcm(pcm, bytes / sizeof(int16_t)) != ESP_OK) {
            fault_.store(true);
            ESP_LOGW(TAG, "speaker write failed");
        }
    }
}

void AmoledVoiceAudio::MicLoop() {
    int16_t pcm[AmoledAudio::kCaptureCapacity];
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
        // Preserve continuous intent during SDK recovery; no stream/mic runs
        // until the replacement connection has received session.ready.
        if (!voice_on && want_voice) {
            const esp_err_t error = agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &voice);
            voice_on = error == ESP_OK;
            if (!voice_on && agent_link_state() == AGENT_STATE_READY) {
                continuous_.store(false); fault_.store(true);
            }
        }
        if (!audio_on && want_audio) {
            agent_stream_opts_t opts = {}; opts.name = "mic";
            audio_on = agent_link_stream_open(AGENT_STREAM_AUDIO, &opts, &audio) == ESP_OK;
            if (!audio_on) asr_.store(false);
        }
        if (!voice_on && !audio_on) {
            PublishDiagnostics(true);
            if (hardware_->codec_.MicOn()) (void)hardware_->codec_.StopMic();
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        if (!hardware_->codec_.MicOn() && hardware_->codec_.StartMic() != ESP_OK) {
            continuous_.store(false); asr_.store(false);
            fault_.store(true);
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        if (!diagnostic_active_) BeginDiagnostics();
        size_t got = 0;
        const esp_err_t read_error = hardware_->codec_.ReadPcm(pcm, AmoledAudio::kCaptureCapacity, &got);
        if (read_error != ESP_OK || !got) {
            ++mic_window_.read_errors;
            mic_window_.last_read_error = read_error == ESP_OK ? ESP_ERR_INVALID_SIZE : read_error;
            PublishDiagnostics(true);
            fault_.store(true); continuous_.store(false); asr_.store(false);
            vTaskDelay(1); continue;
        }
        mic_window_.raw.Add(pcm, got);
        for (size_t channel = 0; channel < mic_window_.channels.size(); ++channel)
            mic_window_.channels[channel].Merge(hardware_->codec_.ChannelLevels()[channel]);
        const size_t bytes = got * sizeof(int16_t);
        if (audio_on) {
            ++mic_window_.asr_frames;
            const esp_err_t error = agent_link_stream_write(audio, pcm, bytes);
            if (error != ESP_OK) {
                ++mic_window_.write_errors; mic_window_.last_write_error = error;
                asr_.store(false);
            }
        }
        if (voice_on) {
            ++mic_window_.voice_frames;
            if (echo_.Muted(esp_timer_get_time())) {
                memset(pcm, 0, bytes); ++mic_window_.echo_muted_frames;
            }
            mic_window_.uplink.Add(pcm, got);
            const esp_err_t error = agent_link_stream_write(voice, pcm, bytes);
            if (error == ESP_ERR_NO_MEM) ++mic_window_.voice_drops;
            else if (error != ESP_OK) { ++mic_window_.write_errors; mic_window_.last_write_error = error; }
            if (error != ESP_OK && error != ESP_ERR_NO_MEM && agent_link_state() == AGENT_STATE_READY) {
                continuous_.store(false); fault_.store(true);
            }
        }
        if (esp_timer_get_time() - mic_window_.started_us >= kDiagnosticIntervalUs) PublishDiagnostics(false);
    }
}
