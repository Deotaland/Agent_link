#include "agent_system.h"
#include "cover_cache.h"
#include "image_identity.h"
#if defined(AGENT_SYSTEM_COMMUNITY)
#include "community_store.h"
#endif
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
#include "dynamic_slots.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>
#include <string>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sha256.h"

namespace agent_system {
namespace {
constexpr char kMagic[8] = {'A', 'G', 'S', 'Y', 'S', '0', '0', '1'};
#if !defined(AGENT_SYSTEM_APP_ID)
#error "Select an image role through build_firmware.py"
#endif
constexpr Descriptor kImageDescriptor{
    {'A', 'G', 'S', 'Y', 'S', '0', '0', '1'},
    1,
    kLayout,
    AGENT_SYSTEM_APP_ID,
    AGENT_SYSTEM_APP_NAME,
    AGENT_SYSTEM_BOARD_MODEL,
    "0.1.0"};
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
struct ExtendedDescriptor {
    Descriptor base;
    SlotCapability capability;
};
static_assert(offsetof(ExtendedDescriptor, capability) == 176);
const ExtendedDescriptor extended_descriptor
    __attribute__((used, section(".rodata_custom_desc"), aligned(4))) = {
        kImageDescriptor, {{'A', 'G', 'L', 'C', 'A', 'P', '0', '1'}, 1, kAppSlotCount, 1}};
const Descriptor& descriptor = extended_descriptor.base;
#else
const Descriptor descriptor __attribute__((used, section(".rodata_custom_desc"), aligned(4))) =
    kImageDescriptor;
#endif
std::mutex operation_mutex;
httpd_handle_t settings_server = nullptr;
std::atomic<bool> launcher_available{false};
std::atomic<uint32_t> app_revision{0};

bool Terminated(const char* text, size_t size) {
    return memchr(text, 0, size) != nullptr;
}
bool Compatible(const Descriptor& d) {
    return memcmp(d.magic, kMagic, 8) == 0 && d.schema == 1 && d.layout == kLayout &&
           Terminated(d.app_id, sizeof d.app_id) && d.app_id[0] &&
           Terminated(d.name, sizeof d.name) && Terminated(d.board, sizeof d.board) &&
           Terminated(d.sdk, sizeof d.sdk) && strcmp(d.board, kBoard) == 0 &&
           strcmp(d.sdk, kSdk) == 0;
}
const esp_partition_t* Factory() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                    nullptr);
}
bool IsAppSlot(const esp_partition_t* partition) {
    return partition && partition->type == ESP_PARTITION_TYPE_APP &&
           partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
           partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 + kAppSlotCount;
}
const esp_partition_t* ValidLauncher() {
    const auto* partition = Factory();
    if (!partition)
        return nullptr;
    esp_app_desc_t idf{};
    Descriptor image{};
    if (esp_ota_get_partition_description(partition, &idf) != ESP_OK ||
        esp_partition_read(partition, 0x120, &image, sizeof image) != ESP_OK ||
        !Compatible(image) || strcmp(image.app_id, "agent.launcher"))
        return nullptr;
    esp_partition_pos_t position{partition->address, partition->size};
    esp_image_metadata_t metadata{};
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) == ESP_OK ? partition
                                                                                     : nullptr;
}
const esp_partition_t* Slot(const char* label) {
    unsigned index;
    char extra;
    if (!label || sscanf(label, "ota_%u%c", &index, &extra) != 1 || index >= kAppSlotCount)
        return nullptr;
    char canonical[8];
    snprintf(canonical, sizeof canonical, "ota_%u", index);
    if (strcmp(label, canonical))
        return nullptr;
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0 + index), label);
}
esp_err_t Marker(const char* label, uint8_t* state, bool write = false) {
    nvs_handle_t h;
    esp_err_t err =
        nvs_open_from_partition("app_nvs", "al_apps", write ? NVS_READWRITE : NVS_READONLY, &h);
    if (!write && err == ESP_ERR_NVS_NOT_FOUND) {
        *state = 0;
        return ESP_OK;
    }
    if (err != ESP_OK)
        return err;
    if (write) {
        err = nvs_set_u8(h, label, *state);
        if (err == ESP_OK)
            err = nvs_commit(h);
    } else {
        err = nvs_get_u8(h, label, state);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            *state = 0;
            err = ESP_OK;
        }
    }
    nvs_close(h);
    return err;
}
esp_err_t StoredCatalog(const char* partition, char* out, size_t capacity) {
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, "al_system", NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_get_str(h, "catalog", out, &capacity);
    nvs_close(h);
    return err;
}
esp_err_t ReadApp(const esp_partition_t* p, App* app) {
    if (!p || p->size <= kCoverReserveBytes)
        return ESP_ERR_NOT_FOUND;
    uint8_t state = 0;
    esp_err_t err = Marker(p->label, &state);
    if (err != ESP_OK)
        return err;
    if (state == 2)
        return ESP_ERR_NOT_FOUND;  // Interrupted write remains unavailable.
    esp_app_desc_t idf{};
    if (esp_ota_get_partition_description(p, &idf) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    Descriptor d{};
    ImageFormat format;
    if (!ReadIdentity(p, &d, &format) ||
        !strcmp(d.app_id, "agent.launcher"))
        return ESP_ERR_NOT_FOUND;
    esp_partition_pos_t position{p->address, p->size};
    esp_image_metadata_t image{};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &image) != ESP_OK ||
        image.image_len > p->size - kCoverReserveBytes)
        return ESP_ERR_NOT_FOUND;
    *app = {};
    app->descriptor = d;
    app->format = format;
    app->size = image.image_len;
    snprintf(app->version, sizeof app->version, "%.*s", 31, idf.version);
    snprintf(app->slot, sizeof app->slot, "%s", p->label);
