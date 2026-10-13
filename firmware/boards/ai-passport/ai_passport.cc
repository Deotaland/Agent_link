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
#include "lvgl.h"   // LV_SYMBOL_* in toasts

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

// Volume is 0..kMaxVolume in steps of 10. The codec maps it onto -50 dB .. +32 dB, see
// Es8311Codec::ApplyVolumeCurve.
constexpr int kMaxVolume = 100;

// Pure-colour display test. Set to 1 to paint the panel solid colours in turn at boot, with LVGL
// left off, so what you see is the panel and the ST7789 driver alone. Keep in mind that it runs
// straight into FillSolid(), bypassing LVGL's flush, so it cannot show a byte-order or gamma
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
        InitDisplay();
        if (kColorTest) {
            xTaskCreate(&AiPassportBoard::ColorTaskEntry, "color_test", 3072, this, 4, nullptr);
            return;   // nothing else starts: the test owns the screen
        }
        InitCodec();
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
        if (!play_buf_ || !pcm16 || bytes == 0) return;
        received_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        last_audio_at_.store(esp_timer_get_time(), std::memory_order_release);
        audio_ended_.store(false, std::memory_order_release);
        // This runs on the BLE host task (L2CAP receive -> audio::Feed -> here), which is the
        // 6 KB NimBLE stack, so it must not block.
        //
        // All-or-nothing on purpose. xStreamBufferSend(…, 0) writes whatever fits and reports the
        // rest as unsent, which truncated replies mid-block - the log showed drops of 246/1010 and
        // 484/1010 bytes, i.e. a partial 505-sample block, and cutting a waveform in half is the
        // click you hear. A block that does not fit whole is refused instead, so the samples that
        // do play stay contiguous. With 0x20 flow control working this path should never trigger.
        const size_t room = xStreamBufferSpacesAvailable(play_buf_);
        if (bytes > room) {
            DropAccounting(bytes, bytes);
            return;
        }
        const size_t sent = xStreamBufferSend(play_buf_, pcm16, bytes, 0);
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
        // Deliberately does not touch the buffer. The App sending 0x05 status=3 means it has
        // stopped pushing, not that we have finished playing: up to one buffer of the tail is
        // still queued and the user has not heard it. Dropping it here would cut the end off
        // every reply.
        audio_ended_.store(true, std::memory_order_release);
    }

    // The agent's answer (and anything else it puts on the screen).
    void ShowText(const char* utf8) override {
        ESP_LOGI(TAG, "screen: %.80s", utf8 ? utf8 : "");
        voice::Ui::Instance().SetAnswer(utf8);
    }

    void OnLinkStatus(const agent_link_status_t& st) override {
        voice::Ui::Instance().SetStatus(st);
    }

