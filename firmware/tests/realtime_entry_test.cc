#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_FAIL = 2;
enum agent_state_t { AGENT_STATE_DISCONNECTED, AGENT_STATE_CONNECTED, AGENT_STATE_READY };
std::atomic<agent_state_t> s_link{AGENT_STATE_CONNECTED};
std::atomic<bool> s_realtime_wanted{false}, s_platform_voice{false};
bool platform_enabled = true, mqtt_started = false, direct_enabled = false, recovering = false;
bool cloud_realtime_recovering() { return recovering; }
unsigned platform_requests = 0, mqtt_requests = 0, direct_requests = 0;
esp_err_t request_result = ESP_OK;
bool cloud_platform_enabled() { return platform_enabled; }
bool cloud_mqtt_started() { return mqtt_started; }
bool cloud_realtime_direct_enabled() { return direct_enabled; }
void OnRealtimeState(bool, const char*) {}
esp_err_t cloud_realtime_request_platform(int agent, const char* session) {
    assert(agent == 4 && std::strcmp(session, "fixture-session") == 0);
    ++platform_requests;
    return request_result;
}
esp_err_t cloud_json_request_realtime() { ++mqtt_requests; return request_result; }
esp_err_t cloud_realtime_request_direct() { ++direct_requests; return request_result; }

// INSERT_PRODUCTION_ENTRIES

int main() {
    // No MQTT client exists. HTTP-granted voice must still reach the worker.
    assert(agent_transport_wifi_request_realtime_session(4, "fixture-session") == ESP_OK);
    assert(platform_requests == 1 && s_realtime_wanted && s_platform_voice);
    assert(agent_transport_wifi_realtime_running());
    assert(!agent_transport_wifi_realtime_recovering()); // Initial startup, not a retry.
    recovering = true;
    assert(agent_transport_wifi_realtime_recovering());
    recovering = false;
    assert(!agent_transport_wifi_realtime_recovering()); // Terminal cleanup still owns the call.
    s_link = AGENT_STATE_READY;
    assert(agent_transport_wifi_realtime_running());
    s_link = AGENT_STATE_CONNECTED; // Recovery still owns the call.
    assert(agent_transport_wifi_realtime_running());
    assert(agent_transport_wifi_request_realtime_session(4, "fixture-session") == ESP_ERR_INVALID_STATE);
    assert(platform_requests == 1); // A duplicate tap must not request another grant.
    s_realtime_wanted = false; s_platform_voice = false;
    assert(!agent_transport_wifi_realtime_running());
    recovering = true;
    assert(!agent_transport_wifi_realtime_recovering()); // A cancelled call cannot resume.
    platform_enabled = false;
    assert(agent_transport_wifi_request_realtime_session(4, "fixture-session") == ESP_ERR_INVALID_STATE);
    platform_enabled = true; s_link = AGENT_STATE_DISCONNECTED;
    assert(agent_transport_wifi_request_realtime_session(4, "fixture-session") == ESP_ERR_INVALID_STATE);
    assert(platform_requests == 1);
    s_link = AGENT_STATE_CONNECTED; request_result = ESP_FAIL;
    assert(agent_transport_wifi_request_realtime_session(4, "fixture-session") == ESP_FAIL);
    assert(platform_requests == 2 && !s_realtime_wanted && !s_platform_voice);
    assert(!agent_transport_wifi_realtime_running());
    request_result = ESP_OK;
    assert(agent_transport_wifi_request_realtime_session(4, "fixture-session") == ESP_OK);
    assert(platform_requests == 3); // A failed enqueue permits a later attempt.

    // The legacy gateway path still needs MQTT; direct test grants do not.
    s_realtime_wanted = false; s_platform_voice = false;
    assert(agent_transport_wifi_request_realtime() == ESP_ERR_INVALID_STATE);
    assert(mqtt_requests == 0 && !s_realtime_wanted);
    mqtt_started = true;
    assert(agent_transport_wifi_request_realtime() == ESP_OK && mqtt_requests == 1);
    mqtt_started = false; direct_enabled = true; s_realtime_wanted = false;
    assert(agent_transport_wifi_request_realtime() == ESP_OK && direct_requests == 1);
    std::puts("PASS: HTTP voice starts without MQTT; offline/unconfigured/duplicate requests rejected; enqueue failure recovers; legacy MQTT and direct grant paths preserved");
}
