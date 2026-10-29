#include "application.h"
#include "config.h"
#include "epaper_hardware.h"
#include "epaper_wifi_audio.h"
#include "epaper_wifi_ui.h"
#include "epaper_controls.h"
#include "epaper_requests.h"
#include "agent_link_platform.h"
#include "agent_link_network.h"

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
using epaper::Action;
struct Input { Action action; uint32_t view; bool setup_at_press; };
enum class JobKind { kList, kCreate };
struct Job { JobKind kind; uint32_t id; int agent; size_t offset; };
struct JobResult {
    Job job{};
    esp_err_t error = ESP_OK;
    agent_link_platform_conversation_t sessions[3]{};
    size_t count = 0, total = 0;
    char created[80]{};
};
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
        events_ = xQueueCreate(32, sizeof(Input));
        jobs_ = xQueueCreate(1, sizeof(Job));
        results_ = xQueueCreate(1, sizeof(JobResult));
        ESP_ERROR_CHECK(events_ && jobs_ && results_ ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(NetworkEntry, "ep397_http", 12288, this, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
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
    void OnLinkStatus(const agent_link_status_t& status) override { phase_.store(status.phase); ui_.Status(status); }
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
    void ChangePage(Page page) {
        requests_.Cancel(); confirm_.Cancel();
        page_ = page; cursor_ = 0; ++view_; page_view_ = view_;
    }
    void Home() { ChangePage(Page::kHome); }
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
            agent_ = 0; voice_ = false; agent_name_[0] = 0;
            if (explain) Message("请先激活设备并在平台绑定助手");
            return false;
        }
        if (agent_ != bound.id) {
            EndCall(); Home();
            if (saved_agent_ != bound.id) {
                session_[0] = session_name_[0] = 0;
                Message("助手已更换，请选择或创建会话");
            }
            agent_ = saved_agent_ = bound.id;
            Save();
        }
        snprintf(agent_name_, sizeof agent_name_, "%s", bound.name);
        voice_ = bound.voice_supported;
        return true;
    }
    void Submit(JobKind kind, size_t offset = 0) {
        const uint32_t id = requests_.Begin();
        if (!id) { Message("上个请求正在结束，请稍后重试"); return; }
        Job job{kind, id, agent_, offset};
        if (xQueueSend(jobs_, &job, 0) != pdTRUE) {
            (void)requests_.Complete(id); Message("请求队列忙，请重试"); return;
        }
        pending_job_ = kind; message_[0] = 0; ++view_;
        ESP_LOGI(TAG, "HTTP queued id=%u", unsigned(id));
    }
    void Sessions(size_t offset = 0) {
        EndCall(); if (!SyncAgent(true)) return;
        Submit(JobKind::kList, offset);
    }
    void NewSession() {
        EndCall(); if (!SyncAgent(true)) return;
        Submit(JobKind::kCreate);
    }
    void NetworkLoop() {
        while (true) {
            Job job{};
            if (xQueueReceive(jobs_, &job, portMAX_DELAY) != pdTRUE) continue;
            JobResult result{}; result.job = job;
            const int64_t started = esp_timer_get_time();
            if (job.kind == JobKind::kList) {
                result.error = agent_link_platform_conversations(job.agent, job.offset, result.sessions, 3, &result.count, &result.total);
                if (result.error == ESP_OK && !result.count && job.offset) {
                    result.job.offset = 0;
                    result.error = agent_link_platform_conversations(job.agent, 0, result.sessions, 3, &result.count, &result.total);
                }
            } else {
                result.error = agent_link_platform_create_conversation(job.agent, result.created, sizeof result.created);
            }
            ESP_LOGI(TAG, "HTTP finished id=%u time=%ums result=%s", unsigned(job.id),
                     unsigned((esp_timer_get_time() - started) / 1000), esp_err_to_name(result.error));
            (void)xQueueSend(results_, &result, portMAX_DELAY);
        }
    }
    void ReceiveNetwork() {
        JobResult result{};
        if (xQueueReceive(results_, &result, 0) != pdTRUE) return;
        if (!requests_.Complete(result.job.id)) {
            ESP_LOGI(TAG, "HTTP result discarded id=%u", unsigned(result.job.id)); return;
        }
        if (!SyncAgent(true) || result.job.agent != agent_) { Message("助手已变更，请重新打开会话"); return; }
        if (Error(result.error)) return;
        if (result.job.kind == JobKind::kList) {
            memcpy(sessions_, result.sessions, sizeof sessions_);
            count_ = result.count; offset_ = result.job.offset; more_ = offset_ + count_ < result.total;
            ChangePage(Page::kSessions); message_[0] = 0;
        } else {
            if (!result.created[0]) { Message("平台没有返回会话"); return; }
            snprintf(session_, sizeof session_, "%s", result.created);
            snprintf(session_name_, sizeof session_name_, "新会话");
            Save(); Home(); Message("会话已创建，按中键通话");
        }
    }
    void Call() {
        if (call_) { EndCall(); Message("通话已结束"); return; }
        if (!SyncAgent(true)) return;
        if (!session_[0]) { Sessions(); return; }
        if (!voice_) { Message("当前助手不支持实时语音"); return; }
        if (!audio_.Ready()) { Message("音频不可用，请检查设备并重启"); return; }
        const agent_state_t state = agent_link_state();
        if (state != AGENT_STATE_CONNECTED && state != AGENT_STATE_READY) { Message("等待平台连接"); return; }
        ui_.Answer("");
        if (!Error(agent_link_request_realtime_session(agent_, session_))) {
            call_ = true; active_ = false; requested_ = esp_timer_get_time();
            Message("正在连接，按中键或 BOOT 结束");
        }
    }
    void Choose() {
        if (page_ == Page::kHome) { Call(); return; }
        if (page_ == Page::kMenu) {
            if (cursor_ == 0) Sessions();
            else if (cursor_ == 1) NewSession();
            else if (cursor_ == 2) ChangePage(Page::kReset);
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
        epaper::Screen screen{}; screen.view = view_; screen.page = static_cast<int>(page_);
        screen.volume = volume_; screen.battery = battery_.load(); screen.charging = charging_.load();
        snprintf(screen.setup_ssid, sizeof screen.setup_ssid, "%s", setup_ssid_);
        snprintf(screen.agent, sizeof screen.agent, "%s", agent_name_);
        snprintf(screen.session, sizeof screen.session, "%s", session_name_[0] ? session_name_ : session_[0] ? "已选择" : "");
        if (requests_.Busy()) {
            snprintf(screen.title, sizeof screen.title, "%s", pending_job_ == JobKind::kList ? "正在加载会话" : "正在创建会话");
            snprintf(screen.detail, sizeof screen.detail, "可按 BOOT 取消等待");
            snprintf(screen.footer, sizeof screen.footer, "BOOT：取消等待");
            ui_.Publish(screen); return;
        }
        if (CLOUD_PRODUCT_ID <= 0) {
            snprintf(screen.title, sizeof screen.title, "Platform product ID is not configured");
            snprintf(screen.detail, sizeof screen.detail, "Register this profile in products.json and rebuild.");
        } else {
            snprintf(screen.title, sizeof screen.title, "%s", page_ == Page::kMenu ? "菜单" :
                     page_ == Page::kSessions ? "选择会话" : page_ == Page::kReset ? "重置激活？" :
                     active_ ? "正在通话" : call_ ? "正在连接语音" : "语音助手");
            if (page_ == Page::kReset) snprintf(screen.detail, sizeof screen.detail, "保留 WiFi，重新激活前请在平台解绑。");
            else if (message_[0]) snprintf(screen.detail, sizeof screen.detail, "%s", message_);
            else snprintf(screen.detail, sizeof screen.detail, "%s", !agent_ ? "请在平台绑定助手" :
                          !session_[0] ? "长按中键选择或创建会话" : active_ ? "请开始说话" :
                          call_ ? "正在连接，请稍候" : "按中键开始通话");
        }
        if (page_ != Page::kHome) {
            screen.selected = static_cast<int>(cursor_);
            snprintf(screen.footer, sizeof screen.footer, "上下：选择   中键：确认\nBOOT：返回");
            if (page_ == Page::kMenu) {
                snprintf(screen.rows[0], sizeof screen.rows[0], "选择已有会话");
                snprintf(screen.rows[1], sizeof screen.rows[1], "创建新会话");
                snprintf(screen.rows[2], sizeof screen.rows[2], "重置平台激活");
                snprintf(screen.rows[3], sizeof screen.rows[3], "返回");
            } else if (page_ == Page::kReset) {
                snprintf(screen.rows[0], sizeof screen.rows[0], "取消");
                snprintf(screen.rows[1], sizeof screen.rows[1], "确认重置激活");
            } else {
                size_t row = 0;
                for (; row < count_; ++row)
                    snprintf(screen.rows[row], sizeof screen.rows[row], "%u. %s", static_cast<unsigned>(offset_ + row + 1),
                             sessions_[row].name[0] ? sessions_[row].name : "Conversation");
                if (more_) snprintf(screen.rows[row++], sizeof screen.rows[0], "下一页");
                if (offset_) snprintf(screen.rows[row++], sizeof screen.rows[0], "上一页");
                snprintf(screen.rows[row], sizeof screen.rows[row], "返回");
                if (!count_) snprintf(screen.detail, sizeof screen.detail, "暂无会话，请在菜单创建。");
            }
        } else {
            snprintf(screen.footer, sizeof screen.footer, "中键：%s   长按：菜单\n上下：音量   BOOT：结束", call_ ? "结束" : "通话");
        }
        ui_.Publish(screen);
    }
    void HandleInput(const Input& input) {
        if (!epaper::InputAllowed(input.setup_at_press || ui_.DisplayingSetup() ||
                                  phase_.load() == AGENT_LINK_PHASE_SETUP, input.action)) return;
        ESP_LOGI(TAG, "key=%d view=%u current=%u", int(input.action), unsigned(input.view), unsigned(view_));
        if (input.action == Action::kBack) {
            confirm_.Cancel();
            if (requests_.Busy()) { requests_.Cancel(); ++view_; Message("已取消等待"); }
            else if (call_) { EndCall(); Home(); Message("通话已结束"); }
            else if (page_ == Page::kSessions || page_ == Page::kReset) ChangePage(Page::kMenu);
            else if (page_ == Page::kMenu) Home();
            return;
        }
        if (page_ == Page::kHome && (input.action == Action::kUp || input.action == Action::kDown)) {
            const int next = epaper::Volume(volume_, input.action == Action::kUp ? 1 : -1);
            if (next != volume_ && hardware_.AudioReady() && !Error(hardware_.codec.SetVolume(next))) {
                volume_ = next; volume_dirty_ = true; volume_at_ = esp_timer_get_time();
            }
            return;
        }
        if (page_ == Page::kHome && call_ && input.action == Action::kCenter) {
            confirm_.Cancel(); EndCall(); Message("通话已结束"); return;
        }
        if (requests_.Busy() || confirm_.Pending() || !epaper::NavigationCurrent(input.view, page_view_, view_)) return;
        if (page_ != Page::kHome && (input.action == Action::kUp || input.action == Action::kDown)) {
            message_[0] = 0; ++view_;
            const size_t rows = Rows();
            cursor_ = input.action == Action::kUp ? (cursor_ + rows - 1) % rows : (cursor_ + 1) % rows;
        } else if (input.action == Action::kMenu) {
            message_[0] = 0; EndCall(); ChangePage(Page::kMenu);
        } else if (input.action == Action::kCenter) {
            // Freeze this target until its highlight has appeared, then execute
            // exactly once. A quick confirm during refresh is queued, not lost.
            confirm_.Request(view_);
            ESP_LOGI(TAG, "confirm queued view=%u row=%u", unsigned(view_), unsigned(cursor_));
        }
    }
    void ControlLoop() {
        int64_t poll = 0;
        while (true) {
            const bool provisioning = phase_.load() == AGENT_LINK_PHASE_SETUP;
            if (provisioning) {
                confirm_.Cancel(); requests_.Cancel();
                if (call_) EndCall();
                if (page_ != Page::kHome) Home();
            }
            ReceiveNetwork();
            Input input;
            if (xQueueReceive(events_, &input, pdMS_TO_TICKS(20)) == pdTRUE) HandleInput(input);
            if (!ui_.DisplayingSetup() && phase_.load() != AGENT_LINK_PHASE_SETUP && confirm_.Take(ui_.Displayed(), view_)) {
                message_[0] = 0; ++view_;
                ESP_LOGI(TAG, "confirm executed row=%u", unsigned(cursor_));
                Choose();
            }
            const int64_t now = esp_timer_get_time();
            if (now >= poll) {
                (void)SyncAgent();
                int percent = -1; bool charging = false;
                const esp_err_t battery_error = hardware_.Battery(&percent, &charging);
                battery_.store(battery_error == ESP_OK ? percent : -2);
                charging_.store(battery_error == ESP_OK && charging);
                agent_link_wifi_info_t wifi{};
                if (agent_link_wifi_info(&wifi) == ESP_OK && wifi.setup_active)
                    snprintf(setup_ssid_, sizeof setup_ssid_, "%s", wifi.setup_ssid);
                else setup_ssid_[0] = 0;
                poll = now + 1000000;
            }
            if (call_) {
                const agent_state_t state = agent_link_state();
                if (state == AGENT_STATE_READY) {
                    if (!active_) { active_ = true; Message("请开始说话"); }
                    audio_.SetContinuous(true);
                } else if (active_ || state == AGENT_STATE_DISCONNECTED || now - requested_ >= 30000000) {
                    EndCall(); Message("通话结束或连接失败，按中键重试");
                }
            }
            if (volume_dirty_ && now - volume_at_ >= 1000000) { Save(); volume_dirty_ = false; }
            Publish();
        }
    }
    void Keys() {
        epaper::Key keys[4];
        bool down[4]{};
        bool pressed_setup[4]{};
        uint32_t pressed_view[4]{};
        const gpio_num_t pins[] = {KEY_UP, KEY_DOWN, KEY_CENTER, KEY_BOOT};
        while (true) {
            const int64_t now = esp_timer_get_time();
            for (int i = 0; i < 4; ++i) {
                const bool pressed = !gpio_get_level(pins[i]);
                if (pressed && !down[i]) {
                    pressed_view[i] = ui_.Displayed();
                    pressed_setup[i] = ui_.DisplayingSetup() || phase_.load() == AGENT_LINK_PHASE_SETUP;
                }
                down[i] = pressed;
                const epaper::Press press = keys[i].Poll(pressed, now, i != 2, i < 2);
                if (press == epaper::Press::kNone) continue;
                Action action = i == 0 ? Action::kUp : i == 1 ? Action::kDown : i == 3 ? Action::kBack :
                                press == epaper::Press::kHold ? Action::kMenu : Action::kCenter;
                Input input{action, pressed_view[i], pressed_setup[i]};
                if (xQueueSend(events_, &input, 0) != pdTRUE) ESP_LOGW(TAG, "Input busy");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    static void ControlEntry(void* context) { static_cast<WaveshareEpaperWifi*>(context)->ControlLoop(); }
    static void KeyEntry(void* context) { static_cast<WaveshareEpaperWifi*>(context)->Keys(); }
    static void NetworkEntry(void* context) { static_cast<WaveshareEpaperWifi*>(context)->NetworkLoop(); }

    EpaperHardware hardware_;
    EpaperWifiAudio audio_;
    epaper::Ui ui_;
    QueueHandle_t events_ = nullptr;
    QueueHandle_t jobs_ = nullptr, results_ = nullptr;
    epaper::RequestState requests_;
    epaper::PendingConfirm confirm_;
    JobKind pending_job_ = JobKind::kList;
    bool screen_ok_ = false, hardware_ok_ = false;
    std::atomic<int> battery_{-2};
    std::atomic<bool> charging_{false};
    std::atomic<agent_link_phase_t> phase_{AGENT_LINK_PHASE_IDLE};
    int volume_ = AUDIO_DEFAULT_VOLUME;
    int32_t agent_ = 0, saved_agent_ = 0;
    char session_[80]{}, session_name_[96]{}, agent_name_[80]{}, message_[192]{};
    char setup_ssid_[33]{};
    agent_link_platform_conversation_t sessions_[3]{};
    size_t cursor_ = 0, count_ = 0, offset_ = 0;
    bool more_ = false, voice_ = false, call_ = false, active_ = false;
    Page page_ = Page::kHome;
    uint32_t view_ = 1;
    uint32_t page_view_ = 1;
    int64_t requested_ = 0;
    int64_t volume_at_ = 0;
    bool volume_dirty_ = false;
};

DECLARE_APPLICATION(WaveshareEpaperWifi);
