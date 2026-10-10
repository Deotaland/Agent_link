// M5Stack StopWatch (ESP32-S3R8): talk to an agent from M5Stack's round AMOLED watch.
//
//   - the link status as the SDK words it (agent_link_status_t), whatever the transport;
//   - hold the yellow key to talk: the mic goes up an AGENT_STREAM_VOICE stream until release;
//   - spoken replies play on the speaker; what the agent sends to the screen is shown as the answer.
//
// Written against agent_link only, so the transport is a menuconfig choice: BLE (the Deotaland App)
// or WiFi (hotspot setup, then an activation code typed into the console).
//
// Keys: yellow = hold to talk (a press while the link is still being set up goes to
// agent_link_confirm()); blue = volume in steps of 20, a beep at the new level; hold blue for
// FORGET_HOLD_MS = forget the pairing (agent_link_forget()). The power key belongs to the PMIC.
//
// Most of the board hangs off one I2C bus: the M5IOE1 expander switches the panel's supply and
// reset, the codec's supply, the speaker amp and the motor; the M5PM1 reports the battery.

#include "board.h"
#include "co5300_panel.h"
#include "config.h"
#include "es8311_audio.h"
#include "m5ioe1.h"
#include "m5pm1.h"
#include "watch_ui.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "agent_link.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lvgl.h"   // LV_SYMBOL_* in toasts
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "M5StopWatch"

namespace {

// Shorter than this, a press is a tap, not speech: the stream is abandoned, not sent.
constexpr int64_t     kMinTalkUs  = 400 * 1000;
constexpr int         kVolumeStep = 20;
constexpr uint32_t    kAmpIdleMs  = 1500;   // amp off after this long with nothing to play
constexpr const char* kNvsNs  = "voice";
constexpr const char* kNvsVol = "vol";

// FreeRTOS runs at 100 Hz here: under 10 ms, pdMS_TO_TICKS() would be no wait at all.
TickType_t Ticks(uint32_t ms) {
    const TickType_t t = pdMS_TO_TICKS(ms);
    return t ? t : 1;
}

// Debounced, active-low key sampled every loop turn.
struct Key {
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

// Linear between 3.3 V and 4.15 V, as M5's own firmware estimates it.
int BatteryPercent(uint16_t mv) {
    const int pct = (static_cast<int>(mv) - 3300) * 100 / (4150 - 3300);
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

}  // namespace

class M5StopWatchBoard : public Board {
public:
    M5StopWatchBoard() {
        InitI2c();
        InitPower();
        InitExpander();
        InitDisplay();
        InitAudio();
        InitHaptic();
        StartUi();
        InitKeys();
        xTaskCreate(&M5StopWatchBoard::StatusEntry, "sw_status", 3072, this, 3, nullptr);
    }

    // Also the WiFi setup hotspot's prefix: "M5StopWatch-XXXX".
    const char* Name() const override { return "M5StopWatch"; }

    // The string every OTA image for this hardware must claim.
    const char* Model() const override { return "M5-STOPWATCH"; }

    uint32_t Capabilities() const override {
        return AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_SCREEN | AGENT_CAP_BUTTON |
               AGENT_CAP_HAPTIC | AGENT_CAP_BATTERY;
    }

    // Used on WiFi, ignored on BLE. Static because the SDK keeps the pointer.
    const agent_platform_t* Platform() const override {
        static const agent_platform_t kPlatform = {
            .base_url   = CLOUD_BASE_URL,
            .product_id = CLOUD_PRODUCT_ID,
            .chip_type  = CLOUD_CHIP_TYPE,
        };
        return &kPlatform;
    }

    void OnLinkStatus(const agent_link_status_t& st) override { watch::Ui::Instance().SetStatus(st); }

    // The agent's answer (and anything else it puts on the screen).
    void ShowText(const char* utf8) override {
        ESP_LOGI(TAG, "screen: %.80s", utf8 ? utf8 : "");
        watch::Ui::Instance().SetAnswer(utf8);
    }

    // Spoken replies: queue and return (this runs on the transport's task); the play task feeds
    // the codec. Dropped while the user is talking.
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
    // ── Bring-up ──────────────────────────────────────────────────────────────────────────

