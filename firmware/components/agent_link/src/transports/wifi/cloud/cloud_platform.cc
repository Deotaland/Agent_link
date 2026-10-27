#include "agent_link_platform.h"
#include "cloud_platform.h"
#include "cloud_limits.h"
#include "cloud_log.h"
#include "cloud_memory.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "cJSON.h"
#include "cloud_credential.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace {
constexpr const char* TAG = "agent_link.platform";
using agentlink::cloud_limits::kListBytes;
using agentlink::cloud_limits::kListPageSize;
constexpr size_t kSmallBytes = 8 * 1024;
// Opaque platform cursors already exceed 300 bytes. Bound their heap allocation
// without putting large cursor, escaped-path and URL arrays on the C3 task stack.
constexpr size_t kMaxCursorLength = 1024;
struct FreeBuffer { void operator()(char* buffer) const { free(buffer); } };
using Buffer = std::unique_ptr<char, FreeBuffer>;
char s_base[160] = {};
std::mutex s_agent_mutex;
agent_link_platform_agent_t s_bound_agent = {};
char s_bound_key[CLOUD_AUTH_KEY_MAX] = {};

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
                   size_t capacity, cJSON** root_out, int* status_out) {
    agentlink::cloud::NetworkMemoryLock memory;
    if (root_out) *root_out = nullptr;
    if (status_out) *status_out = 0;
    const char* device_key = cloud_cred_auth_key();
    if (!s_base[0] || !device_key[0] || !path || !root_out)
        return ESP_ERR_INVALID_STATE;

    const size_t url_size = strlen(s_base) + strlen(path) + 1;
    Buffer url(static_cast<char*>(malloc(url_size)));
    if (!url) return ESP_ERR_NO_MEM;
    snprintf(url.get(), url_size, "%s%s", s_base, path);
    auto* buffer = static_cast<char*>(heap_caps_calloc(capacity, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) buffer = static_cast<char*>(calloc(capacity, 1));
    if (!buffer) return ESP_ERR_NO_MEM;
    Response response{buffer, capacity, 0, false};
    esp_http_client_config_t config = {};
    config.url = url.get();
    config.method = method;
    config.timeout_ms = 12000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEvent;
    config.user_data = &response;
    config.buffer_size = 1024;
    // ESP-IDF requires the full request line and at least one header to fit.
    // Keep the usual 1 KB allocation for short URLs; grow only for long cursors.
    const size_t tx_size = strlen(path) + 256;
    config.buffer_size_tx = static_cast<int>(tx_size > 1024 ? tx_size : 1024);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(buffer); return ESP_ERR_NO_MEM; }

    esp_err_t result = esp_http_client_set_header(client, "X-Device-Auth-Key", device_key);
    if (result == ESP_OK && body) {
        result = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (result == ESP_OK) result = esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
    }
    if (result == ESP_OK) result = esp_http_client_perform(client);
    const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    if (status_out) *status_out = status;
    esp_http_client_cleanup(client);
    if (result == ESP_OK && response.overflow) result = ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK && response.size) {
        cJSON* root = cJSON_ParseWithLength(buffer, response.size);
        if (!root) result = ESP_ERR_INVALID_RESPONSE;
        else {
            const cJSON* success = Field(root, "success");
            const cJSON* code = Field(root, "code");
            if ((cJSON_IsBool(success) && !cJSON_IsTrue(success)) ||
                (cJSON_IsNumber(code) && code->valueint != 0) || status < 200 || status >= 300) {
                char message[192];
                cloud_log_safe_message(message, sizeof message, String(root, "message"), device_key);
                ESP_LOGW(TAG, "%s: HTTP %d, code=%d, message=%s", operation, status,
                         cJSON_IsNumber(code) ? code->valueint : -1, message);
                result = ESP_ERR_INVALID_RESPONSE;
                cJSON_Delete(root);
            } else *root_out = root;
        }
    } else if (result == ESP_OK && method != HTTP_METHOD_DELETE) result = ESP_ERR_INVALID_RESPONSE;
    if (result == ESP_OK && (status < 200 || status >= 300)) result = ESP_ERR_INVALID_RESPONSE;
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

