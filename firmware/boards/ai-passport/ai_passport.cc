// FoloToy AI Passport (ESP32-C3) — BLE push-to-talk with the Deotaland App.
// Hardware: ST7789 240x320, ES8311 codec, three ADC keys (UP/DOWN/OK), CW2017 gauge.
// Hold OK to talk; release to send. Agent TTS plays on the speaker. Screen shows link colour.

#include "board.h"
#include "config.h"
#include "es8311_audio.h"
#include "st7789_lcd.h"
#include "voice_ui.h"

#include "agent_link.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#define TAG "AiPassport"

namespace {

enum class Btn : uint8_t { None, Up, Down, Ok };

// Shorter than this, an OK press is a tap, not speech: the stream is abandoned, not sent.
constexpr int64_t kMinTalkUs = 400 * 1000;
constexpr int64_t kTapHintUs = 2000 * 1000;

// Volume is 0..kMaxVolume in steps of 10, using codec-dev's default -50..0 dB curve.
constexpr int kMaxVolume = 100;

// Pure-colour display test. Set to 1 to paint the panel solid colours in turn at boot, with the UI
// left off, so what you see is the panel and the ST7789 driver alone. Keep in mind that it runs
// straight into FillSolid(), bypassing the UI, so it cannot show a byte-order or gamma
// problem - it only tells you how the panel itself behaves (inverted, swapped, washed out).
constexpr bool kColorTest = 0;

// Cycled in this order, 2s each, forever. The order itself is the measurement: it runs cyan,
// magenta, yellow, black, white, grey first, because those are the pairs that stay put when
// something is wrong in one specific way. Blue, red and green come last so the two single-channel
// colours that a swap would confuse are on screen longest.
constexpr uint16_t kSwatches[] = {
    0x07FF,   // cyan: green + blue
    0xF81F,   // magenta: red + blue
    0xFF00,   // yellow: red + green
    0x0000,   // black
    0xFFFF,   // white
    0x8410,   // mid grey: unchanged by an RGB/BGR swap, which is what makes it the reference
    0x001F,   // blue
    0xF800,   // red
    0x07E0,   // green
    0x4043,   // the UI's actual background, 0x0E1118 converted to RGB565
};
constexpr const char* const kSwatchNames[] = {
    "cyan", "magenta", "yellow", "black", "white",
    "grey", "blue", "red", "green", "UI background",
};

// ST7789P3 reference init, minus the four commands esp_lcd already sends (0x11/0x3A/0x21/0x29) and
// 0x36 MADCTL, which the colour commands below rewrite.
constexpr St7789LcdConfig::VendorCmd kVendorCmds[] = {
    {0xB2, 0, 5, {0x05, 0x05, 0x00, 0x33, 0x33}},   // PORCTRL, frame rate porch
    {0xB7, 0, 1, {0x35}},                          // GCTRL, gate control
    {0xBB, 0, 1, {0x21}},                          // VCOMS
    {0xC0, 0, 1, {0x2C}},                          // LCMCTRL
    {0xC2, 0, 1, {0x01}},                          // VDVVRHEN
    {0xC3, 0, 1, {0x0B}},                          // VRHS
    {0xC4, 0, 1, {0x20}},                          // VDVSET
    {0xC6, 0, 1, {0x0F}},                          // FRCTRL2, 60 Hz inversion
    {0xD0, 0, 2, {0xA7, 0xA1}},                    // PWCTRL1
    {0xD0, 0, 2, {0xA4, 0xA1}},                    // PWCTRL1 again, overrides the line above
    {0xD6, 0, 1, {0xA1}},
    {0xE0, 0, 14, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D,   // PVGAMCTRL
                   0x43, 0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}},
    {0xE1, 10, 14, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, // NVGAMCTRL
                    0x44, 0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}},
};

}  // namespace

