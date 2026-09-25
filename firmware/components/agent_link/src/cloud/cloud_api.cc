// HTTP client for the platform's device-gateway endpoints. See cloud_api.h.

#include "cloud_api.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace {
constexpr const char* TAG = "agent_link.api";

constexpr int    kTimeoutMs  = 10000;
constexpr size_t kRxPsram    = 8192;   // sized for auth's agent config
constexpr size_t kRxFallback = 2048;

char   s_base[96] = {0};
char*  s_rx       = nullptr;
size_t s_rx_cap   = 0;

void SetErr(cloud_api_err_t* err, int code, bool success, const char* msg) {
    if (!err) return;
    err->code    = code;
    err->success = success;
    snprintf(err->message, sizeof err->message, "%s", msg ? msg : "");
}

// One connection is kept open across requests (HTTP keep-alive). A new connection costs DNS, TCP
// and a TLS handshake, several seconds on a slow link, and is where most transient failures occur.
//
// Reuse is decided at the end of each exchange: the connection is dropped unless the response was
// read in full and the server allows keep-alive. A server closing an idle connection shows up as a
// failed request on a reused connection, which is retried once on a new one. Every endpoint here
// is safe to repeat.
//
// While a connection is kept, the client's two 1KB buffers stay allocated in internal RAM.
esp_http_client_handle_t s_client   = nullptr;
bool                     s_reusable = false;   // the next request may reuse the connection

void DropConnection() {
    if (!s_client) return;
    esp_http_client_close(s_client);
    esp_http_client_cleanup(s_client);
    s_client   = nullptr;
    s_reusable = false;
}

esp_http_client_handle_t Client(const char* url) {
    if (s_client) return s_client;

    esp_http_client_config_t hc = {};
    hc.url               = url;
    hc.method            = HTTP_METHOD_POST;
    hc.timeout_ms        = kTimeoutMs;
    hc.crt_bundle_attach = esp_crt_bundle_attach;   // public CA bundle; no pinned cert to rotate
    hc.buffer_size       = 1024;
    hc.buffer_size_tx    = 1024;
    // TCP keep-alive, so a connection that dies while idle is detected within ~25s rather than by
    // a timeout on the next request.
    hc.keep_alive_enable   = true;
    hc.keep_alive_idle     = 10;
    hc.keep_alive_interval = 5;
    hc.keep_alive_count    = 3;

    s_client   = esp_http_client_init(&hc);
    s_reusable = false;
    return s_client;
}

