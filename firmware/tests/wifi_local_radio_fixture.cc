#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = 0x107;
constexpr int ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_INVALID_STATE = 0x103, ESP_ERR_WIFI_NOT_STARTED = 0x3002;
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
void vTaskDelay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
const char* esp_err_to_name(int) { return "test-error"; }

enum class Phase { kIdle, kProvisioning, kStaConnecting, kStaConnected };
std::atomic<Phase> s_phase{Phase::kStaConnected};
std::atomic<bool> s_local_radio{false};
bool StationReady() { return !s_local_radio && s_phase == Phase::kStaConnected; }

namespace session {
std::atomic<bool> s_run{true}, mqtt{true}, http{true}, progress{true};
std::atomic<uint32_t> s_pause_request{0}, s_pause_ack{0}, s_pause_generation{0};
struct { bool (*net_ready)(); } s_cfg{StationReady};
constexpr int AGENT_CLOUD_NO_NETWORK = 1;
void Publish(int, void*) {}
void StopMqtt() { mqtt = false; }
void cloud_api_drop_connection() { http = false; }
// INSERT_SESSION_WAIT
// INSERT_SESSION_API
void Worker() {
    while (s_run) {
        if (progress) {
            WaitForNet();
            if (s_run && NetReady()) { mqtt = true; http = true; }
        }
        vTaskDelay(1);
    }
}
}
namespace voice {
std::atomic<bool> s_busy{false};
// INSERT_VOICE_WAIT
}

std::mutex s_wifi_mtx;
bool s_wifi_inited = false, s_started = true, s_want_connect = true;
std::atomic<bool> s_realtime_wanted{true}, s_platform_voice{true};
int64_t s_portal_close_at = 100;
int s_tick_timer = 1, disconnects = 0, starts = 0, station_starts = 0, ap_queries = 0;
int pause_error = ESP_OK, voice_error = ESP_OK, channel_error = ESP_OK, stop_error = ESP_OK;
uint8_t radio_channel = 0;
bool portal = false;
struct Join { void Stop() {} } s_join;
void UpdateJoinProgress() {}
void UpdateLink() {}
void cloud_realtime_cancel() { s_realtime_wanted = false; }
esp_err_t cloud_session_pause(uint32_t ms) {
    return pause_error == ESP_OK ? session::cloud_session_pause(ms) : pause_error;
}
void cloud_session_resume() { session::cloud_session_resume(); }
esp_err_t cloud_realtime_wait_idle(uint32_t ms) {
    return voice_error == ESP_OK ? voice::cloud_realtime_wait_idle(ms) : voice_error;
}
bool al_wifi_prov_active() { return portal; }
void al_wifi_prov_stop() { portal = false; }
void esp_timer_stop(int) {}
constexpr int WIFI_MODE_STA = 1, WIFI_SECOND_CHAN_NONE = 0;
esp_err_t esp_wifi_disconnect() {
    // The essential ordering invariant: no client loses the radio underneath it.
    assert(!session::mqtt && !session::http && !voice::s_busy);
    assert(session::s_pause_request != 0 && session::s_pause_ack == session::s_pause_request);
    ++disconnects; return ESP_OK;
}
esp_err_t esp_wifi_stop() { return stop_error; }
esp_err_t esp_wifi_set_mode(int mode) { assert(mode == WIFI_MODE_STA); return ESP_OK; }
esp_err_t esp_wifi_start() { ++starts; return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t channel, int) { radio_channel = channel; return channel_error; }
esp_err_t wifi_start(void*) {
    // Cloud resume must follow station startup, and must retain the same task.
    assert(session::s_pause_request != 0);
    ++station_starts; s_started = true; s_want_connect = true;
    s_phase = Phase::kStaConnected; return ESP_OK;
}
// INSERT_LOCAL_RADIO

struct agent_link_wifi_info_t {
    bool connected = false, setup_active = false;
    int rssi = 0;
    char setup_ssid[33]{}, ssid[33]{}, ip[16]{};
};
char s_ap_ssid[33] = "Amoled175-TEST";
struct wifi_ap_record_t { int rssi; uint8_t ssid[33]; };
struct esp_netif_ip_info_t { uint32_t ip; };
int esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap) {
    ++ap_queries; ap->rssi = -45; memcpy(ap->ssid, "test-ap", 8); return ESP_OK;
}
int esp_netif_get_handle_from_ifkey(const char*) { return 1; }
int esp_netif_get_ip_info(int, esp_netif_ip_info_t*) { return ESP_FAIL; }
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) 0U,0U,0U,0U
// INSERT_WIFI_INFO