    void InitI2c() {
        i2c_master_bus_config_t bus = {};
        bus.i2c_port          = SYS_I2C_PORT;
        bus.sda_io_num        = SYS_I2C_SDA;
        bus.scl_io_num        = SYS_I2C_SCL;
        bus.clk_source        = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = true;
        i2c_master_bus_handle_t handle = nullptr;
        i2c_ok_ = i2c_new_master_bus(&bus, &handle) == ESP_OK;
        if (!i2c_ok_) ESP_LOGE(TAG, "I2C bus failed: no screen, audio or battery");
    }

    void InitPower() {
        if (!i2c_ok_) return;
        if (pm1_.Init(SYS_I2C_PORT, PM1_ADDR) != ESP_OK) {
            ESP_LOGW(TAG, "PMIC not found - battery shown as unknown");
            return;
        }
        pm1_ok_ = true;
        // ESP32 rail, RTC/IMU rail and charger on, as M5's own firmware leaves them.
        (void)pm1_.EnablePower(M5Pm1::kPwrCharge | M5Pm1::kPwrDcdc | M5Pm1::kPwrLdo | M5Pm1::kPwrLed);
        (void)pm1_.SetGpioInput(PM1_CHG_STAT_GPIO);
    }

    void InitExpander() {
        if (!i2c_ok_) return;
        if (ioe_.Init(SYS_I2C_PORT, IOE_ADDR) != ESP_OK) {
            ESP_LOGE(TAG, "IO expander not found - no screen power, no audio");
            return;
        }
        ioe_ok_ = true;
        // The levels M5's own firmware sets at boot; audio power and the amp come on later.
        (void)ioe_.SetOutput(IOE_MUX_CTR, true);
        (void)ioe_.SetOutput(IOE_AUDIO_EN, false);
        (void)ioe_.SetOutput(IOE_SPK_EN, false);
        (void)ioe_.SetOutput(IOE_PANEL_PWR, true);
        (void)ioe_.SetOutput(IOE_TP_RST, true);
        (void)ioe_.SetOutput(IOE_PANEL_RST, true);
        vTaskDelay(Ticks(10));
        // Reset the panel and the touch controller together.
        (void)ioe_.Write(IOE_PANEL_RST, false);
        (void)ioe_.Write(IOE_TP_RST, false);
        vTaskDelay(Ticks(10));
        (void)ioe_.Write(IOE_PANEL_RST, true);
        (void)ioe_.Write(IOE_TP_RST, true);
        vTaskDelay(Ticks(20));
    }

    void InitDisplay() {
        Co5300Config c = {};
        c.bus         = Co5300Bus::kQspi;
        c.rst_gpio    = -1;   // reset through the IO expander, above
        c.width       = DISPLAY_WIDTH;
        c.height      = DISPLAY_HEIGHT;
        c.spi_host    = DISPLAY_SPI_HOST;
        c.pin_sclk    = DISPLAY_SCLK_PIN;
        c.pin_d0      = DISPLAY_D0_PIN;
        c.pin_d1      = DISPLAY_D1_PIN;
        c.pin_d2      = DISPLAY_D2_PIN;
        c.pin_d3      = DISPLAY_D3_PIN;
        c.pin_cs      = DISPLAY_CS_PIN;
        c.pclk_hz     = DISPLAY_QSPI_CLK_HZ;
        c.x_gap       = DISPLAY_X_GAP;
        c.y_gap       = DISPLAY_Y_GAP;
        c.bgr         = DISPLAY_BGR;
        c.stripe_rows = DISPLAY_STRIPE_ROWS;
        if (panel_.Init(c) != ESP_OK) {
            ESP_LOGE(TAG, "display init failed");
            return;
        }
        (void)panel_.SetBrightness(DISPLAY_BRIGHTNESS);
    }

