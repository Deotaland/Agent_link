// KorvoAudio: codec + playback + mic fan-out (ASR uplink and/or WAV on the card). See korvo_audio.h.

#include "korvo_audio.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "agent_link.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "korvo_audio";
constexpr size_t kPlayChunkSamples = 256;
constexpr size_t kMicBufSamples    = 640;   // 20ms at 32kHz; more than the 16kHz frame needs
}  // namespace

esp_err_t KorvoAudio::Init(const Config& cfg) {
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

    // PSRAM: this is a jitter buffer for replies that can run for a minute, and a few hundred KB of
    // the 8MB PSRAM is free while the same in internal RAM is not.
    play_buf_ = xStreamBufferCreateWithCaps(cfg_.play_buf_bytes, /*trigger=*/1, MALLOC_CAP_SPIRAM);
    if (!play_buf_) {
        play_buf_ = xStreamBufferCreate(16 * 1024, /*trigger=*/1);
        if (!play_buf_) { ESP_LOGE(TAG, "play buffer alloc failed"); return ESP_ERR_NO_MEM; }
        ESP_LOGW(TAG, "no PSRAM - play buffer is 16KB (0.5s); long replies will stutter");
        agent_link_audio_set_buffer_ms(16 * 1024 / 32);
    } else {
        // Tell the SDK what we hold so it throttles the App (event 0x20) before this overflows.
        // 32 bytes per ms at PCM16/16kHz/mono.
        agent_link_audio_set_buffer_ms(static_cast<uint32_t>(cfg_.play_buf_bytes / 32));
    }

    // The card is optional: without one everything works except recording.
    SdCardConfig sc = {};
    sc.pin_clk     = cfg_.sd_clk;
    sc.pin_cmd     = cfg_.sd_cmd;
    sc.pin_d0      = cfg_.sd_d0;
    sc.mount_point = cfg_.sd_mount;
    if (sd_.Init(sc) == ESP_OK) {
        mkdir(cfg_.rec_dir, 0775);   // fails harmlessly when it already exists
        if (cfg_.msg_dir) mkdir(cfg_.msg_dir, 0775);
        ESP_LOGI(TAG, "recordings go to %s (%lluMB free)", cfg_.rec_dir,
                 static_cast<unsigned long long>(sd_.FreeMb()));
    } else {
        ESP_LOGW(TAG, "no TF card - the recorder app will say so instead of recording");
    }

    xTaskCreate(&KorvoAudio::PlayTaskEntry, "spk_play", 4096, this, 5, nullptr);
    // Bigger stack than the player: this one also runs fwrite() down through FATFS to the card.
    xTaskCreate(&KorvoAudio::MicTaskEntry,  "mic_up",   6144, this, 5, nullptr);
    ESP_LOGI(TAG, "audio ready (%d Hz, play buffer %uKB)", cfg_.sample_rate,
             static_cast<unsigned>(cfg_.play_buf_bytes / 1024));
    return ESP_OK;
}

void KorvoAudio::PlayPcm(const uint8_t* pcm16, size_t bytes) {
    if (!play_buf_ || !pcm16 || bytes == 0) return;
    const size_t sent = xStreamBufferSend(play_buf_, pcm16, bytes, 0);   // 0 = never block the caller
    if (sent < bytes) {
        ESP_LOGW(TAG, "play buffer full - dropped %u/%u bytes",
                 static_cast<unsigned>(bytes - sent), static_cast<unsigned>(bytes));
    }
}

void KorvoAudio::PlayTaskEntry(void* arg) { static_cast<KorvoAudio*>(arg)->PlayLoop(); }

void KorvoAudio::PlayLoop() {
    int16_t buf[kPlayChunkSamples];
    while (true) {
        const size_t n = xStreamBufferReceive(play_buf_, buf, sizeof(buf), portMAX_DELAY);
        if (n >= sizeof(int16_t)) (void)codec_.WritePcm(buf, n / sizeof(int16_t));
    }
}

void KorvoAudio::StartAsr() { want_asr_.store(true,  std::memory_order_release); }
void KorvoAudio::StopAsr()  { want_asr_.store(false, std::memory_order_release); }

void KorvoAudio::StartCommand() { want_cmd_.store(true,  std::memory_order_release); }
void KorvoAudio::StopCommand()  { want_cmd_.store(false, std::memory_order_release); }

void KorvoAudio::StartRecording() {
    if (!sd_.Ready()) { ESP_LOGW(TAG, "record requested but no card mounted"); return; }
    want_rec_.store(true, std::memory_order_release);
}
void KorvoAudio::StopRecording() { want_rec_.store(false, std::memory_order_release); }

// rec-0001.wav, rec-0002.wav, ... A wall clock would be nicer, but the board has no RTC and may
// never see the App, so a boot-local counter is the honest option.
bool KorvoAudio::OpenRecordingFile() {
    char path[128];
    snprintf(path, sizeof(path), "%s/rec-%04u.wav", cfg_.rec_dir, static_cast<unsigned>(++rec_seq_));
    if (wav_.Start(path, static_cast<uint32_t>(cfg_.sample_rate), cfg_.rec_format) != ESP_OK) return false;
    snprintf(last_file_, sizeof(last_file_), "%s", path);
    return true;
}

