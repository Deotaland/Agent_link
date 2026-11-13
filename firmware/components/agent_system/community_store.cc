#include "community_store.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace agent_system::community {
namespace {
constexpr const char* kOrigin = AGENT_COMMUNITY_ORIGIN;
constexpr const char* kList = "/api/v1/community/plays";
constexpr const char* kCodes = "/api/v1/community/download-codes/";
constexpr int kConnectTimeoutMs = 15000, kResponseTimeoutMs = 60000, kReadTimeoutMs = 15000;
constexpr unsigned kRequestAttempts = 3;
std::mutex error_mutex;
StoreError last_error;
bool TimedOut(int64_t result, int network_errno) {
    return result == -ESP_ERR_HTTP_EAGAIN || network_errno == ETIMEDOUT ||
           network_errno == EAGAIN || network_errno == EWOULDBLOCK;
}
bool Retryable(esp_err_t error, int status, unsigned retry_after) {
    if (retry_after > 5)
        return false;
    return error == ESP_ERR_TIMEOUT || error == ESP_ERR_HTTP_CONNECT ||
           (error == kStoreError && (status == 502 || status == 503 || status == 504));
}
bool WaitRetry(unsigned attempt, unsigned retry_after, InstallCancel cancel, void* context) {
    const unsigned wait_ms = std::max(attempt, retry_after) * 1000;
    for (unsigned elapsed = 0; elapsed < wait_ms; elapsed += 100) {
        if (cancel && cancel(context))
            return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !(cancel && cancel(context));
}
bool Equal(const char* a, const char* b) {
    while (*a && *b) {
        const char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        const char y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}
void Text(char* out, size_t size, const char* value) {
    if (!size)
        return;
    if (!value)
        value = "";
    size_t n = std::min(size - 1, strlen(value));
    while (n && (static_cast<unsigned char>(value[n]) & 0xC0) == 0x80)
        --n;
    memcpy(out, value, n);
    out[n] = 0;
}
const char* String(cJSON* object, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
bool Id(const cJSON* value, uint64_t* out, bool zero = false) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < (zero ? 0 : 1) || value->valuedouble > 9007199254740991.0 ||
        std::floor(value->valuedouble) != value->valuedouble)
        return false;
    *out = static_cast<uint64_t>(value->valuedouble);
    return true;
}
bool Id(cJSON* object, const char* key, uint64_t* out, bool zero = false) {
    return Id(cJSON_GetObjectItemCaseSensitive(object, key), out, zero);
}
esp_err_t Failure(const char* message, int status, cJSON* root, unsigned retry = 0) {
    StoreError value{};
    value.http_status = status;
    value.retry_after = retry;
    Text(value.message, sizeof value.message, message);
    if (root) {
        const auto* code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsString(code))
            Text(value.code, sizeof value.code, code->valuestring);
        else if (cJSON_IsNumber(code))
            snprintf(value.code, sizeof value.code, "%.0f", code->valuedouble);
        Text(value.request_id, sizeof value.request_id, String(root, "requestId"));
        const auto* delay = cJSON_GetObjectItemCaseSensitive(root, "retryAfterSeconds");
        if (cJSON_IsNumber(delay) && delay->valuedouble >= 0 && delay->valuedouble <= 86400)
            value.retry_after = static_cast<unsigned>(delay->valuedouble);
    }
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error = value;
    }
    ESP_LOGW("agent_system.store", "HTTP=%d requestId=%s", status, value.request_id);
    return kStoreError;
}
struct JsonHeaders {
    unsigned retry = 0;
    int status = 0, network_errno = 0;
    const char* phase = "connect";
};
esp_err_t JsonEvent(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        Equal(event->header_key, "Retry-After")) {
        auto* out = static_cast<JsonHeaders*>(event->user_data);
        unsigned value = 0;
        char extra;
        if (sscanf(event->header_value, "%u%c", &value, &extra) == 1 && value <= 86400)
            out->retry = value;
    }
    return ESP_OK;
}
esp_err_t JsonOnce(const std::string& url, cJSON** out, const char* key, JsonHeaders& headers,
                   InstallCancel cancel, void* context) {
    *out = nullptr;
    const bool post = key && key[0];
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.timeout_ms = kConnectTimeoutMs;
    config.buffer_size = 2048;
    config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = JsonEvent;
    config.user_data = &headers;
    auto handle = esp_http_client_init(&config);
    if (!handle)
        return ESP_ERR_NO_MEM;
    struct Close {
        esp_http_client_handle_t handle;
        ~Close() {
            esp_http_client_close(handle);
            esp_http_client_cleanup(handle);
        }
    } close{handle};
    (void)esp_http_client_set_header(handle, "Accept", "application/json");
    if (post) {
        (void)esp_http_client_set_method(handle, HTTP_METHOD_POST);
        (void)esp_http_client_set_header(handle, "Idempotency-Key", key);
    }
    const auto open_at = esp_timer_get_time();
    auto error = esp_http_client_open(handle, 0);
    const auto headers_at = esp_timer_get_time();
    ESP_LOGI("agent_system.store", "metadata connect_ms=%lld result=0x%x",
             static_cast<long long>((headers_at - open_at) / 1000), unsigned(error));
    if (error != ESP_OK) {
        headers.network_errno = esp_http_client_get_errno(handle);
        return TimedOut(0, headers.network_errno) ? ESP_ERR_TIMEOUT : error;
    }
    if (cancel && cancel(context))
        return kCancelled;
    headers.phase = "headers";
    error = esp_http_client_set_timeout_ms(handle, kResponseTimeoutMs);
    if (error != ESP_OK)
        return error;
    const auto length = esp_http_client_fetch_headers(handle);
    headers.network_errno = esp_http_client_get_errno(handle);
    if (length < 0)
        return TimedOut(length, headers.network_errno) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    const int status = esp_http_client_get_status_code(handle);
    headers.status = status;
    ESP_LOGI("agent_system.store", "metadata method=%s HTTP=%d headers_ms=%lld",
             post ? "POST" : "GET", status,
             static_cast<long long>((esp_timer_get_time() - headers_at) / 1000));
    error = esp_http_client_set_timeout_ms(handle, kReadTimeoutMs);
    if (error != ESP_OK)
        return error;
    headers.phase = "body";
    std::string body;
    char block[1024];
    for (;;) {
        if (cancel && cancel(context))
            return kCancelled;
        const int bytes = esp_http_client_read(handle, block, sizeof block);
        if (bytes < 0) {
            headers.network_errno = esp_http_client_get_errno(handle);
            return TimedOut(bytes, headers.network_errno) ? ESP_ERR_TIMEOUT
                                                         : ESP_ERR_INVALID_RESPONSE;
        }
        if (!bytes)
            break;
        if (body.size() + bytes > 65536)
            return Failure("Community response exceeds the device limit", status, nullptr);
        body.append(block, bytes);
    }
    ESP_LOGI("agent_system.store", "metadata body_bytes=%u body_ms=%lld", unsigned(body.size()),
             static_cast<long long>((esp_timer_get_time() - headers_at) / 1000));
    if (!esp_http_client_is_complete_data_received(handle)) {
        headers.network_errno = esp_http_client_get_errno(handle);
        return TimedOut(0, headers.network_errno) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    }
    headers.phase = "parse";
    auto* root = cJSON_ParseWithLength(body.c_str(), body.size() + 1);
    const auto* code = root ? cJSON_GetObjectItemCaseSensitive(root, "code") : nullptr;
    if (status != 200 || !cJSON_IsNumber(code) || code->valuedouble != 0) {
        const auto* delay = root ? cJSON_GetObjectItemCaseSensitive(root, "retryAfterSeconds") : nullptr;
        if (cJSON_IsNumber(delay) && delay->valuedouble >= 0 && delay->valuedouble <= 86400)
            headers.retry = static_cast<unsigned>(delay->valuedouble);
        const char* message = root ? String(root, "message") : "";
        error = Failure(message[0] ? message : "Invalid community response", status, root,
                        headers.retry);
        cJSON_Delete(root);
        return error;
    }
    auto* data = cJSON_DetachItemFromObjectCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return Fail("Community response has no data object", status);
    }
    cJSON_Delete(root);
    *out = data;
    headers.phase = "done";
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error = {};
    }
    return ESP_OK;
}
esp_err_t Json(const std::string& url, cJSON** out, bool post = false,
               InstallCancel cancel = nullptr, void* context = nullptr) {
    *out = nullptr;
    if (!IsOrigin(url.c_str()) || !ValidServiceUrl(url.c_str()))
        return Fail("Invalid community endpoint");
    // One logical POST owns one key. A timeout can happen after the server has
    // already issued a ticket; every retry must identify that same operation.
    char key[48]{};
    if (post) {
        uint8_t random[16];
        esp_fill_random(random, sizeof random);
        memcpy(key, "agent-link-", 11);
        for (unsigned i = 0; i < 16; ++i)
            snprintf(key + 11 + i * 2, 3, "%02x", random[i]);
    }
    for (unsigned attempt = 1; attempt <= kRequestAttempts; ++attempt) {
        if (cancel && cancel(context))
            return kCancelled;
        JsonHeaders headers;
        const auto started = esp_timer_get_time();
        const auto error = JsonOnce(url, out, key, headers, cancel, context);
        const auto elapsed_ms = (esp_timer_get_time() - started) / 1000;
        if (error == ESP_OK) {
            ESP_LOGI("agent_system.store", "metadata method=%s attempt=%u/%u HTTP=%d ms=%lld",
                     post ? "POST" : "GET", attempt, kRequestAttempts, headers.status,
                     static_cast<long long>(elapsed_ms));
            return ESP_OK;
        }
        ESP_LOGW("agent_system.store", "metadata method=%s attempt=%u/%u phase=%s result=0x%x "
                 "HTTP=%d errno=%d ms=%lld", post ? "POST" : "GET", attempt, kRequestAttempts,
                 headers.phase, unsigned(error), headers.status, headers.network_errno,
                 static_cast<long long>(elapsed_ms));
        if (attempt == kRequestAttempts || !Retryable(error, headers.status, headers.retry))
            return error;
        ESP_LOGW("agent_system.store", "metadata retry=%u/%u in_ms=%u method=%s", attempt,
                 kRequestAttempts - 1, std::max(attempt, headers.retry) * 1000, post ? "POST" : "GET");
        if (!WaitRetry(attempt, headers.retry, cancel, context))
            return kCancelled;
    }
    return ESP_ERR_TIMEOUT;
}
std::string Resolve(const std::string& base, const char* address) {
    if (!address || !address[0])
        return {};
    if (address[0] == '/' && address[1] != '/') {
        const auto scheme = base.find("://");
        if (scheme == std::string::npos)
            return {};
        const auto end = base.find('/', scheme + 3);
        return base.substr(0, end) + address;
    }
    return address;
}
bool CopyUrl(char* out, size_t capacity, const char* value) {
    auto url = Resolve(kOrigin, value);
    // The service currently emits http file URLs behind its HTTPS reverse proxy.
    // Upgrade only the exact configured host; keep all other URL checks intact.
    if (!strncmp(kOrigin, "https://", 8)) {
        const auto insecure = std::string("http://") + (kOrigin + 8);
        if (url.compare(0, insecure.size(), insecure) == 0 &&
            (url.size() == insecure.size() || url[insecure.size()] == '/' ||
             url[insecure.size()] == '?'))
            url = std::string(kOrigin) + url.substr(insecure.size());
    }
    if (url.empty() || url.size() >= capacity || !ValidServiceUrl(url.c_str()))
        return false;
    memcpy(out, url.c_str(), url.size() + 1);
    return true;
}
bool Hash(cJSON* object, const char* field, char out[65]) {
    const char* value = String(object, field);
    if (strlen(value) != 64)
        return false;
    for (unsigned i = 0; i < 64; ++i) {
        const char c = value[i] >= 'A' && value[i] <= 'F' ? value[i] + 32 : value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        out[i] = c;
    }
    out[64] = 0;
    return true;
}
bool ApplicationFile(const char* name, const char* mime) {
    const size_t size = strlen(name);
    return size > 4 && Equal(name + size - 4, ".bin") &&
           (Equal(mime, "application/octet-stream") || Equal(mime, "application/x-binary"));
}
struct Binding {
    uint32_t magic = 0x314D4341, schema = 1;
    uint64_t play = 0, version = 0;
    char app_id[32]{}, binary_version[32]{}, release_version[32]{};
};
void BindingKey(const char* slot, char key[16]) {
    snprintf(key, 16, "c_%.7s", slot);
}
}  // namespace
const char* DefaultCatalog() {
    static const std::string value =
        std::string(kOrigin) + kList + "?productId=" + std::to_string(AGENT_COMMUNITY_PRODUCT) +
        "&hardwareRevisionId=" + std::to_string(AGENT_COMMUNITY_REVISION) +
        "&sort=featured&page=1&pageSize=8";
    return value.c_str();
}
bool IsOrigin(const char* url) {
    const size_t n = strlen(kOrigin);
    return url && !strncmp(url, kOrigin, n) && (!url[n] || url[n] == '/' || url[n] == '?');
}
bool IsCatalog(const char* url) {
    if (!IsOrigin(url))
        return false;
    const char* path = url + strlen(kOrigin);
    const size_t n = strlen(kList);
    return !strncmp(path, kList, n) && (!path[n] || path[n] == '?');
}
void Error(StoreError* out) {
    if (out) {
        std::lock_guard<std::mutex> lock(error_mutex);
        *out = last_error;
    }
}
esp_err_t Fail(const char* message, int status) {
    return Failure(message, status, nullptr);
}
static bool ValidCode(const char* code) {
    if (!code || strlen(code) != 6)
        return false;
    for (unsigned i = 0; i < 6; ++i)
        if (code[i] < '0' || code[i] > '9')
            return false;
    return true;
}
static esp_err_t ParseDetail(cJSON* data, uint64_t play, uint64_t version, CatalogApp* app) {
    auto* published = cJSON_GetObjectItemCaseSensitive(data, "publishedVersion");
    auto* product = cJSON_GetObjectItemCaseSensitive(published, "product");
    auto* revision = cJSON_GetObjectItemCaseSensitive(published, "hardwareRevision");
    auto* firmware = cJSON_GetObjectItemCaseSensitive(published, "firmware");
    uint64_t actual = 0, product_id = 0, revision_id = 0, size = 0, actual_play = 0;
    const char* release = String(published, "version");
    const char* filename = String(firmware, "filename");
    if (!Id(data, "id", &actual_play) || (play && actual_play != play) || !Id(published, "id", &actual) ||
        (version && actual != version) || !Id(product, "id", &product_id) ||
        product_id != AGENT_COMMUNITY_PRODUCT || !Id(revision, "id", &revision_id) ||
        revision_id != AGENT_COMMUNITY_REVISION || !Id(firmware, "sizeBytes", &size) ||
        size > UINT32_MAX || !release[0] || strlen(release) >= sizeof app->version ||
        !filename[0] || !String(data, "title")[0])
        return Fail("Published version or hardware no longer matches. Refresh the list.");
    *app = {};
    app->community_play = actual_play;
    app->community_version = actual;
    snprintf(app->app_id, sizeof app->app_id, "community.%llu",
             static_cast<unsigned long long>(actual_play));
    Text(app->name, sizeof app->name, String(data, "title"));
    Text(app->version, sizeof app->version, release);
    Text(app->release_version, sizeof app->release_version, release);
    Text(app->filename, sizeof app->filename, filename);
    app->size = static_cast<uint32_t>(size);
    if (!Hash(firmware, "sha256", app->sha256))
        return Fail("Published firmware has no valid SHA-256");
    if (!ApplicationFile(filename, String(firmware, "mimeType")))
        app->package = CatalogApp::Package::kFullPackage;
    else if (size > kAppImageMaxBytes)
        app->package = CatalogApp::Package::kTooLarge;
    const char* cover = String(data, "coverUrl");
    if (cover[0]) {
        if (!CopyUrl(app->cover_url, sizeof app->cover_url, cover))
            return Fail("Invalid cover URL");
        Text(app->cover_format, sizeof app->cover_format, "community-image");
    }
    return ESP_OK;
}
esp_err_t Detail(uint64_t play, uint64_t version, CatalogApp* app, InstallCancel cancel, void* context) {
    if (!play || !app)
        return ESP_ERR_INVALID_ARG;
    cJSON* data = nullptr;
    auto error = Json(std::string(kOrigin) + kList + "/" + std::to_string(play), &data,
                      false, cancel, context);
    if (error == ESP_OK)
        error = ParseDetail(data, play, version, app);
    cJSON_Delete(data);
    return error;
}
esp_err_t LookupCode(const char* code, CatalogApp* app, InstallCancel cancel, void* context) {
    if (!ValidCode(code) || !app)
        return ESP_ERR_INVALID_ARG;
    *app = {};
    cJSON* data = nullptr;
    auto error = Json(std::string(kOrigin) + kCodes + code, &data, false, cancel, context);
    CatalogApp fresh;
    if (error == ESP_OK) {
        if (strcmp(String(data, "downloadCode"), code))
            error = Fail("Download code does not match the requested play");
        else
            error = ParseDetail(data, 0, 0, &fresh);
    }
    cJSON_Delete(data);
    if (error == ESP_OK) {
        Text(fresh.download_code, sizeof fresh.download_code, code);
        *app = fresh;
    }
    return error;
}
esp_err_t Fetch(const char* url, unsigned page, CatalogApp* apps, size_t capacity, size_t* count,
                CatalogPage* pagination) {
    if (!IsCatalog(url) || !page || !apps || !count || !capacity)
        return ESP_ERR_INVALID_ARG;
    *count = 0;
    const unsigned limit = static_cast<unsigned>(std::min(capacity, kMaxApps));
    std::string query;
    const char* first = strchr(url, '?');
    if (first) {
        const std::string original(first + 1);
        size_t start = 0;
        while (start < original.size()) {
            const size_t end = original.find('&', start);
            const auto part = original.substr(start, end - start);
            const auto key = part.substr(0, part.find('='));
            if ((key == "q" || key == "categoryId" || key == "sort") && !part.empty())
                query += "&" + part;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    const std::string address =
        std::string(kOrigin) + kList + "?productId=" + std::to_string(AGENT_COMMUNITY_PRODUCT) +
        "&hardwareRevisionId=" + std::to_string(AGENT_COMMUNITY_REVISION) +
        "&page=" + std::to_string(page) + "&pageSize=" + std::to_string(limit) + query;
    cJSON* data = nullptr;
    auto error = Json(address, &data);
    if (error != ESP_OK)
        return error;
    struct Delete {
        cJSON* value;
        ~Delete() {
            cJSON_Delete(value);
        }
    } cleanup{data};
    auto* items = cJSON_GetObjectItemCaseSensitive(data, "items");
    auto* more = cJSON_GetObjectItemCaseSensitive(data, "hasMore");
    uint64_t total = 0, response_page = 0, response_limit = 0;
    const int length = cJSON_GetArraySize(items);
    if (!cJSON_IsArray(items) || length > static_cast<int>(limit) || !cJSON_IsBool(more) ||
        !Id(data, "page", &response_page) || response_page != page ||
        !Id(data, "pageSize", &response_limit) || response_limit != limit ||
        !Id(data, "total", &total, true) || total > UINT32_MAX)
        return Fail("Invalid community pagination");
    for (int i = 0; i < length; ++i) {
        auto* item = cJSON_GetArrayItem(items, i);
        auto* latest = cJSON_GetObjectItemCaseSensitive(item, "latestVersion");
        uint64_t id = 0, version = 0;
        if (!Id(item, "id", &id) || !Id(latest, "id", &version))
            return Fail("Invalid published play identity");
        error = Detail(id, version, &apps[i]);
        if (error != ESP_OK)
            return error;
        for (int j = 0; j < i; ++j)
            if (apps[j].community_play == id)
                return Fail("Duplicate play in community response");
    }
    if (pagination)
        *pagination = {page, static_cast<uint32_t>(total), cJSON_IsTrue(more) != 0};
    *count = length;
    return ESP_OK;
}
esp_err_t PrepareDownload(CatalogApp* app, InstallCancel cancel, void* context) {
    if (!app || !app->community_play)
        return ESP_ERR_INVALID_ARG;
    const auto play = app->community_play, version = app->community_version;
    CatalogApp fresh;
    const bool by_code = app->download_code[0];
    auto error = by_code ? LookupCode(app->download_code, &fresh, cancel, context)
                         : Detail(play, version, &fresh, cancel, context);
    if (error != ESP_OK)
        return error;
    if (fresh.package != CatalogApp::Package::kApplication)
        return kUnsupportedPackage;
    if (fresh.community_play != play || fresh.community_version != version ||
        fresh.size != app->size || strcmp(fresh.sha256, app->sha256))
        return Fail("Published firmware changed. Look up the app again.");
    cJSON* data = nullptr;
    const auto endpoint = by_code
        ? std::string(kOrigin) + kCodes + app->download_code + "/downloads"
        : std::string(kOrigin) + kList + "/" + std::to_string(play) + "/versions/" +
              std::to_string(version) + "/downloads";
    error = Json(endpoint, &data, true, cancel, context);
    if (error != ESP_OK)
        return error;
    struct Delete {
        cJSON* value;
        ~Delete() {
            cJSON_Delete(value);
        }
    } cleanup{data};
    uint64_t size = 0;
    char hash[65]{};
    if (!Id(data, "sizeBytes", &size) || size != fresh.size || !Hash(data, "sha256", hash) ||
        strcmp(hash, fresh.sha256) ||
        !CopyUrl(fresh.url, sizeof fresh.url, String(data, "downloadUrl")))
        return Fail("Download ticket does not match the published firmware");
    *app = fresh;
    return ESP_OK;
}
esp_err_t SaveBinding(const char* slot, const CatalogApp& app) {
    Binding binding;
    if (app.community_play) {
        binding.play = app.community_play;
        binding.version = app.community_version;
        Text(binding.app_id, sizeof binding.app_id, app.app_id);
        Text(binding.binary_version, sizeof binding.binary_version, app.version);
        // PrepareDownload keeps the public release label separately in filename-independent
        // metadata.
        Text(binding.release_version, sizeof binding.release_version, app.release_version);
    }
    nvs_handle_t handle;
    auto error = nvs_open_from_partition(kMetadataPartition, "al_apps", NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return error;
    char key[16];
    BindingKey(slot, key);
    error = nvs_set_blob(handle, key, &binding, sizeof binding);
    if (error == ESP_OK)
        error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}
void ReadBinding(App* app) {
    nvs_handle_t handle;
    if (nvs_open_from_partition(kMetadataPartition, "al_apps", NVS_READONLY, &handle) != ESP_OK)
        return;
    Binding binding;
    size_t size = sizeof binding;
    char key[16];
    BindingKey(app->slot, key);
    const auto error = nvs_get_blob(handle, key, &binding, &size);
    nvs_close(handle);
    if (error != ESP_OK || size != sizeof binding || binding.magic != 0x314D4341 ||
        binding.schema != 1 || !binding.play || !binding.version ||
        !memchr(binding.app_id, 0, sizeof binding.app_id) ||
        !memchr(binding.binary_version, 0, sizeof binding.binary_version) ||
        !memchr(binding.release_version, 0, sizeof binding.release_version) ||
        strcmp(binding.app_id, app->descriptor.app_id) ||
        strcmp(binding.binary_version, app->version))
        return;
    app->community_play = binding.play;
    app->community_version = binding.version;
    Text(app->release_version, sizeof app->release_version, binding.release_version);
}
File::~File() {
    if (handle) {
        esp_http_client_close(handle);
        esp_http_client_cleanup(handle);
    }
}
esp_err_t File::Event(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        Equal(event->header_key, "Retry-After")) {
        unsigned value;
        char extra;
        if (sscanf(event->header_value, "%u%c", &value, &extra) == 1 && value <= 86400)
            static_cast<File*>(event->user_data)->retry_after_ = value;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        Equal(event->header_key, "Location")) {
        auto* self = static_cast<File*>(event->user_data);
        if (strlen(event->header_value) < 1024)
            self->location_ = event->header_value;
    }
    return ESP_OK;
}
esp_err_t File::Open(const char* url) {
    return OpenWithTimeout(url, kReadTimeoutMs);
}
esp_err_t File::OpenDownload(const char* url, InstallCancel cancel, void* context) {
    if (!ValidServiceUrl(url))
        return ESP_ERR_INVALID_ARG;
    // Explicitly requested device diagnostics: print the exact URL used, including
    // the short-lived download ticket. Never add account/device headers here.
    ESP_LOGI("agent_system.install", "download_url=%s", url);
    constexpr unsigned attempts = kRequestAttempts;
    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        if (cancel && cancel(context))
            return kCancelled;
        const auto started = esp_timer_get_time();
        const auto error = OpenWithTimeout(url, kResponseTimeoutMs);
        ESP_LOGI("agent_system.install", "file attempt=%u/%u result=0x%x ms=%lld", attempt,
                 attempts, unsigned(error),
                 static_cast<long long>((esp_timer_get_time() - started) / 1000));
        if (cancel && cancel(context))
            return kCancelled;
        if (error == ESP_OK)
            return ESP_OK;
        const int status = handle ? esp_http_client_get_status_code(handle) : 0;
        const bool transient = Retryable(error, status, retry_after_);
        if (!transient || attempt == attempts)
            return error;
        const unsigned wait_ms = std::max(attempt, retry_after_) * 1000;
        ESP_LOGW("agent_system.install", "file retry=%u/%u in_ms=%u HTTP=%d result=0x%x",
                 attempt, attempts - 1, wait_ms, status, unsigned(error));
        // Discard the failed response before retrying. Never repeat a file GET
        // once reading or flash writing has started.
        if (handle) {
            esp_http_client_close(handle);
            esp_http_client_cleanup(handle);
            handle = nullptr;
        }
        if (!WaitRetry(attempt, retry_after_, cancel, context))
            return kCancelled;
    }
    return ESP_ERR_TIMEOUT;
}
esp_err_t File::OpenWithTimeout(const char* url, int header_timeout_ms) {
    std::string address = url ? url : "";
    for (unsigned redirect = 0; redirect <= 3; ++redirect) {
        if (!ValidServiceUrl(address.c_str()))
            return ESP_ERR_INVALID_ARG;
        if (handle) {
            esp_http_client_close(handle);
            esp_http_client_cleanup(handle);
            handle = nullptr;
        }
        location_.clear();
        retry_after_ = 0;
        esp_http_client_config_t config{};
        config.url = address.c_str();
        config.timeout_ms = kConnectTimeoutMs;
        config.buffer_size = 4096;
        config.disable_auto_redirect = true;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.event_handler = Event;
        config.user_data = this;
        handle = esp_http_client_init(&config);
        if (!handle)
            return ESP_ERR_NO_MEM;
        const auto open_at = esp_timer_get_time();
        auto error = esp_http_client_open(handle, 0);
        const auto headers_at = esp_timer_get_time();
        ESP_LOGI("agent_system.store", "file connect_ms=%lld result=0x%x",
                 static_cast<long long>((headers_at - open_at) / 1000), unsigned(error));
        if (error != ESP_OK)
            return error;
        error = esp_http_client_set_timeout_ms(handle, header_timeout_ms);
        if (error != ESP_OK)
            return error;
        const auto length = esp_http_client_fetch_headers(handle);
        const int network_errno = esp_http_client_get_errno(handle);
        const auto restored = esp_http_client_set_timeout_ms(handle, kReadTimeoutMs);
        if (length < 0) {
            const bool timed_out = TimedOut(length, network_errno);
            error = timed_out ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
            ESP_LOGW("agent_system.store", "file headers failed result=0x%x fetch=%lld errno=%d "
                     "wait_ms=%lld limit_ms=%d", unsigned(error), static_cast<long long>(length),
                     network_errno,
                     static_cast<long long>((esp_timer_get_time() - headers_at) / 1000),
                     header_timeout_ms);
            return error;
        }
        if (restored != ESP_OK)
            return restored;
        const int status = esp_http_client_get_status_code(handle);
        ESP_LOGI("agent_system.store", "file HTTP=%d headers_ms=%lld redirect=%u", status,
                 static_cast<long long>((esp_timer_get_time() - headers_at) / 1000), redirect);
        if (status == 200)
            return ESP_OK;
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
            !location_.empty()) {
            const auto next = Resolve(address, location_.c_str());
            if (address.rfind("https://", 0) == 0 && next.rfind("https://", 0) != 0)
                return ESP_ERR_INVALID_ARG;
            address = next;
            continue;
        }
        std::string body;
        char block[512];
        while (body.size() < 8192) {
            const int bytes =
                esp_http_client_read(handle, block, std::min(sizeof block, 8192 - body.size()));
            if (bytes <= 0)
                break;
            body.append(block, bytes);
        }
        auto* root = cJSON_ParseWithLength(body.c_str(), body.size() + 1);
        const char* message = root ? String(root, "message") : "";
        const auto failure =
            Failure(message[0] ? message
                               : (status == 401 || status == 403
                                      ? "The public file is not accessible"
                                      : "The download link expired or the file is unavailable"),
                    status, root, retry_after_);
        cJSON_Delete(root);
        return failure;
    }
    return Fail("Too many file redirects");
}
}  // namespace agent_system::community
