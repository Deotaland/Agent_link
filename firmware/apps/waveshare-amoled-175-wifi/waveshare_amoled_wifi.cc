#include "application.h"
#include "config.h"
#include "amoled_hardware.h"
#include "amoled_voice_audio.h"
#include "amoled_voice_ui.h"
#include "amoled_voice_controls.h"
#include "agent_link_network.h"
#include "agent_link_friends.h"
#include "agent_link_platform.h"

#include <algorithm>
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

using amoled_voice::Tr;

namespace {
constexpr const char* TAG = "Amoled175.wifi";
enum class Page { kHome, kLauncher, kConversations, kSettings, kSessions, kWifi, kVolume, kLanguage, kFriend, kDelete, kReset };
enum class Action { kConversations, kSettings, kFriend, kWifi, kVolume, kLanguage, kChinese, kEnglish, kSessions, kNew, kDelete, kReset, kBack, kSession, kConfirmDelete, kConfirmReset, kFriendPeer, kFriendAccept, kFriendReject, kFriendCancel, kFriendGreet };
struct Item { Action action; int value; char text[112]; };
}  // namespace

class WaveshareAmoledWifi : public Application {
public:
    WaveshareAmoledWifi() {
        (void)nvs_flash_init();
        LoadSelection();
        hardware_.Init(volume_.load());
        amoled_voice::SetVolume(volume_.load());
        if (audio_.Init(&hardware_) != ESP_OK) ESP_LOGE(TAG, "audio unavailable");
        events_ = xQueueCreate(8, sizeof(amoled_voice::Input));
        ui_ok_ = amoled_voice::StartUi(&hardware_.panel_, &hardware_.touch_, OnTouch, this) == ESP_OK;
        if (!hardware_.touch_.Ready()) amoled_voice::Toast(Tr("触控不可用，请重启", "Touch unavailable - restart"), 30000);
        if (!events_ || xTaskCreate(ControlEntry, "amoled_ctrl", 12288, this, 4, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "controller unavailable");
            amoled_voice::Toast(Tr("控制器不可用，请重启", "Controls unavailable - restart"), 30000);
        }
        if (hardware_.button_ok_ && xTaskCreate(KeyEntry, "amoled_key", 4096, this, 3, nullptr) == pdPASS)
            key_ok_ = true;
        else amoled_voice::Toast(Tr("按键任务不可用", "Key task unavailable"), 5000);
        ESP_LOGI(TAG, "UI: BOOT toggles call; swipe left opens apps; bottom swipe up returns home; touch mirror X/Y enabled");
    }

