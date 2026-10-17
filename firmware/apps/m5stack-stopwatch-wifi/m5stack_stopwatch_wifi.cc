#include "application.h"
#include "config.h"
#include "m5stack_stopwatch_hardware.h"
#include "watch_wifi_audio.h"
#include "watch_wifi_ui.h"
#include "agent_link_platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char* TAG = "StopWatch.wifi";
enum class KeyAction { kSelect, kNext, kSettings, kBack };
struct KeyEvent { KeyAction action; int64_t at; };
struct Key {
    gpio_num_t pin;
    bool down = false;
    int stable = 0;
    int64_t since = 0;
    int Poll(int64_t now) {
        const bool pressed = gpio_get_level(pin) == 0;
        if (pressed == down) { stable = 0; return 0; }
        if (++stable < 2) return 0;
        stable = 0; down = pressed;
        if (down) since = now;
        return down ? 1 : -1;
    }
};
enum class Page { kHome, kSettings, kSessions, kDelete };
enum class Action { kSessions, kNew, kDelete, kBack, kSession,
                    kSessionPrev, kSessionNext, kConfirmDelete };
struct Item { Action action; int value; char text[112]; };
}  // namespace

class M5StopWatchWifi : public Application {
public:
    M5StopWatchWifi() {
        (void)nvs_flash_init();
        LoadSelection();
        hardware_.Init(volume_);
        if (audio_.Init(&hardware_) != ESP_OK) ESP_LOGE(TAG, "audio unavailable");
        if (watch_wifi::StartUi(&hardware_.panel_) != ESP_OK) ESP_LOGE(TAG, "display unavailable");
        events_ = xQueueCreate(8, sizeof(KeyEvent));
        haptics_ = xQueueCreate(1, sizeof(uint32_t));
        if (hardware_.ioe_ok_) {
            (void)hardware_.ioe_.SetPwm(MOTOR_PWM_CHANNEL, 0, false);
            (void)hardware_.ioe_.SetPwmFrequency(MOTOR_PWM_HZ);
            (void)hardware_.ioe_.SetOutput(IOE_MOTOR, false);
        }
        if (haptics_ && hardware_.ioe_ok_ &&
            xTaskCreate(HapticEntry, "sw_wifi_buzz", 2560, this, 3, nullptr) != pdPASS) {
            vQueueDelete(haptics_); haptics_ = nullptr;
        }
        gpio_config_t keys = {};
        keys.pin_bit_mask = (1ULL << KEY_TALK_PIN) | (1ULL << KEY_VOLUME_PIN);
        keys.mode = GPIO_MODE_INPUT; keys.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&keys));
        if (!events_ || xTaskCreate(ControlEntry, "sw_wifi_ctrl", 12288, this, 4, nullptr) != pdPASS ||
            xTaskCreate(KeyEntry, "sw_wifi_keys", 3072, this, 5, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "key/controller task unavailable");
            watch_wifi::Toast("Controls unavailable - restart", 30000);
        }
    }

    const char* Name() const override { return "M5StopWatch"; }
    const char* Model() const override { return "M5-STOPWATCH"; }
    const agent_platform_t* Platform() const override {
        static const agent_platform_t platform = {
            .base_url = CLOUD_BASE_URL, .product_id = CLOUD_PRODUCT_ID, .chip_type = CLOUD_CHIP_TYPE,
        };
        return &platform;
    }
    uint32_t Capabilities() const override {
        uint32_t caps = AGENT_CAP_BUTTON;
        if (hardware_.panel_.Ready()) caps |= AGENT_CAP_SCREEN;
        if (audio_.Ready()) caps |= AGENT_CAP_MIC | AGENT_CAP_SPEAKER;
        if (hardware_.pm1_ok_) caps |= AGENT_CAP_BATTERY;
        if (haptics_ && hardware_.ioe_ok_) caps |= AGENT_CAP_HAPTIC;
        return caps;
    }
    void OnLinkStatus(const agent_link_status_t& status) override { watch_wifi::SetStatus(status); }
    void PlayAudio(const uint8_t* pcm, size_t bytes) override { audio_.Play(pcm, bytes); }
    void AudioEnd() override { audio_.End(); }
    void AudioReset() override { audio_.Reset(); }
    void OnListen(bool start, uint32_t max_ms) override { audio_.Listen(start, max_ms); }
    void ShowText(const char* text) override { watch_wifi::SetAnswer(text); }
    void Vibrate(uint32_t ms) override { if (haptics_ && ms) xQueueOverwrite(haptics_, &ms); }
    int GetBatteryLevel() override { return battery_.load(); }
    bool IsCharging() override { return charging_.load(); }

