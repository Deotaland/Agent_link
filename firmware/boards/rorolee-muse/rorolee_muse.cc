// RoRoLee Muse (ESP32-S3): talk to an agent from the rorolee-s3 PCB.
//
// Same hardware as boards/rorolee-s3 (SH8501 AMOLED, ES8311/ES7210 codec, BQ27220 gauge, three
// buttons, motor). What this board adds is a screen and a talk loop:
//   - the link status as the SDK words it (agent_link_status_t), whatever the transport;
//   - hold BOOT to talk: the mic goes up an AGENT_STREAM_VOICE stream until release;
//   - whatever the agent sends to the screen (on_show_text) is the answer shown afterwards.
//
// It is written against agent_link only, so the transport is a menuconfig choice, not a code
// change. With Transport backend = Muse it is a Muse gadget: set up from the Muse app (a BOOT press
// confirms the pairing when the screen asks for it), and the answer is the Muse's reply.
//
// Buttons: BOOT = hold to talk (a press while the link is still being set up goes to
// agent_link_confirm()); VOL+/VOL- = speaker volume; hold VOL- for FORGET_HOLD_MS = forget the
// pairing and set the device up again (agent_link_forget()).

#include "board.h"
#include "bq27220.h"
#include "config.h"
#include "es_codec.h"
#include "sh8501_lk_panel.h"
#include "voice_ui.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "agent_link.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lvgl.h"   // LV_SYMBOL_* in toasts
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "RoRoLeeMuse"

namespace {

// Shorter than this, a BOOT press is a tap, not speech: the stream is abandoned, not sent.
constexpr int64_t kMinTalkUs = 400 * 1000;
constexpr int     kVolumeStep = 10;
constexpr const char* kNvsNs  = "voice";
constexpr const char* kNvsVol = "vol";

// Debounced, active-low button sampled every loop turn.
struct Button {
    gpio_num_t gpio;
    bool       pressed = false;
    uint8_t    stable  = 0;
    int64_t    since   = 0;     // when the current state began

    // Returns +1 on a press edge, -1 on a release edge, 0 otherwise.
    int Poll(int64_t now) {
        const bool level = gpio_get_level(gpio) == 0;
        if (level == pressed) { stable = 0; return 0; }
        if (++stable < 2) return 0;   // two consecutive samples (~40 ms) before an edge counts
        stable  = 0;
        pressed = level;
        since   = now;
        return pressed ? 1 : -1;
    }
};

}  // namespace

class RoRoLeeMuseBoard : public Board {
public:
    RoRoLeeMuseBoard() {
        BuildDeviceName();
        PowerOnRail();
        InitDisplay();
        InitCodec();
        InitFuelGauge();
        InitHaptic();
        StartUi();
        InitButtons();
        xTaskCreate(&RoRoLeeMuseBoard::StatusEntry, "voice_batt", 3072, this, 3, nullptr);
    }

    // "ROROLEE_<last 6 MAC hex>", as the production firmware and work-badge advertise: the
    // official App picks units out of a scan by it on BLE. (A Muse gadget advertises the name
    // the Muse app expects instead; the transport takes care of that.)
    const char* Name() const override { return name_; }

    // The production PCB: the model string an OTA image must claim (shared with rorolee-s3).
    const char* Model() const override { return "RRL-01"; }

    uint32_t Capabilities() const override {
        return AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_SCREEN | AGENT_CAP_BUTTON |
               AGENT_CAP_HAPTIC | AGENT_CAP_BATTERY;
    }

    void OnLinkStatus(const agent_link_status_t& st) override {
        voice::Ui::Instance().SetStatus(st);
    }

    // The agent's answer (and anything else it puts on the screen).
    void ShowText(const char* utf8) override {
        ESP_LOGI(TAG, "screen: %.80s", utf8 ? utf8 : "");
        voice::Ui::Instance().SetAnswer(utf8);
    }

    // Spoken replies, on transports that carry them: queue and return (this runs on the
    // transport's task); the play task feeds the codec. Dropped while the user is talking.
    void PlayAudio(const uint8_t* pcm16, size_t bytes) override {
        if (!play_buf_ || !pcm16 || bytes == 0 || talking_.load(std::memory_order_acquire)) return;
        size_t sent;
        {
            std::lock_guard<std::mutex> lk(play_mtx_);
            sent = xStreamBufferSend(play_buf_, pcm16, bytes, 0);
        }
        if (sent < bytes) ESP_LOGW(TAG, "play buffer full - dropped %u bytes", (unsigned)(bytes - sent));
    }

    void Vibrate(uint32_t ms) override { Buzz(ms); }

