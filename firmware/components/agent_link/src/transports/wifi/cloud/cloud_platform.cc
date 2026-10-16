#include "agent_link_platform.h"
#include "cloud_platform.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "cloud_credential.h"
#include "device_identity.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace {
constexpr const char* TAG = "agent_link.platform";
constexpr size_t kListBytes = 96 * 1024;
constexpr size_t kSmallBytes = 8 * 1024;
char s_base[160] = {};

enum class ApiAuth { kUser, kDeviceOrUser };

const char* UserToken() {
#ifdef CONFIG_AGENT_LINK_PLATFORM_USER_TOKEN
    return CONFIG_AGENT_LINK_PLATFORM_USER_TOKEN;
#else
    return "";
#endif
}

bool SafeId(const char* id) {
    if (!id || !*id || strlen(id) > 79) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id); *p; ++p) {
        if (!std::isalnum(*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

const cJSON* Field(const cJSON* object, const char* key) {
    return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

const char* String(const cJSON* object, const char* key) {
    const cJSON* item = Field(object, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

const cJSON* Data(const cJSON* root) {
    const cJSON* data = Field(root, "data");
    return data ? data : root;
}

const cJSON* Items(const cJSON* data) {
    if (cJSON_IsArray(data)) return data;
    const cJSON* items = Field(data, "items");
    if (!cJSON_IsArray(items)) items = Field(data, "list");
    if (!cJSON_IsArray(items)) items = Field(data, "records");
    return items;
}

void Copy(char* out, size_t cap, const char* value) {
    if (out && cap) snprintf(out, cap, "%s", value ? value : "");
}

struct Response {
    char* buffer;
    size_t capacity;
    size_t size;
    bool overflow;
};

esp_err_t HttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data) return ESP_OK;
    auto* response = static_cast<Response*>(event->user_data);
    if (event->data_len < 0 || response->size + static_cast<size_t>(event->data_len) >= response->capacity) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->buffer + response->size, event->data, event->data_len);
    response->size += event->data_len;
    response->buffer[response->size] = '\0';
    return ESP_OK;
}

esp_err_t Exchange(const char* operation, esp_http_client_method_t method, const char* path, const char* body,
                   size_t capacity, cJSON** root_out, int* status_out, ApiAuth auth) {
    if (root_out) *root_out = nullptr;
    if (status_out) *status_out = 0;
    const char* device_key = cloud_cred_auth_key();
    const bool use_device_key = auth == ApiAuth::kDeviceOrUser && device_key[0];
    if (!s_base[0] || (!use_device_key && !UserToken()[0]) || !path || !root_out)
        return ESP_ERR_INVALID_STATE;

    char url[512];
    const int url_len = snprintf(url, sizeof url, "%s%s", s_base, path);
    if (url_len <= 0 || url_len >= static_cast<int>(sizeof url)) return ESP_ERR_INVALID_SIZE;
    auto* buffer = static_cast<char*>(heap_caps_calloc(capacity, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) buffer = static_cast<char*>(calloc(capacity, 1));
    if (!buffer) return ESP_ERR_NO_MEM;
    Response response{buffer, capacity, 0, false};
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = method;
    config.timeout_ms = 12000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEvent;
    config.user_data = &response;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(buffer); return ESP_ERR_NO_MEM; }

    esp_err_t result = ESP_OK;
    if (use_device_key) {
        result = esp_http_client_set_header(client, "X-Device-Auth-Key", device_key);
    } else {
        char authorization[160];
        snprintf(authorization, sizeof authorization, "Bearer %s", UserToken());
        result = esp_http_client_set_header(client, "Authorization", authorization);
        memset(authorization, 0, sizeof authorization);
        if (result == ESP_OK) result = esp_http_client_set_header(client, "X-Token", UserToken());
    }
    if (result == ESP_OK && body) {
        result = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (result == ESP_OK) result = esp_http_client_set_post_field(client, body, strlen(body));
    }
    if (result == ESP_OK) result = esp_http_client_perform(client);
    const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    if (status_out) *status_out = status;
    esp_http_client_cleanup(client);
    if (result == ESP_OK && response.overflow) result = ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK && (status < 200 || status >= 300)) result = ESP_ERR_INVALID_RESPONSE;
    if (result == ESP_OK && response.size) {
        cJSON* root = cJSON_ParseWithLength(buffer, response.size);
        if (!root) result = ESP_ERR_INVALID_RESPONSE;
        else {
            const cJSON* success = Field(root, "success");
            const cJSON* code = Field(root, "code");
            if ((cJSON_IsBool(success) && !cJSON_IsTrue(success)) ||
                (cJSON_IsNumber(code) && code->valueint != 0)) {
                result = ESP_ERR_INVALID_RESPONSE;
                cJSON_Delete(root);
            } else *root_out = root;
        }
    } else if (result == ESP_OK && method != HTTP_METHOD_DELETE) result = ESP_ERR_INVALID_RESPONSE;
    if (result != ESP_OK) ESP_LOGW(TAG, "%s: HTTP %d, %s", operation, status, esp_err_to_name(result));
    memset(buffer, 0, response.size);
    free(buffer);
    return result;
}

const char* GrantString(const cJSON* data, const char* a, const char* b, const char* c) {
    const char* value = String(data, a);
    if (!value) value = String(data, b);
    if (!value) value = String(data, c);
    return value;
}

} // namespace

void cloud_platform_set_base_url(const char* base_url) {
    Copy(s_base, sizeof s_base, base_url);
    size_t len = strlen(s_base);
    while (len && s_base[len - 1] == '/') s_base[--len] = '\0';
}

bool cloud_platform_enabled(void) { return s_base[0] && (cloud_cred_is_bound() || UserToken()[0]); }

extern "C" bool agent_link_platform_available(void) { return cloud_platform_enabled(); }

extern "C" esp_err_t agent_link_platform_agents(int page, agent_link_platform_agent_t* entries,
                                                  size_t capacity, size_t* count, int* total) {
    if (!entries || !count || !total || page < 1 || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0; *total = 0;
    char path[96];
    snprintf(path, sizeof path, "/api/agents?page=%d&page_size=20", page);
    cJSON* root = nullptr;
    esp_err_t result = Exchange("list agents", HTTP_METHOD_GET, path, nullptr, kListBytes, &root,
                                nullptr, ApiAuth::kUser);
    if (result != ESP_OK) return result;
    const cJSON* data = Data(root);
    const cJSON* items = Items(data);
    const cJSON* total_json = Field(data, "total");
    if (!cJSON_IsArray(items)) result = ESP_ERR_INVALID_RESPONSE;
    else {
        *total = cJSON_IsNumber(total_json) ? total_json->valueint : cJSON_GetArraySize(items);
        const int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n && *count < capacity; ++i) {
            const cJSON* item = cJSON_GetArrayItem(items, i);
            const cJSON* id = Field(item, "id");
            if (!cJSON_IsNumber(id) || id->valueint <= 0) continue;
            auto& entry = entries[(*count)++];
            entry.id = id->valueint;
            Copy(entry.name, sizeof entry.name, String(item, "name"));
            const cJSON* actions = Field(item, "supported_actions");
            entry.voice_supported = false;
            if (cJSON_IsArray(actions)) {
                const int action_count = cJSON_GetArraySize(actions);
                for (int a = 0; a < action_count; ++a) {
                    const char* key = String(cJSON_GetArrayItem(actions, a), "action_key");
                    if (key && strcmp(key, "realtime_voice") == 0) {
                        entry.voice_supported = true;
                        break;
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
    return result;
}

extern "C" esp_err_t agent_link_platform_bind_agent(int agent_id) {
    const char* sn = dev_identity_sn();
    if (agent_id <= 0 || !SafeId(sn)) return ESP_ERR_INVALID_ARG;
    char path[160];
    snprintf(path, sizeof path, "/api/devices?keyword=%s&page=1&page_size=20", sn);
    cJSON* root = nullptr;
    esp_err_t result = Exchange("find device", HTTP_METHOD_GET, path, nullptr, kSmallBytes, &root,
                                nullptr, ApiAuth::kUser);
    if (result != ESP_OK) return result;
    int device_id = 0;
    int bound_agent_id = 0;
    const cJSON* items = Items(Data(root));
    if (cJSON_IsArray(items)) {
        const int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n; ++i) {
            const cJSON* item = cJSON_GetArrayItem(items, i);
            const char* found = String(item, "device_sn");
            if (!found) found = String(item, "sn");
            const cJSON* id = Field(item, "id");
            if (found && strcmp(found, sn) == 0 && cJSON_IsNumber(id)) {
                device_id = id->valueint;
                const cJSON* bound = Field(item, "agent_id");
                if (cJSON_IsNumber(bound)) bound_agent_id = bound->valueint;
                break;
            }
        }
    }
    cJSON_Delete(root);
    if (device_id <= 0) return ESP_ERR_NOT_FOUND;
    if (bound_agent_id == agent_id) return ESP_OK;
    snprintf(path, sizeof path, "/api/devices/%d/bind-agent", device_id);
    char body[64];
    snprintf(body, sizeof body, "{\"agent_id\":%d}", agent_id);
    root = nullptr;
    result = Exchange("bind agent", HTTP_METHOD_POST, path, body, kSmallBytes, &root,
                      nullptr, ApiAuth::kUser);
    cJSON_Delete(root);
    return result;
}

extern "C" esp_err_t agent_link_platform_conversations(int agent_id, size_t offset,
                                                           agent_link_platform_conversation_t* entries,
                                                           size_t capacity, size_t* count, size_t* total) {
    if (agent_id <= 0 || !entries || !count || !total || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0; *total = 0;
    char path[160];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/conversations", agent_id);
    cJSON* root = nullptr;
    esp_err_t result = Exchange("list conversations", HTTP_METHOD_GET, path, nullptr, kListBytes,
                                &root, nullptr, ApiAuth::kDeviceOrUser);
    if (result != ESP_OK) return result;
    const cJSON* items = Items(Data(root));
    if (!cJSON_IsArray(items)) result = ESP_ERR_INVALID_RESPONSE;
    else {
        *total = cJSON_GetArraySize(items);
        for (size_t i = offset; i < *total && *count < capacity; ++i) {
            const cJSON* item = cJSON_GetArrayItem(items, i);
            const char* id = String(item, "conversation_id");
            if (!id) id = String(item, "session_id");
            if (!SafeId(id)) continue;
            auto& entry = entries[(*count)++];
            Copy(entry.id, sizeof entry.id, id);
            Copy(entry.name, sizeof entry.name, String(item, "name"));
            Copy(entry.created_at, sizeof entry.created_at, String(item, "created_at"));
        }
    }
    cJSON_Delete(root);
    return result;
}

extern "C" esp_err_t agent_link_platform_create_conversation(int agent_id, char* id, size_t id_size) {
    if (agent_id <= 0 || !id || id_size < 40) return ESP_ERR_INVALID_ARG;
    id[0] = '\0';
    char path[160];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/conversations", agent_id);
    cJSON* root = nullptr;
    esp_err_t result = Exchange("create conversation", HTTP_METHOD_POST, path, nullptr, kSmallBytes,
                                &root, nullptr, ApiAuth::kDeviceOrUser);
    if (result != ESP_OK) return result;
    const cJSON* data = Data(root);
    const char* session = String(data, "conversation_id");
    if (!session) session = String(data, "session_id");
    if (!session) session = String(Field(data, "conversation"), "conversation_id");
    if (SafeId(session) && strlen(session) < id_size) Copy(id, id_size, session);
    cJSON_Delete(root);
    return result;
}

extern "C" esp_err_t agent_link_platform_delete_conversation(int agent_id,
                                                                 const char* conversation_id) {
    if (agent_id <= 0 || !SafeId(conversation_id)) return ESP_ERR_INVALID_ARG;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/conversations/%s",
             agent_id, conversation_id);
    cJSON* root = nullptr;
    const esp_err_t result = Exchange("delete conversation", HTTP_METHOD_DELETE, path, nullptr,
                                      kSmallBytes, &root, nullptr, ApiAuth::kDeviceOrUser);
    cJSON_Delete(root);
    return result;
}

esp_err_t cloud_platform_grant(int agent_id, const char* session_id, bool refresh,
                               char* url, size_t url_size, char* token, size_t token_size,
                               agentlink::realtime::AudioFormat* audio, int* http_status) {
    if (agent_id <= 0 || !SafeId(session_id) || !url || !token || !audio) return ESP_ERR_INVALID_ARG;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/%s", agent_id,
             refresh ? "realtime-refresh" : "realtime-grant");
    char body[512];
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    snprintf(body, sizeof body, "{\"session_id\":\"%s\",\"audio_config\":%s}", session_id,
             agentlink::realtime::kOpusAudioConfig);
#else
    snprintf(body, sizeof body, "{\"session_id\":\"%s\"}", session_id);
#endif
    cJSON* root = nullptr;
    esp_err_t result = Exchange(refresh ? "refresh voice grant" : "request voice grant",
                                HTTP_METHOD_POST, path, body, kSmallBytes, &root, http_status,
                                ApiAuth::kDeviceOrUser);
    if (result != ESP_OK) return result;
    const cJSON* data = Data(root);
    const cJSON* grant = Field(data, "grant");
    if (cJSON_IsObject(grant)) data = grant;
    const char* ws = GrantString(data, "websocket_url", "websocketUrl", "ws_url");
    const char* access = GrantString(data, "connect_token", "connectToken", "token");
    if (!ws || !access || strlen(ws) >= url_size || strlen(access) >= token_size ||
        strncmp(ws, "wss://", 6) != 0) result = ESP_ERR_INVALID_RESPONSE;
    else {
        Copy(url, url_size, ws);
        Copy(token, token_size, access);
        if (!agentlink::realtime::ReadAudioFormat(Data(root), audio, true)) {
            result = ESP_ERR_NOT_SUPPORTED;
        }
#if !CONFIG_AGENT_LINK_REALTIME_OPUS
        if (audio->input_opus || audio->output_opus) result = ESP_ERR_NOT_SUPPORTED;
#else
        if (result == ESP_OK && (!audio->input_opus || !audio->output_opus))
            ESP_LOGD(TAG, "grant metadata reports PCM16; awaiting session.ready audio settings");
#endif
    }
    cJSON_Delete(root);
    return result;
}

void cloud_platform_end_grant(int agent_id, const char* session_id) {
    if (agent_id <= 0 || !SafeId(session_id)) return;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/realtime-grant/%s",
             agent_id, session_id);
    cJSON* root = nullptr;
    (void)Exchange("end voice grant", HTTP_METHOD_DELETE, path, nullptr, kSmallBytes, &root,
                   nullptr, ApiAuth::kDeviceOrUser);
    cJSON_Delete(root);
}
