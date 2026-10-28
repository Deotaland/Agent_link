#include "agent_link_chat.h"
#include "agent_link_platform.h"
#include "cloud_platform.h"
#include "cloud_credential.h"
#include "cloud_memory.h"
#include "chat_response.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "sdkconfig.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr size_t kMaximum = 65536;
constexpr char kBegin[] = "\n[AGENT_LINK_DEVICE_BEGIN]\n";
constexpr char kEnd[] = "\n[AGENT_LINK_DEVICE_END]";
bool Bound(int id) {
    agent_link_platform_agent_t agent{};
    return id > 0 && agent_link_platform_bound_agent(&agent) == ESP_OK && agent.id == id;
}
bool Safe(const char* s) {
    if (!s || !*s || std::strlen(s) > 79) return false;
    for (; *s; ++s) if (!std::isalnum(static_cast<unsigned char>(*s)) && *s != '_' && *s != '-') return false;
    return true;
}
std::string Escape(const char* s) {
    std::string out;
    const char hex[] = "0123456789ABCDEF";
    if (s) for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        if (std::isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') out += *p;
        else { out += '%'; out += hex[*p >> 4]; out += hex[*p & 15]; }
    }
    return out;
}
struct Receive {
    explicit Receive(size_t cap, bool sse) : capacity(cap), stream(sse), decoder(cap) {}
    size_t capacity;
    bool stream, overflow = false, bad_stream = false;
    agentlink::ChatResponse decoder;
    std::string json;
};
esp_err_t DataEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data) return ESP_OK;
    auto& rx = *static_cast<Receive*>(event->user_data);
    if (event->data_len < 0) return ESP_FAIL;
    if (rx.stream) rx.bad_stream = !rx.decoder.Feed(static_cast<const char*>(event->data), event->data_len);
    else if (rx.json.size() + event->data_len >= rx.capacity) rx.overflow = true;
    else rx.json.append(static_cast<const char*>(event->data), event->data_len);
    return rx.overflow || rx.bad_stream ? ESP_FAIL : ESP_OK;
}
esp_err_t Request(int agent, esp_http_client_method_t method, const std::string& path,
                  const char* body, char* out, size_t capacity, bool stream = false, bool management = false) {
    if (!out || capacity < 2 || capacity > kMaximum) return ESP_ERR_INVALID_ARG;
    out[0] = 0;
    if (!Bound(agent)) return ESP_ERR_INVALID_STATE;
    agentlink::cloud::NetworkMemoryLock memory;
    const std::string key = cloud_cred_auth_key();
    const std::string url = std::string(cloud_platform_base_url()) + path;
    Receive rx(capacity, stream);
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str(); cfg.method = method; cfg.timeout_ms = 60000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach; cfg.event_handler = DataEvent;
    cfg.user_data = &rx; cfg.buffer_size = 1024; cfg.buffer_size_tx = 1024;
    auto client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    const char* prompt_token = "";
#ifdef CONFIG_AGENT_LINK_PROMPT_USER_TOKEN
    if (management) prompt_token = CONFIG_AGENT_LINK_PROMPT_USER_TOKEN;
#else
    (void)management;
#endif
    esp_err_t result = esp_http_client_set_header(client,
        prompt_token[0] ? "X-Token" : "X-Device-Auth-Key", prompt_token[0] ? prompt_token : key.c_str());
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Accept", stream ? "text/event-stream" : "application/json");
    if (result == ESP_OK && body) {
        result = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (result == ESP_OK) result = esp_http_client_set_post_field(client, body, static_cast<int>(std::strlen(body)));
    }
    if (result == ESP_OK) result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status == 401 || status == 403) return ESP_ERR_NOT_SUPPORTED;
    if (rx.overflow) return ESP_ERR_INVALID_SIZE;
    if (rx.bad_stream) return ESP_ERR_INVALID_RESPONSE;
    if (result != ESP_OK) return result;
    if (status < 200 || status >= 300 || !Bound(agent) || key != cloud_cred_auth_key()) return ESP_ERR_INVALID_RESPONSE;
    if (stream) {
        if (!rx.decoder.Finish()) return ESP_ERR_INVALID_RESPONSE;
        std::memcpy(out, rx.decoder.Text().c_str(), rx.decoder.Text().size() + 1);
    } else {
        cJSON* root = cJSON_Parse(rx.json.c_str());
        if (!root) return ESP_ERR_INVALID_RESPONSE;
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
        const cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "success");
        const bool ok = !(cJSON_IsNumber(code) && code->valuedouble != 0) &&
                        !(cJSON_IsBool(success) && !cJSON_IsTrue(success));
        cJSON_Delete(root);
        if (!ok) return ESP_ERR_INVALID_RESPONSE;
        std::memcpy(out, rx.json.c_str(), rx.json.size() + 1);
    }
    return ESP_OK;
}
std::string Base(int agent) { return "/api/agents/" + std::to_string(agent); }
std::string Chats(int agent) { return Base(agent) + "/actions/llm/conversations"; }
std::string Page(const char* cursor) { return "?limit=10" + (cursor && *cursor ? "&cursor=" + Escape(cursor) : ""); }
}