    int  GetBatteryLevel() override { return batt_pct_.load(std::memory_order_acquire); }
    bool IsCharging()      override { return charging_.load(std::memory_order_acquire); }

private:
    void BuildDeviceName() {
        uint8_t mac[6] = {};
        if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return;   // keep the compiled-in fallback
        std::snprintf(name_, sizeof(name_), "ROROLEE_%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

    void PowerOnRail() {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << POWER_CTRL_PIN;
        cfg.mode         = GPIO_MODE_OUTPUT;
        if (gpio_config(&cfg) != ESP_OK) { ESP_LOGE(TAG, "power gpio cfg failed"); return; }
        gpio_set_level(POWER_CTRL_PIN, POWER_CTRL_ACTIVE_HIGH ? 1 : 0);
        (void)gpio_hold_en(POWER_CTRL_PIN);
        vTaskDelay(pdMS_TO_TICKS(50));   // let the supply settle before talking to anything
    }

    void InitDisplay() {
        Sh8501LkConfig c = {};
        c.spi_host = DISPLAY_SPI_HOST;
        c.pin_sck  = DISPLAY_SCK_PIN;
        c.pin_mosi = DISPLAY_MOSI_PIN;
        c.pin_cs   = DISPLAY_CS_PIN;
        c.pin_dc   = DISPLAY_DC_PIN;
        c.pin_rst  = DISPLAY_RST_PIN;
        c.width    = DISPLAY_WIDTH;
        c.height   = DISPLAY_HEIGHT;
        c.pclk_hz  = DISPLAY_SPI_CLK_HZ;
        if (panel_.Init(c) != ESP_OK) { ESP_LOGE(TAG, "display init failed"); return; }
        (void)panel_.SetBrightness(DISPLAY_BRIGHTNESS);
    }

    void InitCodec() {
        EsCodecConfig cc = {};
        cc.i2c_port    = AUDIO_I2C_PORT;
        cc.pin_sda     = AUDIO_I2C_SDA;
        cc.pin_scl     = AUDIO_I2C_SCL;
        cc.pin_mclk    = AUDIO_I2S_MCLK;
        cc.pin_bclk    = AUDIO_I2S_BCLK;
        cc.pin_ws      = AUDIO_I2S_WS;
        cc.pin_din     = AUDIO_I2S_DIN;
        cc.pin_dout    = AUDIO_I2S_DOUT;
        cc.pin_pa_en   = AUDIO_PA_EN;
        cc.es7210_addr = ES7210_ADDR;
        cc.es8311_addr = ES8311_ADDR;
        cc.sample_rate = AUDIO_SAMPLE_RATE;
        cc.mic_gain    = AUDIO_MIC_GAIN;
        volume_        = LoadVolume();
        cc.out_volume  = volume_;
        if (codec_.Init(cc) != ESP_OK) { ESP_LOGE(TAG, "codec init failed"); return; }
        codec_ok_ = true;

        // 8 s of PCM16 in PSRAM: spoken replies arrive as fast as the link allows.
        play_buf_ = xStreamBufferCreateWithCaps(kPlayBufBytes, 1, MALLOC_CAP_SPIRAM);
        if (!play_buf_) {
            play_buf_ = xStreamBufferCreate(16 * 1024, 1);
            if (!play_buf_) { ESP_LOGE(TAG, "play buffer alloc failed"); return; }
            agent_link_playback_set_buffer_ms(16 * 1024 / 32);
        } else {
            agent_link_playback_set_buffer_ms(kPlayBufBytes / 32);   // 32 bytes per ms at 16 kHz mono
        }
        xTaskCreate(&RoRoLeeMuseBoard::PlayEntry, "voice_play", 4096, this, 5, nullptr);
    }

    void InitFuelGauge() {
        if (!codec_ok_) return;   // the codec created the shared I2C bus
        if (gauge_.Init(AUDIO_I2C_PORT, BQ27220_ADDR) != ESP_OK) {
            ESP_LOGW(TAG, "BQ27220 init failed - battery shown as unknown");
            return;
        }
        gauge_ok_ = true;
    }

    void InitHaptic() {
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << HAPTIC_PIN;
        c.mode         = GPIO_MODE_OUTPUT;
        if (gpio_config(&c) != ESP_OK) return;
        gpio_set_level(HAPTIC_PIN, HAPTIC_ACTIVE_HIGH ? 0 : 1);
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { gpio_set_level(HAPTIC_PIN, HAPTIC_ACTIVE_HIGH ? 0 : 1); };
        args.name     = "haptic_off";
        haptic_ok_ = esp_timer_create(&args, &haptic_timer_) == ESP_OK;
    }

    void Buzz(uint32_t ms) {
        if (!haptic_ok_ || ms == 0) return;
        if (ms > 2000) ms = 2000;
        esp_timer_stop(haptic_timer_);
        gpio_set_level(HAPTIC_PIN, HAPTIC_ACTIVE_HIGH ? 1 : 0);
        esp_timer_start_once(haptic_timer_, static_cast<uint64_t>(ms) * 1000);
    }

    void StartUi() {
        if (!panel_.Ready()) { ESP_LOGE(TAG, "no display - running headless"); return; }
        if (voice::Ui::Instance().Start(&panel_) != ESP_OK) ESP_LOGE(TAG, "UI start failed");
    }

    void InitButtons() {
        gpio_config_t c = {};
        c.pin_bit_mask = (1ULL << BUTTON_BOOT_PIN) | (1ULL << BUTTON_VOL_UP_PIN) | (1ULL << BUTTON_VOL_DOWN_PIN);
        c.mode         = GPIO_MODE_INPUT;
        c.pull_up_en   = GPIO_PULLUP_ENABLE;
        gpio_config(&c);
        xTaskCreate(&RoRoLeeMuseBoard::ButtonEntry, "voice_keys", 4096, this, 5, nullptr);
    }

    // ── Volume ────────────────────────────────────────────────────────────────────────────

    int LoadVolume() {
        // The board is built before agent_link starts its transport, which is what normally
        // brings NVS up. Opening it here is idempotent; a partition that needs erasing is left
        // for the transport to decide about (it holds the pairing), so the default volume wins.
        (void)nvs_flash_init();
        nvs_handle_t h;
        int8_t v = AUDIO_DEFAULT_VOLUME;
        if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_i8(h, kNvsVol, &v);
            nvs_close(h);
        }
        return v < 0 ? 0 : v > 100 ? 100 : v;
    }

    void StepVolume(int delta) {
        volume_ += delta;
        if (volume_ < 0) volume_ = 0;
        if (volume_ > 100) volume_ = 100;
        if (codec_ok_) (void)codec_.SetVolume(static_cast<uint8_t>(volume_));
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_set_i8(h, kNvsVol, static_cast<int8_t>(volume_));
            (void)nvs_commit(h);
            nvs_close(h);
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, LV_SYMBOL_VOLUME_MAX "  Volume %d%%", volume_);
        voice::Ui::Instance().Toast(buf, 1500);
    }