    void InitAudio() {
        if (!ioe_ok_) return;   // the codec's supply is on the expander
        (void)ioe_.Write(IOE_AUDIO_EN, true);
        vTaskDelay(Ticks(20));  // supply up before the codec is spoken to

        Es8311Config c = {};
        c.i2c_port    = SYS_I2C_PORT;
        c.pin_sda     = GPIO_NUM_NC;   // the board's bus
        c.pin_scl     = GPIO_NUM_NC;
        c.pin_mclk    = AUDIO_I2S_MCLK;
        c.pin_bclk    = AUDIO_I2S_BCLK;
        c.pin_ws      = AUDIO_I2S_WS;
        c.pin_din     = AUDIO_I2S_DIN;
        c.pin_dout    = AUDIO_I2S_DOUT;
        c.pin_pa_en   = GPIO_NUM_NC;   // the amp is on the expander, switched by the play task
        c.es8311_addr = ES8311_ADDR;
        c.i2s_port    = AUDIO_I2S_PORT;
        c.sample_rate = AUDIO_SAMPLE_RATE;
        c.mic_gain    = AUDIO_MIC_GAIN;
        volume_       = LoadVolume();
        c.out_volume  = volume_;
        if (codec_.Init(c) != ESP_OK) {
            ESP_LOGE(TAG, "codec init failed");
            return;
        }
        codec_ok_ = true;

        // Spoken replies arrive as fast as the link allows: 8 s of PCM16 in PSRAM.
        play_buf_ = xStreamBufferCreateWithCaps(AUDIO_PLAY_BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
        if (!play_buf_) {
            ESP_LOGE(TAG, "play buffer alloc failed");
            return;
        }
        agent_link_playback_set_buffer_ms(AUDIO_PLAY_BUF_BYTES / 32);   // 32 bytes per ms at 16 kHz mono
        xTaskCreate(&M5StopWatchBoard::PlayEntry, "sw_play", 4096, this, 5, nullptr);
    }

    void InitHaptic() {
        if (!ioe_ok_) return;
        (void)ioe_.SetPwm(MOTOR_PWM_CHANNEL, 0, false);
        (void)ioe_.SetPwmFrequency(MOTOR_PWM_HZ);
        (void)ioe_.SetOutput(IOE_MOTOR, false);
        haptic_q_ = xQueueCreate(1, sizeof(uint32_t));
        if (!haptic_q_) return;
        xTaskCreate(&M5StopWatchBoard::HapticEntry, "sw_haptic", 2560, this, 4, nullptr);
    }

    void StartUi() {
        if (!panel_.Ready()) { ESP_LOGE(TAG, "no display - running headless"); return; }
        if (watch::Ui::Instance().Start(&panel_) != ESP_OK) ESP_LOGE(TAG, "UI start failed");
    }

    void InitKeys() {
        gpio_config_t c = {};
        c.pin_bit_mask = (1ULL << KEY_TALK_PIN) | (1ULL << KEY_VOLUME_PIN);
        c.mode         = GPIO_MODE_INPUT;
        c.pull_up_en   = GPIO_PULLUP_ENABLE;
        gpio_config(&c);
        xTaskCreate(&M5StopWatchBoard::KeyEntry, "sw_keys", 4096, this, 5, nullptr);
    }

    // ── Haptic ────────────────────────────────────────────────────────────────────────────

    // Any task, never blocks: the motor task does the I2C.
    void Buzz(uint32_t ms) {
        if (haptic_q_ && ms) xQueueOverwrite(haptic_q_, &ms);
    }

    static void HapticEntry(void* arg) { static_cast<M5StopWatchBoard*>(arg)->HapticLoop(); }

    void HapticLoop() {
        uint32_t ms = 0;
        while (true) {
            if (xQueueReceive(haptic_q_, &ms, portMAX_DELAY) != pdTRUE) continue;
            (void)ioe_.SetPwm(MOTOR_PWM_CHANNEL, MOTOR_DUTY, true);
            // A new request while running restarts the timer.
            do {
                if (ms > 2000) ms = 2000;
            } while (xQueueReceive(haptic_q_, &ms, Ticks(ms)) == pdTRUE);
            (void)ioe_.SetPwm(MOTOR_PWM_CHANNEL, 0, false);
        }
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

    // One key, so the volume cycles: 20, 40 ... 100, then 20 again.
    void StepVolume() {
        volume_ = volume_ >= 100 ? kVolumeStep : (volume_ / kVolumeStep + 1) * kVolumeStep;
        if (codec_ok_) (void)codec_.SetVolume(static_cast<uint8_t>(volume_));
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_set_i8(h, kNvsVol, static_cast<int8_t>(volume_));
            (void)nvs_commit(h);
            nvs_close(h);
        }
        char buf[32];
        std::snprintf(buf, sizeof buf, LV_SYMBOL_VOLUME_MAX "  Volume %d%%", volume_);
        watch::Ui::Instance().Toast(buf, 1500);
        Beep();
    }

    // 80 ms at 880 Hz through the play buffer, so it sounds at the new volume.
    void Beep() {
        if (!play_buf_ || talking_.load(std::memory_order_acquire)) return;
        constexpr float kStep = 6.2831853f * 880.0f / AUDIO_SAMPLE_RATE;   // radians per sample
        int16_t chunk[160];   // 10 ms
        uint32_t n = 0;
        std::lock_guard<std::mutex> lk(play_mtx_);
        for (int c = 0; c < 8; ++c) {
            for (int16_t& s : chunk) s = static_cast<int16_t>(6000.0f * sinf(kStep * static_cast<float>(n++)));
            if (xStreamBufferSend(play_buf_, chunk, sizeof chunk, 0) < sizeof chunk) break;
        }
    }

    // ── Talking ───────────────────────────────────────────────────────────────────────────

    bool StartTalking() {
        if (!codec_ok_) { watch::Ui::Instance().Toast("No microphone on this boot", 2000); return false; }
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
            watch::Ui::Instance().Toast("Can't talk right now", 2000);
            return false;
        }
        watch::Ui::Instance().SetListening(true);
        Buzz(30);
        ESP_LOGI(TAG, "talking");
        return true;
    }