// One request/response, on the kept connection or a new one. The body is left in s_rx, unparsed.
// On failure the connection is dropped, and *reused tells the caller whether it had been reused.
esp_err_t Exchange(const char* path, const char* url, const char* body,
                   int* status, int* got, bool* reused, cloud_api_err_t* err) {
    *reused = (s_client != nullptr && s_reusable);
    esp_http_client_handle_t cl = Client(url);
    if (!cl) { SetErr(err, -1, false, "http init failed"); return ESP_FAIL; }

    esp_http_client_set_url(cl, url);   // same host: the connection is kept
    esp_http_client_set_method(cl, HTTP_METHOD_POST);
    esp_http_client_set_header(cl, "Content-Type", "application/json");
    esp_http_client_set_header(cl, "Accept", "application/json");

    // A failure on a reused connection is expected (it went stale) and is retried by the caller, so
    // only failures on a new connection are logged.
    const int body_len = static_cast<int>(strlen(body));
    esp_err_t r = esp_http_client_open(cl, body_len);
    if (r != ESP_OK) {
        if (!*reused) {
            // The heap figures tell a TLS allocation failure from a network one; both surface as
            // ESP_ERR_HTTP_CONNECT.
            ESP_LOGW(TAG, "POST %s: connect failed (%s) — internal heap free=%uB largest=%uB",
                     path, esp_err_to_name(r),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        }
        SetErr(err, -1, false, esp_err_to_name(r));
        DropConnection();
        return r;
    }
    if (esp_http_client_write(cl, body, body_len) != body_len) {
        if (!*reused) ESP_LOGW(TAG, "POST %s: short write", path);
        SetErr(err, -1, false, "short write");
        DropConnection();
        return ESP_FAIL;
    }
    if (esp_http_client_fetch_headers(cl) < 0) {
        if (!*reused) ESP_LOGW(TAG, "POST %s: no headers", path);
        SetErr(err, -1, false, "no response headers");
        DropConnection();
        return ESP_FAIL;
    }

    *status = esp_http_client_get_status_code(cl);
    *got    = esp_http_client_read_response(cl, s_rx, static_cast<int>(s_rx_cap) - 1);
    if (*got < 0) {
        SetErr(err, -1, false, "read failed");
        DropConnection();
        return ESP_FAIL;
    }
    s_rx[*got] = '\0';

    s_reusable = esp_http_client_is_complete_data_received(cl) &&
                 esp_http_client_is_persistent_connection(cl);
    if (!s_reusable) DropConnection();
    return ESP_OK;
}

// POST a JSON body and parse the reply. On success returns the parsed root (the caller frees it)
// and sets *data to body.data, which may be any JSON type including null. The HTTP status is only
// logged: platform errors arrive as 200 too (note 1 in cloud_api.h).
esp_err_t PostJson(const char* path, const char* body, cJSON** root_out, cJSON** data_out,
                   cloud_api_err_t* err) {
    if (root_out) *root_out = nullptr;
    if (data_out) *data_out = nullptr;
    if (!s_rx) { SetErr(err, -1, false, "api not initialised"); return ESP_ERR_INVALID_STATE; }

    char url[160];
    snprintf(url, sizeof url, "%s%s", s_base, path);

    int  status = 0, got = 0;
    bool reused = false;
    esp_err_t r = Exchange(path, url, body, &status, &got, &reused, err);
    if (r != ESP_OK && reused) {
        // The kept connection went stale while idle; retry once on a new one.
        ESP_LOGD(TAG, "POST %s: kept connection was stale — reconnecting", path);
        r = Exchange(path, url, body, &status, &got, &reused, err);
    }
    if (r != ESP_OK) return r;

    cJSON* root = cJSON_Parse(s_rx);
    if (!root) {
        // Not JSON: something other than the platform answered (a proxy or a captive portal).
        ESP_LOGW(TAG, "POST %s: HTTP %d, body is not JSON (%d bytes)", path, status, got);
        SetErr(err, -1, false, "non-JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON* jcode = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON* jok   = cJSON_GetObjectItemCaseSensitive(root, "success");
    const cJSON* jmsg  = cJSON_GetObjectItemCaseSensitive(root, "message");
    cJSON*       jdata = cJSON_GetObjectItemCaseSensitive(root, "data");

    const int  code = cJSON_IsNumber(jcode) ? jcode->valueint : -1;
    const bool ok   = cJSON_IsBool(jok) ? cJSON_IsTrue(jok) : (code == CLOUD_API_OK);
    SetErr(err, code, ok, cJSON_IsString(jmsg) ? jmsg->valuestring : "");

    if (!ok || code != CLOUD_API_OK) {
        ESP_LOGW(TAG, "POST %s -> code=%d %s", path, code,
                 cJSON_IsString(jmsg) ? jmsg->valuestring : "");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (root_out) *root_out = root; else cJSON_Delete(root);
    if (data_out) *data_out = jdata;
    return ESP_OK;
}

// data is {} on a business error and null on a validation error, so never assume an object.
const cJSON* Obj(cJSON* data, const char* key) {
    if (!cJSON_IsObject(data)) return nullptr;
    return cJSON_GetObjectItemCaseSensitive(data, key);
}

uint32_t U32(cJSON* data, const char* key, uint32_t fallback) {
    const cJSON* j = Obj(data, key);
    return cJSON_IsNumber(j) && j->valuedouble >= 0 ? static_cast<uint32_t>(j->valuedouble) : fallback;
}

bool Str(cJSON* data, const char* key, char* out, size_t cap) {
    const cJSON* j = Obj(data, key);
    if (!cJSON_IsString(j) || !j->valuestring) return false;
    snprintf(out, cap, "%s", j->valuestring);
    return true;
}
}  // namespace

esp_err_t cloud_api_init(const char* base_url) {
    if (!base_url || !base_url[0]) return ESP_ERR_INVALID_ARG;
    snprintf(s_base, sizeof s_base, "%s", base_url);

    if (!s_rx) {
        s_rx = static_cast<char*>(heap_caps_malloc(kRxPsram, MALLOC_CAP_SPIRAM));
        s_rx_cap = s_rx ? kRxPsram : 0;
        if (!s_rx) {
            // No PSRAM: fall back to a small internal buffer. A large agent config gets truncated.
            s_rx = static_cast<char*>(malloc(kRxFallback));
            s_rx_cap = s_rx ? kRxFallback : 0;
            if (!s_rx) return ESP_ERR_NO_MEM;
            ESP_LOGW(TAG, "no PSRAM — response buffer is %uB", static_cast<unsigned>(kRxFallback));
        }
    }
    ESP_LOGI(TAG, "platform = %s", s_base);
    return ESP_OK;
}

void cloud_api_drop_connection(void) { DropConnection(); }

void cloud_api_deinit(void) {
    DropConnection();
    free(s_rx);
    s_rx = nullptr;
    s_rx_cap = 0;
}

esp_err_t cloud_api_claim_code(int product_id, const char* sn, const char* mac,
                               const char* chip_type, const char* fw_version,
                               cloud_claim_t* out, cloud_api_err_t* err) {
    if (!out || !sn || !mac) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    char body[320];
    snprintf(body, sizeof body,
             "{\"product_id\":%d,\"device_sn\":\"%s\",\"mac_address\":\"%s\","
             "\"chip_type\":\"%s\",\"firmware_version\":\"%s\"}",
             product_id, sn, mac, chip_type ? chip_type : "", fw_version ? fw_version : "");

    cJSON* root = nullptr;
    cJSON* data = nullptr;
    esp_err_t r = PostJson("/api/device-gateway/claim-code", body, &root, &data, err);
    if (r != ESP_OK) return r;

    if (!Str(data, "code", out->code, sizeof out->code)) {
        ESP_LOGE(TAG, "claim-code: no code in a successful response");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    out->expires_in = U32(data, "expires_in", 600);
    ESP_LOGI(TAG, "activation code %s, valid %us", out->code, static_cast<unsigned>(out->expires_in));
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t cloud_api_bind_status(const char* sn, const char* code,
                                cloud_bind_t* out, cloud_api_err_t* err) {
    if (!out || !sn || !code) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    char body[160];
    snprintf(body, sizeof body, "{\"device_sn\":\"%s\",\"code\":\"%s\"}", sn, code);

    cJSON* root = nullptr;
    cJSON* data = nullptr;
    esp_err_t r = PostJson("/api/device-gateway/bind-status", body, &root, &data, err);
    if (r != ESP_OK) return r;

    // A pending or expired code is reported in data.status; a claimed one returns data.auth_key.
    char status[24] = {0};
    (void)Str(data, "status", status, sizeof status);
    const bool have_key = Str(data, "auth_key", out->auth_key, sizeof out->auth_key);

    if (have_key) {
        out->state = CLOUD_BIND_BOUND;
    } else if (strcmp(status, "expired") == 0) {
        out->state = CLOUD_BIND_EXPIRED;
    } else {
        out->state         = CLOUD_BIND_PENDING;
        out->remaining_ttl = U32(data, "remaining_ttl", 0);
        if (status[0] && strcmp(status, "pending") != 0) {
            // Unknown status: keep polling, and log it so a new platform state gets noticed.
            ESP_LOGW(TAG, "bind-status: unknown status \"%s\", treating as pending", status);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t cloud_api_auth(const char* auth_key, const char* mac, const char* fw_version,
                         char* agent_json, size_t cap, cloud_api_err_t* err) {
    if (!auth_key || !mac) return ESP_ERR_INVALID_ARG;
    if (agent_json && cap) agent_json[0] = '\0';

    char body[256];
    snprintf(body, sizeof body,
             "{\"auth_key\":\"%s\",\"mac_address\":\"%s\",\"firmware_version\":\"%s\"}",
             auth_key, mac, fw_version ? fw_version : "");

    cJSON* root = nullptr;
    cJSON* data = nullptr;
    esp_err_t r = PostJson("/api/device-gateway/auth", body, &root, &data, err);
    if (r != ESP_OK) return r;

    // Returned unparsed; see cloud_api_auth().
    if (agent_json && cap && cJSON_IsObject(data)) {
        char* text = cJSON_PrintUnformatted(data);
        if (text) {
            if (strlen(text) >= cap) {
                ESP_LOGW(TAG, "auth: agent config is %uB, buffer is %uB — truncated",
                         static_cast<unsigned>(strlen(text)), static_cast<unsigned>(cap));
            }
            snprintf(agent_json, cap, "%s", text);
            cJSON_free(text);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t cloud_api_heartbeat(const char* auth_key, cloud_api_err_t* err) {
    if (!auth_key) return ESP_ERR_INVALID_ARG;

    char body[128];
    snprintf(body, sizeof body, "{\"auth_key\":\"%s\"}", auth_key);

    cJSON* root = nullptr;
    esp_err_t r = PostJson("/api/device-gateway/heartbeat", body, &root, nullptr, err);
    if (root) cJSON_Delete(root);
    return r;
}
