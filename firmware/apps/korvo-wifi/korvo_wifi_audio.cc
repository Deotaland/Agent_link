// Codec + playback + mic fan-out. See korvo_wifi_audio.h.

#include "korvo_wifi_audio.h"
#include "voice_playback.h"

#include <cstring>

#include "agent_link.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "kc_audio";
constexpr size_t kPlayChunkSamples = 256;
constexpr size_t kMicBufSamples    = 640;   // 20ms at 32kHz; more than the 16kHz frame needs
}  // namespace

esp_err_t KorvoCloudAudio::Init(const Config& cfg) {
    cfg_ = cfg;

    codec_ = cfg.codec;
    if (!codec_ || !codec_->Ready()) return ESP_ERR_INVALID_STATE;
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
    xTaskCreate(&KorvoCloudAudio::MicTaskEntry,  "mic_up",   6144, this, 5, nullptr);
    ESP_LOGI(TAG, "audio ready (%d Hz, play buffer %uKB)", cfg_.sample_rate,
             static_cast<unsigned>(cfg_.play_buf_bytes / 1024));
    return ESP_OK;
}

void KorvoCloudAudio::PlayPcm(const uint8_t* pcm16, size_t bytes) {
    std::lock_guard<std::mutex> lock(play_mutex_);
    if (!play_buf_ || !pcm16 || bytes == 0) return;
    // Let the consumer finish clearing an interrupted reply before accepting a new one.
    if (play_generation_.load() != play_cleared_.load()) return;
    play_finished_.store(false);
    const size_t sent = xStreamBufferSend(play_buf_, pcm16, bytes, 0);   // 0 = never block the caller
    if (sent < bytes) {
        play_dropped_.fetch_add(bytes - sent);
        ESP_LOGW(TAG, "play buffer full - dropped %u/%u bytes",
                 static_cast<unsigned>(bytes - sent), static_cast<unsigned>(bytes));
    }
}

void KorvoCloudAudio::StopPlayback() { play_generation_.fetch_add(1); }

void KorvoCloudAudio::EndPlayback() {
    play_finished_.store(true);
    ESP_LOGI(TAG, "speaker reply end #%u", play_ends_.fetch_add(1) + 1);
}

void KorvoCloudAudio::PlayTaskEntry(void* arg) { static_cast<KorvoCloudAudio*>(arg)->PlayLoop(); }