void KorvoAudio::MicTaskEntry(void* arg) { static_cast<KorvoAudio*>(arg)->MicLoop(); }

void KorvoAudio::MicLoop() {
    const size_t want_frame = static_cast<size_t>(cfg_.sample_rate) / 50;   // 20ms
    const size_t frame      = want_frame > kMicBufSamples ? kMicBufSamples : want_frame;
    int16_t buf[kMicBufSamples];

    while (true) {
        const bool want_asr = want_asr_.load(std::memory_order_acquire);
        const bool want_cmd = want_cmd_.load(std::memory_order_acquire);
        const bool want_rec = want_rec_.load(std::memory_order_acquire);
        bool asr_on = asr_on_.load(std::memory_order_acquire);
        bool cmd_on = cmd_on_.load(std::memory_order_acquire);
        bool rec_on = rec_on_.load(std::memory_order_acquire);

        // -- Reconcile desired vs actual. Stops run first, so a stop+start in one pass can't overlap --
        if (asr_on && !want_asr) {
            agent_link_asr_end(true);
            asr_on = false;
            asr_on_.store(false, std::memory_order_release);
            ESP_LOGI(TAG, "ASR stream ended");
        }
        if (cmd_on && !want_cmd) {
            agent_link_voice_end();        // closes the 0x40 session the first frame opened
            cmd_on = false;
            cmd_on_.store(false, std::memory_order_release);
            ESP_LOGI(TAG, "voice command sent");
        }
        if (rec_on && !want_rec) {
            wav_.Stop();
            rec_on = false;
            rec_on_.store(false, std::memory_order_release);
        }
        if (!asr_on && want_asr) {
            if (agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "asr requested but the App is not connected");
                want_asr_.store(false, std::memory_order_release);
            } else if (agent_link_asr_start("korvo") != ESP_OK) {
                ESP_LOGW(TAG, "asr_start failed - the App must open the L2CAP channel (PSM 0x0081) first");
                want_asr_.store(false, std::memory_order_release);
            } else {
                asr_on = true;
                asr_on_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "ASR stream started");
            }
        }
        if (!cmd_on && want_cmd) {
            // Nothing to open: push_voice opens the session lazily on its first frame. All we need
            // is a live link, so unlike ASR this does not depend on the App's L2CAP channel.
            if (agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "voice command requested but the App is not connected");
                want_cmd_.store(false, std::memory_order_release);
            } else {
                cmd_on = true;
                cmd_on_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "voice command started (0x40 VoiceChunk)");
            }
        }
        if (!rec_on && want_rec) {
            if (!OpenRecordingFile()) {
                want_rec_.store(false, std::memory_order_release);
            } else {
                rec_ms_.store(0, std::memory_order_release);
                rec_on = true;
                rec_on_.store(true, std::memory_order_release);
            }
        }

        // The mic follows "any uplink wants audio".
        const bool need_mic = asr_on || cmd_on || rec_on;
        if (need_mic && !codec_.MicOn()) {
            if (codec_.StartMic() != ESP_OK) {
                ESP_LOGE(TAG, "mic start failed");
                want_asr_.store(false, std::memory_order_release);
                want_cmd_.store(false, std::memory_order_release);
                want_rec_.store(false, std::memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        } else if (!need_mic && codec_.MicOn()) {
            (void)codec_.StopMic();
        }

        if (!need_mic) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        // -- One read, fanned out to whichever uplinks are live --
        size_t got = 0;
        if (codec_.ReadPcm(buf, frame, &got) != ESP_OK || got == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));   // read failed: don't spin
            continue;
        }
        const size_t bytes = got * sizeof(int16_t);

        if (asr_on) {
            if (agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "link dropped mid-stream, ending ASR");
                agent_link_asr_end(false);
                asr_on_.store(false, std::memory_order_release);
                want_asr_.store(false, std::memory_order_release);
            } else if (agent_link_asr_push(reinterpret_cast<const uint8_t*>(buf), bytes) == ESP_ERR_NO_MEM) {
                // Backpressure truncated us: end this segment rather than send a torn stream.
                ESP_LOGW(TAG, "ASR truncated (link can't keep up), stopping");
                agent_link_asr_end(false);
                asr_on_.store(false, std::memory_order_release);
                want_asr_.store(false, std::memory_order_release);
            }
        }
        if (cmd_on) {
            if (agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "link dropped mid-command, closing the voice session");
                agent_link_voice_end();
                cmd_on_.store(false, std::memory_order_release);
                want_cmd_.store(false, std::memory_order_release);
            } else {
                // ESP_ERR_NO_MEM here means the SDK's voice queue hit its 96KB cap and dropped this
                // frame to keep the head of the utterance. It logs that itself and the session stays
                // open on purpose, so — unlike the ASR path — do NOT tear the stream down.
                (void)agent_link_push_voice(reinterpret_cast<const uint8_t*>(buf), bytes);
            }
        }
        if (rec_on) {
            if (wav_.Write(buf, bytes) != ESP_OK) {
                wav_.Stop();
                rec_on_.store(false, std::memory_order_release);
                want_rec_.store(false, std::memory_order_release);
            } else {
                rec_ms_.store(wav_.DurationMs(), std::memory_order_release);
            }
        }
    }
}