#if defined(AGENT_SYSTEM_COMMUNITY)
    community::ReadBinding(app);
#endif
    return ESP_OK;
}
#if defined(AGENT_SYSTEM_COMMUNITY)
using Client = community::File;
#else
struct Client {
    esp_http_client_handle_t handle = nullptr;
    ~Client() {
        if (handle) {
            esp_http_client_close(handle);
            esp_http_client_cleanup(handle);
        }
    }
    esp_err_t Open(const char* url) {
        if (!ValidServiceUrl(url))
            return ESP_ERR_INVALID_ARG;
        esp_http_client_config_t cfg{};
        cfg.url = url;
        cfg.timeout_ms = 15000;
        cfg.buffer_size = 4096;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.disable_auto_redirect = true;
        handle = esp_http_client_init(&cfg);
        if (!handle)
            return ESP_ERR_NO_MEM;
        esp_err_t err = esp_http_client_open(handle, 0);
        if (err != ESP_OK)
            return err;
        if (esp_http_client_fetch_headers(handle) < 0 ||
            esp_http_client_get_status_code(handle) != 200)
            return ESP_FAIL;
        return ESP_OK;
    }
};
#endif
bool CopyString(cJSON* object, const char* key, char* out, size_t capacity) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring[0] || strlen(value->valuestring) >= capacity)
        return false;
    memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}
bool Digest(const char* hex, unsigned char out[32]) {
    if (strlen(hex) != 64)
        return false;
    for (size_t i = 0; i < 32; ++i) {
        auto nibble = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        const int a = nibble(hex[i * 2]), b = nibble(hex[i * 2 + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = static_cast<unsigned char>((a << 4) | b);
    }
    return true;
}
std::string Escape(const char* s) {
    std::string out;
    for (; *s; ++s) {
        if (*s == '&')
            out += "&amp;";
        else if (*s == '<')
            out += "&lt;";
        else if (*s == '>')
            out += "&gt;";
        else if (*s == '"')
            out += "&quot;";
        else
            out += *s;
    }
    return out;
}
esp_err_t SettingsGet(httpd_req_t* request) {
    char url[256]{};
    (void)CatalogUrl(url, sizeof url);
    const std::string page =
        "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>" +
        Escape(IsLauncher() ? Current().name : "Agent Link") + "</title>"
        "<body style='font:18px sans-serif;max-width:600px;margin:40px auto;padding:16px'>"
        "<h1>应用服务设置</h1><p>填写应用服务的完整列表地址。局域网测试可使用 HTTP；正式服务请使用 "
        "HTTPS。</p>"
        "<form method='post' action='/catalog'><label>应用列表地址<br><input "
        "style='width:100%;font-size:16px' "
        "name='url' maxlength='255' value=\"" +
        Escape(url) +
        "\"></label><p><button>保存</button></p></form>"
        "<p>保存后在设备上重新打开在线应用。清空地址可停用在线目录。此操作保留 WiFi "
        "和设备绑定。</p></body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page.c_str(), page.size());
}
esp_err_t SettingsPost(httpd_req_t* request) {
    if (request->content_len <= 0 || request->content_len >= 800)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
    char form[800]{};
    int received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, form + received, request->content_len - received);
        if (count <= 0)
            return ESP_FAIL;
        received += count;
    }
    char encoded[780]{}, url[256]{};
    if (httpd_query_key_value(form, "url", encoded, sizeof encoded) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing URL");
    size_t j = 0;
    for (size_t i = 0; encoded[i]; ++i) {
        if (j + 1 >= sizeof url)
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "URL too long");
        char c = encoded[i];
        if (c == '%') {
            const auto hex = [](char v) {
                return v >= '0' && v <= '9'   ? v - '0'
                       : v >= 'a' && v <= 'f' ? v - 'a' + 10
                       : v >= 'A' && v <= 'F' ? v - 'A' + 10
                                              : -1;
            };
            if (!encoded[i + 1] || !encoded[i + 2])
                return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid URL");
            const int high = hex(encoded[i + 1]), low = hex(encoded[i + 2]);
            if (high < 0 || low < 0 || (high == 0 && low == 0))
                return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid URL");
            const int value = high * 16 + low;
            c = static_cast<char>(value);
            i += 2;
        } else if (c == '+')
            c = ' ';
        url[j++] = c;
    }
    if (SetCatalogUrl(url) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Use HTTPS or a private LAN HTTP URL");
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, nullptr, 0);
}
}  // namespace

