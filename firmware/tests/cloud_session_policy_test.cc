// The runner inserts production functions; stubs count network operations.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#define ESP_OK 0
#define ESP_ERR_NO_MEM -1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define MALLOC_CAP_SPIRAM 1
#define CLOUD_API_NO_AGENT 1002104
#define AGENT_CLOUD_NO_AGENT 1
namespace {
bool kMqttEnabled = false, s_run = true, s_token_ok = false, mqtt_started = false;
int64_t now_us = 0, s_token_renew = 0, s_token_retry = 0;
unsigned allocations = 0, tokens = 0, starts = 0, stops = 0, beats = 0, auths = 0, wakes = 0;
constexpr unsigned kHeartbeatMs = 60000, kFaultRetryMs = 30000, kNetPollMs = 1000;
constexpr uint32_t kNotifyMqttRefused = 1;
constexpr size_t kAgentJsonCap = 2048;
char s_agent_json[kAgentJsonCap];
struct cloud_api_err_t { int code = 0; };
struct cloud_mqtt_token_t { char credentials[64]; };
cloud_mqtt_token_t* s_token = nullptr;
struct cloud_mqtt_callbacks_t {
    void (*on_refused)(int, void*) = nullptr;
    void (*on_downlink)(const char*, size_t, void*) = nullptr;
    void* ctx = nullptr;
};
cloud_mqtt_callbacks_t s_cfg;
enum class TokenResult { kKeyRevoked, kNoAgent, kOk };
enum class Leave { kStopped, kKeyGone, kNoNetwork, kNoAgent };
bool cloud_cred_is_bound() { return true; }
bool NetReady() { return true; }
int64_t esp_timer_get_time() { return now_us; }
TokenResult FetchToken() { ++tokens; s_token_ok = true; return TokenResult::kOk; }
bool cloud_mqtt_started() { return mqtt_started; }
int cloud_mqtt_start(const cloud_mqtt_token_t*, const cloud_mqtt_callbacks_t*) {
    ++starts; mqtt_started = true; return ESP_OK;
}
void cloud_mqtt_stop() { ++stops; mqtt_started = false; }
void OnMqttRefused(int, void*) {}
void HandleMqttRefused() { assert(false); }
void ulTaskNotifyValueClear(void*, uint32_t) {}
const char* cloud_cred_auth_key() { return "fixture-key"; }
const char* StaMac() { return "fixture-mac"; }
const char* agent_link_ota_running_version() { return "fixture-version"; }
int cloud_api_heartbeat(const char*, cloud_api_err_t*) { ++beats; return ESP_OK; }
int cloud_api_auth(const char*, const char*, const char*, char*, size_t, cloud_api_err_t*) {
    ++auths; return ESP_OK;
}
bool HandleRevoked(const cloud_api_err_t&) { return false; }
void Publish(int, const cloud_api_err_t*) {}
int xTaskNotifyWait(uint32_t, uint32_t, uint32_t*, unsigned) {
    now_us += 61000000;
    if (++wakes == 3) s_run = false;
    return 0;
}
void* heap_caps_calloc(size_t count, size_t size, unsigned) {
    ++allocations; return std::calloc(count, size);
}

// INSERT_STOP_MQTT
// INSERT_STAY_ONLINE
int AllocateCredentials() {
    // INSERT_TOKEN_ALLOCATION
    return ESP_OK;
}
}

int main() {
    for (bool enabled : {false, true}) {
        kMqttEnabled = enabled;
        s_run = true; s_token_ok = mqtt_started = false;
        now_us = s_token_renew = s_token_retry = 0;
        allocations = tokens = starts = stops = beats = auths = wakes = 0;
        assert(AllocateCredentials() == ESP_OK);
        assert(allocations == (enabled ? 1u : 0u));
        assert(StayOnline() == Leave::kStopped);
        assert(tokens == (enabled ? 1u : 0u));
        assert(starts == (enabled ? 1u : 0u));
        assert(stops == (enabled ? 1u : 0u));
        assert(beats == 2 && auths == 2);
        std::free(s_token); s_token = nullptr;
    }
    std::puts("PASS: production session loop skips MQTT token allocation/fetch/start/stop when disabled; HTTP heartbeat and auth still run; enabled behavior retained");
}
