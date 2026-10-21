#include "application.h"
#include "config.h"
#include "epaper_hardware.h"
#include "epaper_wifi_audio.h"
#include "epaper_wifi_ui.h"
#include "epaper_controls.h"
#include "agent_link_platform.h"

#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char* TAG = "WS397.wifi";
enum class Page { kHome, kMenu, kSessions, kReset };
enum class Action { kUp, kDown, kCenter, kMenu, kBack };
struct Input { Action action; uint32_t view; };
}

class WaveshareEpaperWifi : public Application {
public:
    WaveshareEpaperWifi() {
        ESP_ERROR_CHECK(nvs_flash_init());
        Load();
        const esp_err_t hardware = hardware_.Init(volume_);
        hardware_ok_ = hardware == ESP_OK;
        if (hardware != ESP_OK) ESP_LOGE(TAG, "Hardware init: %s", esp_err_to_name(hardware));
        if (audio_.Init(&hardware_) != ESP_OK) ESP_LOGE(TAG, "Audio unavailable");
        screen_ok_ = ui_.Start(&hardware_) == ESP_OK;
        events_ = xQueueCreate(8, sizeof(Input));
        ESP_ERROR_CHECK(events_ ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(ControlEntry, "ep397_control", 12288, this, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(KeyEntry, "ep397_keys", 3072, this, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
    const char* Name() const override { return "WavePaper397"; }
    const char* Model() const override { return EPAPER_MODEL; }
    const agent_platform_t* Platform() const override {
        static const agent_platform_t platform = {
            .base_url = CLOUD_BASE_URL, .product_id = CLOUD_PRODUCT_ID, .chip_type = CLOUD_CHIP_TYPE,
        };
        return CLOUD_PRODUCT_ID > 0 ? &platform : nullptr;
    }
    uint32_t Capabilities() const override {
        uint32_t caps = AGENT_CAP_BUTTON;
        if (screen_ok_) caps |= AGENT_CAP_SCREEN;
        if (audio_.Ready()) caps |= AGENT_CAP_MIC | AGENT_CAP_SPEAKER;
        if (hardware_ok_) caps |= AGENT_CAP_BATTERY;
        return caps;
    }
    void OnLinkStatus(const agent_link_status_t& status) override { ui_.Status(status); }
    void ShowText(const char* text) override { ui_.Answer(text); }
    void PlayAudio(const uint8_t* pcm, size_t bytes) override { audio_.Play(pcm, bytes); }
    void AudioEnd() override { audio_.End(); ui_.EndAnswer(); }
    void AudioReset() override { audio_.Reset(); }
    void OnListen(bool start, uint32_t max_ms) override { audio_.Listen(start, max_ms); }
    int GetBatteryLevel() override { return battery_.load(); }
    bool IsCharging() override { return charging_.load(); }

private:
    void Load() {
        nvs_handle_t handle;
        if (nvs_open("ws397_wifi", NVS_READONLY, &handle) != ESP_OK) return;
        (void)nvs_get_i32(handle, "agent", &saved_agent_);
        size_t size = sizeof session_; (void)nvs_get_str(handle, "session", session_, &size);
        size = sizeof session_name_; (void)nvs_get_str(handle, "name", session_name_, &size);
        int32_t volume = AUDIO_DEFAULT_VOLUME;
        if (nvs_get_i32(handle, "volume", &volume) == ESP_OK && volume >= 0 && volume <= 100) volume_ = volume;
        nvs_close(handle);
        if (saved_agent_ <= 0) session_[0] = session_name_[0] = 0;
    }
    void Save() {
        nvs_handle_t handle;
        if (nvs_open("ws397_wifi", NVS_READWRITE, &handle) != ESP_OK) { Message("Could not save settings"); return; }
        esp_err_t error = nvs_set_i32(handle, "agent", saved_agent_);
        if (error == ESP_OK) error = nvs_set_str(handle, "session", session_);
        if (error == ESP_OK) error = nvs_set_str(handle, "name", session_name_);
        if (error == ESP_OK) error = nvs_set_i32(handle, "volume", volume_);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        if (error != ESP_OK) Message("Could not save settings");
    }
    void Message(const char* message) { snprintf(message_, sizeof message_, "%s", message); }
    bool Error(esp_err_t error) {
        if (error == ESP_OK) return false;
        snprintf(message_, sizeof message_, "Request failed: %s", esp_err_to_name(error));
        ESP_LOGW(TAG, "%s", message_); return true;
    }
    void Home() { page_ = Page::kHome; cursor_ = 0; ++view_; }
    void EndCall() {
        call_ = active_ = false; requested_ = 0;
        audio_.Stop(); (void)agent_link_end_realtime();
    }
    bool SyncAgent(bool explain = false) {
        agent_link_platform_agent_t bound{};
        const esp_err_t error = agent_link_platform_bound_agent(&bound);
        if (error != ESP_OK) {
            // A transient authentication outage must not erase a saved selection.
            if (agent_ && (call_ || page_ != Page::kHome)) { EndCall(); Home(); }
            agent_ = 0; voice_ = false;
            if (explain) Message("Finish activation and bind an Agent in the platform console");
            return false;
        }
        if (agent_ != bound.id) {
            EndCall(); Home();
            if (saved_agent_ != bound.id) {
                session_[0] = session_name_[0] = 0;
                Message("Agent changed. Choose or create a conversation.");
            }
            agent_ = saved_agent_ = bound.id;
            Save();
        }
        snprintf(agent_name_, sizeof agent_name_, "%s", bound.name);
        voice_ = bound.voice_supported;
        return true;
    }
    void Busy(const char* title) {
        epaper::Screen screen{}; screen.view = ++view_;
        snprintf(screen.title, sizeof screen.title, "%s", title);
        snprintf(screen.detail, sizeof screen.detail, "Please wait...");
        screen.footer[0] = 0; ui_.Publish(screen);
    }
    void Sessions(size_t offset = 0) {
        EndCall(); if (!SyncAgent(true)) return;
        Busy("Loading conversations");
        const int requested_agent = agent_;
        agent_link_platform_conversation_t fetched[3]{};
        size_t count = 0, total = 0;
        if (Error(agent_link_platform_conversations(requested_agent, offset, fetched, 3, &count, &total))) return;
        if (!count && offset) {
            offset = 0;
            if (Error(agent_link_platform_conversations(requested_agent, offset, fetched, 3, &count, &total))) return;
        }
        if (!SyncAgent(true) || requested_agent != agent_) { Home(); Message("Agent changed; reopen conversations"); return; }
        memcpy(sessions_, fetched, sizeof sessions_);
        count_ = count; offset_ = offset; more_ = offset + count < total;
        page_ = Page::kSessions; cursor_ = 0; ++view_;
        message_[0] = 0;
    }
    void NewSession() {
        EndCall(); if (!SyncAgent(true)) return;
        Busy("Creating conversation");
        const int requested_agent = agent_;
        char created[sizeof session_]{};
        if (Error(agent_link_platform_create_conversation(requested_agent, created, sizeof created))) return;
        if (!SyncAgent(true) || requested_agent != agent_) { Home(); Message("Agent changed; reopen conversations"); return; }
        if (!created[0]) { Message("Platform returned no conversation"); return; }
        snprintf(session_, sizeof session_, "%s", created);
        snprintf(session_name_, sizeof session_name_, "New conversation");
        Save(); Home(); Message("Conversation ready. Center: start call.");
    }
    void Call() {
        if (call_) { EndCall(); Message("Call ended"); return; }
        if (!SyncAgent(true)) return;
        if (!session_[0]) { Sessions(); return; }
        if (!voice_) { Message("Bound Agent does not support realtime voice"); return; }
        if (!audio_.Ready()) { Message("Audio unavailable. Check hardware and restart."); return; }
        const agent_state_t state = agent_link_state();
        if (state != AGENT_STATE_CONNECTED && state != AGENT_STATE_READY) { Message("Waiting for platform connection"); return; }
        ui_.Answer("");
        if (!Error(agent_link_request_realtime_session(agent_, session_))) {
            call_ = true; active_ = false; requested_ = esp_timer_get_time();
            Message("Connecting voice. BOOT or center: end.");
        }
    }
    void Choose() {
        if (page_ == Page::kHome) { Call(); return; }
        if (page_ == Page::kMenu) {
            if (cursor_ == 0) Sessions();
            else if (cursor_ == 1) NewSession();
            else if (cursor_ == 2) { page_ = Page::kReset; cursor_ = 0; ++view_; }
            else Home();
        } else if (page_ == Page::kReset) {
            if (cursor_ == 1) {
                EndCall();
                if (!Error(agent_link_forget())) {
                    session_[0] = session_name_[0] = 0; agent_ = saved_agent_ = 0;
                    Save(); Home(); Message("Unbind in console before activating again. WiFi is retained.");
                }
            } else Home();
        } else {
            if (!SyncAgent(true) || page_ != Page::kSessions) return;
            if (cursor_ < count_) {
                snprintf(session_, sizeof session_, "%s", sessions_[cursor_].id);
                snprintf(session_name_, sizeof session_name_, "%s", sessions_[cursor_].name);
                Save(); Home(); Message("Conversation selected. Center: start call.");
            } else {
                size_t row = cursor_ - count_;
                if (more_ && row-- == 0) { Sessions(offset_ + count_); return; }
                if (offset_ && row-- == 0) { Sessions(offset_ >= 3 ? offset_ - 3 : 0); return; }
                Home();
            }
        }
    }
    size_t Rows() const {
        if (page_ == Page::kMenu) return 4;
        if (page_ == Page::kReset) return 2;
        return count_ + (more_ ? 1 : 0) + (offset_ ? 1 : 0) + 1;
    }
    void Publish() {
        epaper::Screen screen{}; screen.view = view_;
        if (CLOUD_PRODUCT_ID <= 0) {
            snprintf(screen.title, sizeof screen.title, "Platform product ID is not configured");
            snprintf(screen.detail, sizeof screen.detail, "Register this profile in products.json and rebuild.");
        } else {
            snprintf(screen.title, sizeof screen.title, "%s", page_ == Page::kMenu ? "Conversations / setup" :
                     page_ == Page::kSessions ? "Choose conversation" : page_ == Page::kReset ? "Reset activation?" :
                     active_ ? "Call active / listening" : call_ ? "Connecting voice..." : "Realtime voice");
            if (page_ == Page::kReset) snprintf(screen.detail, sizeof screen.detail, "Keeps WiFi. Unbind in console before activating again.");
            else if (message_[0]) snprintf(screen.detail, sizeof screen.detail, "%s", message_);
            else snprintf(screen.detail, sizeof screen.detail, "%s / %s", agent_name_[0] ? agent_name_ : "Bind Agent in console",
                          session_name_[0] ? session_name_ : session_[0] ? "Selected conversation" : "No conversation selected");
        }
        if (page_ != Page::kHome) {
            screen.selected = static_cast<int>(cursor_);
            snprintf(screen.footer, sizeof screen.footer, "Up/Down: choose   Center: select   BOOT: back");
            if (page_ == Page::kMenu) {
                snprintf(screen.rows[0], sizeof screen.rows[0], "Choose existing conversation");
                snprintf(screen.rows[1], sizeof screen.rows[1], "Create new conversation");
                snprintf(screen.rows[2], sizeof screen.rows[2], "Reset platform activation");
                snprintf(screen.rows[3], sizeof screen.rows[3], "Back");
            } else if (page_ == Page::kReset) {
                snprintf(screen.rows[0], sizeof screen.rows[0], "Cancel");
                snprintf(screen.rows[1], sizeof screen.rows[1], "Confirm reset activation");
            } else {
                size_t row = 0;
                for (; row < count_; ++row)
                    snprintf(screen.rows[row], sizeof screen.rows[row], "%u. %s", static_cast<unsigned>(offset_ + row + 1),
                             sessions_[row].name[0] ? sessions_[row].name : "Conversation");
                if (more_) snprintf(screen.rows[row++], sizeof screen.rows[0], "Next page");
                if (offset_) snprintf(screen.rows[row++], sizeof screen.rows[0], "Previous page");
                snprintf(screen.rows[row], sizeof screen.rows[row], "Back");
                if (!count_) snprintf(screen.detail, sizeof screen.detail, "No conversations. Hold center > Create new conversation.");
            }
        } else {
            snprintf(screen.footer, sizeof screen.footer, "Center: call   Hold: menu   Up/Down: vol %d%%   BOOT: end", volume_);
        }
        ui_.Publish(screen);
    }
    void ControlLoop() {
        int64_t poll = 0, volume_at = 0;
        bool volume_dirty = false;
        while (true) {
            Input input;
            if (xQueueReceive(events_, &input, pdMS_TO_TICKS(50)) == pdTRUE) {
                // Ending a call and changing volume remain usable while e-paper is refreshing.
                if (input.action == Action::kBack) {
                    EndCall(); Home(); Message("Call ended. Hold center: menu.");
                } else if (page_ == Page::kHome && (input.action == Action::kUp || input.action == Action::kDown)) {
                    int next = epaper::Volume(volume_, input.action == Action::kUp ? 1 : -1);
                    if (hardware_.AudioReady() && !Error(hardware_.codec.SetVolume(next))) {
                        volume_ = next; volume_dirty = true; volume_at = esp_timer_get_time();
                    }
                } else if (page_ == Page::kHome && call_ && input.action == Action::kCenter) {
                    EndCall(); Message("Call ended");
                } else if (epaper::Current(input.view, view_)) {
                    message_[0] = 0; ++view_;
                    if (input.action == Action::kMenu) {
                        EndCall(); page_ = Page::kMenu; cursor_ = 0;
                    } else if (input.action == Action::kCenter) Choose();
                    else if (page_ != Page::kHome) {
                        const size_t rows = Rows();
                        cursor_ = input.action == Action::kUp ? (cursor_ + rows - 1) % rows : (cursor_ + 1) % rows;
                    }
                }
            }
            const int64_t now = esp_timer_get_time();
            if (now >= poll) {
                (void)SyncAgent();
                int percent = -1; bool charging = false;
                if (hardware_.Battery(&percent, &charging) == ESP_OK) {
                    battery_.store(percent); charging_.store(charging);
                }
                poll = now + 1000000;
            }
            if (call_) {
                const agent_state_t state = agent_link_state();
                if (state == AGENT_STATE_READY) {
                    if (!active_) { active_ = true; Message("Speak now. Center or BOOT: end call."); }
                    audio_.SetContinuous(true);
                } else if (active_ || state == AGENT_STATE_DISCONNECTED || now - requested_ >= 30000000) {
                    EndCall(); Message("Call ended or connection failed. Center: retry.");
                }
            }
            if (volume_dirty && now - volume_at >= 1000000) { Save(); volume_dirty = false; }
            Publish();
        }
    }
    void Keys() {
        epaper::Key keys[4];
        bool down[4]{};
        uint32_t pressed_view[4]{};
        const gpio_num_t pins[] = {KEY_UP, KEY_DOWN, KEY_CENTER, KEY_BOOT};
        while (true) {
            const int64_t now = esp_timer_get_time();
            for (int i = 0; i < 4; ++i) {
                const bool pressed = !gpio_get_level(pins[i]);
                if (pressed && !down[i]) pressed_view[i] = ui_.Displayed();
                down[i] = pressed;
                const epaper::Press press = keys[i].Poll(pressed, now);
                if (press == epaper::Press::kNone) continue;
                Action action = i == 0 ? Action::kUp : i == 1 ? Action::kDown : i == 3 ? Action::kBack :
                                press == epaper::Press::kHold ? Action::kMenu : Action::kCenter;
                Input input{action, pressed_view[i]};
                if (xQueueSend(events_, &input, 0) != pdTRUE) ESP_LOGW(TAG, "Input busy");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    static void ControlEntry(void* context) { static_cast<WaveshareEpaperWifi*>(context)->ControlLoop(); }
    static void KeyEntry(void* context) { static_cast<WaveshareEpaperWifi*>(context)->Keys(); }

    EpaperHardware hardware_;
    EpaperWifiAudio audio_;
    epaper::Ui ui_;
    QueueHandle_t events_ = nullptr;
    bool screen_ok_ = false, hardware_ok_ = false;
    std::atomic<int> battery_{-1};
    std::atomic<bool> charging_{false};
    int volume_ = AUDIO_DEFAULT_VOLUME;
    int32_t agent_ = 0, saved_agent_ = 0;
    char session_[80]{}, session_name_[96]{}, agent_name_[80]{}, message_[192]{};
    agent_link_platform_conversation_t sessions_[3]{};
    size_t cursor_ = 0, count_ = 0, offset_ = 0;
    bool more_ = false, voice_ = false, call_ = false, active_ = false;
    Page page_ = Page::kHome;
    uint32_t view_ = 1;
    int64_t requested_ = 0;
};

DECLARE_APPLICATION(WaveshareEpaperWifi);