    void StopTalking(bool send) {
        agent_link_stream_close(voice_, send);
        voice_ = nullptr;
        codec_.StopMic();
        talking_.store(false, std::memory_order_release);
        watch::Ui::Instance().SetListening(false);
        if (send) {
            watch::Ui::Instance().SetThinking();
            ESP_LOGI(TAG, "sent");
        } else {
            watch::Ui::Instance().Toast("Hold the key while you talk", 2000);
            ESP_LOGI(TAG, "too short, dropped");
        }
    }

    // One 20 ms mic frame into the stream; returns the frame's level 0-100 for the meter.
    int PumpMic() {
        static int16_t buf[AUDIO_SAMPLE_RATE / 50];   // key task only
        size_t got = 0;
        if (codec_.ReadPcm(buf, sizeof(buf) / sizeof(buf[0]), &got) != ESP_OK || got == 0) return 0;
        (void)agent_link_stream_write(voice_, buf, got * sizeof(int16_t));
        double sum = 0;
        for (size_t i = 0; i < got; ++i) sum += static_cast<double>(buf[i]) * buf[i];
        const double rms = std::sqrt(sum / got) / 32768.0;
        const double db  = rms > 0 ? 20.0 * std::log10(rms) : -100.0;   // -60 dBFS..0 -> 0..100
        return static_cast<int>((db + 60.0) * 100.0 / 60.0);
    }

    // The talk key while the link is not READY: a transport waiting for a press on the device
    // (Muse) takes it as the confirmation; otherwise say why nothing happens.
    void TalkWhileNotReady() {
        if (agent_link_confirm() == ESP_OK) {
            watch::Ui::Instance().Toast(LV_SYMBOL_OK "  Confirmed", 2000);
            Buzz(60);
            return;
        }
        agent_link_status_t st = {};
        agent_link_get_status(&st);
        watch::Ui::Instance().Toast(st.phase == AGENT_LINK_PHASE_CONNECTED ? "No voice on this link yet"
                                                                           : "Not connected yet", 2000);
    }

    static void KeyEntry(void* arg) { static_cast<M5StopWatchBoard*>(arg)->KeyLoop(); }

