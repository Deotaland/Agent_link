#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "agent_link_platform.h"
#include "cloud_platform.h"
#include "cloud_credential.h"
#include "esp_http_client.h"
#include "sdkconfig.h"

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL at line %d\n", __LINE__); std::exit(1); } } while (false)
struct Expected {
    esp_http_client_method_t method;
    std::string path, response, body;
    int status = 200;
};
struct TestHttp {
    esp_http_client_config_t config;
    std::string url, body;
    std::map<std::string, std::string> headers;
    int status = 0;
};
std::string device_key;
std::vector<Expected> expected;
size_t consumed = 0, allocations = 0;
const char* dev_identity_sn() { return "test-device-exact"; }
extern "C" const char* cloud_cred_auth_key() { return device_key.c_str(); }
extern "C" bool cloud_cred_is_bound() { return !device_key.empty(); }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    ++allocations;
    auto* client = new TestHttp;
    client->config = *config; client->url = config->url;
    CHECK(config->crt_bundle_attach != nullptr && config->timeout_ms > 0);
    return client;
}
esp_err_t esp_http_client_set_header(TestHttp* client, const char* key, const char* value) {
    client->headers[key] = value; return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(TestHttp* client, const char* body, int length) {
    client->body.assign(body, length); return ESP_OK;
}
esp_err_t esp_http_client_perform(TestHttp* client) {
    CHECK(consumed < expected.size());
    const Expected& next = expected[consumed++];
    CHECK(client->config.method == next.method);
    CHECK(client->url == "https://platform.example.test" + next.path);
    CHECK(client->headers["X-Device-Auth-Key"] == device_key);
    CHECK(client->headers.count("Authorization") == 0 && client->headers.count("X-Token") == 0);
    if (!next.body.empty()) CHECK(client->body == next.body);
    if (next.method == HTTP_METHOD_POST &&
        (next.path.find("realtime-grant") != std::string::npos ||
         next.path.find("realtime-refresh") != std::string::npos)) {
        cJSON* body = cJSON_Parse(client->body.c_str());
        const cJSON* config = cJSON_GetObjectItemCaseSensitive(body, "audio_config");
        const cJSON* input = cJSON_GetObjectItemCaseSensitive(config, "input");
        const cJSON* output = cJSON_GetObjectItemCaseSensitive(config, "output");
        const char* encoding = CONFIG_AGENT_LINK_REALTIME_OPUS ? "opus" : "pcm16";
        CHECK(std::strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "encoding")), encoding) == 0);
        CHECK(std::strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(output, "encoding")), encoding) == 0);
        CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(input, "sampleRate")) == 16000);
        CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(output, "sampleRate")) == 24000);
        cJSON_Delete(body);
    }
    client->status = next.status;
    // Split network deliveries so response assembly is exercised too.
    for (size_t i = 0; i < next.response.size(); i += 17) {
        const size_t length = next.response.size() - i < 17 ? next.response.size() - i : 17;
        esp_http_client_event_t event{HTTP_EVENT_ON_DATA, const_cast<char*>(next.response.data() + i),
                                     static_cast<int>(length), client->config.user_data};
        CHECK(client->config.event_handler(&event) == ESP_OK);
    }
    return ESP_OK;
}
int esp_http_client_get_status_code(TestHttp* client) { return client->status; }
esp_err_t esp_http_client_cleanup(TestHttp* client) { delete client; return ESP_OK; }
void Expect(esp_http_client_method_t method, const char* path, const char* response,
            const char* body = "", int status = 200) {
    expected.push_back({method, path, response, body, status});
}

