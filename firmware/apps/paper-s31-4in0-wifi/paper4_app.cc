#include "application.h"
#include "config.h"
#include "paper_hardware.h"
#include "paper_storage.h"
#include "paper_touch.h"
#include "paper4_touch.h"
#include "paper_wifi_audio.h"
#include "paper_music.h"
#include "cJSON.h"
#include "paper4_display.h"
#include "agent_link_platform.h"
#include "agent_link_chat.h"
#include "agent_link_network.h"
#include "stripe_canvas.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <sys/time.h>
#include <vector>

namespace {
constexpr const char* TAG = "Paper4.desktop";
using paper4::Action;
enum class Page { Home, Voice, Chat, Presets, Sessions, History, Reader, Alarm, Calendar, Music, Settings, Time, Info, Compose, Review, Calibrate };
enum class Work { VoiceList, ChatList, VoiceCreate, ChatCreate, History, Send, Scan };
struct Item { std::string id, name; };
struct Job {
    Work work;
    uint32_t generation;
    int agent;
    std::string session, cursor, text, response;
    esp_err_t error = ESP_OK;
    std::vector<std::string> tracks;
    bool editor = false;
    uint32_t draft_revision = 0;
};
struct TouchEvent {
    paper4::TouchCommand command = paper4::TouchCommand::None;
    int argument = 0, page = 0, raw_x = 0, raw_y = 0;
    uint32_t epoch = 0, revision = 0, view = 0;
    paper4::InputMode mode = paper4::InputMode::Nine;
    char value[16]{};
};
const char* String(const cJSON* object, const char* key) {
    const char* result = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    return result ? result : "";
}
const cJSON* Data(const cJSON* root) {
    const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return data ? data : root;
}
const cJSON* Items(const cJSON* data) {
    return cJSON_IsArray(data) ? data : cJSON_GetObjectItemCaseSensitive(data, "items");
}
std::string Format(const char* pattern, int a, int b = 0, int c = 0) {
    char text[96];
    snprintf(text, sizeof text, pattern, a, b, c);
    return text;
}
constexpr const char* kPresets[] = {
    "你好，请介绍一下你自己。", "请给我讲一个简短的故事。", "请给我一个今天的学习建议。",
    "请推荐一个放松身心的方法。", "请给我一个有趣的科学知识。"
};
}

