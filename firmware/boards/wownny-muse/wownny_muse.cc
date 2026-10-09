// WOWNNY Muse (ESP32-S3).
//
// WOWNNY is the rorolee PCB with a GC9D01 160x160 round TFT instead of the SH8501 AMOLED. Both
// revisions, WWY-01 and WWY-01-old, build from this file; config.h has the differences.
//
//   - Link status comes from agent_link_status_t, so the board works on any transport.
//   - Hold BOOT to talk: mic audio goes out on an AGENT_STREAM_VOICE stream until release.
//   - Spoken answers (on_audio_out) are played while the avatar talks. Text-only answers
//     (on_show_text) are not shown; the avatar talks for the estimated reading time.
//
// With Transport backend = Muse the device pairs from the Muse app (BOOT confirms the pairing),
// and replies are spoken if the build has a MiniMax key.
//
// Buttons: BOOT hold = talk, or confirm pairing while the link is being set up.
// VOL+/VOL- = volume; hold either one to open the settings menu (VOL+/VOL- move, BOOT selects,
// BOOT held 1 s / Exit / 15 s idle closes it).

#include "board.h"
#include "bq27220.h"
#include "config.h"
#include "es_codec.h"
#include "gc9d01_panel.h"
#include "voice_ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "agent_link.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lvgl.h"   // LV_SYMBOL_*
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "WownnyMuse"

namespace {

constexpr int64_t kMinTalkUs = 400 * 1000;           // shorter presses are dropped, not sent
constexpr int     kVolumeStep = 10;
constexpr int     kVolumeSteps[] = {10, 25, 40, 55, 70, 85, 100};   // menu Volume cycles these
constexpr int64_t kMenuHoldUs = 800 * 1000;          // hold a volume key to open the menu
constexpr int64_t kMenuExitHoldUs = 1000 * 1000;     // hold BOOT in the menu to close it
constexpr int64_t kMenuIdleUs = 15LL * 1000 * 1000;
constexpr const char* kNvsNs  = "voice";
constexpr const char* kNvsVol = "vol";
constexpr const char* kNvsSpk = "spk";

// Speech is over after AudioEnd() and an empty buffer, or after this long without audio
// (the next part of a reply can take a while to arrive).
constexpr int kSpeechGapMs = 4000;

// RMS level 0-100: -50 dBFS -> 0, -10 dBFS -> 100.
int Loudness(const int16_t* pcm, size_t n) {
    if (!n) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += static_cast<double>(pcm[i]) * pcm[i];
    const double rms = std::sqrt(sum / static_cast<double>(n)) / 32768.0;
    const double db  = rms > 0 ? 20.0 * std::log10(rms) : -100.0;
    const int pct = static_cast<int>((db + 50.0) * 100.0 / 40.0);
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// on_show_text carries more than answers: the Muse transport sends the transcript in quotes and
// turn errors in capitals.
enum class Said { kHeard, kFailed, kAnswer };

Said Classify(const char* s) {
    const size_t n = s ? std::strlen(s) : 0;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') return Said::kHeard;
    bool capitals = false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80 || (c >= 'a' && c <= 'z')) return Said::kAnswer;
        if (c >= 'A' && c <= 'Z') capitals = true;
    }
    return capitals ? Said::kFailed : Said::kAnswer;
}

// Debounced active-low button, polled from the button loop.
struct Button {
    gpio_num_t gpio;
    bool       pressed = false;
    uint8_t    stable  = 0;
    int64_t    since   = 0;     // time of the last edge

    // +1 on press, -1 on release, 0 otherwise.
    int Poll(int64_t now) {
        const bool level = gpio_get_level(gpio) == 0;
        if (level == pressed) { stable = 0; return 0; }
        if (++stable < 2) return 0;   // two samples in a row (~40 ms)
        stable  = 0;
        pressed = level;
        since   = now;
        return pressed ? 1 : -1;
    }
};

}  // namespace