    void KeyLoop() {
        Key talk{KEY_TALK_PIN}, vol{KEY_VOLUME_PIN};
        bool    talking = false;
        int64_t talk_since = 0;
        bool    forget_armed = false;   // blue held past the point where a release is no longer a step
        int     forget_shown = -1;
        bool    forgot = false;

        while (true) {
            const int64_t now = esp_timer_get_time();

            switch (talk.Poll(now)) {
            case 1:
                watch::Ui::Instance().Wake();
                if (agent_link_state() == AGENT_STATE_READY) {
                    talking = StartTalking();
                    talk_since = now;
                } else {
                    TalkWhileNotReady();
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

            const int v = vol.Poll(now);
            if (v == 1) {
                watch::Ui::Instance().Wake();
            } else if (v == -1) {
                if (!forget_armed) StepVolume();
                forget_armed = false;
                forget_shown = -1;
            } else if (vol.pressed && !forgot) {
                const int64_t held_ms = (now - vol.since) / 1000;
                if (held_ms >= 1500) {
                    forget_armed = true;
                    const int left = static_cast<int>((FORGET_HOLD_MS - held_ms + 999) / 1000);
                    if (held_ms >= FORGET_HOLD_MS) {
                        watch::Ui::Instance().Toast("Resetting - set up again", 10000);
                        Buzz(200);
                        ESP_LOGW(TAG, "blue key held: forgetting the pairing");
                        (void)agent_link_forget();
                        forgot = true;
                    } else if (left != forget_shown) {
                        forget_shown = left;
                        char buf[48];
                        std::snprintf(buf, sizeof buf, "Hold to reset (%d)", left);
                        watch::Ui::Instance().Toast(buf, 1500);
                    }
                }
            }

            if (talking) {
                watch::Ui::Instance().SetLevel(PumpMic());   // paces the loop: blocks ~20 ms
            } else {
                vTaskDelay(Ticks(20));
            }
        }
    }

    // ── Playback and battery ──────────────────────────────────────────────────────────────

    static void PlayEntry(void* arg) { static_cast<M5StopWatchBoard*>(arg)->PlayLoop(); }

    // The amp hisses and draws current when idle, so it is on only while there is sound.
    void PlayLoop() {
        static int16_t buf[256];
        bool amp = false;
        while (true) {
            const size_t n = xStreamBufferReceive(play_buf_, buf, sizeof(buf),
                                                  amp ? Ticks(kAmpIdleMs) : portMAX_DELAY);
            if (n < sizeof(int16_t)) {
                if (amp) {
                    (void)ioe_.Write(IOE_SPK_EN, false);
                    amp = false;
                }
                continue;
            }
            if (!amp) {
                (void)ioe_.Write(IOE_SPK_EN, true);
                amp = true;
                vTaskDelay(Ticks(20));   // let the amp come up before the first samples
            }
            codec_.WritePcm(buf, n / sizeof(int16_t));
        }
    }

    static void StatusEntry(void* arg) { static_cast<M5StopWatchBoard*>(arg)->StatusLoop(); }

    // The one reader of the PMIC: the UI and app_main's battery report both use the cache.
    void StatusLoop() {
        while (true) {
            int  pct = -1;
            bool chg = false;
            uint16_t mv = 0;
            bool stat = true;
            if (pm1_ok_ && pm1_.BatteryMv(&mv) == ESP_OK && mv > 0) pct = BatteryPercent(mv);
            if (pm1_ok_ && pm1_.ReadGpio(PM1_CHG_STAT_GPIO, &stat) == ESP_OK) chg = !stat;
            batt_pct_.store(pct, std::memory_order_release);
            charging_.store(chg, std::memory_order_release);
            watch::Ui::Instance().SetBattery(pct, chg);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    M5Pm1                 pm1_;
    M5Ioe1                ioe_;
    Co5300Panel           panel_;
    Es8311Codec           codec_;
    bool                  i2c_ok_   = false;
    bool                  pm1_ok_   = false;
    bool                  ioe_ok_   = false;
    bool                  codec_ok_ = false;
    int                   volume_   = AUDIO_DEFAULT_VOLUME;
    QueueHandle_t         haptic_q_ = nullptr;
    StreamBufferHandle_t  play_buf_ = nullptr;
    std::mutex            play_mtx_;            // serializes writers, a stream buffer allows only one
    agent_stream_handle_t voice_    = nullptr;
    std::atomic<int>      batt_pct_{-1};
    std::atomic<bool>     charging_{false};
    std::atomic<bool>     talking_{false};      // mic open, replies are dropped
};

DECLARE_BOARD(M5StopWatchBoard);