class Paper4Desktop : public Application {
public:
    Paper4Desktop() : music_(audio_) {
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(paper_hardware_init());
        if (audio_.Init() != ESP_OK) ESP_LOGW(TAG, "Audio unavailable");
        if (!paper_resources::Init()) ESP_LOGE(TAG,"Shared font/IME unavailable; install the full v2 USB package");
        int percent = -1; bool charging = false;
        battery_present_ = paper_battery_read(&percent, &charging) == ESP_OK;
        battery_.store(percent); charging_.store(charging);
        setenv("TZ", "CST-8", 1); tzset();
        Load();
        const auto touch_error = paper_touch_init(&touch_info_);
        touch_ready_ = touch_error == ESP_OK;
        if (!touch_ready_) ESP_LOGW(TAG, "Touch unavailable: %s", esp_err_to_name(touch_error));
        if (touch_ready_) ConfigureTouchMapping();
        std::tm rtc{};
        if (paper_rtc_read(&rtc) == ESP_OK) {
            const time_t stamp = mktime(&rtc);
            if (stamp > 1577836800) { timeval value{stamp, 0}; settimeofday(&value, nullptr); }
        }
        events_ = xQueueCreate(16, sizeof(Action));
        jobs_ = xQueueCreate(1, sizeof(Job*));
        results_ = xQueueCreate(1, sizeof(Job*));
        touches_ = xQueueCreate(16, sizeof(TouchEvent));
        ESP_ERROR_CHECK(events_ && jobs_ && results_ && touches_ ? ESP_OK : ESP_ERR_NO_MEM);
        Publish();
        ESP_ERROR_CHECK(xTaskCreate(ScreenEntry, "paper4_screen", 8192, this, 2, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(HttpEntry, "paper4_http", 12288, this, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(ControlEntry, "paper4_control", 12288, this, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(KeysEntry, "paper4_keys", 3072, this, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        if (touch_ready_) ESP_ERROR_CHECK(xTaskCreate(TouchEntry, "paper4_touch", 4096, this, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
    const char* Name() const override { return "Paper4IN0"; }
    const char* Model() const override { return PAPER_MODEL; }
    const agent_platform_t* Platform() const override {
        static const agent_platform_t platform{CLOUD_BASE_URL, CLOUD_PRODUCT_ID, CLOUD_CHIP_TYPE};
        return &platform;
    }
    uint32_t Capabilities() const override {
        uint32_t caps = AGENT_CAP_BUTTON | AGENT_CAP_SCREEN | AGENT_CAP_LED | AGENT_CAP_HAPTIC;
        if (battery_present_) caps |= AGENT_CAP_BATTERY;
        if (audio_.Ready()) caps |= AGENT_CAP_MIC | AGENT_CAP_SPEAKER;
        return caps;
    }
    void OnLinkStatus(const agent_link_status_t& status) override {
        std::lock_guard<std::mutex> guard(mutex_);
        status_ = status;
    }
    void ShowText(const char* text) override {
        std::lock_guard<std::mutex> guard(mutex_);
        incoming_text_.assign(text ? text : "", text ? strnlen(text, 8192) : 0);
        incoming_voice_ = voice_output_.load();
        text_at_.store(esp_timer_get_time());
        text_dirty_.store(true);
    }
    void PlayAudio(const uint8_t* pcm, size_t bytes) override {
        if (voice_output_.load()) audio_.Play(pcm, bytes);
    }
    void AudioEnd() override { audio_.End(); text_final_.store(true); }
    void AudioReset() override { audio_.Reset(); }
    bool SupportsListen() const override { return audio_.Ready(); }
    void OnListen(bool start, uint32_t max_ms) override {
        if (voice_output_.load()) audio_.Listen(start, max_ms);
    }
    void Vibrate(uint32_t ms) override { vibration_.store(std::min<uint32_t>(ms, 2000)); }
    void SetLed(uint32_t rgb) override { paper_led(rgb != 0); }
    int GetBatteryLevel() override { return battery_.load(); }
    bool IsCharging() override { return charging_.load(); }

private:
    void Error(esp_err_t error) {
        if (error == ESP_OK) return;
        if (error == ESP_ERR_INVALID_STATE) notice_ = "请先配网、激活并在平台绑定 Agent";
        else if (error == ESP_ERR_NOT_SUPPORTED) notice_ = "当前格式或功能不受支持";
        else notice_ = std::string("操作失败：") + esp_err_to_name(error);
        ESP_LOGW(TAG, "Operation failed: %s", esp_err_to_name(error));
    }
    void Load() {
        nvs_handle_t handle;
        if (nvs_open("paper4_wifi", NVS_READONLY, &handle) != ESP_OK) return;
        (void)nvs_get_i32(handle, "agent", &saved_agent_);
        char value[96]{}; size_t size = sizeof value;
        if (nvs_get_str(handle, "voice", value, &size) == ESP_OK) voice_session_ = value;
        size = sizeof value;
        if (nvs_get_str(handle, "chat", value, &size) == ESP_OK) chat_session_ = value;
        uint8_t hour = 7, minute = 0, enabled = 0;
        (void)nvs_get_u8(handle, "alarm_h", &hour);
        (void)nvs_get_u8(handle, "alarm_m", &minute);
        (void)nvs_get_u8(handle, "alarm_on", &enabled);
        alarm_hour_ = hour < 24 ? hour : 7;
        alarm_minute_ = minute < 60 ? minute : 0;
        alarm_enabled_ = enabled != 0;
        size_t calibration_size = sizeof calibration_;
        if (nvs_get_blob(handle,"touch_cal",&calibration_,&calibration_size) != ESP_OK ||
            calibration_size != sizeof calibration_ || !calibration_.Valid()) calibration_ = {};
        nvs_close(handle);
        if (saved_agent_ <= 0) { voice_session_.clear(); chat_session_.clear(); }
    }
    void Save() {
        nvs_handle_t handle;
        esp_err_t error = nvs_open("paper4_wifi", NVS_READWRITE, &handle);
        if (error != ESP_OK) { Error(error); return; }
        error = nvs_set_i32(handle, "agent", saved_agent_);
        if (error == ESP_OK) error = nvs_set_str(handle, "voice", voice_session_.c_str());
        if (error == ESP_OK) error = nvs_set_str(handle, "chat", chat_session_.c_str());
        if (error == ESP_OK) error = nvs_set_u8(handle, "alarm_h", alarm_hour_);
        if (error == ESP_OK) error = nvs_set_u8(handle, "alarm_m", alarm_minute_);
        if (error == ESP_OK) error = nvs_set_u8(handle, "alarm_on", alarm_enabled_);
        if (error == ESP_OK && calibration_.Valid()) error = nvs_set_blob(handle,"touch_cal",&calibration_,sizeof calibration_);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        Error(error);
    }
    void ConfigureTouchMapping() {
        touch_width_ = touch_info_.width; touch_height_ = touch_info_.height;
        if (touch_info_.type == PAPER_TOUCH_GT911) {
            if (touch_width_ < 2 || touch_width_ > 8192) touch_width_ = PAPER_TOUCH_RAW_WIDTH;
            if (touch_height_ < 2 || touch_height_ > 8192) touch_height_ = PAPER_TOUCH_RAW_HEIGHT;
        }
        const bool stored = calibration_.Matches(touch_width_,touch_height_);
        if (!stored) {
            calibration_ = touch_info_.type == PAPER_TOUCH_GT911
                ? paper4::TouchCalibration::FromRange(touch_width_,touch_height_,PAPER_TOUCH_REVERSE_X,PAPER_TOUCH_REVERSE_Y)
                : paper4::TouchCalibration{};
        }
        ESP_LOGI(TAG,"Touch mapping: %s raw=%dx%d screen=528x792 valid=%d",
                 stored ? "stored v2" : "board default",touch_width_,touch_height_,calibration_.Valid());
    }
    bool SyncAgent(bool explain = false) {
        agent_link_platform_agent_t bound{};
        if (agent_link_platform_bound_agent(&bound) != ESP_OK) {
            if (agent_) { EndCall(); ++generation_; confirm_.Cancel(); }
            agent_ = 0; agent_name_.clear(); voice_supported_ = false;
            if (explain) Error(ESP_ERR_INVALID_STATE);
            return false;
        }
        if (agent_ != bound.id) {
            EndCall(); ++generation_; confirm_.Cancel();
            if (saved_agent_ != bound.id) {
                voice_session_.clear(); chat_session_.clear(); sessions_.clear(); history_.clear();
                reader_.clear(); voice_text_.clear(); notice_ = "已绑定新 Agent，请选择会话";
                page_ = Page::Home;
                calibration_step_ = 0; cursor_ = 0;
            }
            agent_ = saved_agent_ = bound.id;
            Save();
        }
        agent_name_ = bound.name; voice_supported_ = bound.voice_supported;
        return true;
    }
    void EndCall() {
        if (call_) {
            voice_output_.store(false); call_ = active_ = false;
            audio_.Stop(); (void)agent_link_end_realtime();
        }
    }
    void StopAudio() {
        EndCall();
        if (!music_.Stop()) Error(ESP_ERR_TIMEOUT);
        audio_.Stop(); ringing_ = false; paper_motor(false); motor_until_ = 0;
    }
    void Open(Page page) {
        confirm_.Cancel(); ++generation_; editing_ = false;
        page_ = page; cursor_ = 0; reader_page_ = 0; notice_.clear();
        const time_t stamp = time(nullptr);
        std::tm now{}; localtime_r(&stamp, &now);
        if (page == Page::Calendar) {
            calendar_year_ = stamp > 1577836800 ? now.tm_year + 1900 : 2026;
            calendar_month_ = stamp > 1577836800 ? now.tm_mon + 1 : 1;
        }
        if (page == Page::Time) {
            edit_time_ = now;
            if (stamp <= 1577836800) { edit_time_ = {}; edit_time_.tm_year = 126; edit_time_.tm_mday = 1; }
        }
        if (page == Page::Alarm) {
            edit_hour_ = alarm_hour_; edit_minute_ = alarm_minute_; edit_enabled_ = alarm_enabled_;
        }
    }
    void Back() {
        confirm_.Cancel();
        if (editing_) { editing_ = false; return; }
        if (call_) { EndCall(); notice_ = "通话已结束"; return; }
        const Page page = page_;
        if (page == Page::Presets || page == Page::History) Open(Page::Chat);
        else if (page == Page::Compose) Open(Page::Chat);
        else if (page == Page::Review) Open(Page::Compose);
        else if (page == Page::Calibrate) Open(calibration_return_);
        else if (page == Page::Sessions) Open(session_voice_ ? Page::Voice : Page::Chat);
        else if (page == Page::Reader) Open(reader_parent_);
        else if (page == Page::Time || page == Page::Info) Open(Page::Settings);
        else Open(Page::Home);
    }
    void Start(Work work, const std::string& text = "", const std::string& cursor = "", bool editor = false) {
        if (busy_) { notice_ = "上个请求正在结束，请稍后重试"; return; }
        if (work != Work::Scan && !SyncAgent(true)) return;
        auto* job = new (std::nothrow) Job{work, generation_, agent_,
            work == Work::History ? history_session_ : chat_session_, cursor, text, ""};
        if (!job) { Error(ESP_ERR_NO_MEM); return; }
        job->editor = editor; job->draft_revision = ime_.Revision();
        if (xQueueSend(jobs_, &job, 0) != pdTRUE) { delete job; Error(ESP_ERR_NO_MEM); return; }
        busy_ = true; notice_ = work == Work::Scan ? "正在读取 SD 卡" : "正在请求平台，请稍候";
    }
    void Sessions(bool voice) {
        if (!SyncAgent(true)) return;
        EndCall(); session_voice_ = voice; Open(Page::Sessions);
        sessions_.clear(); next_cursor_.clear(); cursors_.clear();
        Start(voice ? Work::VoiceList : Work::ChatList);
    }
    void Call() {
        if (call_) { EndCall(); notice_ = "通话已结束"; return; }
        if (!SyncAgent(true)) return;
        if (voice_session_.empty()) { Sessions(true); return; }
        if (!voice_supported_ || !audio_.Ready()) { Error(ESP_ERR_NOT_SUPPORTED); return; }
        if (!music_.Stop()) { Error(ESP_ERR_TIMEOUT); return; }
        voice_text_.clear(); text_final_.store(false); voice_output_.store(true);
        const auto error = agent_link_request_realtime_session(agent_, voice_session_.c_str());
        if (error != ESP_OK) { voice_output_.store(false); Error(error); return; }
        call_ = true; active_ = false; requested_ = esp_timer_get_time();
        notice_ = "正在接通，KEY1 可结束通话";
    }
    static void HttpEntry(void* context) { static_cast<Paper4Desktop*>(context)->HttpLoop(); }
    void HttpLoop() {
        while (true) {
            Job* job = nullptr;
            if (xQueueReceive(jobs_, &job, portMAX_DELAY) != pdTRUE || !job) continue;
            std::string buffer(32768, '\0');
            if (job->work == Work::Scan) {
                job->error = music_.Scan();
                if (job->error == ESP_OK) job->tracks = music_.Tracks();
            } else if (job->work == Work::VoiceList) {
                agent_link_platform_conversation_t entries[5]{};
                size_t count = 0, total = 0;
                const size_t offset = job->cursor.empty() ? 0 : strtoul(job->cursor.c_str(), nullptr, 10);
                job->error = agent_link_platform_conversations(job->agent, offset, entries, 5, &count, &total);
                cJSON* root = cJSON_CreateObject();
                cJSON* list = cJSON_AddArrayToObject(root, "items");
                for (size_t i = 0; i < count; ++i) {
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "conversation_id", entries[i].id);
                    cJSON_AddStringToObject(item, "name", entries[i].name);
                    cJSON_AddItemToArray(list, item);
                }
                cJSON_AddStringToObject(root, "next_cursor", offset + count < total ? std::to_string(offset + count).c_str() : "");
                char* json = cJSON_PrintUnformatted(root);
                job->response = json ? json : ""; cJSON_free(json); cJSON_Delete(root);
            } else if (job->work == Work::ChatList) {
                job->error = agent_link_chat_conversations(job->agent, job->cursor.c_str(), buffer.data(), buffer.size());
            } else if (job->work == Work::VoiceCreate || job->work == Work::ChatCreate) {
                char id[80]{};
                job->error = job->work == Work::VoiceCreate
                    ? agent_link_platform_create_conversation(job->agent, id, sizeof id)
                    : agent_link_chat_create(job->agent, id, sizeof id);
                job->session = id;
            } else if (job->work == Work::History) {
                job->error = job->session.empty() ? ESP_ERR_INVALID_STATE :
                    agent_link_chat_messages(job->agent, job->session.c_str(), job->cursor.c_str(), buffer.data(), buffer.size());
            } else {
                if (job->session.empty()) {
                    char id[80]{};
                    job->error = agent_link_chat_create(job->agent, id, sizeof id);
                    job->session = id;
                }
                if (job->error == ESP_OK)
                    job->error = agent_link_chat_send(job->agent, job->session.c_str(), job->text.c_str(), buffer.data(), buffer.size());
            }
            if (job->work != Work::VoiceList && job->error == ESP_OK) job->response = buffer.c_str();
            (void)xQueueSend(results_, &job, portMAX_DELAY);
        }
    }
    void Receive() {
        Job* job = nullptr;
        if (xQueueReceive(results_, &job, 0) != pdTRUE || !job) return;
        busy_ = false;
        const bool valid_agent = job->work == Work::Scan || (SyncAgent() && agent_ == job->agent);
        if (!valid_agent || job->generation != generation_) { delete job; return; }
        notice_.clear();
        if (job->error != ESP_OK) { Error(job->error); delete job; return; }
        if (job->work == Work::Scan) {
            tracks_ = std::move(job->tracks);
            notice_ = tracks_.empty() ? "SD 卡 /music 目录没有 WAV 文件" : "SD 音乐已更新";
        } else if (job->work == Work::VoiceCreate || job->work == Work::ChatCreate) {
            if (job->session.empty()) { Error(ESP_ERR_INVALID_RESPONSE); delete job; return; }
            if (job->work == Work::VoiceCreate) voice_session_ = job->session;
            else chat_session_ = job->session;
            Save(); Open(job->work == Work::VoiceCreate ? Page::Voice : Page::Chat);
            notice_ = "新会话已创建";
        } else if (job->work == Work::Send) {
            chat_session_ = job->session; Save();
            if (job->editor && job->draft_revision == ime_.Revision()) ime_.Clear();
            reader_ = job->response; reader_parent_ = Page::Chat;
            Open(Page::Reader);
        } else {
            cJSON* root = cJSON_Parse(job->response.c_str());
            const cJSON* data = Data(root);
            const cJSON* list = Items(data);
            if (!cJSON_IsArray(list)) { cJSON_Delete(root); Error(ESP_ERR_INVALID_RESPONSE); delete job; return; }
            const cJSON* item;
            if (job->work == Work::History) {
                history_.clear();
                cJSON_ArrayForEach(item, list) {
                    std::string text = String(item, "content");
                    if (text.empty()) text = String(item, "text");
                    const cJSON* parts = cJSON_GetObjectItemCaseSensitive(item, "content");
                    if (text.empty() && cJSON_IsArray(parts)) {
                        const cJSON* part; cJSON_ArrayForEach(part, parts) text += String(part, "text");
                    }
                    history_.push_back({String(item, "role"), text});
                }
                history_cursor_ = String(data, "next_cursor");
                Open(Page::History);
            } else {
                sessions_.clear();
                cJSON_ArrayForEach(item, list) {
                    std::string id = String(item, "conversation_id");
                    if (id.empty()) id = String(item, "id");
                    if (!id.empty()) sessions_.push_back({id, String(item, "name")});
                }
                next_cursor_ = String(data, "next_cursor"); cursor_ = 0;
            }
            cJSON_Delete(root);
        }
        delete job;
    }
    std::vector<std::string> Rows() const {
        switch (page_) {
        case Page::Home: return {"语音通话", "文字消息", "会话管理", "每日闹钟", "月历", "SD 音乐", "设置"};
        case Page::Voice: return {call_ ? "结束通话" : "开始通话", "选择语音会话", "创建语音会话", "阅读语音回复", "返回首页"};
        case Page::Chat: return {"编写消息", "发送快捷消息", "选择文字会话", "创建文字会话", "查看消息历史", "返回首页"};
        case Page::Presets: return {"打个招呼", "讲一个故事", "学习建议", "放松一下", "科学小知识", "返回文字消息"};
        case Page::Sessions: {
            std::vector<std::string> rows;
            for (const auto& session : sessions_) rows.push_back(session.name.empty() ? "未命名会话" : session.name);
            if (!next_cursor_.empty()) rows.push_back("下一页");
            if (!cursors_.empty()) rows.push_back("上一页");
            rows.push_back("创建新会话"); rows.push_back("返回"); return rows;
        }
        case Page::History: {
            std::vector<std::string> rows;
            for (const auto& item : history_) rows.push_back((item.id == "user" ? "我：" : "Agent：") + item.name);
            if (!history_cursor_.empty()) rows.push_back("更早的消息");
            rows.push_back("返回文字消息"); return rows;
        }
        case Page::Alarm: return {Format("小时：%02d", edit_hour_), Format("分钟：%02d", edit_minute_),
                                 edit_enabled_ ? "状态：开启" : "状态：关闭", "保存闹钟", "返回首页"};
        case Page::Music: {
            std::vector<std::string> rows{"停止播放", "重新扫描 SD 卡"};
            rows.insert(rows.end(), tracks_.begin(), tracks_.end()); rows.push_back("返回首页"); return rows;
        }
        case Page::Settings: return {"重新配置 WiFi", "设置日期时间", "扫描 SD 音乐", "立即刷新屏幕", "停止全部音频", "设备信息", "触摸校准", "返回首页"};
        case Page::Time: return {Format("年：%04d", edit_time_.tm_year + 1900), Format("月：%02d", edit_time_.tm_mon + 1),
            Format("日：%02d", edit_time_.tm_mday), Format("时：%02d", edit_time_.tm_hour),
            Format("分：%02d", edit_time_.tm_min), "保存时间", "返回设置"};
        default: return {};
        }
    }
    void Adjust(int delta) {
        if (page_ == Page::Alarm) {
            if (cursor_ == 0) edit_hour_ = (edit_hour_ + delta + 24) % 24;
            else if (cursor_ == 1) edit_minute_ = (edit_minute_ + delta + 60) % 60;
        } else {
            if (cursor_ == 0) edit_time_.tm_year = std::clamp(edit_time_.tm_year + delta, 100, 199);
            if (cursor_ == 1) edit_time_.tm_mon = (edit_time_.tm_mon + delta + 12) % 12;
            if (cursor_ == 2) edit_time_.tm_mday = (edit_time_.tm_mday - 1 + delta +
                paper4::MonthDays(edit_time_.tm_year + 1900, edit_time_.tm_mon + 1)) %
                paper4::MonthDays(edit_time_.tm_year + 1900, edit_time_.tm_mon + 1) + 1;
            if (cursor_ == 3) edit_time_.tm_hour = (edit_time_.tm_hour + delta + 24) % 24;
            if (cursor_ == 4) edit_time_.tm_min = (edit_time_.tm_min + delta + 60) % 60;
            paper4::ClampDate(edit_time_);
        }
    }
    void Choose() {
        if (editing_) { editing_ = false; return; }
        switch (page_) {
        case Page::Home: {
            constexpr Page pages[] = {Page::Voice,Page::Chat,Page::Sessions,Page::Alarm,Page::Calendar,Page::Music,Page::Settings};
            if (cursor_ == 2) Sessions(false);
            else { Open(pages[cursor_]); if (page_ == Page::Music) Start(Work::Scan); }
            break;
        }
        case Page::Voice:
            if (cursor_ == 0) Call();
            else if (cursor_ == 1) Sessions(true);
            else if (cursor_ == 2) Start(Work::VoiceCreate);
            else if (cursor_ == 3) { reader_ = voice_text_; reader_parent_ = Page::Voice; Open(Page::Reader); }
            else Open(Page::Home);
            break;
        case Page::Chat:
            if (cursor_ == 0) BeginCompose();
            else if (cursor_ == 1) Open(Page::Presets);
            else if (cursor_ == 2) Sessions(false);
            else if (cursor_ == 3) Start(Work::ChatCreate);
            else if (cursor_ == 4) { history_session_ = chat_session_; Start(Work::History); }
            else Open(Page::Home);
            break;
        case Page::Presets:
            if (cursor_ < 5) { EndCall(); Start(Work::Send, kPresets[cursor_]); }
            else Open(Page::Chat);
            break;
        case Page::Sessions: {
            if (cursor_ < sessions_.size()) {
                if (session_voice_) voice_session_ = sessions_[cursor_].id;
                else chat_session_ = sessions_[cursor_].id;
                Save(); Open(session_voice_ ? Page::Voice : Page::Chat); notice_ = "会话已选择";
            } else {
                size_t row = cursor_ - sessions_.size();
                if (!next_cursor_.empty() && row-- == 0) {
                    cursors_.push_back(next_cursor_); Start(session_voice_ ? Work::VoiceList : Work::ChatList, "", next_cursor_);
                } else if (!cursors_.empty() && row-- == 0) {
                    cursors_.pop_back(); Start(session_voice_ ? Work::VoiceList : Work::ChatList, "", cursors_.empty() ? "" : cursors_.back());
                } else if (row == 0) Start(session_voice_ ? Work::VoiceCreate : Work::ChatCreate);
                else Back();
            }
            break;
        }
        case Page::History:
            if (cursor_ < history_.size()) {
                reader_ = history_[cursor_].name; reader_parent_ = Page::History; Open(Page::Reader);
            } else if (!history_cursor_.empty() && cursor_ == history_.size()) Start(Work::History, "", history_cursor_);
            else Open(Page::Chat);
            break;
        case Page::Reader: Back(); break;
        case Page::Alarm:
            if (cursor_ < 2) editing_ = true;
            else if (cursor_ == 2) edit_enabled_ = !edit_enabled_;
            else if (cursor_ == 3) {
                alarm_hour_ = edit_hour_; alarm_minute_ = edit_minute_; alarm_enabled_ = edit_enabled_;
                Save(); Open(Page::Home); notice_ = "闹钟已保存";
            } else Open(Page::Home);
            break;
        case Page::Calendar: Open(Page::Time); break;
        case Page::Music:
            if (cursor_ == 0) { if (!music_.Stop()) Error(ESP_ERR_TIMEOUT); }
            else if (cursor_ == 1) Start(Work::Scan);
            else if (cursor_ - 2 < tracks_.size()) { EndCall(); Error(music_.Play(tracks_[cursor_ - 2])); }
            else Open(Page::Home);
            break;
        case Page::Settings:
            if (cursor_ == 0) { StopAudio(); Error(agent_link_wifi_setup()); }
            else if (cursor_ == 1) Open(Page::Time);
            else if (cursor_ == 2) Start(Work::Scan);
            else if (cursor_ == 3) { force_refresh_.store(true); notice_ = "已请求刷新"; }
            else if (cursor_ == 4) { StopAudio(); notice_ = "音频已停止"; }
            else if (cursor_ == 5) Open(Page::Info);
            else if (cursor_ == 6) BeginCalibration(Page::Settings);
            else Open(Page::Home);
            break;
        case Page::Time:
            if (cursor_ < 5) editing_ = true;
            else if (cursor_ == 5) {
                edit_time_.tm_sec = 0; edit_time_.tm_isdst = -1;
                const time_t stamp = mktime(&edit_time_);
                if (stamp <= 1577836800) Error(ESP_ERR_INVALID_ARG);
                else {
                    timeval value{stamp, 0}; settimeofday(&value, nullptr);
                    const auto error = paper_rtc_write(&edit_time_);
                    Open(Page::Settings);
                    notice_ = error == ESP_OK ? "系统和 RTC 时间已保存" : "系统时间已保存，RTC 写入失败";
                }
            } else Open(Page::Settings);
            break;
        case Page::Info: Open(Page::Settings); break;
        case Page::Compose:
            if (!ime_.Pending().empty()) (void)ime_.Select(ime_.CandidatePage()*4);
            else ReviewCompose();
            break;
        case Page::Review: EndCall(); Start(Work::Send,ime_.Text(),"",true); break;
        case Page::Calibrate: break;
        }
    }
    void BeginCalibration(Page after) {
        if (!touch_ready_) { notice_ = "未检测到触摸，请检查 FPC2 排线后重启"; return; }
        calibration_return_ = after; calibration_step_ = 0;
        Open(Page::Calibrate);
    }
    void BeginCompose() {
        if (!paper_resources::Ready()) { notice_ = "Resources missing. Install full USB package."; return; }
        if (!touch_ready_) { notice_ = "未检测到触摸，可先使用快捷消息"; return; }
        if (!calibration_.Valid()) { notice_ = "触摸映射未就绪，请在设置中校准"; return; }
        Open(Page::Compose);
    }
    void ReviewCompose() {
        if (!ime_.Pending().empty()) { notice_ = "请先选字，或点拼音原文"; return; }
        if (ime_.Text().empty()) { notice_ = "请先输入消息"; return; }
        reader_ = ime_.Text();
        Open(Page::Review);
    }
    void HandleTouch(const TouchEvent& event) {
        using paper4::TouchCommand;
        if (ringing_) { StopAudio(); return; }
        agent_link_phase_t phase;
        { std::lock_guard<std::mutex> guard(mutex_); phase = status_.phase; }
        if (phase == AGENT_LINK_PHASE_SETUP || phase == AGENT_LINK_PHASE_PAIRING || phase == AGENT_LINK_PHASE_BLOCKED) return;
        if (event.epoch != generation_ || event.page != int(page_)) return;
        if (event.command == TouchCommand::Calibrate) {
            if (page_ != Page::Calibrate || event.argument != calibration_step_) return;
            const paper4::Point raw{event.raw_x,event.raw_y};
            const int repeated = paper4::RepeatedCalibrationPoint(calibration_raw_,calibration_step_,raw);
            if (repeated >= 0) {
                notice_ = Format("采点与第 %d 点重复，请点当前十字",repeated+1);
                ESP_LOGW(TAG,"Touch calibration rejected duplicate: step=%d previous=%d raw=%d,%d",
                         calibration_step_+1,repeated+1,raw.x,raw.y);
                return;
            }
            calibration_raw_[calibration_step_] = raw;
            notice_.clear();
            ESP_LOGI(TAG,"Touch calibration sample: step=%d raw=%d,%d target=%d,%d",
                     calibration_step_+1,raw.x,raw.y,paper4::kCalibrationTargets[calibration_step_].x,
                     paper4::kCalibrationTargets[calibration_step_].y);
            ++calibration_step_;
            vibration_.store(20);
            if (calibration_step_ == 4) {
                paper4::TouchCalibration next;
                paper4::CalibrationReport report;
                if (!next.Fit(calibration_raw_,touch_width_,touch_height_,&report)) {
                    ESP_LOGW(TAG,"Touch calibration failed: reason=%s point=%d residual=%d; previous mapping retained",
                             paper4::CalibrationReason(report.error),report.point+1,report.max_error);
                    calibration_step_ = 0;
                    Open(calibration_return_);
                    notice_ = std::string("校准未保存，保留原映射：") + paper4::CalibrationHint(report.error);
                } else {
                    { std::lock_guard<std::mutex> guard(mutex_); calibration_ = next; }
                    Save(); Open(calibration_return_); notice_ = "触摸校准已保存";
                }
            }
            return;
        }
        if (event.command == TouchCommand::Back) { Back(); return; }
        if (page_ == Page::Compose) {
            if (event.mode != ime_.Mode() && event.command != TouchCommand::Mode) return;
            notice_.clear();
            bool accepted = true;
            switch (event.command) {
            case TouchCommand::Character: accepted = ime_.Input(event.value); break;
            case TouchCommand::Mode: ime_.SetMode(static_cast<paper4::InputMode>(event.argument)); break;
            case TouchCommand::Candidate:
                if (event.revision != ime_.Revision()) { notice_ = "候选字更新中，请稍后点选"; return; }
                accepted = ime_.Select(event.argument); break;
            case TouchCommand::Previous: ime_.Page(-1); break;
            case TouchCommand::Next: ime_.Page(1); break;
            case TouchCommand::Space: accepted = ime_.Space(); break;
            case TouchCommand::Literal: accepted = ime_.Literal(); break;
            case TouchCommand::Erase: ime_.Backspace(); break;
            case TouchCommand::Clear: ime_.Clear(); break;
            case TouchCommand::Review: ReviewCompose(); break;
            default: return;
            }
            if (!accepted) notice_ = "输入未接受：请检查拼音或消息长度";
            else vibration_.store(15);
        } else if (page_ == Page::Review && event.command == TouchCommand::Send) {
            if (event.revision != ime_.Revision() || event.view != view_ || busy_) return;
            EndCall(); Start(Work::Send,ime_.Text(),"",true);
        } else if (page_ == Page::Review && (event.command == TouchCommand::Previous || event.command == TouchCommand::Next)) {
            const int pages = (int(ReaderLines().size())+10)/11;
            reader_page_ = std::clamp(reader_page_+(event.command == TouchCommand::Previous ? -1 : 1),0,std::max(0,pages-1));
        } else if (event.command == TouchCommand::Row) {
            const size_t count = Rows().size();
            if (event.view != view_ || event.argument < 0 || size_t(event.argument) >= count) return;
            cursor_ = size_t(event.argument); Publish(); confirm_.Request(view_);
        } else if (event.command == TouchCommand::MenuNext) {
            const size_t count = Rows().size();
            if (count) { confirm_.Cancel(); cursor_ = ((cursor_/7+1) % ((count+6)/7))*7; }
        }
    }
    void Handle(Action action) {
        if (ringing_) { StopAudio(); notice_ = "闹钟已停止"; return; }
        if (action == Action::Back) { Back(); return; }
        if (page_ == Page::Calibrate) return;
        if (page_ == Page::Compose) {
            if (action == Action::Confirm) confirm_.Request(view_);
            else { confirm_.Cancel(); ime_.Page(action == Action::Up ? -1 : 1); }
            return;
        }
        if (action == Action::Confirm && call_) { EndCall(); notice_ = "通话已结束"; return; }
        if (action == Action::Confirm) {
            if (busy_) return;
            confirm_.Request(view_); return;
        }
        confirm_.Cancel();
        const int delta = action == Action::Up ? -1 : 1;
        notice_.clear();
        if (editing_) Adjust(delta);
        else if (page_ == Page::Calendar) paper4::MoveMonth(calendar_year_, calendar_month_, delta);
        else if (page_ == Page::Reader || page_ == Page::Review) {
            const int pages = page_ == Page::Review ? (int(ReaderLines().size())+10)/11 : ReaderPages();
            reader_page_ = std::clamp(reader_page_ + delta,0,std::max(0,pages-1));
        }
        else {
            const size_t rows = Rows().size();
            if (rows) cursor_ = (cursor_ + rows + delta) % rows;
        }
    }
    std::vector<std::string> ReaderLines() const {
        raster::TextLayout layout;
        layout.Wrap(paper_resources::Font(), reader_.c_str(), 240);
        std::vector<std::string> lines;
        for (unsigned i = 0; i < layout.Count(); ++i)
            lines.push_back(reader_.substr(layout.Start(i), layout.End(i) - layout.Start(i)));
        return lines;
    }
    int ReaderPages() const { return (int(ReaderLines().size()) + 11) / 12; }
    void Publish() {
        paper4::Frame frame;
        frame.page = int(page_); frame.epoch = generation_; frame.touch = touch_ready_;
        frame.rows = Rows();
        if (!frame.rows.empty()) cursor_ = std::min(cursor_, frame.rows.size() - 1);
        frame.selected = frame.rows.empty() ? -1 : int(cursor_);
        frame.footer = editing_ ? "上下修改数值 / 确认完成编辑" : "长按 KEY1 返回上一页";
        switch (page_) {
        case Page::Home: frame.title = "我的桌面"; break;
        case Page::Voice: frame.title = active_ ? "正在通话" : "Agent 语音"; frame.body = voice_text_; break;
        case Page::Chat: frame.title = "文字消息"; break;
        case Page::Presets: frame.title = "发送快捷消息"; break;
        case Page::Sessions: frame.title = session_voice_ ? "语音会话" : "文字会话"; break;
        case Page::History: frame.title = "消息历史"; break;
        case Page::Reader: {
            frame.title = "阅读消息";
            const auto lines = ReaderLines();
            for (size_t i = size_t(reader_page_) * 12; i < lines.size() && i < size_t(reader_page_ + 1) * 12; ++i) frame.body += lines[i] + "\n";
            frame.footer = Format("上下翻页 / 第 %d 页，共 %d 页", reader_page_ + 1, std::max(1, ReaderPages())); break;
        }
        case Page::Alarm: frame.title = "每日闹钟"; break;
        case Page::Calendar:
            frame.title = Format("%04d 年 %02d 月", calendar_year_, calendar_month_);
            frame.calendar_year = calendar_year_; frame.calendar_month = calendar_month_;
            frame.footer = "上下切换月份 / 确认设置时间"; break;
        case Page::Music: frame.title = music_.Playing() ? "正在播放" : "SD 音乐"; break;
        case Page::Settings: frame.title = "设置"; break;
        case Page::Time: frame.title = "日期与时间"; break;
        case Page::Info:
            frame.title = "设备信息";
            frame.body = "PAPER-S31-4IN0\n528 x 792 黑白墨水屏\n产品：14 / ESP32-S31\nWiFi / PCM16 实时语音\nSD：/music/*.wav\n平台：bot.deotaland.cn"; break;
        case Page::Compose: {
            frame.title = "编写消息";
            frame.input.kind = paper4::InputKind::Compose;
            frame.input.mode = ime_.Mode(); frame.input.revision = ime_.Revision();
            frame.input.text = ime_.Text(); frame.input.pending = ime_.Pending(); frame.input.notice = notice_;
            frame.input.candidate_start = ime_.CandidatePage()*4;
            const auto& candidates = ime_.Candidates();
            for (size_t i = frame.input.candidate_start; i < candidates.size() && i < frame.input.candidate_start+4; ++i) {
                frame.input.candidates.push_back(candidates[i].entry.text);
                frame.input.pinyins.push_back(candidates[i].entry.pinyin);
            }
            break;
        }
        case Page::Review: {
            frame.title = "确认发送"; frame.input.kind = paper4::InputKind::Review;
            frame.input.revision = ime_.Revision();
            const auto lines = ReaderLines();
            for (size_t i = size_t(reader_page_)*11; i < lines.size() && i < size_t(reader_page_+1)*11; ++i) frame.body += lines[i]+"\n";
            break;
        }
        case Page::Calibrate:
            frame.title = "触摸校准"; frame.input.kind = paper4::InputKind::Calibrate;
            frame.input.calibration_step = calibration_step_; break;
        }
        const time_t stamp = time(nullptr); std::tm now{}; localtime_r(&stamp, &now);
        char clock[48]{};
        if (stamp > 1577836800) strftime(clock, sizeof clock, "%Y-%m-%d %H:%M", &now);
        else snprintf(clock, sizeof clock, "请设置时间");
        frame.status = std::string(clock) + Format("   电量 %d%%", battery_.load());
        if (page_ == Page::Calendar && stamp > 1577836800 && calendar_year_ == now.tm_year + 1900 &&
            calendar_month_ == now.tm_mon + 1) frame.today = now.tm_mday;
        if (charging_.load()) frame.status += " 充电中";
        agent_link_status_t status{};
        { std::lock_guard<std::mutex> guard(mutex_); status = status_; }
        const bool setup = status.phase == AGENT_LINK_PHASE_SETUP || status.phase == AGENT_LINK_PHASE_PAIRING ||
                           status.phase == AGENT_LINK_PHASE_BLOCKED;
        frame.status = header_.Read(frame.status, esp_timer_get_time(), setup || frame.input.kind != paper4::InputKind::None);
        frame.detail = notice_.empty() ? (agent_name_.empty() ? "请在平台绑定 Agent" : "Agent：" + agent_name_) : notice_;
        if (page_ == Page::Voice && notice_.empty()) frame.detail = voice_text_.empty() ?
            (call_ ? "请开始说话 / KEY1 结束" : voice_session_.empty() ? "先选择或创建语音会话" : "选择开始通话") : voice_text_;
        if (page_ == Page::Alarm && notice_.empty()) frame.detail = "每日按设备时间提醒，按任意应用键停止";
        if (editing_) frame.detail = "编辑中：上下修改，确认完成，长按退出";
        if (page_ == Page::Calibrate) frame.detail = notice_.empty() ? Format("依次点击四个十字：%d / 4",calibration_step_+1) : notice_;
        if (page_ == Page::Review && notice_.empty()) frame.detail = Format("上下翻页：%d / %d 页",reader_page_+1,std::max(1,(int(ReaderLines().size())+10)/11));
        if (setup) {
            frame.title = status.title[0] ? status.title : "连接 Agent Link";
            frame.detail = paper4::ActivationDetail(status.code, status.hint);
            frame.footer = "请在手机或平台完成设置";
            agent_link_wifi_info_t wifi{};
            if (status.phase == AGENT_LINK_PHASE_SETUP) (void)agent_link_wifi_info(&wifi);
            frame.rows.clear(); frame.selected = -1;
            frame.input = {}; frame.touch = false;
            frame.calendar_year = frame.calendar_month = frame.today = 0;
            frame.body = status.phase == AGENT_LINK_PHASE_SETUP ?
                std::string("手机连接设备热点\n") + wifi.setup_ssid + "\n\n浏览器打开\n192.168.4.1\n\n设置 WiFi 后，在平台激活并绑定 Agent。" :
                std::string(status.code[0] ? "激活码：" : "") + status.code + "\n\n打开 bot.deotaland.cn\n\n在平台完成设备激活与 Agent 绑定。";
        }
        if (ringing_) {
            frame.title = "闹钟响了"; frame.detail = Format("每日 %02d:%02d", alarm_hour_, alarm_minute_);
            frame.rows.clear(); frame.selected = -1; frame.body = "按 KEY1 / KEY2 / KEY3\n停止本次提醒";
            frame.input = {};
            frame.calendar_year = frame.calendar_month = frame.today = 0;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        if (!frame.Same(screen_)) { frame.view = ++view_; screen_ = std::move(frame); }
    }
    static void ScreenEntry(void* context) { static_cast<Paper4Desktop*>(context)->ScreenLoop(); }
    void ScreenLoop() {
        paper4::Frame shown;
        paper4::Refresh refresh;
        paper4::Display display;
        bool have_shown = false, force = false;
        while (true) {
            paper4::Frame frame;
            { std::lock_guard<std::mutex> guard(mutex_); frame = screen_; }
            force |= force_refresh_.exchange(false);
            if (have_shown && !force && frame.Same(shown)) {
                { std::lock_guard<std::mutex> guard(mutex_); painted_ = frame; }
                displayed_.store(frame.view);
            }
            else if (refresh.Due(esp_timer_get_time(), frame.view)) {
                const esp_err_t error = display.Draw(frame, force);
                if (error == ESP_OK) {
                    shown = std::move(frame); have_shown = true; force = false;
                    { std::lock_guard<std::mutex> guard(mutex_); painted_ = shown; }
                    displayed_.store(shown.view); refresh.Complete(esp_timer_get_time());
                } else {
                    have_shown = false;
                    ESP_LOGW(TAG, "Display failed: %s; retry in 60 seconds", esp_err_to_name(error));
                    refresh.Failed(esp_timer_get_time());
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    void Tick() {
        const int64_t now = esp_timer_get_time();
        const int vibration = vibration_.exchange(-1);
        if (vibration >= 0) {
            paper_motor(vibration != 0);
            motor_until_ = vibration ? now + int64_t(vibration) * 1000 : 0;
        }
        if (motor_until_ && now >= motor_until_) { paper_motor(false); motor_until_ = 0; }
        if (now >= poll_at_) {
            (void)SyncAgent();
            int percent = -1; bool charging = false;
            (void)paper_battery_read(&percent, &charging);
            battery_.store(percent); charging_.store(charging);
            const time_t stamp = time(nullptr); std::tm time{}; localtime_r(&stamp, &time);
            if (alarm_.Due(time, alarm_enabled_, alarm_hour_, alarm_minute_)) {
                StopAudio(); ringing_ = true; ring_until_ = now + 60000000;
            }
            const auto error = music_.Error();
            if (error != music_error_) { music_error_ = error; Error(error); }
            poll_at_ = now + 1000000;
        }
        if (call_) {
            const auto state = agent_link_state();
            if (state == AGENT_STATE_READY) { active_ = true; notice_.clear(); audio_.SetContinuous(true); }
            else if (active_ || state == AGENT_STATE_DISCONNECTED || now - requested_ >= 30000000) {
                EndCall(); notice_ = "语音已结束，可重新连接";
            }
        }
        if (text_dirty_.load() && (!voice_output_.load() || text_final_.load() ||
            (now - text_at_.load() >= 5000000 && audio_.PlaybackIdle()))) {
            std::lock_guard<std::mutex> guard(mutex_);
            if (incoming_voice_) voice_text_ = incoming_text_;
            else { reader_ = incoming_text_; reader_parent_ = Page::Home; Open(Page::Reader); }
            text_dirty_.store(false); text_final_.store(false);
        }
        if (ringing_ && now >= ring_until_) { StopAudio(); notice_ = "闹钟已结束"; }
        if (ringing_ && now >= next_tone_) {
            int16_t tone[1600];
            for (unsigned i = 0; i < 1600; ++i) tone[i] = (i / 8) & 1 ? 5000 : -5000;
            audio_.Play(reinterpret_cast<const uint8_t*>(tone), sizeof tone); audio_.End();
            paper_motor(true); motor_until_ = now + 100000;
            next_tone_ = now + 1000000;
        }
    }
    static void ControlEntry(void* context) { static_cast<Paper4Desktop*>(context)->ControlLoop(); }
    void ControlLoop() {
        while (true) {
            Tick(); Receive();
            // Publish asynchronous changes before accepting a confirmation for a visible page.
            Publish();
            TouchEvent touch;
            if (xQueueReceive(touches_,&touch,0) == pdTRUE) HandleTouch(touch);
            Action action;
            if (xQueueReceive(events_, &action, pdMS_TO_TICKS(20)) == pdTRUE) {
                agent_link_phase_t phase;
                { std::lock_guard<std::mutex> guard(mutex_); phase = status_.phase; }
                if (ringing_ || (phase != AGENT_LINK_PHASE_SETUP && phase != AGENT_LINK_PHASE_PAIRING &&
                                 phase != AGENT_LINK_PHASE_BLOCKED)) Handle(action);
            }
            Publish();
            if (confirm_.Take(displayed_.load(), view_)) Choose();
            Publish();
        }
    }
    static void KeysEntry(void* context) { static_cast<Paper4Desktop*>(context)->KeysLoop(); }
    void KeysLoop() {
        const gpio_num_t pins[] = {PAPER_KEY_UP, PAPER_KEY_DOWN, PAPER_KEY_CONFIRM};
        constexpr Action taps[] = {Action::Up, Action::Down, Action::Confirm};
        paper4::Button buttons[3];
        while (true) {
            const int64_t now = esp_timer_get_time();
            for (unsigned i = 0; i < 3; ++i) {
                const auto action = buttons[i].Poll(gpio_get_level(pins[i]) == 0, now, taps[i], i == 2);
                if (action != Action::None && xQueueSend(events_, &action, 0) != pdTRUE) ESP_LOGW(TAG, "Key queue full");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    static void TouchEntry(void* context) { static_cast<Paper4Desktop*>(context)->TouchLoop(); }
    void TouchLoop() {
        bool down = false, canceled = false, suppress = false;
        int64_t pressed_at = 0, warning_at = 0;
        paper4::Point first{};
        TouchEvent event;
        while (true) {
            const int64_t now = esp_timer_get_time();
            paper_touch_sample_t sample{};
            const auto error = paper_touch_read(&sample);
            if (error != ESP_OK) {
                down = false; suppress = true;
                if (now >= warning_at) { ESP_LOGW(TAG,"Touch read failed: %s",esp_err_to_name(error)); warning_at = now+5000000; }
            } else if (sample.fresh) {
                if (!sample.down) {
                    if (down && !canceled && now-pressed_at <= 2000000 && event.command != paper4::TouchCommand::None) {
                        if (xQueueSend(touches_,&event,0) != pdTRUE) ESP_LOGW(TAG,"Touch queue full");
                    }
                    down = false; suppress = false;
                } else if (!suppress) {
                    if (!down) {
                        down = true; canceled = false; pressed_at = now; first = {sample.x,sample.y}; event = {};
                        paper4::Frame painted;
                        paper4::TouchCalibration calibration;
                        { std::lock_guard<std::mutex> guard(mutex_); painted = painted_; calibration = calibration_; }
                        event.page = painted.page; event.epoch = painted.epoch; event.view = painted.view;
                        event.revision = painted.input.revision; event.mode = painted.input.mode;
                        event.raw_x = sample.x; event.raw_y = sample.y;
                        if (painted.touch && painted.input.kind == paper4::InputKind::Calibrate) {
                            event.command = paper4::TouchCommand::Calibrate;
                            event.argument = painted.input.calibration_step;
                        } else if (painted.touch) {
                            paper4::Point point;
                            if (calibration.Map(first,point)) {
                                const auto buttons = painted.input.kind == paper4::InputKind::None
                                    ? paper4::MenuButtons(painted.rows.size(),painted.selected) : paper4::InputButtons(painted.input);
                                for (const auto& key : buttons) if (key.Contains(point.x,point.y)) {
                                    event.command = key.command; event.argument = key.argument;
                                    snprintf(event.value,sizeof event.value,"%s",key.value.c_str()); break;
                                }
                            }
                        }
                    } else if (std::abs(sample.x-first.x)>40 || std::abs(sample.y-first.y)>40) canceled = true;
                }
            }
            if (down && now-pressed_at > 2000000) canceled = true;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    PaperWifiAudio audio_;
    PaperMusic music_;
    paper4::Alarm alarm_;
    paper4::Confirm confirm_;
    QueueHandle_t events_ = nullptr, jobs_ = nullptr, results_ = nullptr;
    std::mutex mutex_;
    paper4::Frame screen_;
    paper4::Frame painted_;
    paper4::Ime ime_;
    paper4::TouchCalibration calibration_;
    paper4::Point calibration_raw_[4]{};
    paper_touch_info_t touch_info_{};
    int touch_width_ = 0, touch_height_ = 0;
    bool touch_ready_ = false;
    int calibration_step_ = 0;
    Page calibration_return_ = Page::Home;
    QueueHandle_t touches_ = nullptr;
    paper4::HeaderCache header_;
    agent_link_status_t status_{};
    std::string incoming_text_;
    bool incoming_voice_ = false;
    std::atomic<bool> text_dirty_{false}, text_final_{false}, voice_output_{false}, force_refresh_{false};
    std::atomic<int64_t> text_at_{0};
    std::atomic<int> vibration_{-1};
    std::atomic<uint32_t> displayed_{0};
    std::atomic<int> battery_{-1};
    std::atomic<bool> charging_{false};
    bool battery_present_ = false, busy_ = false, call_ = false, active_ = false, voice_supported_ = false;
    int agent_ = 0; int32_t saved_agent_ = 0;
    uint32_t generation_ = 1, view_ = 1;
    Page page_ = Page::Home, reader_parent_ = Page::Chat;
    size_t cursor_ = 0;
    int reader_page_ = 0;
    bool editing_ = false, session_voice_ = false;
    std::string agent_name_, voice_session_, chat_session_, history_session_, voice_text_, reader_, notice_, next_cursor_, history_cursor_;
    std::vector<std::string> tracks_, cursors_;
    std::vector<Item> sessions_, history_;
    bool alarm_enabled_ = false, edit_enabled_ = false, ringing_ = false;
    int alarm_hour_ = 7, alarm_minute_ = 0, edit_hour_ = 7, edit_minute_ = 0;
    int calendar_year_ = 2026, calendar_month_ = 1;
    std::tm edit_time_{};
    int64_t poll_at_ = 0, motor_until_ = 0, requested_ = 0, ring_until_ = 0, next_tone_ = 0;
    esp_err_t music_error_ = ESP_OK;
};

DECLARE_APPLICATION(Paper4Desktop);