int main() {
    assert(agent_wifi_local_begin(0) == ESP_ERR_INVALID_ARG);
    assert(agent_wifi_local_begin(6) == ESP_ERR_INVALID_STATE);
    s_wifi_inited = true;
    std::thread owner(session::Worker);
    for (int cycle = 0; cycle < 5; ++cycle) {
        assert(agent_wifi_local_begin(6) == ESP_OK);
        assert(s_local_radio && radio_channel == 6 && !s_want_connect);
        assert(agent_wifi_local_begin(6) == ESP_ERR_INVALID_STATE);
        agent_link_wifi_info_t info{};
        assert(agent_link_wifi_info(&info) == ESP_OK && !info.connected && ap_queries == 0);
        if (!cycle) {
            stop_error = ESP_FAIL;
            assert(agent_wifi_local_end() == ESP_FAIL && s_local_radio && session::s_pause_request != 0);
            stop_error = ESP_OK;
        }
        assert(agent_wifi_local_end() == ESP_OK && !s_local_radio && session::s_pause_request == 0);
        assert(agent_wifi_local_end() == ESP_OK);
    }
    assert(disconnects == 5 && station_starts == 5);
    // Failed cleanup never disconnects/restarts the station.
    pause_error = ESP_ERR_TIMEOUT;
    assert(agent_wifi_local_begin(6) == ESP_ERR_TIMEOUT && !s_local_radio && disconnects == 5);
    pause_error = ESP_OK; voice_error = ESP_ERR_TIMEOUT;
    assert(agent_wifi_local_begin(6) == ESP_ERR_TIMEOUT && !s_local_radio && disconnects == 5);
    assert(session::s_pause_request == 0);
    voice_error = ESP_OK;
    // A channel-selection error restores station WiFi and releases the pause.
    channel_error = ESP_FAIL;
    assert(agent_wifi_local_begin(6) == ESP_FAIL && !s_local_radio);
    assert(station_starts == 6 && session::s_pause_request == 0);
    channel_error = ESP_OK;
    session::s_run = false; owner.join();

    // An owner still in a blocking request cannot acknowledge a pause. Timeout
    // cancels the ticket; an earlier acknowledgement must not satisfy a retry.
    session::s_run = true;
    session::s_pause_ack = session::s_pause_generation.load();
    assert(session::cloud_session_pause(40) == ESP_ERR_TIMEOUT);
    assert(session::s_pause_request == 0);
    std::thread retry_owner(session::Worker);
    assert(session::cloud_session_pause(500) == ESP_OK);
    assert(session::s_pause_ack == session::s_pause_request && !session::mqtt && !session::http);
    session::cloud_session_resume();
    session::s_run = false; retry_owner.join();

    // Offline/claim backoffs yield quickly to a local-mode request.
    session::s_run = true;
    std::atomic<bool> sleeping{false};
    std::thread backoff([&] { sleeping = true; session::SessionDelay(30000); session::WaitForNet(); });
    while (!sleeping) vTaskDelay(1);
    assert(session::cloud_session_pause(500) == ESP_OK);
    session::cloud_session_resume(); backoff.join();
    session::s_run = false;

    voice::s_busy = true;
    assert(voice::cloud_realtime_wait_idle(20) == ESP_ERR_TIMEOUT);
    std::thread drain([] { vTaskDelay(20); voice::s_busy = false; });
    assert(voice::cloud_realtime_wait_idle(500) == ESP_OK); drain.join();

    agent_link_wifi_info_t info{};
    s_phase = Phase::kProvisioning; portal = true;
    assert(agent_link_wifi_info(&info) == ESP_OK && info.setup_active && !info.connected && ap_queries == 0);
    assert(strcmp(info.setup_ssid, s_ap_ssid) == 0);
    s_phase = Phase::kStaConnected; portal = false;
    assert(agent_link_wifi_info(&info) == ESP_OK && info.connected && ap_queries == 1);
    assert(info.rssi == -45 && strcmp(info.ssid, "test-ap") == 0);
    puts("Local radio: cloud cleanup precedes disconnect; repeat entry/exit, timeout rollback, stale ack, voice drain and AP query guards passed.");
}
