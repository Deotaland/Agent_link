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
enum class Page { kHome, kSettings, kSessions, kDelete, kReset };
enum class Action { kSessions, kNew, kDelete, kReset, kBack, kSession, kConfirmDelete, kConfirmReset };
struct Item { Action action; int value; char text[112]; };
}  // namespace

class M5StopWatchWifi : public Application {
public:
    M5StopWatchWifi() {
        (void)nvs_flash_init();
        LoadSelection();
        hardware_.Init(volume_.load());
        watch_wifi::SetVolume(volume_.load());
        if (hardware_.ioe_ok_ && touch_.Init(SYS_I2C_PORT, DISPLAY_WIDTH, DISPLAY_HEIGHT) != ESP_OK)
            ESP_LOGE(TAG, "CST820 unavailable");
        if (audio_.Init(&hardware_) != ESP_OK) ESP_LOGE(TAG, "audio unavailable");
        events_ = xQueueCreate(8, sizeof(watch_wifi::Input));
        if (watch_wifi::StartUi(&hardware_.panel_, &touch_, OnTouch, this) != ESP_OK)
            ESP_LOGE(TAG, "display unavailable");
        if (!touch_.Ready()) watch_wifi::Toast("Touch unavailable - restart", 30000);
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
            voice_supported_ = false;
            return false;
        }
        voice_supported_ = bound.voice_supported;
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
        if (error == ESP_OK) return false;
        char message[96];
        snprintf(message, sizeof message, "Request failed: %s", esp_err_to_name(error));
        ESP_LOGW(TAG, "%s", message);
        snprintf(error_message_, sizeof error_message_, "%s", message);
        watch_wifi::Toast(message, 5000);
        watch_wifi::SetAnswer(message);
        return true;
    }

    void Busy(const char* message) {
        error_message_[0] = '\0';
        watch_wifi::Screen screen = {};
        screen.menu = true; screen.busy = true; screen.view = ++view_;
        snprintf(screen.title, sizeof screen.title, "%s", message);
        snprintf(screen.detail, sizeof screen.detail, "Please wait...");
        watch_wifi::SetScreen(screen);
    }

    void Clear(Page page) { page_ = page; count_ = 0; cursor_ = 0; ++view_; error_message_[0] = '\0'; }
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
        Add(Action::kReset, "Reset device activation");
    }
    void Sessions(size_t offset = 0) {
        (void)SyncBoundAgent(true);
        if (!agent_) return;
        EndCall();
        Busy("Loading conversations");
        size_t count = 0, total = 0;
        agent_link_platform_conversation_t fetched[3] = {};
        if (Error(agent_link_platform_conversations(agent_, offset, fetched, 3, &count, &total))) return;
        // One bounded retry if a remotely deleted page no longer exists.
        if (!count && offset) {
            offset = total ? ((total - 1) / 3) * 3 : 0;
            if (Error(agent_link_platform_conversations(agent_, offset, fetched, 3, &count, &total))) return;
        }
        memcpy(sessions_, fetched, sizeof sessions_);
        session_offset_ = offset; session_total_ = total; Clear(Page::kSessions);
        for (size_t i = 0; i < count; ++i)
            Add(Action::kSession, sessions_[i].name[0] ? sessions_[i].name : sessions_[i].id, static_cast<int>(i));

    }
    void NewSession() {
        (void)SyncBoundAgent(true);
        if (!agent_) return;
        Busy("Creating conversation");
        char created[sizeof session_] = {};
        if (Error(agent_link_platform_create_conversation(agent_, created, sizeof created))) return;
        if (!created[0]) { Sessions(); return; }
        snprintf(session_, sizeof session_, "%s", created);
        snprintf(session_name_, sizeof session_name_, "New conversation");
        SaveSelection(); Clear(Page::kHome);
        watch_wifi::SetAnswer("Conversation created. Tap Start call to talk.");
    }

    void Select(int row = -1) {
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
        if (row < 0 || row >= 3 || cursor_ + row >= count_) return;
        const Item item = items_[cursor_ + row];
        switch (item.action) {
        case Action::kSessions: Sessions(); break;
        case Action::kNew: NewSession(); break;
        case Action::kDelete:
            if (!session_[0]) { watch_wifi::Toast("No conversation selected"); break; }
            Clear(Page::kDelete);
            Add(Action::kBack, "Cancel");
            Add(Action::kConfirmDelete, "Confirm delete");
            break;
        case Action::kReset:
            Clear(Page::kReset);
            Add(Action::kBack, "Cancel");
            Add(Action::kConfirmReset, "Confirm reset activation");
            break;
        case Action::kConfirmReset:
            Busy("Resetting activation");
            EndCall();
            if (!Error(agent_link_forget())) {
                agent_ = saved_agent_ = 0; voice_supported_ = false;
                session_[0] = agent_name_[0] = session_name_[0] = '\0';
                nvs_handle_t handle;
                if (nvs_open("sw_wifi", NVS_READWRITE, &handle) == ESP_OK) {
                    (void)nvs_erase_all(handle); (void)nvs_commit(handle); nvs_close(handle);
                }
                Clear(Page::kHome); watch_wifi::SetAnswer("");
                watch_wifi::Toast("Activate again in console", 5000);
            }
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
            watch_wifi::SetAnswer("Tap Start call to start a realtime call.");
            break;
        case Action::kBack: Back(); break;
        }
    }

    void Back() {
        if (page_ == Page::kSettings || page_ == Page::kHome) { EndCall(); Clear(Page::kHome); }
        else Settings();
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
            EndCall(); watch_wifi::Toast("Call ended - tap Start call to retry", 5000); return;
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
        screen.view = view_;
        screen.menu = page_ != Page::kHome; screen.calling = call_;
        const agent_state_t state = agent_link_state();
        screen.can_sessions = agent_ > 0 && (state == AGENT_STATE_CONNECTED || state == AGENT_STATE_READY);
        screen.can_call = voice_supported_ && audio_.Ready() &&
                          (state == AGENT_STATE_CONNECTED || state == AGENT_STATE_READY);
        const char* title = page_ == Page::kSettings ? "Voice settings" :
                            page_ == Page::kSessions ? "Conversations" :
                            page_ == Page::kDelete ? "Delete conversation?" :
                            page_ == Page::kReset ? "Reset activation?" :
                            active_ ? "Listening / speaking" : call_ ? "Connecting voice..." : "Realtime voice";
        snprintf(screen.title, sizeof screen.title, "%s", title);
        if (page_ == Page::kDelete)
            snprintf(screen.detail, sizeof screen.detail, "%s", session_name_[0] ? session_name_ : session_);
        else if (page_ == Page::kReset)
            snprintf(screen.detail, sizeof screen.detail, "Keeps WiFi. Unbind in console before activating again.");
        else if (page_ == Page::kSessions && !count_)
            snprintf(screen.detail, sizeof screen.detail, "No conversations. Back > New conversation.");
        else snprintf(screen.detail, sizeof screen.detail, "%s / %s", agent_name_[0] ? agent_name_ : "Bind agent in console",
                      session_name_[0] ? session_name_ : session_[0] ? session_ : "No conversation");
        if (error_message_[0]) snprintf(screen.detail, sizeof screen.detail, "%s", error_message_);
        if (screen.menu) {
            for (size_t i = 0; i < 3 && cursor_ + i < count_; ++i)
                snprintf(screen.rows[i], sizeof screen.rows[i], "%s", items_[cursor_ + i].text);
            screen.previous = page_ == Page::kSessions ? session_offset_ > 0 : cursor_ > 0;
            screen.next = page_ == Page::kSessions ? session_offset_ + count_ < session_total_ : cursor_ + 3 < count_;
            if (page_ == Page::kSessions && session_total_)
                snprintf(screen.footer, sizeof screen.footer, "%u-%u / %u", static_cast<unsigned>(session_offset_ + 1),
                         static_cast<unsigned>(session_offset_ + count_), static_cast<unsigned>(session_total_));
            else if (page_ == Page::kSettings)
                snprintf(screen.footer, sizeof screen.footer, "Page %u / %u", static_cast<unsigned>(cursor_ / 3 + 1),
                         static_cast<unsigned>((count_ + 2) / 3));
        }
        watch_wifi::SetScreen(screen);
    }

    void ControlLoop() {
        int64_t battery_at = 0;
        while (true) {
            (void)SyncBoundAgent();
            watch_wifi::Input event;
            if (xQueueReceive(events_, &event, pdMS_TO_TICKS(100)) == pdTRUE &&
                watch_wifi::CurrentInput(event, view_, esp_timer_get_time())) {
                error_message_[0] = '\0';
                ++view_;  // acknowledge accepted input even when an API guard declines it
                switch (event.action) {
                case watch_wifi::TouchAction::kCall:
                    if (page_ == Page::kHome) Select();
                    break;
                case watch_wifi::TouchAction::kSessions: Sessions(); break;
                case watch_wifi::TouchAction::kSettings: Settings(); break;
                case watch_wifi::TouchAction::kRow:
                    if (page_ != Page::kHome) Select(event.row);
                    break;
                case watch_wifi::TouchAction::kBack: Back(); break;
                case watch_wifi::TouchAction::kPrevious:
                    if (page_ == Page::kSessions && session_offset_) Sessions(session_offset_ >= 3 ? session_offset_ - 3 : 0);
                    else if (cursor_ >= 3) cursor_ -= 3;
                    break;
                case watch_wifi::TouchAction::kNext:
                    if (page_ == Page::kSessions && session_offset_ + count_ < session_total_) Sessions(session_offset_ + 3);
                    else if (cursor_ + 3 < count_) cursor_ += 3;
                    break;
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

    static bool OnTouch(const watch_wifi::Input& input, void* context) {
        auto* self = static_cast<M5StopWatchWifi*>(context);
        watch_wifi::Wake();
        if (self->events_ && xQueueSend(self->events_, &input, 0) == pdTRUE) return true;
        watch_wifi::Toast("Busy - try again"); return false;
    }
    void KeyLoop() {
        watch_wifi::VolumeKey yellow, blue;
        bool dirty = false;
        int64_t changed_at = 0;
        while (true) {
            const int64_t now = esp_timer_get_time();
            const bool y_down = gpio_get_level(KEY_TALK_PIN) == 0;
            const bool b_down = gpio_get_level(KEY_VOLUME_PIN) == 0;
            const bool y = yellow.Poll(y_down, now), b = blue.Poll(b_down, now);
            if (y || b) watch_wifi::Wake();
            if (!(y_down && b_down) && (y || b)) {
                const int before = volume_.load();
                const int next = watch_wifi::AdjustVolume(before, y ? -1 : 1);
                if (next != before && hardware_.codec_ok_) {
                    if (hardware_.codec_.SetVolume(next) == ESP_OK) {
                        volume_.store(next); watch_wifi::SetVolume(next);
                        dirty = true; changed_at = now;
                    } else watch_wifi::Toast("Volume change failed");
                }
            }
            // One write after release, including holds, avoids wearing NVS on every repeat.
            if (dirty && !yellow.Down() && !blue.Down() && now - changed_at >= 500000) {
                nvs_handle_t handle;
                if (nvs_open("voice", NVS_READWRITE, &handle) == ESP_OK) {
                    esp_err_t error = nvs_set_i8(handle, "vol", static_cast<int8_t>(volume_.load()));
                    if (error == ESP_OK) error = nvs_commit(handle);
                    nvs_close(handle);
                    if (error != ESP_OK) watch_wifi::Toast("Volume was not saved");
                } else watch_wifi::Toast("Volume was not saved");
                dirty = false;
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
    Cst820Touch touch_;
    QueueHandle_t events_ = nullptr, haptics_ = nullptr;
    std::atomic<int> battery_{-1};
    std::atomic<bool> charging_{false};
    std::atomic<int> volume_{AUDIO_DEFAULT_VOLUME};
    uint32_t view_ = 1;
    bool voice_supported_ = false;
    Page page_ = Page::kHome;
    Item items_[8] = {};
    size_t count_ = 0, cursor_ = 0, session_offset_ = 0, session_total_ = 0;
    agent_link_platform_conversation_t sessions_[3] = {};
    int32_t agent_ = 0, saved_agent_ = 0;
    char agent_name_[80] = {}, session_[80] = {}, session_name_[96] = {};
    char error_message_[96] = {};
    bool call_ = false, active_ = false;
    int attempts_ = 0;
    int64_t requested_at_ = 0, last_attempt_ = 0;
};

DECLARE_APPLICATION(M5StopWatchWifi);
