#pragma once
#include "agent_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstdio>

namespace agent_system {
// One trace belongs to one install invocation; it contains no URLs or credentials.
class InstallTrace {
   public:
    InstallTrace(uint32_t total, InstallObserver observer, InstallCancel cancel, void* context)
        : observer_(observer), cancel_(cancel), context_(context) {
        state.total = total;
        ESP_LOGI("agent_system.install", "start bytes=%lu", static_cast<unsigned long>(total));
        Notify();
    }
    static const char* Name(InstallStage stage) {
        static const char* const names[]{"wait_cover", "inspect",  "ticket",  "connect",
                                         "header",     "allocate", "erase",   "download",
                                         "verify",     "save",     "publish", "done"};
        return names[static_cast<unsigned>(stage)];
    }
    bool Step(InstallStage stage) {
        if (Cancelled())
            return false;
        LogStage();
        state.stage = stage;
        stage_at_ = esp_timer_get_time();
        Notify();
        return !Cancelled();
    }
    bool Cancelled() const {
        return cancel_ && cancel_(context_);
    }
    void Target(const char* slot, const char* app_id) {
        snprintf(state.slot, sizeof state.slot, "%s", slot);
        snprintf(state.app_id, sizeof state.app_id, "%s", app_id);
    }
    void Received(uint32_t bytes, int64_t read_us, int64_t write_us) {
        state.received = bytes;
        read_us_ += read_us;
        write_us_ += write_us;
        state.read_ms = read_us_ / 1000;
        state.write_ms = write_us_ / 1000;
        Notify();
        const int64_t now = esp_timer_get_time();
        if (now - logged_at_ >= 5000000) {
            ESP_LOGD("agent_system.install", "bytes=%lu/%lu read_ms=%lu write_ms=%lu",
                     static_cast<unsigned long>(bytes), static_cast<unsigned long>(state.total),
                     static_cast<unsigned long>(state.read_ms),
                     static_cast<unsigned long>(state.write_ms));
            logged_at_ = now;
        }
    }
    esp_err_t Finish(esp_err_t error) {
        LogStage();
        state.error = error;
        state.elapsed_ms = (esp_timer_get_time() - started_) / 1000;
        if (error == ESP_OK)
            state.stage = InstallStage::kDone;
        const auto kib_s =
            state.elapsed_ms ? uint64_t(state.received) * 1000 / state.elapsed_ms / 1024 : 0;
        if (error == ESP_OK || error == kRestartRequired || error == kCancelled) {
            ESP_LOGI(
                "agent_system.install",
                "end stage=%s result=0x%x bytes=%lu/%lu ms=%lu KiB/s=%llu read_ms=%lu write_ms=%lu",
                Name(state.stage), unsigned(error), static_cast<unsigned long>(state.received),
                static_cast<unsigned long>(state.total),
                static_cast<unsigned long>(state.elapsed_ms),
                static_cast<unsigned long long>(kib_s), static_cast<unsigned long>(state.read_ms),
                static_cast<unsigned long>(state.write_ms));
        } else {
            ESP_LOGE("agent_system.install",
                     "failed stage=%s error=%s (0x%x) HTTP=%d errno=%d bytes=%lu/%lu ms=%lu "
                     "read_ms=%lu write_ms=%lu",
                     Name(state.stage), esp_err_to_name(error), unsigned(error), state.http_status,
                     state.network_errno, static_cast<unsigned long>(state.received),
                     static_cast<unsigned long>(state.total),
                     static_cast<unsigned long>(state.elapsed_ms),
                     static_cast<unsigned long>(state.read_ms),
                     static_cast<unsigned long>(state.write_ms));
        }
        Notify();
        return error;
    }
    InstallStatus state{};

   private:
    void Notify() {
        state.elapsed_ms = (esp_timer_get_time() - started_) / 1000;
        if (observer_)
            observer_(state, context_);
    }
    void LogStage() const {
        ESP_LOGI("agent_system.install", "stage=%s ms=%lld bytes=%lu", Name(state.stage),
                 static_cast<long long>((esp_timer_get_time() - stage_at_) / 1000),
                 static_cast<unsigned long>(state.received));
    }
    InstallObserver observer_;
    InstallCancel cancel_;
    void* context_;
    int64_t started_ = esp_timer_get_time(), stage_at_ = started_, logged_at_ = started_;
    int64_t read_us_ = 0, write_us_ = 0;
};
}  // namespace agent_system