private:
    void LoadSelection() {
        nvs_handle_t handle;
        if (nvs_open("voice", NVS_READONLY, &handle) == ESP_OK) {
            int8_t volume = AUDIO_DEFAULT_VOLUME;
            (void)nvs_get_i8(handle, "vol", &volume);
            volume_ = volume < 0 ? 0 : volume > 100 ? 100 : volume;
            nvs_close(handle);
        }
        if (nvs_open("sw_wifi", NVS_READONLY, &handle) != ESP_OK) return;
        (void)nvs_get_i32(handle, "agent", &saved_agent_);
        size_t size = sizeof session_;
        (void)nvs_get_str(handle, "session", session_, &size);
        size = sizeof session_name_; (void)nvs_get_str(handle, "session_name", session_name_, &size);
        nvs_close(handle);
        if (saved_agent_ <= 0) { saved_agent_ = 0; session_[0] = '\0'; }
    }

    void SaveSelection() {
        if (reset_.load()) return;
        nvs_handle_t handle;
        if (nvs_open("sw_wifi", NVS_READWRITE, &handle) != ESP_OK) return;
        esp_err_t error = nvs_set_i32(handle, "agent", agent_);
        if (error == ESP_OK) error = nvs_set_str(handle, "session", session_);
        if (error == ESP_OK) error = nvs_set_str(handle, "agent_name", agent_name_);
        if (error == ESP_OK) error = nvs_set_str(handle, "session_name", session_name_);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        if (error != ESP_OK) watch_wifi::Toast("Selection could not be saved");
    }

    bool SyncBoundAgent(bool explain = false) {
        agent_link_platform_agent_t bound = {};
        const esp_err_t result = agent_link_platform_bound_agent(&bound);
        if (result != ESP_OK) {
            if (agent_) {
                EndCall();
                agent_ = saved_agent_ = 0;
                session_[0] = session_name_[0] = agent_name_[0] = '\0';
                Clear(Page::kHome); SaveSelection();
            }
            if (explain) watch_wifi::Toast(result == ESP_ERR_NOT_FOUND ?
                "Bind an agent in platform console" : "Waiting for platform authentication", 4000);
            return false;
        }
        const bool changed = agent_ != bound.id;
        if (changed) {
            if (agent_) { EndCall(); Clear(Page::kHome); }
            if (saved_agent_ != bound.id) session_[0] = session_name_[0] = '\0';
            agent_ = saved_agent_ = bound.id;
        }
        const bool renamed = strcmp(agent_name_, bound.name) != 0;
        snprintf(agent_name_, sizeof agent_name_, "%s", bound.name);
        if (changed || renamed) SaveSelection();
        if (!bound.voice_supported && explain) watch_wifi::Toast("Bound agent has no realtime voice", 4000);
        return bound.voice_supported;
    }

    void EndCall() {
        call_ = false; active_ = false; requested_at_ = 0;
        audio_.Stop();
        (void)agent_link_end_realtime();
    }

    bool Error(esp_err_t error) {
        if (error == ESP_OK && !reset_.load()) return false;
        if (!reset_.load()) {
            char message[96];
            snprintf(message, sizeof message, "Request failed: %s", esp_err_to_name(error));
            watch_wifi::Toast(message, 5000);
        }
        return true;
    }

    void Busy(const char* message) {
        watch_wifi::Screen screen = {};
        screen.menu = true;
        snprintf(screen.title, sizeof screen.title, "%s", message);
        snprintf(screen.detail, sizeof screen.detail, "Please wait...");
        watch_wifi::SetScreen(screen);
    }

    void Clear(Page page) { page_ = page; count_ = 0; cursor_ = 0; }
    void Add(Action action, const char* text, int value = 0) {
        if (count_ >= sizeof items_ / sizeof items_[0]) return;
        Item& item = items_[count_++]; item.action = action; item.value = value;
        snprintf(item.text, sizeof item.text, "%s", text);
    }
    void Settings() {
        EndCall(); Clear(Page::kSettings);
        Add(Action::kSessions, "Choose conversation");
        Add(Action::kNew, "New conversation");
        Add(Action::kDelete, "Delete selected conversation");
        Add(Action::kBack, "Back to voice");
    }
    void Sessions(size_t offset = 0) {
        if (!SyncBoundAgent(true)) return;
        Busy("Loading conversations");
        size_t count = 0, total = 0;
        if (Error(agent_link_platform_conversations(agent_, offset, sessions_, 3, &count, &total))) return;
        session_offset_ = offset; Clear(Page::kSessions);
        for (size_t i = 0; i < count; ++i)
            Add(Action::kSession, sessions_[i].name[0] ? sessions_[i].name : sessions_[i].id, static_cast<int>(i));
        Add(Action::kNew, "New conversation");
        if (offset) Add(Action::kSessionPrev, "Previous page");
        if (offset + count < total) Add(Action::kSessionNext, "Next page");
        Add(Action::kBack, "Back");
    }
    void NewSession() {
        if (!SyncBoundAgent(true)) return;
        Busy("Creating conversation");
        char created[sizeof session_] = {};
        if (Error(agent_link_platform_create_conversation(agent_, created, sizeof created))) return;
        if (!created[0]) { Sessions(); return; }
        snprintf(session_, sizeof session_, "%s", created);
        snprintf(session_name_, sizeof session_name_, "New conversation");
        SaveSelection(); Clear(Page::kHome);
        watch_wifi::SetAnswer("Conversation created. Press yellow to start talking.");
    }

    void Select() {
        if (page_ == Page::kHome) {
            if (call_) { EndCall(); return; }
            if (!audio_.Ready()) { watch_wifi::Toast("Audio unavailable - restart"); return; }
            if (!SyncBoundAgent(true)) return;
            if (!session_[0]) { Sessions(); return; }
            if (agent_link_state() != AGENT_STATE_CONNECTED && agent_link_state() != AGENT_STATE_READY) {
                watch_wifi::Toast("Finish WiFi setup and activation"); return;
            }
            watch_wifi::SetAnswer("");
            call_ = true; attempts_ = 0; last_attempt_ = 0;
            return;
        }
        if (!count_) return;
        const Item item = items_[cursor_];
        switch (item.action) {
        case Action::kSessions: Sessions(); break;
        case Action::kNew: NewSession(); break;
        case Action::kDelete:
            if (!session_[0]) { watch_wifi::Toast("No conversation selected"); break; }
            Clear(Page::kDelete);
            Add(Action::kBack, "Cancel");
            Add(Action::kConfirmDelete, "Confirm delete");
            break;
        case Action::kConfirmDelete:
            Busy("Deleting conversation");
            if (!Error(agent_link_platform_delete_conversation(agent_, session_))) {
                session_[0] = '\0'; session_name_[0] = '\0'; SaveSelection(); Sessions();
            }
            break;
        case Action::kSession:
            snprintf(session_, sizeof session_, "%s", sessions_[item.value].id);
            snprintf(session_name_, sizeof session_name_, "%s", sessions_[item.value].name);
            SaveSelection(); Clear(Page::kHome);
            watch_wifi::SetAnswer("Press yellow to start a realtime call.");
            break;
        case Action::kSessionPrev: Sessions(session_offset_ >= 3 ? session_offset_ - 3 : 0); break;
        case Action::kSessionNext: Sessions(session_offset_ + 3); break;
        case Action::kBack: Back(); break;
        }
    }

    void Back() {
        if (page_ == Page::kSettings || page_ == Page::kHome) { EndCall(); Clear(Page::kHome); }
        else Settings();
    }
    void StepVolume() {
        volume_ = volume_ >= 100 ? 20 : (volume_ / 20 + 1) * 20;
        if (hardware_.codec_ok_) (void)hardware_.codec_.SetVolume(volume_);
        nvs_handle_t handle;
        if (nvs_open("voice", NVS_READWRITE, &handle) == ESP_OK) {
            (void)nvs_set_i8(handle, "vol", static_cast<int8_t>(volume_));
            (void)nvs_commit(handle); nvs_close(handle);
        }
        char message[40]; snprintf(message, sizeof message, "Volume %d%%", volume_);
        watch_wifi::Toast(message);
    }

    void TickCall() {
        if (!call_) return;
        const int64_t now = esp_timer_get_time();
        const agent_state_t state = agent_link_state();
        if (state == AGENT_STATE_READY) {
            if (!active_) { active_ = true; requested_at_ = 0; Vibrate(40); }
            audio_.SetContinuous(true);
            return;
        }
        if (active_ || (requested_at_ && now - requested_at_ > 30000000)) {
            EndCall(); watch_wifi::Toast("Call ended - press yellow to retry", 5000); return;
        }
        if (requested_at_ || now - last_attempt_ < 1000000) return;
        last_attempt_ = now;
        if (state != AGENT_STATE_CONNECTED || ++attempts_ > 10) {
            EndCall(); watch_wifi::Toast("Connection unavailable - retry", 5000); return;
        }
        const esp_err_t error = agent_link_request_realtime_session(agent_, session_);
        if (error == ESP_OK) requested_at_ = now;
        else if (error != ESP_ERR_INVALID_STATE) { EndCall(); Error(error); }
    }

    void Publish() {
        watch_wifi::Screen screen = {};
        screen.menu = page_ != Page::kHome;
        const char* title = page_ == Page::kSettings ? "Voice settings" :
                            page_ == Page::kSessions ? "Conversations" : page_ == Page::kDelete ? "Delete conversation?" :
                            active_ ? "Listening / speaking" : call_ ? "Connecting voice..." : "Realtime voice";
        snprintf(screen.title, sizeof screen.title, "%s", title);
        if (page_ == Page::kDelete)
            snprintf(screen.detail, sizeof screen.detail, "%s", session_name_[0] ? session_name_ : session_);
        else snprintf(screen.detail, sizeof screen.detail, "%s / %s", agent_name_[0] ? agent_name_ : "No agent",
                      session_name_[0] ? session_name_ : session_[0] ? session_ : "No conversation");
        if (screen.menu) {
            const size_t start = (cursor_ / 3) * 3;
            screen.selected = cursor_ % 3;
            for (size_t i = 0; i < 3 && start + i < count_; ++i)
                snprintf(screen.rows[i], sizeof screen.rows[i], "%s", items_[start + i].text);
        }
        snprintf(screen.footer, sizeof screen.footer, "%s", screen.menu ?
                 "BLUE: next   YELLOW: select\nHold YELLOW: back" :
                 call_ ? "YELLOW: end call   BLUE: volume\nHold BLUE: settings" :
                         "YELLOW: start   BLUE: volume\nHold BLUE: settings");
        watch_wifi::SetScreen(screen);
    }

    void ControlLoop() {
        int64_t battery_at = 0;
        while (true) {
            (void)SyncBoundAgent();
            if (reset_.load()) {
                EndCall();
                agent_ = 0; session_[0] = agent_name_[0] = session_name_[0] = '\0';
                nvs_handle_t handle;
                if (nvs_open("sw_wifi", NVS_READWRITE, &handle) == ESP_OK) {
                    (void)nvs_erase_all(handle); (void)nvs_commit(handle); nvs_close(handle);
                }
                Clear(Page::kHome); xQueueReset(events_); reset_.store(false);
            }
            KeyEvent event;
            if (xQueueReceive(events_, &event, pdMS_TO_TICKS(100)) == pdTRUE &&
                esp_timer_get_time() - event.at < 500000) {
                switch (event.action) {
                case KeyAction::kSelect: Select(); break;
                case KeyAction::kNext:
                    if (page_ == Page::kHome) StepVolume();
                    else if (count_) cursor_ = (cursor_ + 1) % count_;
                    break;
                case KeyAction::kSettings: Settings(); break;
                case KeyAction::kBack: Back(); break;
                }
            }
            TickCall(); Publish();
            const int64_t now = esp_timer_get_time();
            if (now >= battery_at) {
                uint16_t mv = 0; bool status = true; int percent = -1;
                if (hardware_.pm1_ok_ && hardware_.pm1_.BatteryMv(&mv) == ESP_OK && mv) {
                    percent = (static_cast<int>(mv) - 3300) * 100 / 850;
                    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
                }
                const bool charging = hardware_.pm1_ok_ &&
                    hardware_.pm1_.ReadGpio(PM1_CHG_STAT_GPIO, &status) == ESP_OK && !status;
                battery_.store(percent); charging_.store(charging);
                watch_wifi::SetBattery(percent, charging); battery_at = now + 5000000;
            }
        }
    }

    void Post(KeyAction action) {
        const KeyEvent event{action, esp_timer_get_time()};
        if (xQueueSend(events_, &event, 0) != pdTRUE) watch_wifi::Toast("Busy - try again");
    }
    void KeyLoop() {
        Key yellow{KEY_TALK_PIN}, blue{KEY_VOLUME_PIN};
        bool back = false, forgot = false;
        int countdown = -1;
        while (true) {
            const int64_t now = esp_timer_get_time();
            const int y = yellow.Poll(now);
            if (y == 1) { watch_wifi::Wake(); back = false; }
            if (y == -1 && !back) Post(KeyAction::kSelect);
            if (yellow.down && !back && now - yellow.since >= 1200000) {
                Post(KeyAction::kBack); back = true;
            }
            const int b = blue.Poll(now);
            if (b == 1) { watch_wifi::Wake(); forgot = false; countdown = -1; }
            const int64_t held = (now - blue.since) / 1000;
            if (b == -1 && !forgot) Post(held >= 1500 ? KeyAction::kSettings : KeyAction::kNext);
            if (blue.down && !forgot && held >= 1500) {
                if (held >= FORGET_HOLD_MS) {
                    reset_.store(true); forgot = true;
                    audio_.Stop(); Vibrate(200);
                    watch_wifi::SetAnswer("");
                    watch_wifi::Toast("Resetting - activate again", 10000);
                    (void)agent_link_forget();
                } else {
                    const int left = (FORGET_HOLD_MS - held + 999) / 1000;
                    if (left != countdown) {
                        countdown = left;
                        char message[80];
                        snprintf(message, sizeof message, "Release: settings / reset in %d", left);
                        watch_wifi::Toast(message, 1500);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void HapticLoop() {
        uint32_t ms;
        while (true) {
            if (xQueueReceive(haptics_, &ms, portMAX_DELAY) != pdTRUE) continue;
            (void)hardware_.ioe_.SetPwm(MOTOR_PWM_CHANNEL, MOTOR_DUTY, true);
            do { if (ms > 2000) ms = 2000; }
            while (xQueueReceive(haptics_, &ms, pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1) == pdTRUE);
            (void)hardware_.ioe_.SetPwm(MOTOR_PWM_CHANNEL, 0, false);
        }
    }
    static void ControlEntry(void* ctx) { static_cast<M5StopWatchWifi*>(ctx)->ControlLoop(); }
    static void KeyEntry(void* ctx) { static_cast<M5StopWatchWifi*>(ctx)->KeyLoop(); }
    static void HapticEntry(void* ctx) { static_cast<M5StopWatchWifi*>(ctx)->HapticLoop(); }

    M5StopWatchHardware hardware_;
    WatchWifiAudio audio_;
    QueueHandle_t events_ = nullptr, haptics_ = nullptr;
    std::atomic<int> battery_{-1};
    std::atomic<bool> charging_{false}, reset_{false};
    int volume_ = AUDIO_DEFAULT_VOLUME;
    Page page_ = Page::kHome;
    Item items_[8] = {};
    size_t count_ = 0, cursor_ = 0, session_offset_ = 0;
    agent_link_platform_conversation_t sessions_[3] = {};
    int32_t agent_ = 0, saved_agent_ = 0;
    char agent_name_[80] = {}, session_[80] = {}, session_name_[96] = {};
    bool call_ = false, active_ = false;
    int attempts_ = 0;
    int64_t requested_at_ = 0, last_attempt_ = 0;
};

DECLARE_APPLICATION(M5StopWatchWifi);