class WownnyMuseBoard : public Board {
public:
    WownnyMuseBoard() {
        BuildDeviceName();
        gpio_deep_sleep_hold_dis();   // a previous firmware may have left pads held
#ifdef POWER_CTRL_PIN
        PowerOnRail();
#endif
        InitDisplay();
        InitCodec();
        InitFuelGauge();
        InitHaptic();
        StartUi();
        InitButtons();
        xTaskCreate(&WownnyMuseBoard::StatusEntry, "voice_batt", 3072, this, 3, nullptr);
    }

    // Same name as the production firmware; the App looks for it when scanning on BLE.
    // (On the Muse transport the BLE name is set by the transport.)
    const char* Name() const override { return name_; }

    // Checked against every OTA image: the two revisions use different display pins.
    const char* Model() const override { return WOWNNY_OLD_BOARD ? "WWY-01-old" : "WWY-01"; }

    uint32_t Capabilities() const override {
        return AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_SCREEN | AGENT_CAP_BUTTON |
               AGENT_CAP_HAPTIC | AGENT_CAP_BATTERY;
    }

    void OnLinkStatus(const agent_link_status_t& st) override {
        voice::Ui::Instance().SetStatus(st);
    }

    void ShowText(const char* utf8) override {
        ESP_LOGI(TAG, "screen: %.80s", utf8 ? utf8 : "");
        switch (Classify(utf8)) {
        case Said::kHeard:
            break;   // keep thinking
        case Said::kFailed:
            voice::Ui::Instance().EndTurn();
            voice::Ui::Instance().Toast(LV_SYMBOL_WARNING, 2500, voice::Tone::kError);
            break;
        case Said::kAnswer:
            voice::Ui::Instance().SetAnswer(utf8);
            break;
        }
    }

    // Called on the transport's task: only queue. Dropped while the mic is open (a new question
    // interrupts the answer) and when the speaker is turned off in the menu.
    void PlayAudio(const uint8_t* pcm16, size_t bytes) override {
        if (!play_buf_ || !pcm16 || bytes == 0) return;
        if (talking_.load(std::memory_order_acquire) || !speaker_on_.load(std::memory_order_acquire)) return;
        if (!speech_.exchange(true, std::memory_order_acq_rel)) {   // first chunk of an answer
            speech_end_.store(false, std::memory_order_release);
            voice::Ui::Instance().SpeechStart();
        }
        size_t sent;
        {
            std::lock_guard<std::mutex> lk(play_mtx_);
            sent = xStreamBufferSend(play_buf_, pcm16, bytes, 0);
        }
        if (sent < bytes) ESP_LOGW(TAG, "play buffer full - dropped %u bytes", (unsigned)(bytes - sent));
    }

    void AudioEnd() override { speech_end_.store(true, std::memory_order_release); }

    void Vibrate(uint32_t ms) override { Buzz(ms); }