    // ── Talking ───────────────────────────────────────────────────────────────────────────

    bool StartTalking() {
        if (!codec_ok_) { voice::Ui::Instance().Toast("No microphone on this boot", 2000); return false; }
        if (codec_.StartMic() != ESP_OK) { ESP_LOGE(TAG, "mic start failed"); return false; }
        // Barge-in: drop the rest of the previous answer and play nothing while the mic is open.
        talking_.store(true, std::memory_order_release);
        if (play_buf_) {
            std::lock_guard<std::mutex> lk(play_mtx_);
            (void)xStreamBufferReset(play_buf_);   // only fails if it is already empty
        }
        if (agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &voice_) != ESP_OK) {
            ESP_LOGW(TAG, "voice stream would not open");
            talking_.store(false, std::memory_order_release);
            codec_.StopMic();
            voice::Ui::Instance().Toast("Can't talk right now", 2000);
            return false;
        }
        voice::Ui::Instance().SetListening(true);
        Buzz(30);
        ESP_LOGI(TAG, "talking");
        return true;
    }

    void StopTalking(bool send) {
        agent_link_stream_close(voice_, send);
        voice_ = nullptr;
        codec_.StopMic();
        talking_.store(false, std::memory_order_release);
        voice::Ui::Instance().SetListening(false);
        if (send) {
            voice::Ui::Instance().SetThinking();
            ESP_LOGI(TAG, "sent");
        } else {
            voice::Ui::Instance().Toast("Hold BOOT while you talk", 2000);
            ESP_LOGI(TAG, "too short, dropped");
        }
    }

    // One 20 ms mic frame into the stream; returns the frame's level 0-100 for the meter.
    int PumpMic() {
        static int16_t buf[AUDIO_SAMPLE_RATE / 50];   // button task only
        size_t got = 0;
        if (codec_.ReadPcm(buf, sizeof(buf) / sizeof(buf[0]), &got) != ESP_OK || got == 0) return 0;
        (void)agent_link_stream_write(voice_, buf, got * sizeof(int16_t));
        double sum = 0;
        for (size_t i = 0; i < got; ++i) sum += static_cast<double>(buf[i]) * buf[i];
        const double rms = std::sqrt(sum / got) / 32768.0;
        const double db  = rms > 0 ? 20.0 * std::log10(rms) : -100.0;   // -60 dBFS..0 -> 0..100
        return static_cast<int>((db + 60.0) * 100.0 / 60.0);
    }

    static void ButtonEntry(void* arg) { static_cast<RoRoLeeMuseBoard*>(arg)->ButtonLoop(); }

    void ButtonLoop() {
        Button boot{BUTTON_BOOT_PIN}, up{BUTTON_VOL_UP_PIN}, down{BUTTON_VOL_DOWN_PIN};
        bool    talking = false;
        int64_t talk_since = 0;
        bool    forget_armed = false;    // VOL- held past the point where a release is no longer a step
        int     forget_shown = -1;
        bool    forgot = false;

        while (true) {
            const int64_t now = esp_timer_get_time();

            switch (boot.Poll(now)) {
            case 1:
                if (agent_link_state() == AGENT_STATE_READY) {
                    talking = StartTalking();
                    talk_since = now;
                } else if (agent_link_confirm() == ESP_OK) {
                    // The link was waiting for proof someone is holding the device.
                    voice::Ui::Instance().Toast(LV_SYMBOL_OK "  Confirmed", 2000);
                    Buzz(60);
                }
                break;
            case -1:
                if (talking) StopTalking(now - talk_since >= kMinTalkUs);
                talking = false;
                break;
            default:
                break;
            }
            // The link can drop mid-sentence; end the stream rather than talk into nothing.
            if (talking && agent_link_state() != AGENT_STATE_READY) {
                StopTalking(false);
                talking = false;
            }

            if (up.Poll(now) == 1) StepVolume(kVolumeStep);

            const int d = down.Poll(now);
            if (d == -1) {
                if (!forget_armed) StepVolume(-kVolumeStep);
                forget_armed = false;
                forget_shown = -1;
            } else if (down.pressed && !forgot) {
                const int64_t held_ms = (now - down.since) / 1000;
                if (held_ms >= 1500) {
                    forget_armed = true;
                    const int left = static_cast<int>((FORGET_HOLD_MS - held_ms + 999) / 1000);
                    if (held_ms >= FORGET_HOLD_MS) {
                        voice::Ui::Instance().Toast("Resetting - set up again", 10000);
                        Buzz(200);
                        ESP_LOGW(TAG, "VOL- held: forgetting the pairing");
                        (void)agent_link_forget();
                        forgot = true;
                    } else if (left != forget_shown) {
                        forget_shown = left;
                        char buf[48];
                        std::snprintf(buf, sizeof buf, "Keep holding to reset (%d)", left);
                        voice::Ui::Instance().Toast(buf, 1500);
                    }
                }
            }

            if (talking) {
                voice::Ui::Instance().SetLevel(PumpMic());   // paces the loop: blocks ~20 ms
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }

    // ── Playback and battery ──────────────────────────────────────────────────────────────

    static void PlayEntry(void* arg) { static_cast<RoRoLeeMuseBoard*>(arg)->PlayLoop(); }

    void PlayLoop() {
        static int16_t buf[256];
        while (true) {
            const size_t n = xStreamBufferReceive(play_buf_, buf, sizeof(buf), portMAX_DELAY);
            if (n >= sizeof(int16_t)) codec_.WritePcm(buf, n / sizeof(int16_t));
        }
    }

    static void StatusEntry(void* arg) { static_cast<RoRoLeeMuseBoard*>(arg)->StatusLoop(); }

    // The one reader of the gauge: the UI and app_main's battery report both use the cache.
    void StatusLoop() {
        while (true) {
            const int  pct = gauge_ok_ ? gauge_.Soc() : -1;
            const bool chg = gauge_ok_ && gauge_.IsCharging();
            batt_pct_.store(pct, std::memory_order_release);
            charging_.store(chg, std::memory_order_release);
            voice::Ui::Instance().SetBattery(pct, chg);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    static constexpr size_t kPlayBufBytes = 256 * 1024;   // 8.192 s

    char                  name_[32] = "ROROLEE_MUSE";    // replaced with the MAC-suffixed name at boot
    Sh8501LkPanel         panel_;
    EsCodec               codec_;
    Bq27220               gauge_;
    bool                  codec_ok_  = false;
    bool                  gauge_ok_  = false;
    bool                  haptic_ok_ = false;
    int                   volume_    = AUDIO_DEFAULT_VOLUME;
    esp_timer_handle_t    haptic_timer_ = nullptr;
    StreamBufferHandle_t  play_buf_  = nullptr;
    std::mutex            play_mtx_;            // serializes writers, a stream buffer allows only one
    agent_stream_handle_t voice_     = nullptr;
    std::atomic<int>      batt_pct_{-1};
    std::atomic<bool>     charging_{false};
    std::atomic<bool>     talking_{false};      // mic open, replies are dropped
};

DECLARE_BOARD(RoRoLeeMuseBoard);