bool cloud_platform_enabled(void) { return s_base[0] && cloud_cred_is_bound(); }

extern "C" bool agent_link_platform_available(void) { return cloud_platform_enabled(); }

void cloud_platform_set_bound_agent(const cJSON* auth_data) {
    agent_link_platform_agent_t agent = {};
    const cJSON* object = Field(auth_data, "agent");
    const cJSON* id = Field(object, "id");
    if (cJSON_IsNumber(id) && id->valueint > 0 && id->valuedouble == id->valueint) {
        agent.id = id->valueint;
        Copy(agent.name, sizeof agent.name, String(object, "name"));
        agent.voice_supported = cJSON_IsObject(Field(Field(object, "capabilities"), "realtime_voice"));
    }
    std::lock_guard<std::mutex> lock(s_agent_mutex);
    s_bound_agent = agent;
    Copy(s_bound_key, sizeof s_bound_key, cloud_cred_auth_key());
}

extern "C" esp_err_t agent_link_platform_bound_agent(agent_link_platform_agent_t* agent) {
    if (!agent) return ESP_ERR_INVALID_ARG;
    *agent = {};
    std::lock_guard<std::mutex> lock(s_agent_mutex);
    const char* key = cloud_cred_auth_key();
    if (!s_base[0] || !key[0] || strcmp(s_bound_key, key) != 0) return ESP_ERR_INVALID_STATE;
    if (!s_bound_agent.id) return ESP_ERR_NOT_FOUND;
    *agent = s_bound_agent;
    return ESP_OK;
}

extern "C" esp_err_t agent_link_platform_agents(int, agent_link_platform_agent_t*, size_t,
                                               size_t* count, int* total) {
    if (count) *count = 0;
    if (total) *total = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t agent_link_platform_bind_agent(int) { return ESP_ERR_NOT_SUPPORTED; }

namespace {
bool BoundAgent(int id) {
    agent_link_platform_agent_t agent;
    return agent_link_platform_bound_agent(&agent) == ESP_OK && agent.id == id;
}
bool EncodeCursor(const char* value, char* out, size_t capacity) {
    constexpr char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        const bool plain = std::isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (used + (plain ? 1u : 3u) >= capacity) return false;
        if (plain) out[used++] = static_cast<char>(*p);
        else { out[used++] = '%'; out[used++] = hex[*p >> 4]; out[used++] = hex[*p & 15]; }
    }
    out[used] = '\0';
    return true;
}
}  // namespace