    int  GetBatteryLevel() override { return batt_pct_.load(std::memory_order_acquire); }
    bool IsCharging()      override { return charging_.load(std::memory_order_acquire); }

private:
    void BuildDeviceName() {
        uint8_t mac[6] = {};
        if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return;   // keep the default name
        std::snprintf(name_, sizeof(name_), "WOWNNY_%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

#ifdef POWER_CTRL_PIN
    // Old board only. The panel's reset line has no effect, so the panel only initialises
    // properly after a power-on, and GPIO46's strapping pull-down keeps the module powered across
    // chip resets. Turn the supply off long enough for it to drain, then back on and hold it.
    void PowerOnRail() {
        constexpr uint32_t kOffMs = 1000;
        (void)gpio_hold_dis(POWER_CTRL_PIN);
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << POWER_CTRL_PIN;
        cfg.mode         = GPIO_MODE_OUTPUT;
        if (gpio_config(&cfg) != ESP_OK) { ESP_LOGE(TAG, "power gpio cfg failed"); return; }
        gpio_set_level(POWER_CTRL_PIN, POWER_CTRL_ACTIVE_HIGH ? 0 : 1);   // off
        ESP_LOGI(TAG, "display supply off for %lu ms, for a clean power-on", static_cast<unsigned long>(kOffMs));
        vTaskDelay(pdMS_TO_TICKS(kOffMs));
        gpio_set_level(POWER_CTRL_PIN, POWER_CTRL_ACTIVE_HIGH ? 1 : 0);   // on
        (void)gpio_hold_en(POWER_CTRL_PIN);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif

    void InitDisplay() {
        Gc9d01Config c = {};
        c.spi_host              = DISPLAY_SPI_HOST;
        c.pin_sck               = DISPLAY_SCK_PIN;
        c.pin_mosi              = DISPLAY_MOSI_PIN;
        c.pin_cs                = DISPLAY_CS_PIN;
        c.pin_dc                = DISPLAY_DC_PIN;
        c.pin_rst               = DISPLAY_RST_PIN;
        c.pin_backlight         = DISPLAY_BACKLIGHT_PIN;
        c.backlight_active_high = DISPLAY_BACKLIGHT_ACTIVE_HIGH;
        c.width                 = DISPLAY_WIDTH;
        c.height                = DISPLAY_HEIGHT;
        c.pclk_hz               = DISPLAY_SPI_CLK_HZ;
        if (panel_.Init(c) != ESP_OK) ESP_LOGE(TAG, "display init failed");
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
        LoadSettings();
        cc.out_volume  = volume_;
        if (codec_.Init(cc) != ESP_OK) { ESP_LOGE(TAG, "codec init failed"); return; }
        codec_ok_ = true;

        // 8 s of PCM16 in PSRAM; spoken replies arrive faster than real time.
        play_buf_ = xStreamBufferCreateWithCaps(kPlayBufBytes, 1, MALLOC_CAP_SPIRAM);
        if (!play_buf_) {
            play_buf_ = xStreamBufferCreate(16 * 1024, 1);
            if (!play_buf_) { ESP_LOGE(TAG, "play buffer alloc failed"); return; }
            agent_link_playback_set_buffer_ms(16 * 1024 / 32);
        } else {
            agent_link_playback_set_buffer_ms(kPlayBufBytes / 32);   // 32 bytes per ms at 16 kHz mono
        }
        xTaskCreate(&WownnyMuseBoard::PlayEntry, "voice_play", 4096, this, 5, nullptr);
    }

    void InitFuelGauge() {
        if (!codec_ok_) return;   // the codec owns the I2C bus
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
        xTaskCreate(&WownnyMuseBoard::ButtonEntry, "voice_keys", 4096, this, 5, nullptr);
    }

    // ── Settings ──────────────────────────────────────────────────────────────────────────

    void LoadSettings() {
        // Runs before agent_link brings up NVS. nvs_flash_init() is safe to call twice; if the
        // partition needs erasing, the transport decides (it stores the pairing) and we use
        // the defaults.
        (void)nvs_flash_init();
        nvs_handle_t h;
        int8_t v = AUDIO_DEFAULT_VOLUME, spk = 1;
        if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_i8(h, kNvsVol, &v);
            (void)nvs_get_i8(h, kNvsSpk, &spk);
            nvs_close(h);
        }
        volume_ = v < 0 ? 0 : v > 100 ? 100 : v;
        speaker_on_.store(spk != 0, std::memory_order_release);
    }

    void Save(const char* key, int8_t value) {
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
        (void)nvs_set_i8(h, key, value);
        (void)nvs_commit(h);
        nvs_close(h);
    }

    void SetVolume(int v) {
        volume_ = v < 0 ? 0 : v > 100 ? 100 : v;
        if (codec_ok_) (void)codec_.SetVolume(static_cast<uint8_t>(volume_));
        Save(kNvsVol, static_cast<int8_t>(volume_));
        Beep();
    }

    // Volume keys with the menu closed.
    void StepVolume(int delta) {
        SetVolume(volume_ + delta);
        char buf[16];
        std::snprintf(buf, sizeof buf, LV_SYMBOL_VOLUME_MAX " %d%%", volume_);
        voice::Ui::Instance().Toast(buf, 1500);
    }

    // 100 ms, 880 Hz feedback tone. Skipped while an answer plays or the mic is open.
    void Beep() {
        if (!play_buf_ || speech_.load(std::memory_order_acquire) || talking_.load(std::memory_order_acquire)) return;
        static int16_t tone[AUDIO_SAMPLE_RATE / 10];   // button task only
        static bool made = false;
        constexpr size_t kN = sizeof(tone) / sizeof(tone[0]);
        if (!made) {
            constexpr size_t kRamp = AUDIO_SAMPLE_RATE / 200;   // 5 ms fade in/out against clicks
            for (size_t i = 0; i < kN; ++i) {
                const float env = static_cast<float>(std::min({i, kN - 1 - i, kRamp})) / kRamp;
                tone[i] = static_cast<int16_t>(6000.0f * env * sinf(2.0f * 3.14159265f * 880.0f * i / AUDIO_SAMPLE_RATE));
            }
            made = true;
        }
        std::lock_guard<std::mutex> lk(play_mtx_);
        (void)xStreamBufferSend(play_buf_, tone, sizeof tone, 0);
    }

    void SetSpeaker(bool on) {
        speaker_on_.store(on, std::memory_order_release);
        Save(kNvsSpk, on ? 1 : 0);
        if (on) Beep();
    }

    // ── Talking ───────────────────────────────────────────────────────────────────────────

    bool StartTalking() {
        if (!codec_ok_) {
            ESP_LOGW(TAG, "no mic");
            voice::Ui::Instance().Toast(LV_SYMBOL_WARNING, 2000, voice::Tone::kError);
            return false;
        }
        if (codec_.StartMic() != ESP_OK) { ESP_LOGE(TAG, "mic start failed"); return false; }
        // Interrupt any answer that is still playing.
        talking_.store(true, std::memory_order_release);
        if (play_buf_) {
            std::lock_guard<std::mutex> lk(play_mtx_);
            (void)xStreamBufferReset(play_buf_);   // only fails if it is already empty
        }
        if (speech_.exchange(false, std::memory_order_acq_rel)) voice::Ui::Instance().SpeechEnd();
        if (agent_link_stream_open(AGENT_STREAM_VOICE, nullptr, &voice_) != ESP_OK) {
            ESP_LOGW(TAG, "voice stream would not open");
            talking_.store(false, std::memory_order_release);
            codec_.StopMic();
            voice::Ui::Instance().Toast(LV_SYMBOL_WARNING, 2000, voice::Tone::kError);
            return false;
        }
        voice::Ui::Instance().SetListening(true);
        Buzz(30);
        ESP_LOGI(TAG, "talking");
        return true;
    }

    // If !send, `why` is logged and `icon` is shown as a toast.
    void StopTalking(bool send, const char* why, const char* icon, voice::Tone tone) {
        agent_link_stream_close(voice_, send);
        voice_ = nullptr;
        codec_.StopMic();
        talking_.store(false, std::memory_order_release);
        voice::Ui::Instance().SetListening(false);
        if (send) {
            voice::Ui::Instance().SetThinking();
            ESP_LOGI(TAG, "sent");
        } else {
            voice::Ui::Instance().Toast(icon, 2000, tone);
            ESP_LOGI(TAG, "dropped: %s", why);
        }
    }

    // Reads one 20 ms mic frame into the stream. Returns its level 0-100 for the meter.
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

    static void ButtonEntry(void* arg) { static_cast<WownnyMuseBoard*>(arg)->ButtonLoop(); }

    // ── Menu ──────────────────────────────────────────────────────────────────────────────
    // Button task only.

    enum MenuItemId { kMenuVolume, kMenuSpeaker, kMenuWifi, kMenuReset, kMenuExit, kMenuItems };
    enum class MenuPage { kList, kWifi, kReset };
    static constexpr const char* kMenuNames[kMenuItems] = {"Volume", "Speaker", "Wi-Fi", "Reset", "Exit"};
    static constexpr uint32_t kText = 0xF2F5FA, kMuted = 0x8A93A6, kGood = 0x35D07F, kFair = 0xFFB020,
                              kBad = 0xFF5C5C;

    // Station state. Reads as not connected on transports that don't use Wi-Fi.
    struct WifiInfo {
        bool up = false;
        char ssid[33] = "";
        int  rssi = 0;
    };

    static WifiInfo ReadWifi() {
        WifiInfo w;
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            w.up = true;
            std::snprintf(w.ssid, sizeof w.ssid, "%s", reinterpret_cast<const char*>(ap.ssid));
            w.rssi = ap.rssi;
        }
        return w;
    }

    static const char* SignalWord(int rssi) { return rssi >= -60 ? "Good" : rssi >= -72 ? "Fair" : "Weak"; }
    static uint32_t SignalColor(int rssi) { return rssi >= -60 ? kGood : rssi >= -72 ? kFair : kBad; }

    void ShowMenu() {
        voice::MenuView v = {};
        char value[20] = "", line0[40] = "", line1[32] = "";
        switch (menu_page_) {
        case MenuPage::kList:
            v.heading = "MENU";
            v.heading_color = kMuted;
            v.list = true;
            v.above = kMenuNames[(menu_sel_ + kMenuItems - 1) % kMenuItems];
            v.label = kMenuNames[menu_sel_];
            v.below = kMenuNames[(menu_sel_ + 1) % kMenuItems];
            v.value = value;
            v.value_color = kText;
            switch (menu_sel_) {
            case kMenuVolume:
                std::snprintf(value, sizeof value, "%d%%", volume_);
                break;
            case kMenuSpeaker: {
                const bool on = speaker_on_.load(std::memory_order_acquire);
                std::snprintf(value, sizeof value, "%s", on ? "On" : "Off");
                v.value_color = on ? kGood : kMuted;
                break;
            }
            case kMenuWifi: {
                const WifiInfo w = ReadWifi();
                std::snprintf(value, sizeof value, "%s", w.up ? SignalWord(w.rssi) : "Off");
                v.value_color = w.up ? SignalColor(w.rssi) : kMuted;
                break;
            }
            case kMenuReset:
                std::snprintf(value, sizeof value, "%s", LV_SYMBOL_RIGHT);
                v.value_color = kMuted;
                break;
            default:
                break;
            }
            break;
        case MenuPage::kWifi: {
            const WifiInfo w = ReadWifi();
            const bool online = agent_link_state() == AGENT_STATE_READY;
            v.heading = "WI-FI";
            v.heading_color = kMuted;
            if (w.up) {
                std::snprintf(line0, sizeof line0, "%s", w.ssid);
                std::snprintf(line1, sizeof line1, "%d dBm", w.rssi);
                v.lines[0] = line0;
                v.line_colors[0] = kText;
                v.lines[1] = line1;
                v.line_colors[1] = SignalColor(w.rssi);
            } else {
                v.lines[0] = "Not";
                v.line_colors[0] = kFair;
                v.lines[1] = "connected";
                v.line_colors[1] = kFair;
            }
            v.lines[2] = online ? "Online" : "Offline";
            v.line_colors[2] = online ? kGood : kBad;
            v.lines[3] = LV_SYMBOL_LEFT " Back";
            v.line_colors[3] = kMuted;
            break;
        }
        case MenuPage::kReset:
            v.heading = "RESET";
            v.heading_color = kBad;
            v.lines[0] = "Forget";
            v.line_colors[0] = kText;
            v.lines[1] = "pairing?";
            v.line_colors[1] = kText;
            v.lines[2] = "BOOT: yes";
            v.line_colors[2] = kBad;
            v.lines[3] = "VOL: no";
            v.line_colors[3] = kMuted;
            break;
        }
        voice::Ui::Instance().ShowMenu(v);
    }

    void OpenMenu(int64_t now) {
        menu_open_ = true;
        menu_page_ = MenuPage::kList;
        menu_sel_ = kMenuVolume;
        menu_at_ = now;
        ShowMenu();
        Buzz(20);
        ESP_LOGI(TAG, "menu open");
    }

    void CloseMenu() {
        menu_open_ = false;
        voice::Ui::Instance().HideMenu();
        ESP_LOGI(TAG, "menu closed");
    }

    // step: -1 = VOL+ (up), +1 = VOL- (down). On a page any volume key goes back to the list.
    void MenuKey(int step, int64_t now) {
        menu_at_ = now;
        if (menu_page_ != MenuPage::kList) {
            menu_page_ = MenuPage::kList;
        } else {
            menu_sel_ = (menu_sel_ + step + kMenuItems) % kMenuItems;
        }
        ShowMenu();
    }

    // BOOT released (short press).
    void MenuSelect(int64_t now) {
        menu_at_ = now;
        if (menu_page_ == MenuPage::kWifi) {
            menu_page_ = MenuPage::kList;
            ShowMenu();
            return;
        }
        if (menu_page_ == MenuPage::kReset) {
            CloseMenu();
            voice::Ui::Instance().Toast(LV_SYMBOL_REFRESH, 10000, voice::Tone::kError);
            Buzz(200);
            ESP_LOGW(TAG, "menu: forgetting the pairing");
            (void)agent_link_forget();
            return;
        }
        switch (menu_sel_) {
        case kMenuVolume: {
            int next = kVolumeSteps[0];
            for (int v : kVolumeSteps) {
                if (v > volume_) { next = v; break; }
            }
            SetVolume(next);
            break;
        }
        case kMenuSpeaker:
            SetSpeaker(!speaker_on_.load(std::memory_order_acquire));
            break;
        case kMenuWifi:
            menu_page_ = MenuPage::kWifi;
            break;
        case kMenuReset:
            menu_page_ = MenuPage::kReset;
            Buzz(40);
            break;
        default:
            CloseMenu();
            return;
        }
        ShowMenu();
    }

    // With the menu closed a volume key steps the volume on release, or opens the menu when held.
    struct VolumeKey {
        Button button;
        int    step;
        bool   held = false;   // the press opened the menu, so its release is ignored
    };

    void PollVolumeKey(VolumeKey& k, int64_t now) {
        const int edge = k.button.Poll(now);
        if (menu_open_) {
            if (edge == 1 && !k.held) MenuKey(k.step > 0 ? -1 : 1, now);
            if (edge == -1) k.held = false;
            return;
        }
        if (edge == -1) {
            if (!k.held) StepVolume(k.step);
            k.held = false;
        } else if (k.button.pressed && !k.held && now - k.button.since >= kMenuHoldUs) {
            k.held = true;
            OpenMenu(now);
        }
    }

    void ButtonLoop() {
        Button boot{BUTTON_BOOT_PIN};
        VolumeKey up{{BUTTON_VOL_UP_PIN}, kVolumeStep}, down{{BUTTON_VOL_DOWN_PIN}, -kVolumeStep};
        bool    talking = false;
        int64_t talk_since = 0;
        bool    boot_menu = false;   // BOOT pressed inside the menu; acts on release or long hold
        int64_t wifi_shown = 0;

        while (true) {
            const int64_t now = esp_timer_get_time();

            switch (boot.Poll(now)) {
            case 1:
                if (agent_link_state() != AGENT_STATE_READY && agent_link_confirm() == ESP_OK) {
                    // pairing confirmation
                    if (menu_open_) CloseMenu();
                    voice::Ui::Instance().Toast(LV_SYMBOL_OK, 2000, voice::Tone::kOk);
                    Buzz(60);
                } else if (menu_open_) {
                    boot_menu = true;
                } else if (agent_link_state() == AGENT_STATE_READY) {
                    talking = StartTalking();
                    talk_since = now;
                }
                break;
            case -1:
                if (talking) {
                    StopTalking(now - talk_since >= kMinTalkUs, "too short", LV_SYMBOL_CLOSE, voice::Tone::kMuted);
                }
                talking = false;
                if (boot_menu && menu_open_) MenuSelect(now);
                boot_menu = false;
                break;
            default:
                break;
            }
            if (boot_menu && boot.pressed && now - boot.since >= kMenuExitHoldUs) {
                boot_menu = false;
                if (menu_open_) {
                    CloseMenu();
                    Buzz(20);
                }
            }
            if (talking && agent_link_state() != AGENT_STATE_READY) {
                StopTalking(false, "link lost", LV_SYMBOL_WARNING, voice::Tone::kError);
                talking = false;
            }

            PollVolumeKey(up, now);
            PollVolumeKey(down, now);
            if (menu_open_ && now - menu_at_ >= kMenuIdleUs) CloseMenu();
            if (menu_open_ && menu_page_ == MenuPage::kWifi && now - wifi_shown >= 1000 * 1000) {
                wifi_shown = now;   // refresh RSSI once a second
                ShowMenu();
            }

            if (talking) {
                voice::Ui::Instance().SetLevel(PumpMic());   // blocks ~20 ms
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }

    // ── Playback and battery ──────────────────────────────────────────────────────────────

    static void PlayEntry(void* arg) { static_cast<WownnyMuseBoard*>(arg)->PlayLoop(); }

    // Writes PCM to the codec, reports the speech level to the UI and detects the end of speech.
    void PlayLoop() {
        static int16_t buf[256];   // 16 ms
        int level = 0;             // decays between syllables
        int quiet_ms = 0;          // time since the last chunk while speaking
        while (true) {
            const size_t n = xStreamBufferReceive(play_buf_, buf, sizeof(buf), pdMS_TO_TICKS(40));
            if (n >= sizeof(int16_t)) {
                const size_t frames = n / sizeof(int16_t);
                if (speech_.load(std::memory_order_acquire)) {
                    level = std::max(Loudness(buf, frames), level - 6);
                    voice::Ui::Instance().SpeechLevel(level);
                }
                codec_.WritePcm(buf, frames);   // blocking
                quiet_ms = 0;
                continue;
            }
            if (!speech_.load(std::memory_order_acquire)) continue;
            quiet_ms += 40;
            level = std::max(0, level - 20);
            voice::Ui::Instance().SpeechLevel(level);
            if ((speech_end_.load(std::memory_order_acquire) && quiet_ms >= 80) || quiet_ms >= kSpeechGapMs) {
                speech_end_.store(false, std::memory_order_release);
                if (speech_.exchange(false, std::memory_order_acq_rel)) voice::Ui::Instance().SpeechEnd();
                level = 0;
            }
        }
    }

    static void StatusEntry(void* arg) { static_cast<WownnyMuseBoard*>(arg)->StatusLoop(); }

    // Only reader of the gauge; the UI and the battery report use the cached values.
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

    char                  name_[32] = "WOWNNY_MUSE";     // MAC suffix added at boot
    Gc9d01Panel           panel_;
    EsCodec               codec_;
    Bq27220               gauge_;
    bool                  codec_ok_  = false;
    bool                  gauge_ok_  = false;
    bool                  haptic_ok_ = false;
    int                   volume_    = AUDIO_DEFAULT_VOLUME;
    esp_timer_handle_t    haptic_timer_ = nullptr;
    StreamBufferHandle_t  play_buf_  = nullptr;
    std::mutex            play_mtx_;            // two writers (PlayAudio, Beep/reset), one reader
    agent_stream_handle_t voice_     = nullptr;
    std::atomic<int>      batt_pct_{-1};
    std::atomic<bool>     charging_{false};
    std::atomic<bool>     talking_{false};      // mic open
    std::atomic<bool>     speaker_on_{true};
    std::atomic<bool>     speech_{false};       // answer audio playing
    std::atomic<bool>     speech_end_{false};   // AudioEnd() received
    bool                  menu_open_    = false;
    MenuPage              menu_page_    = MenuPage::kList;
    int                   menu_sel_     = 0;
    int64_t               menu_at_      = 0;    // last key press in the menu
};

DECLARE_BOARD(WownnyMuseBoard);