const Descriptor& Current() {
    return descriptor;
}
uint32_t Revision() { return app_revision.load(); }
const char* SettingsPartition() {
#if defined(AGENT_SYSTEM_SDGOODS_COMPAT)
    return "agent_nvs";
#else
    return "nvs";
#endif
}
bool DecodeIdentity(const esp_app_desc_t& idf, const Descriptor& raw,
                    Descriptor* out, ImageFormat* format) {
    if (!Terminated(idf.version, sizeof idf.version) || !idf.version[0])
        return false;
    if (!memcmp(raw.magic, "AGSYS", 5)) {
        if (!Compatible(raw))
            return false;  // Never bypass a native image's board/layout/SDK checks.
        *out = raw;
        *format = ImageFormat::kAgentLink;
        return true;
    }
#if defined(AGENT_SYSTEM_SDGOODS_COMPAT)
    if (!VendorProject(idf.project_name, sizeof idf.project_name))
        return false;
    *out = {};
    memcpy(out->magic, kMagic, sizeof kMagic);
    out->schema = 1;
    out->layout = kLayout;
    snprintf(out->app_id, sizeof out->app_id, "%s", idf.project_name);
    snprintf(out->name, sizeof out->name, "%s", idf.project_name);
    snprintf(out->board, sizeof out->board, "%s", kBoard);
    snprintf(out->sdk, sizeof out->sdk, "sdgoods");
    *format = ImageFormat::kSdgoods;
    return true;
#else
    return false;
#endif
}
bool IsLauncher() {
    return strcmp(descriptor.app_id, "agent.launcher") == 0;
}
bool CanReturnToLauncher() {
    return !IsLauncher() && launcher_available.load();
}
esp_err_t Init() {
#if defined(AGENT_SYSTEM_SDGOODS_COMPAT)
    esp_err_t err = nvs_flash_init_partition("agent_nvs");
    if (err != ESP_OK)
        return err;
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW("agent_system", "Reinitializing vendor NVS; private Agent Link settings preserved");
        err = nvs_flash_erase_partition("nvs");
        if (err == ESP_OK)
            err = nvs_flash_init();
    }
#else
    esp_err_t err = nvs_flash_init();
#endif
    if (err != ESP_OK)
        return err;
    err = nvs_flash_init_partition("app_nvs");
    if (err == ESP_OK)
        cover_cache::Init();
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
    if (err == ESP_OK)
        err = dynamic_slots::Init(IsLauncher());