extern "C" esp_err_t agent_link_platform_conversations(int agent_id, size_t offset,
                                                           agent_link_platform_conversation_t* entries,
                                                           size_t capacity, size_t* count, size_t* total) {
    if (agent_id <= 0 || !entries || !count || !total || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0; *total = 0;
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    Buffer cursor;
    size_t skipped = 0;
    for (unsigned page = 0; page < 128; ++page) {
        const char* current = cursor ? cursor.get() : "";
        const size_t path_size = 128 + 3 * strlen(current);
        Buffer path(static_cast<char*>(malloc(path_size)));
        if (!path) return ESP_ERR_NO_MEM;
        const int length = snprintf(path.get(), path_size,
            "/api/agents/%d/actions/realtime_voice/conversations?limit=%u%s",
            agent_id, kListPageSize, current[0] ? "&cursor=" : "");
        if (length < 0 || static_cast<size_t>(length) >= path_size ||
            !EncodeCursor(current, path.get() + length, path_size - length)) return ESP_ERR_INVALID_SIZE;
        cJSON* root = nullptr;
        esp_err_t result = Exchange("list conversations", HTTP_METHOD_GET, path.get(), nullptr,
                                    kListBytes, &root, nullptr);
        path.reset();
        if (result != ESP_OK) return result;
        const cJSON* data = Data(root);
        const cJSON* rows = Items(data);
        if (!cJSON_IsArray(rows) || !BoundAgent(agent_id)) {
            cJSON_Delete(root);
            *count = 0;
            return ESP_ERR_INVALID_RESPONSE;
        }
        const int n = cJSON_GetArraySize(rows);
        for (int i = 0; i < n; ++i) {
            const cJSON* row = cJSON_GetArrayItem(rows, i);
            const char* id = String(row, "conversation_id");
            if (!id) id = String(row, "session_id");
            if (!SafeId(id)) continue;
            if (skipped++ < offset) continue;
            if (*count >= capacity) continue;
            auto& entry = entries[(*count)++];
            Copy(entry.id, sizeof entry.id, id);
            Copy(entry.name, sizeof entry.name, String(row, "name"));
            Copy(entry.created_at, sizeof entry.created_at, String(row, "created_at"));
        }
        const bool more = cJSON_IsTrue(Field(data, "has_more"));
        const char* next = String(data, "next_cursor");
        if (!more || *count >= capacity) {
            *total = skipped + (more ? 1u : 0u);
            ESP_LOGI(TAG, "list conversations: returned=%zu offset=%zu more=%u",
                     *count, offset, unsigned(more));
            cJSON_Delete(root);
            return ESP_OK;
        }
        const size_t next_length = next ? strlen(next) : 0;
        // Filtering by Agent may remove every row from an upstream page. A valid
        // advancing cursor still has to be followed, including for a new Agent.
        if (!next_length || next_length > kMaxCursorLength || strcmp(next, current) == 0) {
            ESP_LOGW(TAG, "list conversations: invalid pagination (rows=%d, cursor length=%zu, limit=%zu)",
                     n, next_length, kMaxCursorLength);
            cJSON_Delete(root);
            *count = 0;
            return ESP_ERR_INVALID_RESPONSE;
        }
        Buffer next_cursor(static_cast<char*>(malloc(next_length + 1)));
        if (!next_cursor) {
            cJSON_Delete(root);
            *count = 0;
            return ESP_ERR_NO_MEM;
        }
        memcpy(next_cursor.get(), next, next_length + 1);
        cursor = std::move(next_cursor);
        cJSON_Delete(root);
    }
    *count = 0;
    return ESP_ERR_INVALID_SIZE;
}

extern "C" esp_err_t agent_link_platform_create_conversation(int agent_id, char* id, size_t id_size) {
    if (agent_id <= 0 || !id || id_size < 40) return ESP_ERR_INVALID_ARG;
    id[0] = '\0';
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    char path[160];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/conversations", agent_id);
    cJSON* root = nullptr;
    esp_err_t result = Exchange("create conversation", HTTP_METHOD_POST, path, nullptr, kSmallBytes,
                                &root, nullptr);
    if (result != ESP_OK) return result;
    if (!BoundAgent(agent_id)) { cJSON_Delete(root); return ESP_ERR_INVALID_STATE; }
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
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/conversations/%s",
             agent_id, conversation_id);
    cJSON* root = nullptr;
    const esp_err_t result = Exchange("delete conversation", HTTP_METHOD_DELETE, path, nullptr,
                                      kSmallBytes, &root, nullptr);
    cJSON_Delete(root);
    return result;
}

esp_err_t cloud_platform_grant(int agent_id, const char* session_id, bool refresh,
                               char* url, size_t url_size, char* token, size_t token_size,
                               agentlink::realtime::AudioFormat* audio, int* http_status) {
    if (http_status) *http_status = 0;
    if (agent_id <= 0 || !SafeId(session_id) || !url || !token || !audio) return ESP_ERR_INVALID_ARG;
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/%s", agent_id,
             refresh ? "realtime-refresh" : "realtime-grant");
    char body[512];
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    snprintf(body, sizeof body, "{\"session_id\":\"%s\",\"audio_config\":%s}", session_id,
             agentlink::realtime::kOpusAudioConfig);
#else
    snprintf(body, sizeof body, "{\"session_id\":\"%s\",\"audio_config\":%s}", session_id,
             agentlink::realtime::kPcmAudioConfig);
#endif
    cJSON* root = nullptr;
    esp_err_t result = Exchange(refresh ? "refresh voice grant" : "request voice grant",
                                HTTP_METHOD_POST, path, body, kSmallBytes, &root, http_status);
    if (result != ESP_OK) return result;
    if (!BoundAgent(agent_id)) { cJSON_Delete(root); return ESP_ERR_INVALID_STATE; }
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
                   nullptr);
    cJSON_Delete(root);
}
