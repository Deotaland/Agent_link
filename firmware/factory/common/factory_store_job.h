#pragma once
#include "agent_system.h"
#include "amoled_ui_language.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

// The controller owns navigation. This worker owns network/install work and
// publishes value snapshots; it never accesses the screen or application list.
class FactoryStoreJob {
   public:
    enum class Kind : uint32_t { kCatalog, kInstall, kLookupCode };
    enum class State : uint32_t { kNone, kRunning, kRestart, kSuccess, kFailed, kCancelled };
    // This private receipt is versioned independently of application image ABI.
    // Bump its magic/key when changing InstallStatus layout or stage numbering.
    struct Result {
        uint32_t magic = 0x414C4A33;
        Kind kind = Kind::kCatalog;
        State state = State::kNone;
        unsigned page = 1;
        uint64_t play = 0, version = 0;
        uint64_t firmware = 0;
        uint32_t firmware_version = 0;
        char name[64]{}, release[32]{};
        agent_system::InstallStatus status{};
        agent_system::StoreError store{};
    };
    struct Snapshot {
        Result result;
        bool running;
        uint32_t revision;
    };
    using Wait = void (*)(void*);
    esp_err_t Init(Wait wait, void* context) {
        wait_ = wait;
        context_ = context;
        return xTaskCreate(Entry, "factory_store", 20480, this, 3, &task_) == pdPASS
                   ? ESP_OK
                   : ESP_ERR_NO_MEM;
    }
    Snapshot Read() {
        std::lock_guard<std::mutex> guard(mutex_);
        return {result_, running_, revision_};
    }
    bool Running() {
        std::lock_guard<std::mutex> guard(mutex_);
        return running_;
    }
    // Publication sets a durable checkpoint while flash work is still active.
    // Only the completed worker may request a restart, once, on the control task.
    bool TakeRestartRequest() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_ || !restart_pending_)
            return false;
        restart_pending_ = false;
        return true;
    }
    void Cancel() {
        cancel_.store(true);
    }
    esp_err_t StartCatalog(unsigned page) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_)
            return ESP_ERR_INVALID_STATE;
        result_ = {};
        result_.page = page;
        result_.state = State::kRunning;
        result_.status.stage = agent_system::InstallStage::kWaitCover;
        snprintf(result_.name, sizeof result_.name, "%s",
                 amoled_ui::Tr("在线应用列表", "Online apps"));
        Begin();
        return ESP_OK;
    }
    esp_err_t StartInstall(const agent_system::CatalogApp& app) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_)
            return ESP_ERR_INVALID_STATE;
        app_ = app;
        result_ = {};
        result_.kind = Kind::kInstall;
        result_.state = State::kRunning;
        result_.play = app.community_play;
        result_.version = app.community_version;
        result_.firmware = app.firmware_id;
        result_.firmware_version = app.firmware_version_code;
        result_.status.total = app.size;
        result_.status.stage = agent_system::InstallStage::kWaitCover;
        snprintf(result_.name, sizeof result_.name, "%s", app.name);
        snprintf(result_.release, sizeof result_.release, "%s", app.version);
        snprintf(result_.status.app_id, sizeof result_.status.app_id, "%s", app.app_id);
        const auto error = Save();
        if (error != ESP_OK) {
            result_.state = State::kFailed;
            result_.status.error = error;
            result_.status.stage = agent_system::InstallStage::kSave;
            ++revision_;
            return error;
        }
        Begin();
        return ESP_OK;
    }
    esp_err_t StartLookupCode(const char* code) {
        if (!code || strlen(code) != 6)
            return ESP_ERR_INVALID_ARG;
        for (unsigned i = 0; i < 6; ++i)
            if (code[i] < '0' || code[i] > '9')
                return ESP_ERR_INVALID_ARG;
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_)
            return ESP_ERR_INVALID_STATE;
        snprintf(code_, sizeof code_, "%s", code);
        count_ = 0;
        result_ = {};
        result_.kind = Kind::kLookupCode;
        result_.state = State::kRunning;
        result_.status.stage = agent_system::InstallStage::kWaitCover;
        snprintf(result_.name, sizeof result_.name, "%s",
                 amoled_ui::Tr("查询下载码", "Looking up download code"));
        Begin();
        return ESP_OK;
    }
    void CopyCatalog(agent_system::CatalogApp* apps, size_t* count,
                     agent_system::CatalogPage* page) {
        std::lock_guard<std::mutex> guard(mutex_);
        *count = count_;
        *page = page_;
        for (size_t i = 0; i < count_; ++i)
            apps[i] = catalog_[i];
    }
    void Acknowledge() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_)
            return;
        result_ = {};
        restart_pending_ = false;
        ++revision_;
        const auto error = Save();
        if (error != ESP_OK)
            ESP_LOGW("factory.store", "clear result failed: %s", esp_err_to_name(error));
    }
    // Called after the real installed-app scan, before starting the worker.
    bool Restore(const agent_system::App* apps, size_t count) {
        nvs_handle_t handle;
        if (nvs_open_from_partition(agent_system::kMetadataPartition, "factory_job", NVS_READONLY, &handle) != ESP_OK)
            return false;
        size_t size = sizeof result_;
        const auto error = nvs_get_blob(handle, "result_v3", &result_, &size);
        nvs_close(handle);
        if (error != ESP_OK || size != sizeof result_ || result_.magic != 0x414C4A33 ||
            result_.kind > Kind::kLookupCode || result_.state > State::kCancelled ||
            result_.status.stage > agent_system::InstallStage::kDone ||
            !memchr(result_.name, 0, sizeof result_.name) ||
            !memchr(result_.release, 0, sizeof result_.release) ||
            !memchr(result_.store.message, 0, sizeof result_.store.message) ||
            !memchr(result_.store.code, 0, sizeof result_.store.code) ||
            !memchr(result_.store.request_id, 0, sizeof result_.store.request_id) ||
            !memchr(result_.status.slot, 0, sizeof result_.status.slot) ||
            !memchr(result_.status.app_id, 0, sizeof result_.status.app_id)) {
            result_ = {};
            return false;
        }
        if (result_.state == State::kNone)
            return false;
        if (result_.state == State::kRunning || result_.state == State::kRestart) {
            bool installed = false;
            for (size_t i = 0; i < count; ++i) {
                const auto& app = apps[i];
                if (result_.firmware ? app.firmware_id == result_.firmware &&
                                           app.firmware_version_code == result_.firmware_version
                    : result_.play ? app.community_play == result_.play &&
                                         app.community_version == result_.version
                                   : !strcmp(app.descriptor.app_id, result_.status.app_id) &&
                                         !strcmp(app.version, result_.release))
                    installed = true;
            }
            result_.state = installed ? State::kSuccess : State::kFailed;
            result_.status.error = installed ? ESP_OK : ESP_ERR_INVALID_STATE;
            (void)Save();
        }
        ++revision_;
        return true;
    }

   private:
    void Begin() {
        running_ = true;
        restart_pending_ = false;
        cancel_.store(false);
        persistence_error_ = ESP_OK;
        ++revision_;
        xTaskNotifyGive(task_);
    }
    esp_err_t Save() {
        nvs_handle_t handle;
        auto error = nvs_open_from_partition(agent_system::kMetadataPartition, "factory_job", NVS_READWRITE, &handle);
        if (error != ESP_OK)
            return error;
        error = nvs_set_blob(handle, "result_v3", &result_, sizeof result_);
        if (error == ESP_OK)
            error = nvs_commit(handle);
        nvs_close(handle);
        return error;
    }
    static bool Cancelled(void* context) {
        return static_cast<FactoryStoreJob*>(context)->cancel_.load();
    }
    static void Observe(const agent_system::InstallStatus& status, void* context) {
        auto& self = *static_cast<FactoryStoreJob*>(context);
        std::lock_guard<std::mutex> guard(self.mutex_);
        const auto now = esp_timer_get_time();
        const bool changed = self.result_.status.stage != status.stage;
        self.result_.status = status;
        if (changed || now - self.published_at_ >= 250000 || status.error != ESP_OK) {
            ++self.revision_;
            self.published_at_ = now;
        }
        if (changed && status.stage == agent_system::InstallStage::kPublish) {
            self.result_.state = State::kRestart;
            self.persistence_error_ = self.Save();
            if (self.persistence_error_ != ESP_OK)
                self.cancel_.store(true);
        }
    }
    static void Entry(void* context) {
        static_cast<FactoryStoreJob*>(context)->Run();
    }
    void Run() {
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            const auto started = esp_timer_get_time();
            if (wait_)
                wait_(context_);
            ESP_LOGI("factory.store", "cover wait ms=%lld",
                     static_cast<long long>((esp_timer_get_time() - started) / 1000));
            const Kind kind = Read().result.kind;
            agent_system::InstallStatus status{};
            esp_err_t error = agent_system::kCancelled;
            if (!cancel_.load()) {
                if (kind == Kind::kInstall)
                    error = agent_system::Install(app_, nullptr, this, &status, Observe, Cancelled);
                else {
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        result_.status.stage = agent_system::InstallStage::kTicket;
                        ++revision_;
                    }
                    if (kind == Kind::kLookupCode) {
                        error = agent_system::FetchDownloadCode(code_, &catalog_[0]);
                        count_ = error == ESP_OK ? 1 : 0;
                        page_ = {1, static_cast<uint32_t>(count_), false};
                    } else
                        error = agent_system::FetchCatalogPage(catalog_, agent_system::kMaxApps,
                                                               &count_, Read().result.page, &page_);
                }
            }
            std::lock_guard<std::mutex> guard(mutex_);
            if (persistence_error_ != ESP_OK)
                error = persistence_error_;
            if (kind == Kind::kInstall)
                result_.status = status;
            result_.status.error = error;
            if (error == agent_system::kStoreError)
                agent_system::LastStoreError(&result_.store);
            result_.state = error == ESP_OK                           ? State::kSuccess
                            : error == agent_system::kRestartRequired ? State::kRestart
                            : error == agent_system::kCancelled       ? State::kCancelled
                                                                      : State::kFailed;
            const auto saved = kind == Kind::kInstall || error != ESP_OK ? Save() : ESP_OK;
            if (saved != ESP_OK) {
                ESP_LOGE("factory.store", "save outcome failed: %s", esp_err_to_name(saved));
                if (error == ESP_OK) {
                    result_.state = State::kFailed;
                    result_.status.error = saved;
                }
            }
            restart_pending_ = kind == Kind::kInstall && (result_.state == State::kRestart ||
                                                          result_.state == State::kSuccess);
            running_ = false;
            ++revision_;
            ESP_LOGI("factory.store", "job=%u result=0x%x total_ms=%lld", unsigned(kind),
                     unsigned(error),
                     static_cast<long long>((esp_timer_get_time() - started) / 1000));
        }
    }
    std::mutex mutex_;
    Result result_{};
    bool running_ = false;
    bool restart_pending_ = false;
    uint32_t revision_ = 0;
    TaskHandle_t task_ = nullptr;
    Wait wait_ = nullptr;
    void* context_ = nullptr;
    std::atomic<bool> cancel_{false};
    esp_err_t persistence_error_ = ESP_OK;
    int64_t published_at_ = 0;
    agent_system::CatalogApp app_{}, catalog_[agent_system::kMaxApps]{};
    size_t count_ = 0;
    agent_system::CatalogPage page_{};
    char code_[7]{};
};