#endif
    return err;
}
esp_err_t Ready() {
    launcher_available.store(false);
    if (IsLauncher())
        return ESP_OK;
    const auto* running = esp_ota_get_running_partition();
    if (!running)
        return ESP_ERR_INVALID_STATE;
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        // The same application image also supports ordinary USB installation.
        // It owns the factory boot partition, so there is no separate launcher
        // to arm and no OTA rollback state to confirm here.
        ESP_LOGI("agent_system", "Standalone application ready in factory boot partition");
        return ESP_OK;
    }
    if (!IsAppSlot(running))
        return ESP_ERR_INVALID_STATE;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK)
            return err;
    }
    const auto* factory = ValidLauncher();
    if (!factory) {
        ESP_LOGW("agent_system",
                 "Application ready; no compatible launcher, keeping current boot selection");
        return ESP_OK;
    }
    // A USB app write may replace an explicitly removed slot. Publish this
    // successfully started image without touching shared device settings.
    uint8_t installed = 1;
    const esp_err_t marker = Marker(running->label, &installed, true);
    if (marker != ESP_OK)
        return marker;
    launcher_available.store(true);
    const esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK)
        ESP_LOGW("agent_system", "Application ready; cannot arm launcher for next boot: %s",
                 esp_err_to_name(err));
    else
        ESP_LOGI("agent_system",
                 "Installed application ready; verified launcher armed for next boot");
    return ESP_OK;
}
esp_err_t ReturnToLauncher() {
    if (IsLauncher() || !IsAppSlot(esp_ota_get_running_partition()))
        return ESP_ERR_INVALID_STATE;
    const auto* p = ValidLauncher();
    launcher_available.store(p != nullptr);
    if (!p)
        return ESP_ERR_NOT_FOUND;
    const esp_err_t err = esp_ota_set_boot_partition(p);
    if (err == ESP_OK)
        esp_restart();
    return err;
}
esp_err_t List(App* apps, size_t capacity, size_t* count) {
    if (!apps || !count)
        return ESP_ERR_INVALID_ARG;
    *count = 0;
    for (int i = 0; i < kAppSlotCount; ++i) {
        char label[8];
        snprintf(label, sizeof label, "ota_%d", i);
        const auto* p = Slot(label);
        App app;
        if (!p)
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
            continue;
#else
            return ESP_ERR_NOT_FOUND;
#endif
        const esp_err_t err = ReadApp(p, &app);
        if (err == ESP_ERR_NOT_FOUND)
            continue;
        if (err != ESP_OK)
            return err;
        if (*count >= capacity)
            return ESP_ERR_INVALID_SIZE;
        apps[(*count)++] = app;
    }
    return ESP_OK;
}
esp_err_t ReadCover(const char* label, Cover* cover) {
    if (!cover)
        return ESP_ERR_INVALID_ARG;
    *cover = {};
    const auto* p = Slot(label);
    if (!p)
        return ESP_ERR_INVALID_ARG;
    uint8_t state = 0;
    const esp_err_t marker = Marker(label, &state);
    if (marker != ESP_OK)
        return marker;
    if (state == 2)
        return ESP_ERR_NOT_FOUND;
    Descriptor app{};
    ImageFormat format;
    if (!ReadIdentity(p, &app, &format))
        return ESP_ERR_NOT_FOUND;
    return cover_cache::Read(label, app.app_id, cover);
}
esp_err_t GetCoverStatus(const char* label, CoverStatus* status) {
    if (!Slot(label) || !status)
        return ESP_ERR_INVALID_ARG;
    return cover_cache::Status(label, status);
}
esp_err_t Launch(const char* label) {
    if (!IsLauncher())
        return ESP_ERR_INVALID_STATE;
    const auto* p = Slot(label);
    App app;
    if (!p || ReadApp(p, &app) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    const esp_err_t err = esp_ota_set_boot_partition(p);
    if (err == ESP_OK)
        esp_restart();
    return err;
}
esp_err_t Remove(const char* label) {
    std::unique_lock<std::mutex> lock(operation_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !IsLauncher())
        return ESP_ERR_INVALID_STATE;
    const auto* p = Slot(label);
    if (!p || p == esp_ota_get_running_partition())
        return ESP_ERR_INVALID_ARG;
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
    if (!dynamic_slots::Writable())
        return kNeedsFullPackage;
#endif
    uint8_t state = 2;
    esp_err_t err = Marker(label, &state, true);
    if (err == ESP_OK)
        err = esp_partition_erase_range(p, 0, p->size);
    if (err == ESP_OK)
        cover_cache::Forget(label);
    if (err == ESP_OK)
        ++app_revision;
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
    if (err == ESP_OK)
        return dynamic_slots::Release(label);
#endif
    return err;  // Keep marker unavailable; successful installs explicitly commit it.
}
bool ValidServiceUrl(const char* url) {
#if !defined(AGENT_SYSTEM_COMMUNITY)
    if (!url || strlen(url) >= 256 || strchr(url, '%'))
        return false;
#endif
    if (!url || strlen(url) >= 1024 || strpbrk(url, "\r\n\t @#\\"))
        return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(url); *p; ++p)
        if (*p < 0x21 || *p > 0x7E)
            return false;
    const bool https = strncmp(url, "https://", 8) == 0;
    if (!https && strncmp(url, "http://", 7) != 0)
        return false;
    const char* host = url + (https ? 8 : 7);
    const char* end = strpbrk(host, "/:?");
    const size_t length = end ? static_cast<size_t>(end - host) : strlen(host);
    if (!length || length >= 128)
        return false;
    for (const char* at = url; *at; ++at)
        if (*at == '%') {
            const auto hex = [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            };
            if (at < host + length || !at[1] || !at[2] || !hex(at[1]) || !hex(at[2]))
                return false;
            at += 2;
        }
    if (https)
        return true;
#if defined(AGENT_SYSTEM_COMMUNITY)
    // The operator explicitly configured this production HTTP origin. Keep the
    // general public-HTTP restriction for all unrelated hosts.
    if (community::IsOrigin(url))
        return true;
#endif
    char name[128]{};
    memcpy(name, host, length);
    unsigned a, b, c, d;
    char extra;
    if (sscanf(name, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 && a < 256 && b < 256 &&
        c < 256 && d < 256)
        return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
               (a == 169 && b == 254);
    return length > 6 && strcmp(name + length - 6, ".local") == 0;
}
esp_err_t CatalogUrl(char* out, size_t capacity) {
    if (!out || !capacity)
        return ESP_ERR_INVALID_ARG;
    out[0] = 0;
    esp_err_t err = StoredCatalog(SettingsPartition(), out, capacity);
#if defined(AGENT_SYSTEM_SDGOODS_COMPAT)
    // Preserve a catalog configured by an earlier V3 build. Explicitly clearing
    // the new key returns an empty string above, so this never re-enables it.
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = StoredCatalog("nvs", out, capacity);
        if (err == ESP_OK) {
#if defined(AGENT_SYSTEM_COMMUNITY)
            if (!ValidServiceUrl(out) ||
                (!strncmp(out, "http://", 7) && strstr(out, ":8765/api/apps"))) {
                if (strlen(community::DefaultCatalog()) >= capacity)
                    return ESP_ERR_INVALID_SIZE;
                snprintf(out, capacity, "%s", community::DefaultCatalog());
            }
#else
            if (!ValidServiceUrl(out)) {
                out[0] = 0;
                return ESP_OK;
            }
#endif
            err = SetCatalogUrl(out);
        }
    }
#endif
#if defined(AGENT_SYSTEM_COMMUNITY)
    // One-time switch of the old development store. Explicit settings saved
    // after this migration (including an empty URL) remain under user control.
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        return err;
    nvs_handle_t handle;
    esp_err_t opened =
        nvs_open_from_partition(SettingsPartition(), "al_system", NVS_READWRITE, &handle);
    if (opened != ESP_OK)
        return opened;
    uint8_t migrated = 0;
    const auto marker = nvs_get_u8(handle, "community_v1", &migrated);
    nvs_close(handle);
    if (marker == ESP_ERR_NVS_NOT_FOUND || !migrated) {
        const bool old_test =
            out[0] && !strncmp(out, "http://", 7) && strstr(out, ":8765/api/apps");
        if (err == ESP_ERR_NVS_NOT_FOUND || !out[0] || old_test) {
            if (strlen(community::DefaultCatalog()) >= capacity)
                return ESP_ERR_INVALID_SIZE;
            snprintf(out, capacity, "%s", community::DefaultCatalog());
            const auto saved = SetCatalogUrl(out);
            if (saved != ESP_OK)
                return saved;
            err = ESP_OK;
        } else {
            opened =
                nvs_open_from_partition(SettingsPartition(), "al_system", NVS_READWRITE, &handle);
            if (opened != ESP_OK)
                return opened;
            opened = nvs_set_u8(handle, "community_v1", 1);
            if (opened == ESP_OK)
                opened = nvs_commit(handle);
            nvs_close(handle);
            if (opened != ESP_OK)
                return opened;
        }
    } else if (marker != ESP_OK)
        return marker;
#endif
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}
esp_err_t SetCatalogUrl(const char* url) {
    if (!url || strlen(url) >= 256 || (url[0] && !ValidServiceUrl(url)))
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(SettingsPartition(), "al_system", NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(h, "catalog", url);
#if defined(AGENT_SYSTEM_COMMUNITY)
    if (err == ESP_OK)
        err = nvs_set_u8(h, "community_v1", 1);
#endif
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}
esp_err_t FetchCatalog(CatalogApp* apps, size_t capacity, size_t* count) {
    return FetchCatalogPage(apps, capacity, count, 1, nullptr);
}
bool CommunityCatalog() {
#if defined(AGENT_SYSTEM_COMMUNITY)
    char url[256]{};
    return CatalogUrl(url, sizeof url) == ESP_OK && community::IsCatalog(url);
#else
    return false;
#endif
}
void LastStoreError(StoreError* error) {
    if (!error)
        return;
#if defined(AGENT_SYSTEM_COMMUNITY)
    community::Error(error);
#else
    *error = {};
#endif
}
esp_err_t FetchCatalogPage(CatalogApp* apps, size_t capacity, size_t* count, unsigned page,
                           CatalogPage* pagination) {
    if (!apps || !count)
        return ESP_ERR_INVALID_ARG;
    *count = 0;
    char url[256]{};
    esp_err_t err = CatalogUrl(url, sizeof url);
    if (err != ESP_OK)
        return err;
    if (!url[0])
        return ESP_ERR_NOT_FOUND;
#if defined(AGENT_SYSTEM_COMMUNITY)
    if (community::IsCatalog(url))
        return community::Fetch(url, page, apps, capacity, count, pagination);
#endif
    if (page != 1)
        return ESP_ERR_NOT_SUPPORTED;
    if (pagination)
        *pagination = {};
    Client client;
    err = client.Open(url);
    if (err != ESP_OK) {
        ESP_LOGW("agent_system", "catalog connection/HTTP failed: %s", esp_err_to_name(err));
        return err;
    }
    std::string body;
    char buffer[1024];
    while (true) {
        const int bytes = esp_http_client_read(client.handle, buffer, sizeof buffer);
        if (bytes < 0)
            return ESP_FAIL;
        if (!bytes)
            break;
        if (body.size() + bytes > 16384)
            return ESP_ERR_INVALID_SIZE;
        body.append(buffer, bytes);
    }
    if (!esp_http_client_is_complete_data_received(client.handle))
        return ESP_FAIL;
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size() + 1);
    if (!root)
        return ESP_ERR_INVALID_RESPONSE;
    const auto cleanup = [&]() { cJSON_Delete(root); };
    char board[48]{};
    const auto* schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const auto* layout = cJSON_GetObjectItemCaseSensitive(root, "layout");
    cJSON* entries = cJSON_GetObjectItemCaseSensitive(root, "apps");
    const int size = cJSON_GetArraySize(entries);
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1 || !cJSON_IsNumber(layout) ||
        layout->valuedouble != kLayout || !CopyString(root, "board", board, sizeof board) ||
        strcmp(board, kBoard) || !cJSON_IsArray(entries) ||
        size > static_cast<int>(std::min(capacity, kMaxApps))) {
        cleanup();
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (int i = 0; i < size; ++i) {
        auto* item = cJSON_GetArrayItem(entries, i);
        CatalogApp& app = apps[i];
        app = {};
        auto* bytes = cJSON_GetObjectItemCaseSensitive(item, "size");
        unsigned char digest[32];
        if (!CopyString(item, "app_id", app.app_id, sizeof app.app_id) ||
            !strcmp(app.app_id, "agent.launcher") ||
            !CopyString(item, "name", app.name, sizeof app.name) ||
            !CopyString(item, "version", app.version, sizeof app.version) ||
            !CopyString(item, "url", app.url, sizeof app.url) ||
            !CopyString(item, "sha256", app.sha256, sizeof app.sha256) ||
            !Digest(app.sha256, digest) || !cJSON_IsNumber(bytes) ||
            !std::isfinite(bytes->valuedouble) || bytes->valuedouble <= 0 ||
            bytes->valuedouble > kAppImageMaxBytes ||
            std::floor(bytes->valuedouble) != bytes->valuedouble) {
            cleanup();
            return ESP_ERR_INVALID_RESPONSE;
        }
        app.size = static_cast<uint32_t>(bytes->valuedouble);
        if (cJSON_GetObjectItemCaseSensitive(item, "image_format")) {
            char format[24]{};
            if (!CopyString(item, "image_format", format, sizeof format)) {
                cleanup();
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (!strcmp(format, "sdgoods")) {
#if defined(AGENT_SYSTEM_SDGOODS_COMPAT)
                app.format = ImageFormat::kSdgoods;
#else
                cleanup();
                return ESP_ERR_INVALID_RESPONSE;
#endif
            } else if (strcmp(format, "agent-link")) {
                cleanup();
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        const auto resolve = [&](char* address, size_t capacity) {
            if (address[0] == '/' && address[1] != '/') {
                const char* host = strstr(url, "://") + 3;
                const char* path = strchr(host, '/');
                std::string full(url, path ? static_cast<size_t>(path - url) : strlen(url));
                full += address;
                if (full.size() >= capacity)
                    return false;
                snprintf(address, capacity, "%s", full.c_str());
            }
            return ValidServiceUrl(address);
        };
        if (!resolve(app.url, sizeof app.url)) {
            cleanup();
            return ESP_ERR_INVALID_ARG;
        }
        if (cJSON_GetObjectItemCaseSensitive(item, "cover_url")) {
            const auto* size = cJSON_GetObjectItemCaseSensitive(item, "cover_size");
            if (!CopyString(item, "cover_url", app.cover_url, sizeof app.cover_url) ||
                !CopyString(item, "cover_sha256", app.cover_sha256, sizeof app.cover_sha256) ||
                !CopyString(item, "cover_format", app.cover_format, sizeof app.cover_format) ||
                strcmp(app.cover_format, kCoverFormat) || !Digest(app.cover_sha256, digest) ||
                !cJSON_IsNumber(size) || !std::isfinite(size->valuedouble) ||
                size->valuedouble != kCoverBytes ||
                std::floor(size->valuedouble) != size->valuedouble ||
                !resolve(app.cover_url, sizeof app.cover_url)) {
                cleanup();
                return ESP_ERR_INVALID_RESPONSE;
            }
            app.cover_size = static_cast<uint32_t>(size->valuedouble);
        }
        for (int j = 0; j < i; ++j)
            if (!strcmp(apps[j].app_id, app.app_id)) {
                cleanup();
                return ESP_ERR_INVALID_RESPONSE;
            }
    }
    *count = size;
    cleanup();
    ESP_LOGI("agent_system", "catalog ready: %d applications", size);
    return ESP_OK;
}
esp_err_t SyncCovers(const CatalogApp* apps, size_t count) {
    if (!apps || count > kMaxApps)
        return ESP_ERR_INVALID_ARG;
    std::unique_lock<std::mutex> lock(operation_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !IsLauncher())
        return ESP_ERR_INVALID_STATE;
    App installed[kAppSlotCount];
    size_t installed_count = 0;
    esp_err_t err = List(installed, kAppSlotCount, &installed_count);
    if (err != ESP_OK)
        return err;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < installed_count; ++i) {
        const CatalogApp* match = nullptr;
        for (size_t j = 0; j < count; ++j)
            if (!strcmp(installed[i].descriptor.app_id, apps[j].app_id) &&
                !strcmp(installed[i].version, apps[j].version)) {
                match = &apps[j];
                break;
            }
        if (!match) {
            cover_cache::NotListed(installed[i].slot);
            ESP_LOGI("agent_system", "cover skipped: slot=%s app=%s version=%s absent from catalog",
                     installed[i].slot, installed[i].descriptor.app_id, installed[i].version);
            continue;
        }
        err = cover_cache::Update(installed[i].slot, *match);
        if (result == ESP_OK && err != ESP_OK)
            result = err;
    }
    return result;
}
esp_err_t SyncStoreCovers() {
#if defined(AGENT_SYSTEM_COMMUNITY)
    if (CommunityCatalog()) {
        std::unique_lock<std::mutex> lock(operation_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !IsLauncher())
            return ESP_ERR_INVALID_STATE;
        App installed[kAppSlotCount];
        size_t count = 0;
        auto error = List(installed, kAppSlotCount, &count);
        if (error != ESP_OK)
            return error;
        esp_err_t result = ESP_OK;
        for (size_t i = 0; i < count; ++i) {
            if (!installed[i].community_play) {
                cover_cache::NotListed(installed[i].slot);
                continue;
            }
            CatalogApp app;
            error = community::Detail(installed[i].community_play, installed[i].community_version,
                                      &app);
            if (error == ESP_OK) {
                snprintf(app.app_id, sizeof app.app_id, "%s", installed[i].descriptor.app_id);
                error = cover_cache::Update(installed[i].slot, app);
            }
            if (result == ESP_OK && error != ESP_OK)
                result = error;
        }
        return result;
    }
#endif
    auto catalog = std::unique_ptr<CatalogApp[]>(new (std::nothrow) CatalogApp[kMaxApps]);
    if (!catalog)
        return ESP_ERR_NO_MEM;
    size_t count = 0;
    auto error = FetchCatalog(catalog.get(), kMaxApps, &count);
    return error == ESP_OK ? SyncCovers(catalog.get(), count) : error;
}
esp_err_t Install(const CatalogApp& requested, Progress progress, void* context) {
    std::unique_lock<std::mutex> lock(operation_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !IsLauncher())
        return ESP_ERR_INVALID_STATE;
    CatalogApp app = requested;
#if defined(AGENT_SYSTEM_COMMUNITY)
    if (app.community_play && app.package != CatalogApp::Package::kApplication)
        return kUnsupportedPackage;
#endif
    unsigned char expected[32];
    if (!Terminated(app.app_id, sizeof app.app_id) || !app.app_id[0] ||
        !Terminated(app.sha256, sizeof app.sha256) || !Digest(app.sha256, expected) || !app.size ||
        app.size > kAppImageMaxBytes)
        return ESP_ERR_INVALID_ARG;
#if !defined(AGENT_SYSTEM_SDGOODS_COMPAT)
    if (app.format != ImageFormat::kAgentLink)
        return ESP_ERR_INVALID_ARG;
#endif
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
    if (!dynamic_slots::Writable())
        return kNeedsFullPackage;
    if (app.size < 512)
        return ESP_ERR_INVALID_SIZE;
    uint32_t free_mask = 0;
    for (int i = 0; i < kAppSlotCount; ++i) {
        char label[8];
        snprintf(label, sizeof label, "ota_%d", i);
        App existing;
        const auto* part = Slot(label);
        const esp_err_t result = part ? ReadApp(part, &existing) : ESP_ERR_NOT_FOUND;
        if (result == ESP_ERR_NOT_FOUND)
            free_mask |= 1u << i;
        else if (result != ESP_OK)
            return result;
        else if (!strcmp(existing.descriptor.app_id, app.app_id) ||
                 (app.community_play && existing.community_play == app.community_play))
            return kAlreadyInstalled;
    }
    if (!free_mask)
        return kSlotsFull;
#if defined(AGENT_SYSTEM_COMMUNITY)
    if (app.community_play) {
        const auto prepared = community::PrepareDownload(&app);
        if (prepared != ESP_OK)
            return prepared;
    }
#endif
    Client client;
    esp_err_t err = client.Open(app.url);
    if (err != ESP_OK)
        return err;
    if (esp_http_client_get_content_length(client.handle) != app.size)
        return ESP_ERR_INVALID_SIZE;
    agentlink::ota::Sha256 hash;
    if (!hash.Start())
        return ESP_FAIL;
    uint8_t buffer[4096];
    uint32_t total = 0;
    while (total < 512) {
        const int bytes = esp_http_client_read(
            client.handle, reinterpret_cast<char*>(buffer + total), 512 - total);
        if (bytes <= 0 || static_cast<uint32_t>(bytes) > 512 - total)
            return ESP_ERR_INVALID_RESPONSE;
        total += bytes;
    }
    esp_app_desc_t idf{};
    Descriptor raw{}, identity{};
    ImageFormat format;
    memcpy(&idf, buffer + 0x20, sizeof idf);
    memcpy(&raw, buffer + 0x120, sizeof raw);
    if (buffer[0] != 0xE9 || buffer[12] != 9 || buffer[13] ||
        idf.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !DecodeIdentity(idf, raw, &identity, &format) || !strcmp(identity.app_id, "agent.launcher"))
        return ESP_ERR_INVALID_VERSION;
    if (app.community_play) {
        if (!memchr(idf.version, 0, sizeof idf.version))
            return ESP_ERR_INVALID_VERSION;
        app.format = format;
        snprintf(app.app_id, sizeof app.app_id, "%s", identity.app_id);
        snprintf(app.version, sizeof app.version, "%s", idf.version);
        for (int i = 0; i < kAppSlotCount; ++i) {
            char label[8];
            snprintf(label, sizeof label, "ota_%d", i);
            App existing;
            const auto* part = Slot(label);
            if (part && ReadApp(part, &existing) == ESP_OK &&
                !strcmp(existing.descriptor.app_id, app.app_id))
                return kAlreadyInstalled;
        }
    } else if (format != app.format || strcmp(identity.app_id, app.app_id) ||
               strncmp(idf.version, app.version, sizeof idf.version))
        return ESP_ERR_INVALID_VERSION;
    unsigned limit = 16;
    if (format == ImageFormat::kAgentLink) {
        SlotCapability capability{};
        memcpy(&capability, buffer + 0x120 + sizeof(Descriptor), sizeof capability);
        if (memcmp(capability.magic, "AGLCAP01", 8) || capability.version != 1 ||
            capability.slots != 16 || capability.flags != 1)
            limit = 2;
    }
    std::unique_ptr<dynamic_slots::Plan> plan(new (std::nothrow) dynamic_slots::Plan);
    if (!plan)
        return ESP_ERR_NO_MEM;
    err = dynamic_slots::Allocate(app.size, free_mask, limit, plan.get());
    if (err != ESP_OK)
        return err;
    const auto* target = &plan->partition;
    uint8_t writing = 2;
    err = Marker(target->label, &writing, true);
    if (err != ESP_OK)
        return err;
    err = esp_partition_erase_range(target, 0, target->size);
    if (err != ESP_OK)
        return err;
    if (!hash.Update(buffer, total))
        return ESP_FAIL;
    err = esp_partition_write(target, 0, buffer, total);
    while (err == ESP_OK && total < app.size) {
        const int bytes = esp_http_client_read(client.handle, reinterpret_cast<char*>(buffer),
                                               std::min<uint32_t>(sizeof buffer, app.size - total));
        if (bytes <= 0)
            return ESP_ERR_INVALID_RESPONSE;
        if (!hash.Update(buffer, bytes))
            return ESP_FAIL;
        err = esp_partition_write(target, total, buffer, bytes);
        total += bytes;
        if (progress)
            progress(total, app.size, context);
    }
    if (err != ESP_OK)
        return err;
    uint8_t actual[32];
    if (!hash.Finish(actual) || memcmp(actual, expected, 32) ||
        !esp_http_client_is_complete_data_received(client.handle))
        return ESP_ERR_INVALID_CRC;
    esp_partition_pos_t position{target->address, target->size};
    esp_image_metadata_t metadata{};
    err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata);
    if (err != ESP_OK)
        return err;
    if (metadata.image_len > app.size || metadata.image_len > target->size - kCoverReserveBytes)
        return ESP_ERR_INVALID_SIZE;
    if (!ReadIdentity(target, &identity, &format) || strcmp(identity.app_id, app.app_id))
        return ESP_ERR_INVALID_VERSION;
#if defined(AGENT_SYSTEM_COMMUNITY)
    err = community::SaveBinding(target->label, app);
    if (err != ESP_OK)
        return err;
#endif
    uint8_t ready = 1;
    err = Marker(target->label, &ready, true);
    if (err != ESP_OK)
        return err;
    // Reload IDF's cached partition list before publishing covers or launching.
    return dynamic_slots::Commit(*plan);
#else
    const esp_partition_t* target = nullptr;
    for (int i = 0; i < kAppSlotCount; ++i) {
        char label[8];
        snprintf(label, sizeof label, "ota_%d", i);
        const auto* p = Slot(label);
        App existing;
        if (!p)
            return ESP_ERR_NOT_FOUND;
        const esp_err_t err = ReadApp(p, &existing);
        if (err == ESP_ERR_NOT_FOUND) {
            if (!target)
                target = p;
            continue;
        }
        if (err != ESP_OK)
            return err;
        // Scan every slot, including those after a hole, before opening HTTP or erasing flash.
        if (!strcmp(existing.descriptor.app_id, app.app_id))
            return kAlreadyInstalled;
    }
    if (!target)
        return kSlotsFull;  // Never overwrite an installed app implicitly.
    Client client;
    esp_err_t err = client.Open(app.url);
    if (err != ESP_OK)
        return err;
    if (esp_http_client_get_content_length(client.handle) != app.size)
        return ESP_ERR_INVALID_SIZE;
    uint8_t writing = 2;
    err = Marker(target->label, &writing, true);
    if (err != ESP_OK)
        return err;
    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(target, app.size, &ota);
    if (err != ESP_OK)
        return err;
    agentlink::ota::Sha256 hash;
    if (!hash.Start()) {
        esp_ota_abort(ota);
        return ESP_FAIL;
    }
    uint32_t total = 0;
    uint8_t buffer[4096];
    while (err == ESP_OK && total < app.size) {
        const int bytes = esp_http_client_read(client.handle, reinterpret_cast<char*>(buffer),
                                               std::min<uint32_t>(sizeof buffer, app.size - total));
        if (bytes <= 0) {
            err = ESP_FAIL;
            break;
        }
        if (!hash.Update(buffer, bytes)) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buffer, bytes);
        total += bytes;
        if (progress)
            progress(total, app.size, context);
    }
    unsigned char actual[32]{};
    if (!hash.Finish(actual))
        err = ESP_FAIL;
    if (err == ESP_OK &&
        (memcmp(actual, expected, 32) || !esp_http_client_is_complete_data_received(client.handle)))
        err = ESP_ERR_INVALID_CRC;
    Descriptor d{};
    esp_app_desc_t idf{};
    ImageFormat format;
    if (err == ESP_OK &&
        (!ReadIdentity(target, &d, &format) || format != app.format ||
         strcmp(d.app_id, app.app_id) || !strcmp(d.app_id, "agent.launcher") ||
         esp_ota_get_partition_description(target, &idf) != ESP_OK ||
         strncmp(idf.version, app.version, sizeof idf.version)))
        err = ESP_ERR_INVALID_VERSION;
    if (err != ESP_OK) {
        esp_ota_abort(ota);
        return err;
    }
    err = esp_ota_end(ota);
    if (err != ESP_OK)
        return err;
    // Cover errors fall back to a placeholder; firmware validation stays independent.
    (void)cover_cache::Update(target->label, app);
    uint8_t ready = 1;
    err = Marker(target->label, &ready, true);  // Commit last; boot target stays unchanged.
    if (err == ESP_OK)
        ++app_revision;
    return err;
#endif
}
esp_err_t StartSettingsServer() {
    if (settings_server)
        return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8081;
    config.ctrl_port = 32770;
    config.max_uri_handlers = 2;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&settings_server, &config);
    if (err != ESP_OK)
        return err;
    httpd_uri_t get{};
    get.uri = "/";
    get.method = HTTP_GET;
    get.handler = SettingsGet;
    httpd_uri_t post{};
    post.uri = "/catalog";
    post.method = HTTP_POST;
    post.handler = SettingsPost;
    err = httpd_register_uri_handler(settings_server, &get);
    if (err == ESP_OK)
        err = httpd_register_uri_handler(settings_server, &post);
    if (err != ESP_OK) {
        httpd_stop(settings_server);
        settings_server = nullptr;
    }
    return err;
}
}  // namespace agent_system
