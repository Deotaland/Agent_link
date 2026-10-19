#include "application.h"
#include "config.h"
#include "epd.h"
#include "paper_hardware.h"
#include "paper_wifi_audio.h"
#include "paper_refresh.h"
#include "agent_link_platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char *TAG = "Paper.wifi";
enum class Action { kPress, kHold, kVibrate };
struct Event { Action action; uint32_t value; };
struct Screen {
    agent_link_status_t status{};
    char message[160] = "BOOT: start/end call";
    char answer[192]{};
    char session[96] = "No conversation selected";
    uint32_t version = 1;
    uint32_t important_version = 1;
    int64_t text_changed_at = 0;
    bool answer_complete = false;
};
}

class PaperS31Wifi : public Application {
public:
    PaperS31Wifi() {
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(paper_hardware_init());
        screen_ready_ = epd_probe();
        if (audio_.Init() != ESP_OK) ESP_LOGE(TAG, "Audio unavailable");
        int percent; bool charging;
        if (paper_battery_read(&percent, &charging) == ESP_OK) {
            battery_ = percent; charging_ = charging; battery_present_ = true;
        }
        events_ = xQueueCreate(8, sizeof(Event));
        ESP_ERROR_CHECK(events_ ? ESP_OK : ESP_ERR_NO_MEM);
        LoadSelection();
        ESP_ERROR_CHECK(xTaskCreate(ScreenEntry, "paper_screen", 6144, this, 2, nullptr) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(ControlEntry, "paper_control", 12288, this, 4, nullptr) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(KeyEntry, "paper_keys", 3072, this, 4, nullptr) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
    }
    const char *Name() const override { return "PaperS31"; }
    const char *Model() const override { return PAPER_MODEL; }
    const agent_platform_t *Platform() const override {
        static const agent_platform_t platform = {
            .base_url = CLOUD_BASE_URL, .product_id = CLOUD_PRODUCT_ID, .chip_type = CLOUD_CHIP_TYPE,
        };
        return &platform;
    }
    uint32_t Capabilities() const override {
        uint32_t caps = AGENT_CAP_BUTTON | AGENT_CAP_HAPTIC | AGENT_CAP_LED;
        if (battery_present_) caps |= AGENT_CAP_BATTERY;
        if (screen_ready_) caps |= AGENT_CAP_SCREEN;
        if (audio_.Ready()) caps |= AGENT_CAP_MIC | AGENT_CAP_SPEAKER;
        return caps;
    }
    void OnLinkStatus(const agent_link_status_t &status) override {
        std::lock_guard<std::mutex> lock(screen_lock_);
        agent_link_status_t next = status;
        if (status.phase == AGENT_LINK_PHASE_PAIRING) {
            snprintf(next.hint, sizeof next.hint, "Enter this code in the Agent Link console. Code expires automatically.");
        } else if (status.phase == AGENT_LINK_PHASE_SETUP) {
            const char* join = strstr(status.hint, "Join ");
            if (join) snprintf(next.hint, sizeof next.hint, "%s", join);
            else if (screen_.status.phase == AGENT_LINK_PHASE_SETUP)
                snprintf(next.hint, sizeof next.hint, "%s", screen_.status.hint);
        } else if (status.phase == AGENT_LINK_PHASE_CONNECTED || status.phase == AGENT_LINK_PHASE_READY) {
            next.phase = AGENT_LINK_PHASE_CONNECTED;
            snprintf(next.title, sizeof next.title, "Online");
            snprintf(next.hint, sizeof next.hint, "Connected to platform. BOOT: start/end call. Hold BOOT: sessions.");
        } else if (status.phase == AGENT_LINK_PHASE_CONNECTING) {
            const bool retry = strcmp(status.title, "Reconnecting") == 0;
            snprintf(next.title, sizeof next.title, "%s", retry ? "Network unavailable" : "Connecting");
            snprintf(next.hint, sizeof next.hint, "%s", retry ?
                     "Retrying automatically. Check network or platform." : "Connecting to platform. Please wait.");
            next.detail = 0;
        }
        if (screen_.status.phase == next.phase && screen_.status.detail == next.detail &&
            strcmp(screen_.status.title, next.title) == 0 && strcmp(screen_.status.hint, next.hint) == 0 &&
            strcmp(screen_.status.code, next.code) == 0) return;
        screen_.status = next;
        ++screen_.version;
        if (next.phase == AGENT_LINK_PHASE_SETUP || next.phase == AGENT_LINK_PHASE_PAIRING ||
            next.phase == AGENT_LINK_PHASE_BLOCKED || next.phase == AGENT_LINK_PHASE_CONNECTED ||
            strcmp(next.title, "Network unavailable") == 0) screen_.important_version = screen_.version;
    }
    void ShowText(const char *text) override {
        if (!text) return;
        std::lock_guard<std::mutex> lock(screen_lock_);
        char answer[sizeof screen_.answer];
        snprintf(answer, sizeof answer, "%s", text);
        if (strcmp(screen_.answer, answer) == 0) return;
        snprintf(screen_.answer, sizeof screen_.answer, "%s", answer);
        screen_.text_changed_at = esp_timer_get_time();
        screen_.answer_complete = false;
        ++screen_.version;
    }
    void PlayAudio(const uint8_t *pcm, size_t bytes) override { audio_.Play(pcm, bytes); }
    void AudioEnd() override {
        audio_.End();
        std::lock_guard<std::mutex> lock(screen_lock_);
        screen_.answer_complete = true;
    }
    void AudioReset() override { audio_.Reset(); }
    void OnListen(bool start, uint32_t max_ms) override { audio_.Listen(start, max_ms); }
    void Vibrate(uint32_t ms) override { Queue(Action::kVibrate, ms); }
    void SetLed(uint32_t rgb) override { paper_led(rgb != 0); }
    int GetBatteryLevel() override { return battery_.load(); }
    bool IsCharging() override { return charging_.load(); }

private:
    void Queue(Action action, uint32_t value = 0) {
        Event event{action, value};
        (void)xQueueSend(events_, &event, 0);
    }
    uint32_t Message(const char *message, bool important = true) {
        std::lock_guard<std::mutex> lock(screen_lock_);
        snprintf(screen_.message, sizeof screen_.message, "%s", message);
        ++screen_.version;
        if (important) screen_.important_version = screen_.version;
        return screen_.version;
    }
    bool Error(esp_err_t error) {
        if (error == ESP_OK) return false;
        char text[80];
        snprintf(text, sizeof text, "Request failed: %s", esp_err_to_name(error));
        Message(text);
        ESP_LOGW(TAG, "%s", text);
        return true;
    }
    void LoadSelection() {
        nvs_handle_t handle;
        if (nvs_open("paper_wifi", NVS_READONLY, &handle) != ESP_OK) return;
        (void)nvs_get_i32(handle, "agent", &saved_agent_);
        size_t size = sizeof session_;
        (void)nvs_get_str(handle, "session", session_, &size);
        size = sizeof session_name_;
        (void)nvs_get_str(handle, "name", session_name_, &size);
        nvs_close(handle);
        if (saved_agent_ <= 0) session_[0] = session_name_[0] = 0;
    }
    void SaveSelection() {
        nvs_handle_t handle;
        if (nvs_open("paper_wifi", NVS_READWRITE, &handle) != ESP_OK) {
            Message("Could not save selection"); return;
        }
        esp_err_t error = nvs_set_i32(handle, "agent", agent_);
        if (error == ESP_OK) error = nvs_set_str(handle, "session", session_);
        if (error == ESP_OK) error = nvs_set_str(handle, "name", session_name_);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        (void)Error(error);
    }
    void SelectedLabel() {
        std::lock_guard<std::mutex> lock(screen_lock_);
        snprintf(screen_.session, sizeof screen_.session, "%s",
                 session_name_[0] ? session_name_ : (session_[0] ? "Selected conversation" : "No conversation selected"));
        ++screen_.version;
    }
    void EndCall() {
        call_ = false; active_ = false; requested_at_ = 0;
        audio_.Stop();
        (void)agent_link_end_realtime();
    }
    bool SyncAgent() {
        agent_link_platform_agent_t bound{};
        if (agent_link_platform_bound_agent(&bound) != ESP_OK) {
            if (agent_) {
                EndCall(); agent_ = 0; menu_ = false;
                Message("Waiting for platform binding");
            }
            return false;
        }
        if (agent_ != bound.id) {
            EndCall(); menu_ = false;
            if (saved_agent_ != bound.id) session_[0] = session_name_[0] = 0;
            agent_ = saved_agent_ = bound.id;
            SelectedLabel(); SaveSelection();
        }
        voice_supported_ = bound.voice_supported;
        return true;
    }
    void MenuLabel() {
        char message[160];
        if (cursor_ < count_) {
            snprintf(message, sizeof message, "Tap:next Hold:select\n%u: %s",
                     static_cast<unsigned>(offset_ + cursor_ + 1),
                     entries_[cursor_].name[0] ? entries_[cursor_].name : "Conversation");
        } else if (cursor_ == count_) {
            snprintf(message, sizeof message, "Tap:next Hold:create\nNew conversation");
        } else {
            snprintf(message, sizeof message, "Tap:next Hold:return\nBack to home");
        }
        menu_version_ = Message(message);
    }
    void Sessions(size_t offset = 0) {
        EndCall();
        if (!SyncAgent()) { Message("Bind an agent in console"); return; }
        Message("Loading conversations...", false);
        const int requested_agent = agent_;
        size_t count = 0, total = 0;
        if (Error(agent_link_platform_conversations(agent_, offset, entries_, 4, &count, &total))) return;
        if (!count && offset) {
            offset = 0;
            if (Error(agent_link_platform_conversations(agent_, offset, entries_, 4, &count, &total))) return;
        }
        if (!SyncAgent() || requested_agent != agent_) {
            Message("Agent changed; reopen sessions"); return;
        }
        count_ = count; total_ = total; offset_ = offset; cursor_ = 0; menu_ = true;
        MenuLabel();
    }
    void Choose() {
        if (!SyncAgent()) { menu_ = false; Message("Bind an agent in console"); return; }
        if (!menu_) { Message("Agent changed; reopen sessions"); return; }
        if (cursor_ < count_) {
            snprintf(session_, sizeof session_, "%s", entries_[cursor_].id);
            snprintf(session_name_, sizeof session_name_, "%s", entries_[cursor_].name);
        } else if (cursor_ == count_) {
            Message("Creating conversation...", false);
            char created[sizeof session_]{};
            if (Error(agent_link_platform_create_conversation(agent_, created, sizeof created))) return;
            if (!created[0]) { Message("No conversation returned"); return; }
            snprintf(session_, sizeof session_, "%s", created);
            snprintf(session_name_, sizeof session_name_, "New conversation");
        } else {
            menu_ = false; Message("BOOT: start/end call"); return;
        }
        menu_ = false; saved_agent_ = agent_;
        SelectedLabel(); SaveSelection();
        Message("BOOT: start call. Hold: sessions.");
    }
    void Press(bool held) {
        if (menu_) {
            if (screen_ready_ && displayed_version_.load() < menu_version_) return;
            if (held) { Choose(); return; }
            ++cursor_;
            if (cursor_ > count_ + 1) {
                if (offset_ + count_ < total_) { Sessions(offset_ + count_); return; }
                if (offset_) { Sessions(); return; }
                cursor_ = 0;
            }
            MenuLabel(); return;
        }
        if (call_) { EndCall(); Message("Call ended"); return; }
        if (!SyncAgent()) { Message("Finish setup and bind an agent"); return; }
        if (held || !session_[0]) { Sessions(); return; }
        if (!audio_.Ready()) { Message("Audio unavailable - restart"); return; }
        if (!voice_supported_) { Message("Agent has no realtime voice"); return; }
        Message("Starting PCM16 call...", false);
        { std::lock_guard<std::mutex> lock(screen_lock_); screen_.answer[0] = 0; ++screen_.version; }
        esp_err_t error = agent_link_request_realtime_session(agent_, session_);
        if (!Error(error)) {
            call_ = true; active_ = false; requested_at_ = esp_timer_get_time();
        }
    }
    static void ControlEntry(void *ctx) { static_cast<PaperS31Wifi *>(ctx)->ControlLoop(); }
    void ControlLoop() {
        int64_t polled = 0;
        agent_state_t previous_state = AGENT_STATE_DISCONNECTED;
        bool led_on = false;
        while (true) {
            Event event;
            if (xQueueReceive(events_, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (event.action == Action::kVibrate) paper_vibrate(event.value);
                else { paper_vibrate(30); Press(event.action == Action::kHold); }
            }
            const int64_t now = esp_timer_get_time();
            if (now - polled > 1000000) {
                (void)SyncAgent();
                int percent; bool charging;
                if (paper_battery_read(&percent, &charging) != ESP_OK) { percent = -1; charging = false; }
                battery_.store(percent);
                charging_.store(charging);
                const agent_state_t state = agent_link_state();
                if (state == AGENT_STATE_DISCONNECTED) {
                    led_on = !led_on; paper_led(led_on);
                } else if (state != previous_state) {
                    paper_led(false);
                    if (previous_state == AGENT_STATE_DISCONNECTED) paper_vibrate(60);
                }
                previous_state = state;
                polled = now;
            }
            if (call_) {
                if (agent_link_state() == AGENT_STATE_READY) {
                    if (!active_) { active_ = true; paper_vibrate(40); Message("Call active. BOOT to end.", false); }
                    audio_.SetContinuous(true);
                } else if (active_ || now - requested_at_ > 30000000 ||
                           agent_link_state() == AGENT_STATE_DISCONNECTED) {
                    EndCall(); Message("Call ended. BOOT to retry.");
                }
            }
        }
    }
    static void KeyEntry(void *ctx) {
        auto *self = static_cast<PaperS31Wifi *>(ctx);
        int stable = gpio_get_level(BOOT_GPIO), previous = stable, count = 0;
        int64_t pressed = 0;
        while (true) {
            int level = gpio_get_level(BOOT_GPIO);
            count = level == previous ? count + 1 : 1; previous = level;
            if (count >= 3) {
                count = 3;
                if (stable != level) {
                    stable = level;
                    if (!level) pressed = esp_timer_get_time();
                    else if (pressed) {
                        self->Queue(esp_timer_get_time() - pressed >= 1000000 ? Action::kHold : Action::kPress);
                        pressed = 0;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    static void Wrap(char lines[][24], unsigned row, unsigned rows, const char *text) {
        unsigned col = 0;
        while (*text && rows) {
            unsigned char c = static_cast<unsigned char>(*text++);
            if ((c & 0xc0) == 0x80) continue;
            if (c >= 0x80) c = '?';
            if (c == '\n' || col == 23) {
                ++row; --rows; col = 0;
                if (!rows) break;
                if (c == '\n') continue;
            }
            lines[row][col++] = c >= 32 && c < 127 ? c : ' ';
        }
    }
    static void ScreenEntry(void *ctx) { static_cast<PaperS31Wifi *>(ctx)->ScreenLoop(); }
    static void BatteryLine(paper::Page& page, int percent, bool charging) {
        if (percent >= 0) snprintf(page.lines[2], sizeof page.lines[2], "Battery: %d%% %s", percent, charging ? "+" : "");
        else snprintf(page.lines[2], sizeof page.lines[2], "Battery: unavailable");
    }
    void ScreenLoop() {
        paper::RefreshGate gate;
        uint32_t handled_important = 0;
        int painted_battery = battery_.load();
        bool painted_charging = charging_.load();
        char answer[sizeof Screen::answer]{}, painted_answer[sizeof Screen::answer]{};
        while (true) {
            Screen snapshot;
            { std::lock_guard<std::mutex> lock(screen_lock_); snapshot = screen_; }
            const int64_t now = esp_timer_get_time();
            if (!snapshot.answer[0] || snapshot.answer_complete || now - snapshot.text_changed_at >= 5000000)
                snprintf(answer, sizeof answer, "%s", snapshot.answer);
            if (screen_ready_) {
                paper::Page page;
                auto& lines = page.lines;
                snprintf(lines[0], sizeof lines[0], "PaperS31 / WiFi");
                Wrap(lines, 1, 1, snapshot.status.title);
                if (snapshot.status.code[0]) {
                    snprintf(lines[2], sizeof lines[2], "Activation: %s", snapshot.status.code);
                } else BatteryLine(page, painted_battery, painted_charging);
                Wrap(lines, 3, 5, snapshot.status.hint);
                Wrap(lines, 8, 1, snapshot.session);
                Wrap(lines, 9, 2, snapshot.message);
                Wrap(lines, 11, 3, answer);
                const bool important = snapshot.important_version != handled_important || strcmp(answer, painted_answer) != 0;
                const auto action = gate.Observe(page, important, now);
                if (action == paper::Refresh::kSame) {
                    handled_important = snapshot.important_version;
                    displayed_version_.store(snapshot.version);
                    snprintf(painted_answer, sizeof painted_answer, "%s", answer);
                } else if (action == paper::Refresh::kDraw) {
                    const int percent = battery_.load();
                    const bool charging = charging_.load();
                    if (!snapshot.status.code[0]) BatteryLine(page, percent, charging);
                    const char *ptrs[14];
                    for (int i = 0; i < 14; ++i) ptrs[i] = lines[i];
                    ESP_LOGI(TAG, "Display refresh starting (%s)", important ? "important page" : "automatic");
                    const int64_t started = esp_timer_get_time();
                    const esp_err_t error = paper_display_lines(ptrs, 14);
                    const int64_t completed = esp_timer_get_time();
                    ESP_LOGI(TAG, "Display refresh finished: %s, %u ms", esp_err_to_name(error),
                             static_cast<unsigned>((completed - started) / 1000));
                    if (error == ESP_OK) {
                        gate.Complete(page, completed);
                        painted_battery = percent; painted_charging = charging;
                        handled_important = snapshot.important_version;
                        snprintf(painted_answer, sizeof painted_answer, "%s", answer);
                        displayed_version_.store(snapshot.version);
                    } else gate.Failed(completed);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }

    PaperWifiAudio audio_;
    bool screen_ready_ = false;
    bool battery_present_ = false;
    QueueHandle_t events_ = nullptr;
    std::mutex screen_lock_;
    Screen screen_;
    std::atomic<uint32_t> displayed_version_{0};
    uint32_t menu_version_ = 0;
    std::atomic<int> battery_{-1};
    std::atomic<bool> charging_{false};
    int32_t saved_agent_ = 0;
    int agent_ = 0;
    char session_[80]{}, session_name_[96]{};
    agent_link_platform_conversation_t entries_[4]{};
    size_t count_ = 0, total_ = 0, offset_ = 0, cursor_ = 0;
    bool menu_ = false, call_ = false, active_ = false, voice_supported_ = false;
    int64_t requested_at_ = 0;
};

DECLARE_APPLICATION(PaperS31Wifi)
