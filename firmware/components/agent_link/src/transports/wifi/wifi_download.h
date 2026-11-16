#pragma once
#include "esp_log.h"
#include "esp_wifi.h"

namespace agentlink::wifi {
// The file client owns this guard until the response is closed, including on
// cancellation and failure. It does not change the saved WiFi configuration.
class DownloadPower {
   public:
    DownloadPower() = default;
    DownloadPower(const DownloadPower&) = delete;
    DownloadPower& operator=(const DownloadPower&) = delete;
    ~DownloadPower() {
        if (!active_)
            return;
        Log("end");
        if (changed_) {
            const auto error = esp_wifi_set_ps(previous_);
            if (error != ESP_OK)
                ESP_LOGW(kTag, "power save restore ps=%d result=0x%x", int(previous_),
                         unsigned(error));
            else
                ESP_LOGI(kTag, "power save restored ps=%d", int(previous_));
        }
    }
    void Begin() {
        if (active_)
            return;
        active_ = true;
        const auto error = esp_wifi_get_ps(&previous_);
        if (error != ESP_OK) {
            ESP_LOGW(kTag, "power save query failed result=0x%x; keeping current mode",
                     unsigned(error));
        } else if (previous_ != WIFI_PS_NONE) {
            const auto changed = esp_wifi_set_ps(WIFI_PS_NONE);
            changed_ = changed == ESP_OK;
            if (!changed_)
                ESP_LOGW(kTag, "power save suspend failed result=0x%x", unsigned(changed));
        }
        ESP_LOGI(kTag, "power save before=%d suspended=%d", error == ESP_OK ? int(previous_) : -1,
                 int(changed_));
        Log("begin");
    }

   private:
    static constexpr const char* kTag = "agent_link.download";
    static void Log(const char* phase) {
        int rssi = 0;
        const auto signal_error = esp_wifi_sta_get_rssi(&rssi);
        wifi_ps_type_t ps = WIFI_PS_NONE;
        const auto ps_error = esp_wifi_get_ps(&ps);
        ESP_LOGI(kTag, "network=%s rssi_dbm=%d rssi_result=0x%x ps=%d ps_result=0x%x", phase, rssi,
                 unsigned(signal_error), ps_error == ESP_OK ? int(ps) : -1, unsigned(ps_error));
#if defined(CONFIG_LWIP_TCP_WND_DEFAULT) && defined(CONFIG_LWIP_TCP_RECVMBOX_SIZE)
        ESP_LOGI(kTag, "tcp rx_window=%d rx_mbox=%d", CONFIG_LWIP_TCP_WND_DEFAULT,
                 CONFIG_LWIP_TCP_RECVMBOX_SIZE);
#endif
    }
    wifi_ps_type_t previous_ = WIFI_PS_MIN_MODEM;
    bool active_ = false;
    bool changed_ = false;
};
}  // namespace agentlink::wifi