class AiPassportBoard : public Board {
public:
    AiPassportBoard() {
        LogMemory("before display");
        InitDisplay();
        LogMemory("after display");
        if (kColorTest) {
            xTaskCreate(&AiPassportBoard::ColorTaskEntry, "color_test", 3072, this, 4, nullptr);
            return;   // nothing else starts: the test owns the screen
        }
        InitCodec();
        LogMemory("after codec and playback buffer");
        InitAdcButtons();
        if (codec_ok_) {
            xTaskCreate(&AiPassportBoard::PttTaskEntry, "ptt_voice", 4096, this, 5, &ptt_task_);
            ESP_LOGI(TAG, "PTT ready: hold OK to talk, release to send");
        }
        // Runs even without the codec: UP/DOWN still drive the screen, and their volume steps
        // are skipped rather than the whole task.
        xTaskCreate(&AiPassportBoard::KeyTaskEntry, "keys", 3072, this, 4, &key_task_);
        ESP_LOGI(TAG, "before link: internal heap free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    const char* Name() const override { return "AIPassport"; }
    const char* Model() const override { return "AI-PASSPORT"; }

    uint32_t Capabilities() const override {
        return AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_SCREEN | AGENT_CAP_BUTTON |
               AGENT_CAP_RECORDING;
    }

    void PlayAudio(const uint8_t* pcm16, size_t bytes) override {
        if (!play_buf_ || !pcm16 || !bytes || (bytes & 1u)) return;
        taskENTER_CRITICAL(&audio_lock_);
        const uint32_t token = session_token_.load(std::memory_order_acquire);
        if (!link_ready_.load() || playback_hold_.load() || !voice::Session::AcceptsAudio(token) ||
            play_generation_ != voice::TokenGeneration(token)) {
            taskEXIT_CRITICAL(&audio_lock_);
            return;
        }
        received_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        last_audio_at_.store(esp_timer_get_time(), std::memory_order_release);
        if (!reply_control_.Ended(token)) audio_ended_.store(false, std::memory_order_release);
        playback_done_.store(0, std::memory_order_release);
        // Never block the transport callback or enqueue a partial PCM block.
        const size_t room = xStreamBufferSpacesAvailable(play_buf_);
        const size_t sent = bytes <= room ? xStreamBufferSend(play_buf_, pcm16, bytes, 0) : 0;
        taskEXIT_CRITICAL(&audio_lock_);
        if (sent != bytes) DropAccounting(bytes - sent, bytes);
    }

    // Rate-limited: a full buffer failed on every 1010-byte block, and the logging itself costs
    // the CPU that playback needs. Reports the running total, which is the number that matters.
    void DropAccounting(size_t dropped, size_t of) {
        const uint32_t total = drop_total_.fetch_add(dropped, std::memory_order_relaxed) + dropped;
        const int64_t now = esp_timer_get_time();
        if (now - drop_log_at_ < 1000 * 1000) return;   // at most one line per second
        const uint32_t lost = total - drop_reported_;
        drop_reported_ = total;
        drop_log_at_ = now;
        ESP_LOGW(TAG, "play buffer full — dropped %u/%u bytes (%u ms of audio lost)",
                 (unsigned)dropped, (unsigned)of, (unsigned)(lost / 32));
    }

    void AudioEnd() override {
        taskENTER_CRITICAL(&audio_lock_);
        if (reply_control_.Ended(session_token_.load())) audio_ended_.store(true, std::memory_order_release);
        taskEXIT_CRITICAL(&audio_lock_);
    }

    bool OnVoiceReply(uint32_t, uint8_t status) override {
        taskENTER_CRITICAL(&audio_lock_);
        const uint32_t token = session_token_.load(std::memory_order_acquire);
        const bool accepted = reply_control_.Post(token, status);
        if (accepted && status == 3) audio_ended_.store(true, std::memory_order_release);
        if (accepted && status != 2 && status != 3) playback_hold_.store(true, std::memory_order_release);
        taskEXIT_CRITICAL(&audio_lock_);
        return accepted;
    }

    // The agent's answer (and anything else it puts on the screen).
    void ShowText(const char* utf8) override {
        ESP_LOGI(TAG, "screen: %.80s", utf8 ? utf8 : "");
        voice::Ui::Instance().SetAnswer(utf8);
    }

    void OnLinkStatus(const agent_link_status_t& st) override {
        link_ready_.store(st.phase == AGENT_LINK_PHASE_READY, std::memory_order_release);
        if (st.phase != AGENT_LINK_PHASE_READY) playback_hold_.store(true, std::memory_order_release);
        voice::Ui::Instance().SetStatus(st);
    }

private:
    static void LogMemory(const char* stage) {
        constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        ESP_LOGI(TAG, "memory %s: free=%u largest=%u DMA=%u", stage,
                 (unsigned)heap_caps_get_free_size(caps),
                 (unsigned)heap_caps_get_largest_free_block(caps),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }

    void InitDisplay() {
        St7789LcdConfig c = {};
        c.spi_host      = DISPLAY_SPI_HOST;
        c.pin_sck       = DISPLAY_SCK_PIN;
        c.pin_mosi      = DISPLAY_MOSI_PIN;
        c.pin_cs        = DISPLAY_CS_PIN;
        c.pin_dc        = DISPLAY_DC_PIN;
        c.pin_rst       = DISPLAY_RST_PIN;
        c.pin_bl        = DISPLAY_BL_PIN;
        c.width         = DISPLAY_WIDTH;
        c.height        = DISPLAY_HEIGHT;
        c.pclk_hz       = DISPLAY_SPI_CLK_HZ;
        c.spi_mode      = DISPLAY_SPI_MODE;
        c.invert_color  = DISPLAY_INVERT_COLOR;
        c.stripe_rows   = 4;
        c.single_buffer = true;
#ifdef DISPLAY_HAS_VENDOR_CMDS
        c.vendor_cmds      = kVendorCmds;
        c.vendor_cmd_count = sizeof(kVendorCmds) / sizeof(kVendorCmds[0]);
#endif
        if (lcd_.Init(c) != ESP_OK) {
            ESP_LOGW(TAG, "LCD init failed — continuing without display");
            return;
        }
        lcd_ok_ = true;
        (void)lcd_.FillSolid(rgb565::kBlack);
        if (kColorTest) return;
        if (voice::Ui::Instance().Start(&lcd_) != ESP_OK) {
            ESP_LOGE(TAG, "UI start failed");
        }
    }

    // Paint solid colours in turn to exercise the panel init and driver without the UI.
    // Owns its own task so it keeps going after the board constructor returns.
    static void ColorTaskEntry(void* arg) { static_cast<AiPassportBoard*>(arg)->RunColorTest(); }

    void RunColorTest() {
        constexpr size_t kCount = sizeof kSwatches / sizeof kSwatches[0];
        ESP_LOGW(TAG, "COLOUR TEST: LVGL off, %u swatches, 2s each, looping", (unsigned)kCount);
        size_t i = 0;
        uint32_t round = 0;
        while (true) {
            ESP_LOGW(TAG, "COLOUR TEST: %-13s RGB565=0x%04X", kSwatchNames[i], kSwatches[i]);
            if (lcd_.FillSolid(kSwatches[i]) != ESP_OK) {
                ESP_LOGE(TAG, "COLOUR TEST: fill failed at %s", kSwatchNames[i]);
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            if (++i >= kCount) { i = 0; ESP_LOGW(TAG, "COLOUR TEST: round %u done", ++round); }
        }
    }

    void InitCodec() {
        Es8311Config c = {};
        c.i2c_port    = AUDIO_I2C_PORT;
        c.pin_sda     = AUDIO_I2C_SDA;
        c.pin_scl     = AUDIO_I2C_SCL;
        c.pin_mclk    = AUDIO_I2S_MCLK;
        c.pin_bclk    = AUDIO_I2S_BCLK;
        c.pin_ws      = AUDIO_I2S_WS;
        c.pin_din     = AUDIO_I2S_DIN;
        c.pin_dout    = AUDIO_I2S_DOUT;
        c.pin_pa_en   = AUDIO_PA_EN;
        c.es8311_addr = ES8311_ADDR;
        c.sample_rate = AUDIO_SAMPLE_RATE;
        c.mic_gain    = 30;
        c.out_volume  = volume_;   // the UP/DOWN task adjusts from here
        c.use_default_volume_curve = true;
        // Capture the microphone without the DAC reference.
        c.no_dac_ref  = true;
        // 5.0 V, not 0. With pa_voltage = 0 the driver substitutes 5.0 anyway, but saying it here
        // keeps the volume curve honest if that default ever changes.
        c.pa_voltage  = 5.0f;
        if (codec_.Init(c) != ESP_OK) {
            ESP_LOGE(TAG, "codec init failed");
            return;
        }
        (void)codec_.SetMute(true);

        play_buf_ = xStreamBufferCreate(kPlayBufBytes, kPlayFrameBytes);
        if (!play_buf_) {
            ESP_LOGE(TAG, "play buffer alloc failed");
            return;
        }
        // Tell the SDK what we actually hold, so event 0x20 throttles the App before this buffer
        // overflows. Without this the SDK assumes its 8s default, never sends PAUSE, and the App
        // pushes at full speed into a buffer that holds a quarter of a second: the reply stutters
        // and the overflow is dropped. 32 bytes per ms at PCM16/16 kHz/mono.
        if (xTaskCreate(&AiPassportBoard::PlayTaskEntry, "spk_play", 4096, this, 8, &play_task_) != pdPASS) {
            ESP_LOGE(TAG, "play task create failed");
            return;
        }
        agent_link_playback_set_buffer_level_callback([](void* ctx) -> size_t {
            auto* board = static_cast<AiPassportBoard*>(ctx);
            return xStreamBufferBytesAvailable(board->play_buf_);
        }, this);
        agent_link_playback_set_buffer_ms(static_cast<uint32_t>(kPlayBufBytes / 32));
        codec_ok_ = true;
    }

    void InitAdcButtons() {
        adc_oneshot_unit_init_cfg_t ucfg = {};
        ucfg.unit_id = BUTTON_ADC_UNIT;
        if (adc_oneshot_new_unit(&ucfg, &adc_) != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit create failed");
            return;
        }
        adc_oneshot_chan_cfg_t ch = {};
        ch.atten = ADC_ATTEN_DB_12;
        ch.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(adc_, BUTTON_ADC_CHANNEL, &ch) != ESP_OK) {
            ESP_LOGE(TAG, "ADC channel config failed");
            return;
        }
        adc_cali_curve_fitting_config_t cal = {};
        cal.unit_id  = BUTTON_ADC_UNIT;
        cal.chan     = BUTTON_ADC_CHANNEL;
        cal.atten    = ADC_ATTEN_DB_12;
        cal.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_curve_fitting(&cal, &adc_cali_) != ESP_OK) {
            ESP_LOGW(TAG, "ADC calibration failed — using raw thresholds");
        }
        adc_ok_ = true;
    }

    int ReadButtonMv() {
        if (!adc_ok_ || !adc_) return -1;
        int raw = 0;
        if (adc_oneshot_read(adc_, BUTTON_ADC_CHANNEL, &raw) != ESP_OK) return -1;
        if (adc_cali_) {
            int mv = 0;
            if (adc_cali_raw_to_voltage(adc_cali_, raw, &mv) == ESP_OK) return mv;
        }
        // Uncalibrated fallback: 12-bit full scale ≈ 3100 mV at 12 dB atten
        return (raw * 3100) / 4095;
    }

    Btn ReadButton() {
        const int mv = ReadButtonMv();
        if (mv < 0) return Btn::None;
        if (mv < BUTTON_MV_UP_MAX) return Btn::Up;
        if (mv < BUTTON_MV_DOWN_MAX) return Btn::Down;
        if (mv < BUTTON_MV_OK_MAX) return Btn::Ok;
        return Btn::None;
    }

    static void PlayTaskEntry(void* arg) { static_cast<AiPassportBoard*>(arg)->PlayLoop(); }
    static void PttTaskEntry(void* arg) { static_cast<AiPassportBoard*>(arg)->PttLoop(); }
    static void KeyTaskEntry(void* arg) { static_cast<AiPassportBoard*>(arg)->KeyLoop(); }

    // UP/DOWN step volume on press and repeat while held. OK belongs to the PTT task.
    void KeyLoop() {
        constexpr int64_t kDebounceUs = 20 * 1000;
        constexpr int64_t kLongPressUs = 500 * 1000;
        constexpr int64_t kRepeatUs   = 250 * 1000;   // repeats while held past the long press
        constexpr uint8_t kVolumeStep = 10;
        Btn held = Btn::None, candidate = Btn::None;
        int64_t candidate_at = 0, next_repeat = 0;

        while (true) {
            const Btn sampled = ReadButton();
            const int64_t now = esp_timer_get_time();
            if (sampled != candidate) {
                candidate = sampled;
                candidate_at = now;
            }
            if (candidate != held && now - candidate_at >= kDebounceUs) {
                held = candidate;
                next_repeat = 0;
                if (held == Btn::Up || held == Btn::Down) {
                    SetVolume(held == Btn::Up ? kVolumeStep : -kVolumeStep);
                    next_repeat = now + kLongPressUs;
                }
            } else if (sampled == held && next_repeat && now >= next_repeat) {
                SetVolume(held == Btn::Up ? kVolumeStep : -kVolumeStep);
                next_repeat = now + kRepeatUs;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    void SetVolume(int delta) {
        if (!codec_ok_) return;
        // Clamp, don't wrap. The modulo made 100 + 10 land on 8 and 0 - 10 land on 91, so holding
        // a key past the end dropped the volume instead of holding it there.
        const int before = volume_;
        volume_ += delta;
        if (volume_ > kMaxVolume) volume_ = kMaxVolume;
        if (volume_ < 0)        volume_ = 0;
        if (volume_ == before) return;   // already at the end; stop the key repeat's log spam
        (void)codec_.SetVolume(static_cast<uint8_t>(volume_));
        ESP_LOGI(TAG, "volume %d", volume_);
        char msg[24];
        snprintf(msg, sizeof msg, "Volume %d", volume_);
        voice::Ui::Instance().Toast(msg, 1200);
    }

    void PlayLoop() {
        int16_t buf[kPlayFrameBytes / sizeof(int16_t)];
        static const int16_t silence[kPlayFrameBytes / sizeof(int16_t)] = {};
        bool priming = true, played = false;
        size_t pending_bytes = 0;
        uint32_t generation = UINT32_MAX;
        uint32_t underruns = 0, partials = 0, write_errors = 0;
        uint32_t previous_received = 0;
        int64_t stats_at = esp_timer_get_time(), last_write = 0;
        while (true) {
            const int64_t now = esp_timer_get_time();
            const uint32_t token = session_token_.load(std::memory_order_acquire);
            const uint32_t next_generation = voice::TokenGeneration(token);
            if (generation != next_generation) {
                MutePlayback();
                if (played) {
                    // Replace all 90 ms of queued DMA audio while the DAC is muted.
                    for (unsigned i = 0; i < 5; ++i) {
                        if (codec_.WritePcm(silence, sizeof(silence) / sizeof(silence[0])) != ESP_OK) {
                            playback_error_.store(next_generation, std::memory_order_release);
                            break;
                        }
                    }
                }
                taskENTER_CRITICAL(&audio_lock_);
                xStreamBufferReset(play_buf_);
                play_generation_ = next_generation;
                audio_ended_.store(false, std::memory_order_release);
                last_audio_at_.store(0, std::memory_order_release);
                playback_done_.store(0);
                taskEXIT_CRITICAL(&audio_lock_);
                generation = next_generation;
                priming = true;
                played = false;
                last_write = 0;
                pending_bytes = 0;
            }
            const size_t queued = QueuedAudio() + pending_bytes;
            if (now - stats_at >= 2000 * 1000) {
                const uint32_t received = received_bytes_.load(std::memory_order_relaxed);
                const uint32_t span_ms = static_cast<uint32_t>((now - stats_at) / 1000);
                if (queued || received != previous_received || (last_write && now - last_write < 2000 * 1000)) {
                    ESP_LOGI(TAG, "play: queued=%ums rx=%uB/s under=%u partial=%u dropped=%uB write_err=%u prime=%d",
                             (unsigned)(queued / 32), (unsigned)((received - previous_received) * 1000ULL / span_ms),
                             (unsigned)underruns, (unsigned)partials, (unsigned)drop_total_.load(),
                             (unsigned)write_errors, (int)priming);
                }
                previous_received = received;
                stats_at = now;
            }
            if (playback_hold_.load(std::memory_order_acquire) || !link_ready_.load() ||
                !voice::Session::AcceptsAudio(token)) {
                MutePlayback();
                pause_ack_.store(pause_request_.load(std::memory_order_acquire), std::memory_order_release);
                vTaskDelay(1);
                continue;
            }
            const bool ended = audio_ended_.load(std::memory_order_acquire);
            const size_t need = played ? kReprimeBytes : kPrimeBytes;
            const int64_t arrived = last_audio_at_.load(std::memory_order_acquire);
            const bool tail = ended || (arrived && now - arrived >= 150 * 1000);
            if (voice::Session::DrainReady(token, ended, queued, last_write, now)) {
                MutePlayback();
                playback_done_.store(generation, std::memory_order_release);
                vTaskDelay(1);
                continue;
            }
            taskENTER_CRITICAL(&audio_lock_);
            const bool formal_requested = reply_control_.Started(token);
            taskEXIT_CRITICAL(&audio_lock_);
            if (!voice::Session::CanPlay(token, voice::Ui::Instance().SessionRendered(token), link_ready_.load(), playback_hold_.load(), formal_requested) ||
                (priming && queued < need && !(queued && tail))) {
                MutePlayback();
                vTaskDelay(1);
                continue;
            }
            taskENTER_CRITICAL(&audio_lock_);
            const size_t n = pending_bytes ? pending_bytes : xStreamBufferReceive(play_buf_, buf, sizeof(buf), 0);
            taskEXIT_CRITICAL(&audio_lock_);
            if (!n) {
                if (!last_write || now - last_write >= 90 * 1000) {
                    MutePlayback();
                    if (!ended && !priming) ++underruns;
                    priming = true;
                }
                vTaskDelay(1);
                continue;
            }
            taskENTER_CRITICAL(&audio_lock_);
            const bool still_can_play = session_token_.load(std::memory_order_acquire) == token &&
                voice::Session::CanPlay(token, voice::Ui::Instance().SessionRendered(token), link_ready_.load(),
                                       playback_hold_.load(), reply_control_.Started(token));
            taskEXIT_CRITICAL(&audio_lock_);
            if (!still_can_play) {
                pending_bytes = n;
                MutePlayback();
                vTaskDelay(1);
                continue;
            }
            if (priming) (void)codec_.SetMute(false);
            priming = false;
            if (n < sizeof(buf) && !ended) ++partials;
            if (codec_.WritePcm(buf, n / sizeof(int16_t)) != ESP_OK) {
                ++write_errors;
                MutePlayback();
                playback_error_.store(generation, std::memory_order_release);
            }
            pending_bytes = 0;
            last_write = esp_timer_get_time();
            played = true;
        }
    }

    size_t QueuedAudio() {
        taskENTER_CRITICAL(&audio_lock_);
        const size_t queued = xStreamBufferBytesAvailable(play_buf_);
        taskEXIT_CRITICAL(&audio_lock_);
        return queued;
    }

    void MutePlayback() {
        if (!codec_.Muted()) (void)codec_.SetMute(true);
    }

    bool FinishReply(uint32_t generation) {
        taskENTER_CRITICAL(&audio_lock_);
        const bool finished = playback_done_.load(std::memory_order_acquire) == generation &&
            audio_ended_.load(std::memory_order_acquire) && !xStreamBufferBytesAvailable(play_buf_) &&
            session_.Finish(generation);
        if (finished) playback_hold_.store(true, std::memory_order_release);
        taskEXIT_CRITICAL(&audio_lock_);
        return finished;
    }

    bool PauseSpeaker() {
        playback_hold_.store(true, std::memory_order_release);
        const uint32_t ticket = pause_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
        const int64_t until = esp_timer_get_time() + 1000 * 1000;
        while (pause_ack_.load(std::memory_order_acquire) != ticket) {
            if (esp_timer_get_time() >= until) return false;
            vTaskDelay(1);
        }
        return true;
    }

    void PublishSession() {
        static const char* const names[] = {"Disconnected", "Ready", "Listening", "Thinking", "Speaking"};
        const uint32_t previous = session_token_.load(std::memory_order_acquire);
        const uint32_t token = session_.Token();
        state_changed_at_ = esp_timer_get_time();
        voice::Ui::Instance().SetSession(token);
        taskENTER_CRITICAL(&audio_lock_);
        reply_control_.Bind(token);
        session_token_.store(token, std::memory_order_release);
        taskEXIT_CRITICAL(&audio_lock_);
        ESP_LOGI(TAG, "[voice] %s -> %s (generation=%u)", names[(unsigned)voice::TokenState(previous)],
                 names[(unsigned)session_.State()], (unsigned)voice::TokenGeneration(token));
    }

    // One 20 ms mic frame into the stream; returns the frame's level 0-100 for the meter.
    esp_err_t PumpMic(agent_stream_handle_t stream, int* level) {
        static int16_t buf[AUDIO_SAMPLE_RATE / 50];   // PTT task only
        size_t got = 0;
        const esp_err_t read = codec_.ReadPcm(buf, sizeof(buf) / sizeof(buf[0]), &got);
        if (read != ESP_OK || !got) return read == ESP_OK ? ESP_FAIL : read;
        const esp_err_t sent = agent_link_stream_write(stream, buf, got * sizeof(int16_t));
        if (sent != ESP_OK) return sent;
        double sum = 0;
        for (size_t i = 0; i < got; ++i) sum += static_cast<double>(buf[i]) * buf[i];
        const double rms = std::sqrt(sum / got) / 32768.0;
        const double db  = rms > 0 ? 20.0 * std::log10(rms) : -100.0;   // -60 dBFS..0 -> 0..100
        *level = static_cast<int>((db + 60.0) * 100.0 / 60.0);
        return ESP_OK;
    }

    // Hold OK to talk: open AGENT_STREAM_VOICE, push 20 ms PCM frames, close on release.
    void PttLoop() {
        bool wait_release = false;
        agent_stream_handle_t stream = nullptr;
        int64_t talk_since = 0;
        int64_t last_hint = 0;

        while (true) {
            const bool ready = link_ready_.load(std::memory_order_acquire);
            if (!ready && session_.State() != voice::SessionState::Disconnected) {
                if (stream) (void)agent_link_stream_close(stream, false);
                stream = nullptr;
                (void)codec_.StopMic();
                (void)PauseSpeaker();
            }
            if (session_.SetConnected(ready)) PublishSession();
            const bool pressed = (ReadButton() == Btn::Ok);
            const int64_t now = esp_timer_get_time();
            const uint32_t generation = voice::TokenGeneration(session_.Token());
            taskENTER_CRITICAL(&audio_lock_);
            const uint8_t replies = reply_control_.Take(session_.Token());
            taskEXIT_CRITICAL(&audio_lock_);
            if (replies & (voice::ReplyControl::Success | voice::ReplyControl::Failure)) {
                FailSession(stream, replies & voice::ReplyControl::Success ? "Done" : "Agent reply failed");
                stream = nullptr;
                wait_release = pressed;
                continue;
            }
            if ((replies & voice::ReplyControl::Start) && session_.Speak(generation)) PublishSession();
            if (replies & voice::ReplyControl::End) (void)session_.Drain(generation, now);
            const auto state = session_.State();
            if (state == voice::SessionState::Thinking || state == voice::SessionState::Speaking) {
                const esp_err_t error = agent_link_stream_error(AGENT_STREAM_VOICE);
                if (error != ESP_OK || playback_error_.load(std::memory_order_acquire) == generation) {
                    ESP_LOGW(TAG, "voice session failed: %s", esp_err_to_name(error));
                    FailSession(stream, error != ESP_OK ? "Voice send failed" : "Playback stopped");
                    stream = nullptr;
                    wait_release = pressed;
                    continue;
                }
                if (FinishReply(generation)) {
                    PublishSession();
                } else if (session_.DrainTimedOut(now)) {
                    ESP_LOGW(TAG, "reply drain timed out after 30 seconds");
                    (void)PauseSpeaker();
                    session_.Cancel();
                    PublishSession();
                } else if (session_.TimedOut(now)) {
                    FailSession(stream, "Reply timed out");
                    stream = nullptr;
                } else if (state == voice::SessionState::Speaking &&
                           !voice::Ui::Instance().SessionRendered(session_.Token()) && now - state_changed_at_ > 2000 * 1000) {
                    FailSession(stream, "Screen unavailable");
                    stream = nullptr;
                }
            }
            if (!pressed) wait_release = false;
            if (wait_release) {
                vTaskDelay(1);
                continue;
            }
            if (session_.State() != voice::SessionState::Listening) {
                if (!pressed) { vTaskDelay(1); continue; }
                vTaskDelay(pdMS_TO_TICKS(20));
                if (ReadButton() != Btn::Ok) continue;
                if (!ready) {
                    if (agent_link_confirm() == ESP_OK) {
                        voice::Ui::Instance().Toast("\xef\x80\x8c  Confirmed", 2000);
                    }
                    wait_release = true;
                    continue;
                }
                if (!PauseSpeaker()) {
                    FailSession(stream, "Playback busy");
                    wait_release = true;
                    continue;
                }
                session_.Listen();
                voice::Ui::Instance().SetAnswer("");
                PublishSession();
                playback_hold_.store(false, std::memory_order_release);
                if (codec_.StartMic() != ESP_OK) {
                    FailSession(stream, "Microphone unavailable");
                    wait_release = true;
                    continue;
                }
                if (agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &stream) != ESP_OK) {
                    FailSession(stream, "Can't talk right now");
                    stream = nullptr;
                    wait_release = true;
                    continue;
                }
                talk_since = esp_timer_get_time();
                last_hint = talk_since;
                ESP_LOGI(TAG, "PTT pressed, starting voice uplink");
            } else {
                int level = 0;
                const esp_err_t error = PumpMic(stream, &level);
                if (error != ESP_OK || playback_error_.load() == generation) {
                    ESP_LOGW(TAG, "recording stopped: %s", esp_err_to_name(error));
                    FailSession(stream, "Recording stopped");
                    stream = nullptr;
                    wait_release = true;
                    continue;
                }
                voice::Ui::Instance().SetLevel(level);
                const int64_t sampled_at = esp_timer_get_time();
                if (ReadButton() != Btn::Ok) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    if (ReadButton() != Btn::Ok) {
                        StopTalking(stream, sampled_at - talk_since >= kMinTalkUs);
                        stream = nullptr;
                        ESP_LOGI(TAG, "PTT released, ending voice");
                    }
                } else if (sampled_at - last_hint > kTapHintUs) {
                    last_hint = sampled_at;
                    voice::Ui::Instance().Toast("\xef\x80\xa8  Keep talking, release to send", 4000);
                }
            }
        }
    }

    void StopTalking(agent_stream_handle_t stream, bool send) {
        (void)codec_.StopMic();
        session_.Release(send, esp_timer_get_time());
        PublishSession();
        const esp_err_t error = agent_link_stream_close(stream, send);
        if (error != ESP_OK) FailSession(nullptr, "Voice send failed");
        else if (!send) voice::Ui::Instance().Toast("Hold OK while you talk", 3000);
    }

    void FailSession(agent_stream_handle_t stream, const char* message) {
        if (stream) (void)agent_link_stream_close(stream, false);
        (void)codec_.StopMic();
        (void)PauseSpeaker();
        session_.Cancel();
        voice::Ui::Instance().SetAnswer("");
        PublishSession();
        voice::Ui::Instance().Toast(message, 3000);
    }

    // One second of PCM with room for a full decoded L2CAP SDU.
    static constexpr size_t kPlayBufBytes = 32 * 1024;
    static constexpr size_t kPlayFrameBytes = 640;  // 20 ms
    static constexpr size_t kPrimeBytes = 192 * 32;
    static constexpr size_t kReprimeBytes = 128 * 32;

    Es8311Codec          codec_;
    St7789Lcd            lcd_;
    bool                 codec_ok_  = false;
    bool                 lcd_ok_    = false;
    bool                 adc_ok_    = false;
    StreamBufferHandle_t play_buf_  = nullptr;
    TaskHandle_t         play_task_ = nullptr;
    TaskHandle_t         ptt_task_  = nullptr;
    TaskHandle_t         key_task_  = nullptr;
    voice::Session       session_;
    voice::ReplyControl  reply_control_;
    portMUX_TYPE         audio_lock_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t             play_generation_ = 0;
    int64_t              state_changed_at_ = 0;
    std::atomic<uint32_t> session_token_{0};
    std::atomic<bool>     link_ready_{false};
    std::atomic<bool>     playback_hold_{false};
    std::atomic<uint32_t> pause_request_{0};
    std::atomic<uint32_t> pause_ack_{0};
    std::atomic<uint32_t> playback_done_{0};
    std::atomic<uint32_t> playback_error_{0};
    std::atomic<uint32_t> drop_total_{0};
    std::atomic<uint32_t> received_bytes_{0};
    std::atomic<int64_t>  last_audio_at_{0};
    std::atomic<bool>     audio_ended_{true};
    uint32_t             drop_reported_  = 0;   // already counted in a log line
    int64_t              drop_log_at_    = 0;   // esp_timer, so the log is at most 1/s
    int                  volume_    = 80;   // must match out_volume passed to InitCodec
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t    adc_cali_ = nullptr;
};

DECLARE_BOARD(AiPassportBoard);
