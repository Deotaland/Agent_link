// Wall-clock time for the WiFi channel. See cloud_clock.h.

#include "cloud_clock.h"

#include <atomic>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

namespace {
constexpr const char* TAG = "agent_link.clock";

// One server: CONFIG_LWIP_SNTP_MAX_SERVERS is 1, and the platform's clock covers its absence.
constexpr const char* kNtpServer = "pool.ntp.org";

std::atomic<bool> s_sntp_synced{false};
std::atomic<bool> s_valid{false};
bool              s_started = false;

// Runs on the lwIP task, on the first synchronisation and on every hourly one after it.
void OnSntpSync(struct timeval* /*tv*/) {
    if (!s_sntp_synced.exchange(true)) ESP_LOGI(TAG, "clock synchronised by SNTP");
    s_valid = true;
}
}  // namespace

void cloud_clock_start(void) {
    if (s_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(kNtpServer);
    cfg.sync_cb = OnSntpSync;
    const esp_err_t r = esp_netif_sntp_init(&cfg);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "SNTP did not start (%s) — relying on the platform's clock", esp_err_to_name(r));
        return;
    }
    s_started = true;
}

void cloud_clock_from_platform(int64_t unix_s) {
    if (s_sntp_synced || unix_s <= 0) return;
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(unix_s);
    settimeofday(&tv, nullptr);
    if (!s_valid.exchange(true)) ESP_LOGI(TAG, "clock set from the platform (SNTP has not synchronised)");
}

int64_t cloud_clock_now_ms(void) {
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

bool cloud_clock_valid(void) { return s_valid; }