void KorvoCloudAudio::PlayLoop() {
    int16_t buf[kPlayChunkSamples];
    const size_t capacity = xStreamBufferSpacesAvailable(play_buf_);
    const size_t start_bytes = kc_audio::PrebufferBytes(capacity, 500);
    const size_t resume_bytes = kc_audio::PrebufferBytes(capacity, 1000);
    kc_audio::PlaybackGate gate(start_bytes, resume_bytes);
    size_t played = 0;
    unsigned drains = 0, resets = 0;
    ESP_LOGI(TAG, "speaker prebuffer: start=%u ms resume=%u ms (reply end bypasses both)",
             static_cast<unsigned>(start_bytes / 32), static_cast<unsigned>(resume_bytes / 32));
    while (true) {
        const uint32_t generation = play_generation_.load();
        if (generation != play_cleared_.load()) {
            std::lock_guard<std::mutex> lock(play_mutex_);
            ESP_LOGI(TAG, "speaker reset #%u (queued=%u ms)", ++resets,
                     static_cast<unsigned>(xStreamBufferBytesAvailable(play_buf_) / 32));
            while (xStreamBufferReceive(play_buf_, buf, sizeof(buf), 0)) {}
            play_finished_.store(false);
            play_dropped_.store(0);
            gate.Reset();
            played = 0;
            play_cleared_.store(generation);
        }
        const size_t available = xStreamBufferBytesAvailable(play_buf_);
        const bool finished = play_finished_.load();
        const bool was_playing = gate.Playing();
        if (!gate.Ready(available, finished, esp_timer_get_time() / 1000)) {
            if (finished && !available && played && echo_guard_.Drained(esp_timer_get_time())) {
                ESP_LOGI(TAG, "speaker drained #%u: %u ms, underruns=%u, dropped=%u bytes",
                         ++drains, static_cast<unsigned>(played / 32), gate.Underruns(),
                         static_cast<unsigned>(play_dropped_.exchange(0)));
                played = 0;
                gate.Reset();
            }
            vTaskDelay(1);  // Always block; sub-tick millisecond delays become zero.
            continue;
        }
        if (!was_playing) {
            ESP_LOGI(TAG, "speaker %s at %u ms queued%s", played ? "resumed" : "started",
                     static_cast<unsigned>(available / 32), finished ? " (reply ended)" : "");
        }
        const size_t n = xStreamBufferReceive(play_buf_, buf, sizeof(buf), 0);
        if (generation != play_generation_.load()) continue;
        if (n >= sizeof(int16_t)) {
            echo_guard_.OnWrite(esp_timer_get_time(), n / sizeof(int16_t));
            if (codec_->WritePcm(buf, n / sizeof(int16_t)) != ESP_OK)
                ESP_LOGW(TAG, "speaker I2S write failed");
            played += n;
        }
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

void KorvoCloudAudio::StartCommand() {
    StopPlayback();
    want_cmd_.store(true, std::memory_order_release);
}
void KorvoCloudAudio::StopCommand()  { want_cmd_.store(false, std::memory_order_release); }
void KorvoCloudAudio::StartContinuous() { want_continuous_.store(true, std::memory_order_release); }
void KorvoCloudAudio::StopContinuous() { want_continuous_.store(false, std::memory_order_release); }

void KorvoCloudAudio::MicTaskEntry(void* arg) { static_cast<KorvoCloudAudio*>(arg)->MicLoop(); }

void KorvoCloudAudio::MicLoop() {
    const size_t want_frame = static_cast<size_t>(cfg_.sample_rate) / 50;   // 20ms
    const size_t frame      = want_frame > kMicBufSamples ? kMicBufSamples : want_frame;
    int16_t buf[kMicBufSamples];
    bool echo_muted = false;
    unsigned muted_frames = 0;

    while (true) {
        const bool want_asr = want_asr_.load(std::memory_order_acquire);
        const bool continuous = want_continuous_.load(std::memory_order_acquire);
        const bool want_cmd = want_cmd_.load(std::memory_order_acquire) || continuous;
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
            ESP_LOGI(TAG, "voice capture stopped");
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
                want_continuous_.store(false, std::memory_order_release);
            } else if (agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &voice_stream_) != ESP_OK) {
                ESP_LOGW(TAG, "voice stream failed to open");
                want_cmd_.store(false, std::memory_order_release);
                want_continuous_.store(false, std::memory_order_release);
            } else {
                cmd_on = true;
                cmd_on_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "voice stream started (%s)", continuous ? "continuous" : "command");
            }
        }

        const bool need_mic = asr_on || cmd_on;
        if (need_mic && !codec_->MicOn()) {
            if (codec_->StartMic() != ESP_OK) {
                ESP_LOGE(TAG, "mic start failed");
                want_asr_.store(false, std::memory_order_release);
                want_cmd_.store(false, std::memory_order_release);
                want_continuous_.store(false, std::memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        } else if (!need_mic && codec_->MicOn()) {
            (void)codec_->StopMic();
        }

        if (!need_mic) {
            if (echo_muted) {
                ESP_LOGI(TAG, "mic echo guard off (%u frames sent as silence)", muted_frames);
                echo_muted = false;
                muted_frames = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // One read, fanned out to whichever uplinks are live.
        size_t got = 0;
        if (codec_->ReadPcm(buf, frame, &got) != ESP_OK || got == 0) {
            vTaskDelay(1);   // read failed: block for at least one tick
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
                if (continuous ? want_continuous_.load() : want_cmd_.load())
                    ESP_LOGW(TAG, "link dropped mid-command, closing the voice session");
                agent_link_stream_close(voice_stream_, continuous);
                cmd_on_.store(false, std::memory_order_release);
                want_cmd_.store(false, std::memory_order_release);
                want_continuous_.store(false, std::memory_order_release);
            } else {
                const bool silence = continuous && echo_guard_.Muted(esp_timer_get_time());
                if (silence != echo_muted) {
                    if (silence) ESP_LOGI(TAG, "mic echo guard on");
                    else ESP_LOGI(TAG, "mic echo guard off (%u frames sent as silence)", muted_frames);
                    echo_muted = silence;
                    muted_frames = 0;
                }
                if (silence) { memset(buf, 0, bytes); ++muted_frames; }
                const esp_err_t r = agent_link_stream_write(voice_stream_, buf, bytes);
                // A full continuous queue drops this frame; the worker reports totals every five seconds.
                if (r != ESP_OK && !(r == ESP_ERR_NO_MEM && continuous)) {
                    ESP_LOGW(TAG, "voice upload failed (%s), ending capture", esp_err_to_name(r));
                    agent_link_stream_close(voice_stream_, continuous);
                    cmd_on_.store(false, std::memory_order_release);
                    want_cmd_.store(false, std::memory_order_release);
                    want_continuous_.store(false, std::memory_order_release);
                }
            }
        }
    }
}