    const char* Name() const override { return "Amoled175"; }
    const char* Model() const override { return "ESP32-S3-Touch-AMOLED-1.75"; }
    const agent_platform_t* Platform() const override {
        static const agent_platform_t platform = {
            .base_url = CLOUD_BASE_URL, .product_id = CLOUD_PRODUCT_ID, .chip_type = CLOUD_CHIP_TYPE,
        };
        return &platform;
    }
    uint32_t Capabilities() const override {
        uint32_t caps = 0;
        if (ui_ok_) caps |= AGENT_CAP_SCREEN;
        if (key_ok_ && events_) caps |= AGENT_CAP_BUTTON;
        if (audio_.Ready()) caps |= AGENT_CAP_MIC | AGENT_CAP_SPEAKER;
        if (hardware_.power_ok_) caps |= AGENT_CAP_BATTERY;
        return caps;
    }
    void OnLinkStatus(const agent_link_status_t& status) override {
        // The SDK publishes response.started as this hint, before PCM arrives.
        // Actual buffered/playing PCM takes priority over the thinking state.
        thinking_.store(status.phase == AGENT_LINK_PHASE_READY &&
                        strcmp(status.hint, "Receiving agent reply") == 0);
        amoled_voice::SetStatus(status);
    }
    void PlayAudio(const uint8_t* pcm, size_t bytes) override { audio_.Play(pcm, bytes); }
    void AudioEnd() override { audio_.End(); }
    void AudioReset() override { audio_.Reset(); }
    void OnListen(bool start, uint32_t max_ms) override { audio_.Listen(start, max_ms); }
    void ShowText(const char* text) override { amoled_voice::SetAnswer(text); }
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
        if (nvs_open("amoled175", NVS_READONLY, &handle) != ESP_OK) return;
        (void)nvs_get_i32(handle, "agent", &saved_agent_);
        int8_t lang = 0;
        (void)nvs_get_i8(handle, "language", &lang);
        amoled_voice::language.store(lang == 1 ? amoled_voice::Language::kEnglish : amoled_voice::Language::kChinese);
        size_t size = sizeof session_;
        (void)nvs_get_str(handle, "session", session_, &size);
        size = sizeof session_name_; (void)nvs_get_str(handle, "session_name", session_name_, &size);
        nvs_close(handle);
        if (saved_agent_ <= 0) { saved_agent_ = 0; session_[0] = '\0'; }
    }

    void SaveSelection() {
        nvs_handle_t handle;
        if (nvs_open("amoled175", NVS_READWRITE, &handle) != ESP_OK) return;
        esp_err_t error = nvs_set_i32(handle, "agent", agent_);
        if (error == ESP_OK) error = nvs_set_str(handle, "session", session_);
        if (error == ESP_OK) error = nvs_set_str(handle, "agent_name", agent_name_);
        if (error == ESP_OK) error = nvs_set_str(handle, "session_name", session_name_);
        if (error == ESP_OK) error = nvs_set_i8(handle, "language", amoled_voice::language.load() == amoled_voice::Language::kEnglish ? 1 : 0);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        if (error != ESP_OK) amoled_voice::Toast(Tr("会话选择未能保存", "Selection was not saved"));
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
            if (explain) amoled_voice::Toast(result == ESP_ERR_NOT_FOUND ?
                Tr("请先在平台绑定 Agent", "Bind an Agent in the console") : Tr("等待平台认证", "Waiting for authentication"), 4000);
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
        if (!bound.voice_supported && explain) amoled_voice::Toast(Tr("当前 Agent 不支持语音通话", "Agent does not support voice"), 4000);
        return bound.voice_supported;
    }

    void EndCall() {
        const bool was_calling = call_;
        call_ = false; active_ = false; requested_at_ = 0;
        thinking_.store(false);
        if (was_calling) {
            ended_ = true;
            // Publish before waiting for audio/network shutdown. Keep the reply
            // available for reading after the BOOT hangup.
            Publish(); amoled_voice::Wake();
            amoled_voice::Toast(Tr("通话已结束", "Call ended"));
        }
        audio_.Stop();
        (void)agent_link_end_realtime();
    }

    bool Error(esp_err_t error) {
        if (error == ESP_OK) return false;
        char message[96];
        snprintf(message, sizeof message, Tr("请求失败：%s", "Request failed: %s"), esp_err_to_name(error));
        ESP_LOGW(TAG, "%s", message);
        snprintf(error_message_, sizeof error_message_, "%s", message);
        amoled_voice::Toast(message, 5000);
        amoled_voice::SetAnswer(message);
        return true;
    }

    void Busy(const char* message) {
        error_message_[0] = '\0';
        auto& screen = publish_buffer_; screen = {};
        screen.navigation = ++navigation_;
        screen.kind = amoled_voice::View::kInfo; screen.busy = true; screen.calling = call_; screen.view = ++view_;
        snprintf(screen.title, sizeof screen.title, "%s", message);
        snprintf(screen.detail, sizeof screen.detail, "%s", Tr("请稍候", "Please wait"));
        amoled_voice::SetScreen(screen);
    }

    void Clear(Page page) {
        page_ = page; count_ = 0; ++view_; ++navigation_; error_message_[0] = '\0';
        ESP_LOGI(TAG, "Page: %d", static_cast<int>(page));
    }
    void Add(Action action, const char* text, int value = 0) {
        if (count_ >= sizeof items_ / sizeof items_[0]) return;
        Item& item = items_[count_++]; item.action = action; item.value = value;
        snprintf(item.text, sizeof item.text, "%s", text);
    }
    void Launcher() {
        Clear(Page::kLauncher);
        Add(Action::kConversations, Tr("会话", "Sessions"));
        Add(Action::kSettings, Tr("设置", "Settings"));
        Add(Action::kFriend, Tr("交友", "Nearby friends"));
    }
    void Conversations() {
        Clear(Page::kConversations);
        Add(Action::kSessions, Tr("选择会话", "Select session"));
        Add(Action::kNew, Tr("新建会话", "New session"));
        Add(Action::kDelete, Tr("删除当前会话", "Delete current session"));
    }
    void Settings() {
        Clear(Page::kSettings);
        Add(Action::kWifi, Tr("WiFi 状态", "WiFi status"));
        Add(Action::kVolume, Tr("音量调节", "Volume"));
        Add(Action::kLanguage, Tr("语言 / Language", "Language / 语言"));
        Add(Action::kReset, Tr("设备激活", "Device activation"));
    }
    void Languages() {
        Clear(Page::kLanguage);
        Add(Action::kChinese, "中文");
        Add(Action::kEnglish, "English");
    }
    void Volume() { Clear(Page::kVolume); }
    void Sessions(size_t offset = 0, bool scrolling = false) {
        (void)SyncBoundAgent(true);
        if (!agent_) return;
        if (!scrolling) Busy(Tr("正在加载会话", "Loading sessions"));
        size_t count = 0, total = 0;
        memset(fetched_, 0, sizeof fetched_);
        if (Error(agent_link_platform_conversations(agent_, offset, fetched_, amoled_voice::kListRows, &count, &total))) {
            if (!scrolling) Conversations();
            return;
        }
        if (!count && offset) {
            offset = total ? ((total - 1) / amoled_voice::kListStride) * amoled_voice::kListStride : 0;
            if (Error(agent_link_platform_conversations(agent_, offset, fetched_, amoled_voice::kListRows, &count, &total))) {
                if (!scrolling) Conversations();
                return;
            }
        }
        memcpy(sessions_, fetched_, sizeof sessions_);
        if (!scrolling) Clear(Page::kSessions);
        else count_ = 0; // Keep the navigation identity and the overlapping scroll anchor.
        session_offset_ = offset; session_total_ = total;
        for (size_t i = 0; i < count; ++i)
            Add(Action::kSession, sessions_[i].name[0] ? sessions_[i].name : sessions_[i].id, static_cast<int>(i));
    }
    void NewSession() {
        (void)SyncBoundAgent(true);
        if (!agent_) return;
        EndCall();
        Busy(Tr("正在新建会话", "Creating session"));
        char created[sizeof session_] = {};
        if (Error(agent_link_platform_create_conversation(agent_, created, sizeof created))) { Conversations(); return; }
        if (!created[0]) { Sessions(); return; }
        snprintf(session_, sizeof session_, "%s", created);
        snprintf(session_name_, sizeof session_name_, "%s", Tr("新会话", "New session"));
        SaveSelection(); Clear(Page::kHome);
        amoled_voice::SetAnswer(Tr("会话已创建，短按 BOOT开始通话。", "Session created. Press BOOT to call."));
    }

    void Select(int row = -1) {
        if (page_ == Page::kHome) {
            if (call_) { EndCall(); return; }
            if (!audio_.Ready()) { amoled_voice::Toast(Tr("音频不可用，请重启", "Audio unavailable - restart")); return; }
            if (!SyncBoundAgent(true)) { ESP_LOGW(TAG, "Call guard: bound voice agent unavailable"); return; }
            if (!session_[0]) { ESP_LOGI(TAG, "Call guard: select a conversation first"); Sessions(); return; }
            if (agent_link_state() != AGENT_STATE_CONNECTED && agent_link_state() != AGENT_STATE_READY) {
                ESP_LOGW(TAG, "Call guard: platform not connected");
                amoled_voice::Toast(Tr("请先完成 WiFi 配网与设备激活", "Complete WiFi setup and activation")); return;
            }
            amoled_voice::SetAnswer("");
            audio_.ClearFault();
            call_ = true; ended_ = false; attempts_ = 0; last_attempt_ = 0;
            return;
        }
        if (row < 0 || static_cast<size_t>(row) >= count_) return;
        const Item item = items_[row];
        switch (item.action) {
        case Action::kConversations: Conversations(); break;
        case Action::kSettings: Settings(); break;
        case Action::kFriend: OpenFriends(); break;
        case Action::kWifi: Clear(Page::kWifi); break;
        case Action::kVolume: Volume(); break;
        case Action::kLanguage: Languages(); break;
        case Action::kChinese:
        case Action::kEnglish:
            amoled_voice::language.store(item.action == Action::kEnglish ? amoled_voice::Language::kEnglish : amoled_voice::Language::kChinese);
            ESP_LOGI(TAG, "Language: %s", item.action == Action::kEnglish ? "English" : "Chinese");
            SaveSelection(); Languages(); amoled_voice::SetAnswer(""); break;
        case Action::kSessions: Sessions(); break;
        case Action::kNew: NewSession(); break;
        case Action::kDelete:
            if (!session_[0]) { amoled_voice::Toast(Tr("尚未选择会话", "No session selected")); break; }
            Clear(Page::kDelete);
            Add(Action::kBack, Tr("取消", "Cancel"));
            Add(Action::kConfirmDelete, Tr("确认删除", "Confirm delete"));
            break;
        case Action::kReset:
            Clear(Page::kReset);
            Add(Action::kBack, Tr("取消", "Cancel"));
            Add(Action::kConfirmReset, Tr("确认重置激活", "Confirm reset"));
            break;
        case Action::kConfirmReset:
            Busy(Tr("正在重置激活", "Resetting activation"));
            EndCall();
            if (!Error(agent_link_forget())) {
                agent_ = saved_agent_ = 0; voice_supported_ = false;
                session_[0] = agent_name_[0] = session_name_[0] = '\0';
                nvs_handle_t handle;
                if (nvs_open("amoled175", NVS_READWRITE, &handle) == ESP_OK) {
                    (void)nvs_erase_all(handle); (void)nvs_commit(handle); nvs_close(handle);
                }
                Clear(Page::kHome); amoled_voice::SetAnswer("");
                amoled_voice::Toast(Tr("请在平台重新激活设备", "Activate again in the console"), 5000);
            }
            break;
        case Action::kConfirmDelete:
            EndCall();
            Busy(Tr("正在删除会话", "Deleting session"));
            if (!Error(agent_link_platform_delete_conversation(agent_, session_))) {
                session_[0] = '\0'; session_name_[0] = '\0'; SaveSelection(); Sessions();
            }
            break;
        case Action::kSession:
            EndCall();
            snprintf(session_, sizeof session_, "%s", sessions_[item.value].id);
            snprintf(session_name_, sizeof session_name_, "%s", sessions_[item.value].name);
            SaveSelection(); Clear(Page::kHome);
            amoled_voice::SetAnswer(Tr("会话已选择，短按 BOOT开始通话。", "Session selected. Press BOOT to call."));
            break;
        case Action::kFriendPeer:
            if (item.value >= 0 && item.value < friend_view_.peer_count) Error(agent_link_friends_select(friend_view_.peers[item.value].mac));
            break;
        case Action::kFriendAccept: Error(agent_link_friends_confirm(true)); break;
        case Action::kFriendReject: Error(agent_link_friends_confirm(false)); break;
        case Action::kFriendCancel: Error(agent_link_friends_disconnect()); break;
        case Action::kFriendGreet: Error(agent_link_friends_greet()); break;
        case Action::kBack: Back(); break;
        }
    }

    void Back() {
        if (friends_open_) { CloseFriends(Page::kLauncher); return; }
        if (page_ == Page::kHome || page_ == Page::kLauncher) Clear(Page::kHome);
        else if (page_ == Page::kSessions || page_ == Page::kDelete) Conversations();
        else if (page_ == Page::kWifi || page_ == Page::kVolume || page_ == Page::kLanguage || page_ == Page::kReset) Settings();
        else Launcher();
    }
    void PhysicalButton() {
        ESP_LOGI(TAG, "BOOT dispatch: page=%d call=%d state=%d audio=%d session=%d",
            static_cast<int>(page_), call_, static_cast<int>(agent_link_state()), audio_.Ready(), session_[0] != 0);
        amoled_voice::Wake();
        if (friends_open_) { CloseFriends(Page::kHome); return; }
        if (call_) {
            EndCall(); Clear(Page::kHome);
            ESP_LOGI(TAG, "BOOT: call ended");
        } else if (page_ != Page::kHome) Clear(Page::kHome);
        else {
            ESP_LOGI(TAG, "BOOT: start selected conversation");
            Select();
        }
    }
    void ChangeVolume(int next) {
        next = std::max(0, std::min(100, next));
        if (next == volume_.load()) return;
        if (!hardware_.codec_ok_ || hardware_.codec_.SetVolume(next) != ESP_OK) {
            amoled_voice::SetVolume(volume_.load());
            amoled_voice::Toast(Tr("音量调节失败", "Volume change failed"));
            return;
        }
        volume_.store(next);
    }
    void SaveVolume(int value) {
        nvs_handle_t handle;
        if (nvs_open("voice", NVS_READWRITE, &handle) == ESP_OK) {
            esp_err_t error = nvs_set_i8(handle, "vol", static_cast<int8_t>(value));
            if (error == ESP_OK) error = nvs_commit(handle);
            nvs_close(handle);
            if (error != ESP_OK) amoled_voice::Toast(Tr("音量未能保存", "Volume was not saved"));
        } else amoled_voice::Toast(Tr("音量未能保存", "Volume was not saved"));
    }
    void TickCall() {
        if (!call_) return;
        const int64_t now = esp_timer_get_time();
        const agent_state_t state = agent_link_state();
        if (audio_.Fault()) { EndCall(); Error(ESP_FAIL); return; }
        if (state == AGENT_STATE_READY && requested_at_) {
            active_ = true;
            audio_.SetContinuous(true);
            return;
        }
        // SDK retries own established-call recovery. Keep the user's call intent
        // and selected conversation while capture pauses outside READY.
        if (requested_at_) {
            if (agent_link_realtime_running()) {
                if (!active_ && now - requested_at_ > 30000000) {
                    EndCall(); amoled_voice::Toast(Tr("通话连接超时，短按 BOOT重试", "Call timed out - press a key to retry"), 5000);
                }
                return;
            }
            EndCall(); amoled_voice::Toast(Tr("通话已结束，短按 BOOT重试", "Call ended - press a key to retry"), 5000); return;
        }
        if (now - last_attempt_ < 1000000) return;
        last_attempt_ = now;
        if (state != AGENT_STATE_CONNECTED || ++attempts_ > 10) {
            EndCall(); amoled_voice::Toast(Tr("连接暂不可用，请稍后重试", "Connection unavailable - retry later"), 5000); return;
        }
        const esp_err_t error = agent_link_request_realtime_session(agent_, session_);
        if (error == ESP_OK) requested_at_ = now;
        else if (error != ESP_ERR_INVALID_STATE) { EndCall(); Error(error); }
    }

    void OpenFriends() {
        EndCall(); Clear(Page::kFriend);
        friends_open_ = true; friends_closing_ = false;
        if (Error(agent_link_friends_start())) { friends_open_ = false; Launcher(); }
        else RefreshFriends();
    }
    void CloseFriends(Page destination) {
        if (friends_closing_) return;
        if (!Error(agent_link_friends_stop())) {
            friends_closing_ = true; friend_return_ = destination; ++view_;
        }
    }
    void RefreshFriends() {
        agent_friend_snapshot_t next{};
        agent_link_friends_snapshot(&next);
        if (friends_closing_ && (next.state == AGENT_FRIEND_OFF || next.state == AGENT_FRIEND_ERROR)) {
            friends_open_ = friends_closing_ = false;
            if (next.error != ESP_OK) Error(next.error);
            if (friend_return_ == Page::kHome) Clear(Page::kHome); else Launcher();
            return;
        }
        bool changed = next.state != friend_view_.state || next.local_confirmed != friend_view_.local_confirmed ||
                       next.peer_count != friend_view_.peer_count;
        for (int i = 0; i < next.peer_count && !changed; ++i)
            changed = memcmp(next.peers[i].mac, friend_view_.peers[i].mac, 6) != 0;
        if (changed) { ++view_; }
        if (next.received > friend_view_.received) { amoled_voice::Toast(Tr("对方打了个招呼", "Your friend says hello")); amoled_voice::Wake(); }
        if (next.sent > friend_view_.sent) amoled_voice::Toast(Tr("对方已收到招呼", "Your greeting was received"));
        if (next.event != friend_view_.event) {
            if (next.event == AGENT_FRIEND_EVENT_LOST) amoled_voice::Toast(Tr("对方已断开，正在重新寻找", "Peer disconnected - searching again"));
            else if (next.event == AGENT_FRIEND_EVENT_REJECTED) amoled_voice::Toast(Tr("连接已取消", "Connection cancelled"));
            else if (next.event == AGENT_FRIEND_EVENT_TIMEOUT) amoled_voice::Toast(Tr("等待确认超时，请重试", "Confirmation timed out - retry"));
            else if (next.event == AGENT_FRIEND_EVENT_GREETING_FAILED) amoled_voice::Toast(Tr("招呼未收到确认，请重试", "Greeting not confirmed - retry"));
        }
        friend_view_ = next; count_ = 0;
        if (next.state == AGENT_FRIEND_SCANNING) {
            for (int i = 0; i < next.peer_count; ++i) {
                char text[112]; snprintf(text, sizeof text, "%s   %d dBm", next.peers[i].name, next.peers[i].rssi);
                Add(Action::kFriendPeer, text, i);
            }
        } else if (next.state == AGENT_FRIEND_WAITING) Add(Action::kFriendCancel, Tr("取消连接", "Cancel connection"));
        else if (next.state == AGENT_FRIEND_CONFIRM) {
            if (!next.local_confirmed) Add(Action::kFriendAccept, Tr("验证码一致，确认连接", "Codes match - confirm"));
            Add(Action::kFriendReject, Tr("拒绝连接", "Reject connection"));
        } else if (next.state == AGENT_FRIEND_CONNECTED) {
            Add(Action::kFriendGreet, Tr("打个招呼", "Say hello")); Add(Action::kFriendCancel, Tr("断开，继续寻找", "Disconnect and search"));
        }
    }

    void Publish() {
        auto& screen = publish_buffer_; screen = {};
        screen.view = view_; screen.navigation = navigation_; screen.calling = call_; screen.ended = ended_;
        screen.volume_control = page_ == Page::kVolume;
        screen.kind = page_ == Page::kHome ? amoled_voice::View::kHome :
            page_ == Page::kLauncher ? amoled_voice::View::kLauncher :
            page_ == Page::kWifi || page_ == Page::kFriend ? amoled_voice::View::kInfo : amoled_voice::View::kList;
        screen.activity = error_message_[0] || audio_.Fault() ? amoled_voice::Activity::kError :
            !call_ ? amoled_voice::Activity::kIdle :
            agent_link_state() != AGENT_STATE_READY ? amoled_voice::Activity::kConnecting :
            audio_.Speaking() ? amoled_voice::Activity::kSpeaking :
            thinking_.load() ? amoled_voice::Activity::kThinking : amoled_voice::Activity::kListening;
        const char* title = page_ == Page::kLauncher ? Tr("应用菜单", "Apps") :
                            page_ == Page::kConversations ? Tr("会话", "Sessions") :
                            page_ == Page::kSettings ? Tr("设置", "Settings") :
                            page_ == Page::kSessions ? Tr("选择会话", "Select session") :
                            page_ == Page::kWifi ? Tr("WiFi 状态", "WiFi status") :
                            page_ == Page::kVolume ? Tr("音量调节", "Volume") :
                            page_ == Page::kLanguage ? Tr("语言 / Language", "Language / 语言") :
                            page_ == Page::kFriend ? Tr("交友", "Nearby friends") :
                            page_ == Page::kDelete ? Tr("删除会话？", "Delete session?") :
                            page_ == Page::kReset ? Tr("重置设备激活？", "Reset activation?") :
                            agent_name_[0] ? agent_name_ : Tr("语音通话", "Voice call");
        snprintf(screen.title, sizeof screen.title, "%s", title);
        if (page_ == Page::kLauncher)
            snprintf(screen.detail, sizeof screen.detail, "%s", Tr("轻点打开 · 底部上滑回通话", "Tap an app - swipe up to return"));
        else if (page_ == Page::kSettings)
            snprintf(screen.detail, sizeof screen.detail, "%s", Tr("网络、音量与设备", "Network, volume and device"));
        else if (page_ == Page::kVolume)
            snprintf(screen.detail, sizeof screen.detail, Tr("音量 %d%% · 拖动下方滑块", "Volume %d%% - drag below"), volume_.load());
        else if (page_ == Page::kReset)
            snprintf(screen.detail, sizeof screen.detail, "%s", Tr("保留 WiFi，请先在平台解除绑定", "Keeps WiFi. Unbind in console first."));
        else if (page_ == Page::kFriend) {
            screen.local_mode = true;
            screen.busy = friends_closing_ || friend_view_.state == AGENT_FRIEND_STARTING;
            screen.kind = friend_view_.state == AGENT_FRIEND_SCANNING && friend_view_.peer_count ?
                amoled_voice::View::kList : amoled_voice::View::kFriend;
            screen.activity = friend_view_.state == AGENT_FRIEND_ERROR ? amoled_voice::Activity::kError :
                friend_view_.state == AGENT_FRIEND_CONNECTED ? amoled_voice::Activity::kIdle :
                friend_view_.state == AGENT_FRIEND_CONFIRM ? amoled_voice::Activity::kSetup : amoled_voice::Activity::kConnecting;
            snprintf(screen.detail, sizeof screen.detail, "%s", friend_view_.local_name);
            if (friends_closing_) snprintf(screen.info, sizeof screen.info, "%s", Tr("正在恢复 WiFi 与云端连接", "Restoring WiFi and cloud connection"));
            else if (friend_view_.state == AGENT_FRIEND_STARTING) snprintf(screen.info, sizeof screen.info, "%s", Tr("正在进入交友模式", "Starting nearby mode"));
            else if (friend_view_.state == AGENT_FRIEND_SCANNING)
                snprintf(screen.info, sizeof screen.info, "%s", Tr("正在寻找附近设备\n请让另一台设备也进入交友模式", "Searching for devices\nOpen nearby mode on the other device"));
            else if (friend_view_.state == AGENT_FRIEND_WAITING) {
                snprintf(screen.detail, sizeof screen.detail, "%s", friend_view_.remote_name);
                snprintf(screen.info, sizeof screen.info, "%s", Tr("已发出邀请\n等待对方进入配对确认", "Invitation sent\nWaiting for peer confirmation"));
            } else if (friend_view_.state == AGENT_FRIEND_CONFIRM) {
                snprintf(screen.title, sizeof screen.title, "%s", Tr("核对两端验证码", "Compare both codes"));
                snprintf(screen.detail, sizeof screen.detail, "%s", friend_view_.remote_name);
                snprintf(screen.pair_code, sizeof screen.pair_code, "%s", friend_view_.code);
                snprintf(screen.info, sizeof screen.info, "%s / %s", friend_view_.local_confirmed ? Tr("本机已确认", "You: ready") : Tr("本机待确认", "You: waiting"),
                    friend_view_.remote_confirmed ? Tr("对方已确认", "Peer: ready") : Tr("对方待确认", "Peer: waiting"));
            } else if (friend_view_.state == AGENT_FRIEND_CONNECTED) {
                snprintf(screen.title, sizeof screen.title, "%s", Tr("已连接好友", "Friend connected"));
                snprintf(screen.detail, sizeof screen.detail, "%s", friend_view_.remote_name);
                snprintf(screen.info, sizeof screen.info, Tr("收到招呼：%u\n对方确认收到：%u\n短按 BOOT或右滑退出交友", "Greetings received: %u\nGreetings delivered: %u\nPress BOOT or swipe right to exit"), static_cast<unsigned>(friend_view_.received), static_cast<unsigned>(friend_view_.sent));
            } else if (friend_view_.state == AGENT_FRIEND_ERROR) {
                snprintf(screen.info, sizeof screen.info, Tr("交友启动失败：%s\n返回后可重试", "Nearby mode failed: %s\nGo back and retry"), esp_err_to_name(friend_view_.error));
            }
        } else if (page_ == Page::kWifi) {
            const agent_state_t state = agent_link_state();
            const char* platform = state == AGENT_STATE_READY ? Tr("通话中", "In call") :
                                   state == AGENT_STATE_CONNECTED ? Tr("已在线", "Online") : Tr("未就绪", "Not ready");
            snprintf(screen.detail, sizeof screen.detail, "%s", wifi_.connected ? Tr("已连接", "Connected") : wifi_.setup_active ? Tr("正在配网", "WiFi setup") : Tr("未连接", "Disconnected"));
            if (wifi_.connected)
                snprintf(screen.info, sizeof screen.info, Tr("网络：%s\n地址：%s\n信号：%d dBm\n平台：%s", "SSID: %s\nIP: %s\nSignal: %d dBm\nCloud: %s"), wifi_.ssid, wifi_.ip, wifi_.rssi, platform);
            else if (wifi_.setup_active)
                snprintf(screen.info, sizeof screen.info, Tr("连接设备热点：\n%s\n打开 192.168.4.1 完成配网\n平台：%s", "Join device hotspot:\n%s\nOpen 192.168.4.1 to set up WiFi\nCloud: %s"), wifi_.setup_ssid, platform);
            else snprintf(screen.info, sizeof screen.info, Tr("WiFi 正在等待连接\n平台：%s", "Waiting for WiFi\nCloud: %s"), platform);
        } else if (page_ == Page::kSessions) {
            if (!count_) snprintf(screen.detail, sizeof screen.detail, "%s", Tr("暂无会话，返回后可新建", "No sessions. Go back to create one."));
            else snprintf(screen.detail, sizeof screen.detail, Tr("会话 %u–%u / %u", "Sessions %u–%u / %u"), static_cast<unsigned>(session_offset_ + 1),
                static_cast<unsigned>(session_offset_ + count_), static_cast<unsigned>(session_total_));
        } else snprintf(screen.detail, sizeof screen.detail, "%s", session_name_[0] ? session_name_ : session_[0] ? session_ : Tr("尚未选择会话", "No session selected"));
        if (error_message_[0]) snprintf(screen.detail, sizeof screen.detail, "%s", error_message_);
        if (page_ == Page::kHome) {
            snprintf(screen.footer, sizeof screen.footer, "%s", call_ ? Tr("BOOT 挂断 · 左滑应用", "BOOT end · swipe left") : Tr("BOOT 通话 · 左滑应用", "BOOT call · swipe left"));
        } else {
            screen.row_count = static_cast<int>(count_);
            for (size_t i = 0; i < count_; ++i)
                snprintf(screen.rows[i], sizeof screen.rows[i], "%s", items_[i].text);
            if (page_ == Page::kSessions) {
                screen.row_offset = session_offset_;
                screen.previous = session_offset_ > 0;
                screen.next = session_offset_ + count_ < session_total_;
            }
        }
        if (last_activity_ != screen.activity || last_calling_ != call_) {
            ESP_LOGI(TAG, "UI state: activity=%d calling=%d ended=%d view=%lu",
                static_cast<int>(screen.activity), call_, ended_, static_cast<unsigned long>(view_));
            last_activity_ = screen.activity; last_calling_ = call_;
        }
        amoled_voice::SetScreen(screen);
    }

    void ControlLoop() {
        int64_t battery_at = 0, wifi_at = 0;
        while (true) {
            if (friends_open_) RefreshFriends();
            else (void)SyncBoundAgent();
            // Key input is a separate latch: an HTTP request/view change must not
            // discard a physical hangup as an old touchscreen event.
            if (button_pending_.exchange(false)) PhysicalButton();
            amoled_voice::Input event;
            const bool received = xQueueReceive(events_, &event, pdMS_TO_TICKS(50)) == pdTRUE;
            const bool fresh = received && amoled_voice::CurrentInput(event, view_, esp_timer_get_time());
            if (received && !fresh && event.view == view_) ++view_;
            if (fresh) {
                error_message_[0] = '\0';
                ++view_;
                switch (event.action) {
                case amoled_voice::TouchAction::kMenu: if (!friends_open_) Launcher(); break;
                case amoled_voice::TouchAction::kHome: if (friends_open_) CloseFriends(Page::kHome); else Clear(Page::kHome); break;
                case amoled_voice::TouchAction::kRow:
                    if (page_ != Page::kHome) Select(event.row);
                    break;
                case amoled_voice::TouchAction::kBack: Back(); break;
                case amoled_voice::TouchAction::kLoadBefore:
                    if (page_ == Page::kSessions && session_offset_)
                        Sessions(session_offset_ >= amoled_voice::kListStride ? session_offset_ - amoled_voice::kListStride : 0, true);
                    break;
                case amoled_voice::TouchAction::kLoadAfter:
                    if (page_ == Page::kSessions && session_offset_ + count_ < session_total_)
                        Sessions(session_offset_ + amoled_voice::kListStride, true);
                    break;
                case amoled_voice::TouchAction::kVolume: break; // Dedicated mailbox below.

                }
            }
            int desired = 0, saved = -1;
            if (volume_requests_.Take(desired, saved)) {
                ChangeVolume(desired);
                if (saved >= 0) SaveVolume(volume_.load() == desired ? saved : volume_.load());
            }
            TickCall(); Publish(); audio_.ReportDiagnostics(); amoled_voice::ReportUiDiagnostics();
            const int64_t now = esp_timer_get_time();
            if (now >= wifi_at) {
                agent_link_wifi_info_t info = {};
                if (agent_link_wifi_info(&info) == ESP_OK) wifi_ = info;
                else wifi_ = {};
                wifi_at = now + 1000000;
            }
            if (now >= battery_at) {
                int percent = -1; bool charging = false;
                hardware_.Battery(&percent, &charging);
                battery_.store(percent); charging_.store(charging);
                amoled_voice::SetBattery(percent, charging); battery_at = now + 5000000;
            }
        }
    }

    void KeyLoop() {
        amoled_voice::ButtonDebounce key;
        bool previous = hardware_.ButtonDown();
        ESP_LOGI(TAG, "BOOT polling started: GPIO0 level=%d, debounce=40ms", previous ? 0 : 1);
        while (true) {
            const bool down = hardware_.ButtonDown();
            if (down != previous) {
                ESP_LOGI(TAG, "BOOT GPIO0 level=%d", down ? 0 : 1);
                previous = down;
            }
            if (key.Update(down, esp_timer_get_time())) {
                ESP_LOGI(TAG, "BOOT press accepted");
                button_pending_.store(true);
                amoled_voice::Wake();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    static bool OnTouch(const amoled_voice::Input& input, void* context) {
        auto* self = static_cast<WaveshareAmoledWifi*>(context);
        amoled_voice::Wake();
        if (input.action == amoled_voice::TouchAction::kVolume) {
            self->volume_requests_.Submit(input.row, input.commit); return true;
        }
        if (self->events_ && xQueueSend(self->events_, &input, 0) == pdTRUE) return true;
        amoled_voice::Toast(Tr("正在处理，请稍后重试", "Busy - try again")); return false;
    }
    static void KeyEntry(void* ctx) { static_cast<WaveshareAmoledWifi*>(ctx)->KeyLoop(); }
    static void ControlEntry(void* ctx) { static_cast<WaveshareAmoledWifi*>(ctx)->ControlLoop(); }

    AmoledHardware hardware_;
    AmoledVoiceAudio audio_;
    QueueHandle_t events_ = nullptr;
    bool ui_ok_ = false, key_ok_ = false;
    std::atomic<bool> button_pending_{false};
    agent_link_wifi_info_t wifi_ = {};
    std::atomic<bool> thinking_{false};
    std::atomic<int> battery_{-1};
    std::atomic<bool> charging_{false};
    std::atomic<int> volume_{AUDIO_DEFAULT_VOLUME};
    uint32_t view_ = 1, navigation_ = 1;
    amoled_voice::VolumeMailbox volume_requests_;
    amoled_voice::Screen publish_buffer_;
    amoled_voice::Activity last_activity_ = amoled_voice::Activity::kOffline;
    bool last_calling_ = false;
    bool voice_supported_ = false;
    Page page_ = Page::kHome;
    Item items_[amoled_voice::kListRows] = {};
    size_t count_ = 0, session_offset_ = 0, session_total_ = 0;
    agent_link_platform_conversation_t sessions_[amoled_voice::kListRows] = {}, fetched_[amoled_voice::kListRows] = {};
    int32_t agent_ = 0, saved_agent_ = 0;
    char agent_name_[80] = {}, session_[80] = {}, session_name_[96] = {};
    char error_message_[96] = {};
    agent_friend_snapshot_t friend_view_ = {};
    bool friends_open_ = false, friends_closing_ = false;
    Page friend_return_ = Page::kLauncher;
    bool call_ = false, active_ = false, ended_ = false;
    int attempts_ = 0;
    int64_t requested_at_ = 0, last_attempt_ = 0;
};

DECLARE_APPLICATION(WaveshareAmoledWifi);