int main() {
    cloud_platform_set_base_url("https://platform.example.test///");
    agent_link_platform_agent_t agents[20] = {};
    agent_link_platform_conversation_t sessions[3] = {};
    size_t count = 99, total = 99; int agent_total = 99;
    char session[80] = {}, url[256] = {}, token[256] = {};
    agentlink::realtime::AudioFormat audio;
    int http_status = 99;
    CHECK(!agent_link_platform_available());
    CHECK(agent_link_platform_agents(1, agents, 20, &count, &agent_total) == ESP_ERR_NOT_SUPPORTED);
    CHECK(count == 0 && agent_total == 0);
    CHECK(agent_link_platform_bind_agent(7) == ESP_ERR_NOT_SUPPORTED);
    CHECK(agent_link_platform_conversations(7, 0, sessions, 3, &count, &total) == ESP_ERR_INVALID_STATE);
    CHECK(agent_link_platform_create_conversation(7, session, sizeof session) == ESP_ERR_INVALID_STATE);
    CHECK(agent_link_platform_delete_conversation(7, "owned-session") == ESP_ERR_INVALID_STATE);
    CHECK(cloud_platform_grant(7, "owned-session", false, url, sizeof url, token, sizeof token,
                               &audio, &http_status) == ESP_ERR_INVALID_STATE);
    cloud_platform_end_grant(7, "owned-session");
    CHECK(allocations == 0 && http_status == 0);
    std::puts("PASS: no device key rejects every operation, even with a legacy user token configured");

    device_key = "test-device-key";
    CHECK(agent_link_platform_available());
    agent_link_platform_agent_t bound = {};
    CHECK(agent_link_platform_bound_agent(&bound) == ESP_ERR_INVALID_STATE);
    const char* auth = R"({"device":{"id":999},"agent":{"id":7,"name":"Platform-bound voice agent","capabilities":{"realtime_voice":{"provider_key":"example"}}}})";
    cJSON* configuration = cJSON_Parse(auth);
    cloud_platform_set_bound_agent(configuration);
    cJSON_Delete(configuration);
    CHECK(agent_link_platform_bound_agent(&bound) == ESP_OK);
    CHECK(bound.id == 7 && bound.voice_supported);
    CHECK(std::strcmp(bound.name, "Platform-bound voice agent") == 0);
    CHECK(agent_link_platform_agents(1, agents, 20, &count, &agent_total) == ESP_ERR_NOT_SUPPORTED);
    CHECK(agent_link_platform_bind_agent(7) == ESP_ERR_NOT_SUPPORTED);
    CHECK(allocations == 0);
    CHECK(agent_link_platform_create_conversation(8, session, sizeof session) == ESP_ERR_INVALID_STATE);
    std::puts("PASS: platform binding comes only from auth.agent; device Agent management sends no request");

    Expect(HTTP_METHOD_POST, "/api/agents/7/actions/realtime_voice/conversations",
           R"({"data":{"conversation_id":"owned-session"}})");
    CHECK(agent_link_platform_create_conversation(7, session, sizeof session) == ESP_OK);
    CHECK(std::strcmp(session, "owned-session") == 0);
    Expect(HTTP_METHOD_GET, "/api/agents/7/actions/realtime_voice/conversations?limit=20",
           R"({"data":{"items":[{"conversation_id":"previous"},{"conversation_id":"owned-session","name":"Test"}]}})");
    CHECK(agent_link_platform_conversations(7, 1, sessions, 3, &count, &total) == ESP_OK);
    CHECK(count == 1 && total == 2 && std::strcmp(sessions[0].id, session) == 0);
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    const char* grant = R"({"data":{"grant":{"websocket_url":"wss://voice.example.test/socket","connect_token":"test-one-use-token","audio_config":{"input":{"encoding":"opus","sampleRate":16000,"channels":1,"packetDurationMs":20},"output":{"encoding":"opus","sampleRate":24000,"channels":1,"packetDurationMs":20}}}}})";
#else
    const char* grant = R"({"data":{"grant":{"websocket_url":"wss://voice.example.test/socket","connect_token":"test-one-use-token","audio_config":{"input":{"encoding":"pcm16","sampleRate":16000,"channels":1},"output":{"encoding":"pcm16","sampleRate":24000,"channels":1}}}}})";
#endif
    Expect(HTTP_METHOD_POST, "/api/agents/7/actions/realtime_voice/realtime-grant", grant);
    CHECK(cloud_platform_grant(7, session, false, url, sizeof url, token, sizeof token,
                               &audio, &http_status) == ESP_OK);
    CHECK(http_status == 200 && audio.input_opus == !!CONFIG_AGENT_LINK_REALTIME_OPUS &&
          audio.output_opus == !!CONFIG_AGENT_LINK_REALTIME_OPUS);
    Expect(HTTP_METHOD_POST, "/api/agents/7/actions/realtime_voice/realtime-refresh", grant);
    CHECK(cloud_platform_grant(7, session, true, url, sizeof url, token, sizeof token,
                               &audio, &http_status) == ESP_OK);
    Expect(HTTP_METHOD_DELETE, "/api/agents/7/actions/realtime_voice/realtime-grant/owned-session", "", "", 204);
    cloud_platform_end_grant(7, session);
    Expect(HTTP_METHOD_DELETE, "/api/agents/7/actions/realtime_voice/conversations/owned-session", "", "", 204);
    CHECK(agent_link_platform_delete_conversation(7, session) == ESP_OK);
    std::puts("PASS: conversation lifecycle, negotiated audio grant/refresh and cancellation use device authentication");

    const size_t allocations_before = allocations;
    device_key = "rotated-test-device-key";
    CHECK(agent_link_platform_bound_agent(&bound) == ESP_ERR_INVALID_STATE);
    CHECK(agent_link_platform_conversations(7, 0, sessions, 3, &count, &total) == ESP_ERR_INVALID_STATE);
    CHECK(allocations == allocations_before);
    configuration = cJSON_Parse(auth);
    cloud_platform_set_bound_agent(configuration);
    cJSON_Delete(configuration);
    Expect(HTTP_METHOD_GET, "/api/agents/7/actions/realtime_voice/conversations?limit=20",
           R"({"code":1005,"success":false,"data":{}})");
    CHECK(agent_link_platform_conversations(7, 0, sessions, 3, &count, &total) == ESP_ERR_INVALID_RESPONSE);
    Expect(HTTP_METHOD_GET, "/api/agents/7/actions/realtime_voice/conversations?limit=20", R"({"code":0,"data":{}})", "", 401);
    CHECK(agent_link_platform_conversations(7, 0, sessions, 3, &count, &total) == ESP_ERR_INVALID_RESPONSE);

    std::string page = R"({"data":{"items":[)";
    for (int i = 0; i < 20; ++i) {
        if (i) page += ',';
        page += "{\"conversation_id\":\"s" + std::to_string(i) + "\"}";
    }
    page += R"(],"has_more":true,"next_cursor":"abc+/="}})";
    Expect(HTTP_METHOD_GET, "/api/agents/7/actions/realtime_voice/conversations?limit=20", page.c_str());
    Expect(HTTP_METHOD_GET, "/api/agents/7/actions/realtime_voice/conversations?limit=20&cursor=abc%2B%2F%3D",
           R"({"data":{"items":[{"conversation_id":"s20"},{"conversation_id":"s21"},{"conversation_id":"s22"}],"has_more":false}})");
    CHECK(agent_link_platform_conversations(7, 18, sessions, 3, &count, &total) == ESP_OK);
    CHECK(count == 3 && total == 23);
    CHECK(std::strcmp(sessions[0].id, "s18") == 0 && std::strcmp(sessions[2].id, "s20") == 0);
    std::puts("PASS: cursor pagination spans the first 20 results without skipping boundary conversations");

    configuration = cJSON_Parse(R"({"agent":{"id":8,"name":"Rebound agent","capabilities":{"realtime_voice":{}}}})");
    cloud_platform_set_bound_agent(configuration); cJSON_Delete(configuration);
    CHECK(agent_link_platform_bound_agent(&bound) == ESP_OK && bound.id == 8);
    CHECK(agent_link_platform_create_conversation(7, session, sizeof session) == ESP_ERR_INVALID_STATE);
    CHECK(agent_link_platform_delete_conversation(7, "owned-session") == ESP_ERR_INVALID_STATE);
    CHECK(cloud_platform_grant(7, "owned-session", false, url, sizeof url, token, sizeof token,
                              &audio, &http_status) == ESP_ERR_INVALID_STATE);
    cloud_platform_set_bound_agent(nullptr);
    CHECK(agent_link_platform_bound_agent(&bound) == ESP_ERR_NOT_FOUND);
    CHECK(consumed == expected.size());
    std::puts("PASS: credential rotation, platform rebinding and unbinding invalidate stale sessions");
    std::puts("PASS: HTTP 200 business denial and HTTP 401 are rejected");
}