private:
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
        c.stripe_rows   = 16;  // small DMA stripes — C3 has no PSRAM
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

    // Paint solid colours in turn, forever. LVGL never starts, so this exercises the panel init and
    // the ST7789 driver's blit path only - which is the point: no UI state, no font, no LVGL buffer.
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
        // FoloToy's own BSP sets no_dac_ref = true with the comment "Mono capture must use ADC,
        // not the DAC reference". Left false, REG44 = 0x58 and the ES8311 mixes an internal
        // reference built from the DAC output into the capture path - the speaker's own signal,
        // amplified by +32 dB of DAC headroom. That is audible two ways: the mic hears the reply
        // being played, and the DAC working that hard adds its own hiss on top of it.
        c.no_dac_ref  = true;
        // 5.0 V, not 0. With pa_voltage = 0 the driver substitutes 5.0 anyway, but saying it here
        // keeps the volume curve honest if that default ever changes.
        c.pa_voltage  = 5.0f;
        if (codec_.Init(c) != ESP_OK) {
            ESP_LOGE(TAG, "codec init failed");
            return;
        }
        codec_ok_ = true;
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
        bool priming = true, played = false;
        uint32_t underruns = 0, partials = 0, write_errors = 0;
        uint32_t previous_received = 0;
        int64_t stats_at = esp_timer_get_time(), last_write = 0;
        while (true) {
            const int64_t now = esp_timer_get_time();
            const size_t queued = xStreamBufferBytesAvailable(play_buf_);
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
            if (priming) {
                const size_t need = played ? kReprimeBytes : kPrimeBytes;
                const bool tail = audio_ended_.load(std::memory_order_acquire) ||
                    now - last_audio_at_.load(std::memory_order_acquire) >= 150 * 1000;
                if (queued < need && !(queued && tail)) {
                    if (last_write && now - last_write >= 120 * 1000 && !codec_.Muted()) {
                        (void)codec_.SetMute(true);
                    }
                    if (!queued && now - last_audio_at_.load() >= 500 * 1000) played = false;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                priming = false;
                (void)codec_.SetMute(false);
            }
            const size_t n = xStreamBufferReceive(play_buf_, buf, sizeof(buf), pdMS_TO_TICKS(20));
            if (!n) {
                if (!audio_ended_.load()) ++underruns;
                priming = true;
                continue;
            }
            if (n < sizeof(buf) && !audio_ended_.load()) ++partials;
            if (codec_.WritePcm(buf, n / sizeof(int16_t)) != ESP_OK) ++write_errors;
            last_write = esp_timer_get_time();
            played = true;
        }
    }

    // One 20 ms mic frame into the stream; returns the frame's level 0-100 for the meter.
    int PumpMic(agent_stream_handle_t voice) {
        static int16_t buf[AUDIO_SAMPLE_RATE / 50];   // PTT task only
        size_t got = 0;
        if (codec_.ReadPcm(buf, sizeof(buf) / sizeof(buf[0]), &got) != ESP_OK || got == 0) return 0;
        (void)agent_link_stream_write(voice, buf, got * sizeof(int16_t));
        double sum = 0;
        for (size_t i = 0; i < got; ++i) sum += static_cast<double>(buf[i]) * buf[i];
        const double rms = std::sqrt(sum / got) / 32768.0;
        const double db  = rms > 0 ? 20.0 * std::log10(rms) : -100.0;   // -60 dBFS..0 -> 0..100
        return static_cast<int>((db + 60.0) * 100.0 / 60.0);
    }

    // Hold OK to talk: open AGENT_STREAM_VOICE, push 20 ms PCM frames, close on release.
    void PttLoop() {
        bool talking = false;
        agent_stream_handle_t voice = nullptr;
        int64_t talk_since = 0;
        int64_t last_hint = 0;

        while (true) {
            const bool pressed = (ReadButton() == Btn::Ok);
            if (!talking) {
                if (!pressed) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                if (ReadButton() != Btn::Ok) continue;
                // A press while the link is still being set up confirms it (a no-op on BLE).
                if (agent_link_state() != AGENT_STATE_READY) {
                    if (agent_link_confirm() == ESP_OK) {
                        voice::Ui::Instance().Toast(LV_SYMBOL_OK "  Confirmed", 2000);
                    }
                    while (ReadButton() == Btn::Ok) vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                if (codec_.StartMic() != ESP_OK) {
                    ESP_LOGE(TAG, "mic start failed");
                    voice::Ui::Instance().Toast("Microphone unavailable", 3000);
                    continue;
                }
                if (agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &voice) != ESP_OK) {
                    ESP_LOGW(TAG, "voice stream would not open");
                    codec_.StopMic();
                    voice::Ui::Instance().Toast("Can't talk right now", 3000);
                    continue;
                }
                talking = true;
                talk_since = esp_timer_get_time();
                voice::Ui::Instance().SetListening(true);
                ESP_LOGI(TAG, "PTT pressed, starting voice uplink");
            } else {
                if (agent_link_state() != AGENT_STATE_READY) {
                    StopTalking(voice, false);
                    talking = false;
                    ESP_LOGW(TAG, "connection lost, ending voice");
                    continue;
                }
                voice::Ui::Instance().SetLevel(PumpMic(voice));   // paces the loop, ~20 ms
                const int64_t now = esp_timer_get_time();
                if (ReadButton() != Btn::Ok) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    if (ReadButton() != Btn::Ok) {
                        StopTalking(voice, now - talk_since >= kMinTalkUs);
                        talking = false;
                        ESP_LOGI(TAG, "PTT released, ending voice");
                    }
                } else if (now - last_hint > kTapHintUs) {
                    last_hint = now;
                    voice::Ui::Instance().Toast(LV_SYMBOL_AUDIO "  Keep talking, release to send", 4000);
                }
            }
        }
    }

    void StopTalking(agent_stream_handle_t voice, bool send) {
        agent_link_stream_close(voice, send);
        codec_.StopMic();
        voice::Ui::Instance().SetListening(false);
        if (send) {
            voice::Ui::Instance().SetThinking();
            ESP_LOGI(TAG, "sent");
        } else {
            voice::Ui::Instance().Toast("Hold OK while you talk", 3000);
            ESP_LOGI(TAG, "too short, dropped");
        }
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