extern "C" esp_err_t agent_link_chat_conversations(int agent, const char* cursor, char* out, size_t cap) {
    if (cursor && std::strlen(cursor) > 512) return ESP_ERR_INVALID_ARG;
    return Request(agent, HTTP_METHOD_GET, Chats(agent) + Page(cursor), nullptr, out, cap);
}
extern "C" esp_err_t agent_link_chat_create(int agent, char* id, size_t cap) {
    if (!id || cap < 40) return ESP_ERR_INVALID_ARG;
    id[0] = 0;
    char response[4096];
    const auto result = Request(agent, HTTP_METHOD_POST, Chats(agent), "{}", response, sizeof response);
    if (result != ESP_OK) return result;
    cJSON* root = cJSON_Parse(response);
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const char* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "conversation_id"));
    if (!value) value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "session_id"));
    const bool valid = Safe(value) && std::strlen(value) < cap;
    if (valid) std::strcpy(id, value);
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
extern "C" esp_err_t agent_link_chat_messages(int agent, const char* session, const char* cursor,
                                               char* out, size_t cap) {
    if (!Safe(session)) return ESP_ERR_INVALID_ARG;
    if (cursor && std::strlen(cursor) > 512) return ESP_ERR_INVALID_ARG;
    return Request(agent, HTTP_METHOD_GET, Chats(agent) + "/" + session + "/messages" + Page(cursor), nullptr, out, cap);
}
extern "C" esp_err_t agent_link_chat_send(int agent, const char* session, const char* text, char* out, size_t cap) {
    if (!Safe(session) || !text || !*text || std::strlen(text) > 8192) return ESP_ERR_INVALID_ARG;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "conversation_id", session);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "user"); cJSON_AddStringToObject(message, "content", text);
    cJSON_AddItemToArray(messages, message);
    char* body = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    const auto result = Request(agent, HTTP_METHOD_POST, Base(agent) + "/actions/llm/execute", body, out, cap, true);
    cJSON_free(body);
    return result;
}
extern "C" esp_err_t agent_link_chat_sync_prompt(int agent, const char* instructions) {
    if (!instructions || !*instructions || std::strlen(instructions) > 12000) return ESP_ERR_INVALID_ARG;
    std::string response(32768, '\0');
    auto result = Request(agent, HTTP_METHOD_GET, Base(agent), nullptr, response.data(), response.size(), false, true);
    if (result != ESP_OK) return result;
    cJSON* root = cJSON_Parse(response.c_str());
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON* prompt_field = cJSON_GetObjectItemCaseSensitive(data, "prompt");
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(data, "id");
    if (!cJSON_IsObject(data) || !cJSON_IsNumber(id) || id->valuedouble != agent ||
        (!cJSON_IsString(prompt_field) && !cJSON_IsNull(prompt_field))) {
        cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE;
    }
    const char* old = cJSON_GetStringValue(prompt_field);
    std::string prompt = old ? old : "";
    cJSON_Delete(root);
    const size_t begin = prompt.find(kBegin);
    if (begin != std::string::npos) {
        const size_t end = prompt.find(kEnd, begin + std::strlen(kBegin));
        if (end == std::string::npos) return ESP_ERR_INVALID_RESPONSE;
        prompt.erase(begin, end + std::strlen(kEnd) - begin);
    }
    prompt += kBegin; prompt += instructions; prompt += kEnd;
    root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "prompt", prompt.c_str());
    char* body = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    result = Request(agent, HTTP_METHOD_PUT, Base(agent), body, response.data(), response.size(), false, true);
    cJSON_free(body);
    return result;
}
