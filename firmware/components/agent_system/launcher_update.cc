#include "ui_text.h"
#include "launcher_update.h"
#include "agent_system.h"
#include "agent_launcher_boot.h"
#include "dynamic_layout_config.h"
#include "launcher_version.h"
#include "community_store.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_md5.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <strings.h>
#include <memory>
#include <mutex>
#include <string>

namespace agent_system {
std::mutex& OperationMutex();
namespace launcher {
namespace {
constexpr const char* kTag = "launcher.update";
std::mutex status_mutex;
Status status;
TaskHandle_t worker = nullptr;
esp_timer_handle_t trial_timer = nullptr;
std::atomic<bool> cancelled{false};
std::atomic<bool> task_busy{false};
std::atomic<bool> background_task{false}, auto_download{true}, online{false}, idle{false};
std::atomic<bool> forced_download{false};
bool boot_checked = false;
std::atomic<int64_t> next_auto_attempt{0};
constexpr uint32_t kSector = 4096, kCheckpoint = 65536;
struct ResumeRecord {
    uint32_t magic = 0x414C5231, policy = ALPT_POLICY_ID;
    uint64_t firmware_id = 0;
    uint32_t version_code = 0, size = 0, offset = 0;
    uint8_t target = 0, ready = 0, forced = 0, rejected = 0;
    char version[32]{}, md5[33]{};
};
esp_err_t ReadResume(ResumeRecord& record) {
    nvs_handle_t handle;
    auto error =
        nvs_open_from_partition(SettingsPartition(), "launcher_update", NVS_READONLY, &handle);
    if (error != ESP_OK)
        return error;
    size_t bytes = sizeof record;
    error = nvs_get_blob(handle, "resume_v1", &record, &bytes);
    nvs_close(handle);
    if (error != ESP_OK)
        return error;
    if (bytes != sizeof record || record.magic != 0x414C5231 || record.policy != ALPT_POLICY_ID ||
        record.target > 1 || record.ready > 1 || record.forced > 1 || record.rejected > 1 ||
        !record.firmware_id || record.version_code <= AGENT_LAUNCHER_VERSION_CODE ||
        record.size < 512 || record.size > ALBOOT_IMAGE_BYTES || record.offset > record.size ||
        (record.offset != record.size && record.offset % kSector) ||
        !memchr(record.version, 0, sizeof record.version) || !record.version[0] ||
        !memchr(record.md5, 0, sizeof record.md5) || strlen(record.md5) != 32 ||
        (record.ready && record.offset != record.size))
        return ESP_ERR_INVALID_RESPONSE;
    for (unsigned i = 0; i < 32; ++i)
        if (!isxdigit(static_cast<unsigned char>(record.md5[i])))
            return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}
esp_err_t SaveResume(const ResumeRecord& record) {
    nvs_handle_t handle;
    auto error =
        nvs_open_from_partition(SettingsPartition(), "launcher_update", NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return error;
    error = nvs_set_blob(handle, "resume_v1", &record, sizeof record);
    if (error == ESP_OK)
        error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}
bool SameRelease(const ResumeRecord& record, const Status& value) {
    return record.firmware_id == value.firmware_id && record.version_code == value.version_code &&
           record.size == value.size && !strcmp(record.version, value.version) &&
           !strcasecmp(record.md5, value.md5);
}
bool Pausing() {
    return background_task && (!online || !idle || (!auto_download && !forced_download));
}
struct Receipt {
    uint32_t magic;
    esp_err_t error;
    int http;
    char message[384];
};
int ReadFlash(void*, uint32_t at, void* out, size_t bytes) {
    return esp_flash_read(esp_flash_default_chip, out, at, bytes);
}
int EraseFlash(void*, uint32_t at, size_t bytes) {
    return esp_flash_erase_region(esp_flash_default_chip, at, bytes);
}
int WriteFlash(void*, uint32_t at, const void* in, size_t bytes) {
    return esp_flash_write(esp_flash_default_chip, in, at, bytes);
}
const alpt_io io{nullptr, ReadFlash, EraseFlash, WriteFlash, nullptr};
esp_err_t Load(alboot_record& value) {
    int bank;
    return alboot_load(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &value, &bank) < 0 ? ESP_FAIL
                                                                                    : ESP_OK;
}
esp_err_t Save(alboot_record& value) {
    return alboot_save(&io, ALBOOT_STATE_OFFSET, ALPT_POLICY_ID, &value) ? ESP_FAIL : ESP_OK;
}
const esp_partition_t* Partition(unsigned slot) {
    if (slot > 1)
        return nullptr;
    auto* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0 + 14 + slot), nullptr);
    return part && part->address == (slot ? ALBOOT_B_OFFSET : ALBOOT_A_OFFSET) &&
                   part->size == ALBOOT_IMAGE_BYTES
               ? part
               : nullptr;
}
int Running() {
    auto* part = esp_ota_get_running_partition();
    if (!part)
        return -1;
    return part->address == ALBOOT_A_OFFSET ? 0 : part->address == ALBOOT_B_OFFSET ? 1 : -1;
}
void Publish(const Status& value) {
    std::lock_guard<std::mutex> lock(status_mutex);
    const auto revision = status.revision + 1;
    status = value;
    status.revision = revision;
}
void Persist(const Status& value) {
    Receipt receipt{0x41425531, value.error, value.http_status, {}};
    snprintf(receipt.message, sizeof receipt.message, "%s", value.message);
    nvs_handle_t handle;
    esp_err_t err =
        nvs_open_from_partition(SettingsPartition(), "launcher_update", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Cannot save result: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(handle, "result", &receipt, sizeof receipt);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK)
        ESP_LOGW(kTag, "Cannot save result: %s", esp_err_to_name(err));
}
esp_err_t Fail(Status& value, esp_err_t error, const char* message) {
    value.state = State::kError;
    value.error = error;
    snprintf(value.message, sizeof value.message, "%s", message);
    ESP_LOGW(kTag, "failed error=%s HTTP=%d errno=%d bytes=%lu/%lu elapsed_ms=%lu",
             esp_err_to_name(error), value.http_status, value.network_errno,
             (unsigned long)value.received, (unsigned long)value.size,
             (unsigned long)value.elapsed_ms);
    return error;
}
const char* String(cJSON* object, const char* key) {
    auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
bool Number(cJSON* object, const char* key, uint64_t* out, uint64_t minimum = 1) {
    auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < minimum ||
        item->valuedouble > 9007199254740991.0 ||
        std::floor(item->valuedouble) != item->valuedouble)
        return false;
    *out = static_cast<uint64_t>(item->valuedouble);
    return true;
}
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
esp_err_t Request(const std::string& path, Status& value, Json& root) {
    std::string url = AGENT_LAUNCHER_UPDATE_ORIGIN + path;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    config.disable_auto_redirect = true;
    auto* handle = esp_http_client_init(&config);
    if (!handle)
        return ESP_ERR_NO_MEM;
    esp_http_client_set_header(handle, "Accept", "application/json");
    const auto start = esp_timer_get_time();
    auto err = esp_http_client_open(handle, 0);
    if (err != ESP_OK)
        err = community::ConnectionError(handle, err);
    if (err == ESP_OK && esp_http_client_fetch_headers(handle) < 0)
        err = ESP_ERR_HTTP_CONNECT;
    value.http_status = esp_http_client_get_status_code(handle);
    value.network_errno = esp_http_client_get_errno(handle);
    std::string body;
    char buffer[1024];
    while (err == ESP_OK && !cancelled && body.size() < 32768) {
        const int bytes = esp_http_client_read(handle, buffer, sizeof buffer);
        if (bytes < 0) {
            err = ESP_ERR_HTTP_FETCH_HEADER;
            break;
        }
        if (!bytes)
            break;
        body.append(buffer, bytes);
    }
    if (cancelled)
        err = kCancelled;
    if (!esp_http_client_is_complete_data_received(handle) || body.size() >= 32768)
        if (err == ESP_OK)
            err = ESP_ERR_INVALID_SIZE;
    esp_http_client_cleanup(handle);
    value.elapsed_ms = (esp_timer_get_time() - start) / 1000;
    ESP_LOGI(kTag, "metadata HTTP=%d elapsed_ms=%lu", value.http_status,
             (unsigned long)value.elapsed_ms);
    if (err != ESP_OK)
        return Fail(value, err,
                    cancelled ? DefaultUiText("已取消更新操作", "Update cancelled")
                    : err == kCertificateError
                        ? DefaultUiText("更新服务证书校验失败，请检查设备时间或更新启动器证书支持",
                                        "Update service certificate verification failed. Check the "
                                        "device time or update Launcher certificate support.")
                        : DefaultUiText("更新服务连接失败或响应不完整，请检查网络后重试",
                                        "Update service connection failed or response incomplete. "
                                        "Check the network and retry."));
    root.reset(cJSON_ParseWithLength(body.c_str(), body.size() + 1));
    if (value.http_status != 200) {
        char text[384];
        snprintf(text, sizeof text,
                 DefaultUiText("升级服务返回 HTTP %d：%.250s",
                               "Update service returned HTTP %d: %.250s"),
                 value.http_status,
                 root ? String(root.get(), "message")
                      : DefaultUiText("服务器未返回错误详情",
                                      "No error details returned by the server"));
        return Fail(value, kStoreError, text);
    }
    uint64_t code;
    if (!root || !Number(root.get(), "code", &code, 0))
        return Fail(value, ESP_ERR_INVALID_RESPONSE,
                    DefaultUiText("升级接口缺少有效的业务状态码",
                                  "Update response has no valid result code"));
    auto* success = cJSON_GetObjectItemCaseSensitive(root.get(), "success");
    if (code || (success && !cJSON_IsTrue(success))) {
        char text[384];
        snprintf(text, sizeof text,
                 DefaultUiText("升级服务拒绝请求（code=%llu）：%.250s",
                               "Update service rejected the request (code=%llu): %.250s"),
                 (unsigned long long)code, String(root.get(), "message"));
        return Fail(value, kStoreError, text);
    }
    if (!root || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root.get(), "data")))
        return Fail(value, ESP_ERR_INVALID_RESPONSE,
                    DefaultUiText("升级接口响应缺少有效的 data 字段",
                                  "Update response has no valid data object"));
    return ESP_OK;
}
esp_err_t CheckNow(Status& value, bool refresh = false) {
    Json root(nullptr, cJSON_Delete);
    const auto path = std::string("/api/firmware/check-update?product_id=") +
                      std::to_string(AGENT_LAUNCHER_PRODUCT_ID) +
                      "&current_version_code=" + std::to_string(AGENT_LAUNCHER_VERSION_CODE) +
                      "&chip_type=esp32-s3&firmware_type=ota";
    auto err = Request(path, value, root);
    if (err != ESP_OK)
        return err;
    auto* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
    auto* update = cJSON_GetObjectItemCaseSensitive(data, "has_update");
    if (!cJSON_IsBool(update))
        return Fail(
            value, ESP_ERR_INVALID_RESPONSE,
            DefaultUiText("升级接口缺少 has_update", "Update response is missing has_update"));
    if (!cJSON_IsTrue(update)) {
        if (refresh)
            return Fail(
                value, ESP_ERR_INVALID_RESPONSE,
                DefaultUiText("所选更新已不可用，请重新检查更新",
                              "This update is no longer available. Check for updates again."));
        value.state = State::kCurrent;
        snprintf(value.message, sizeof value.message,
                 DefaultUiText("已是最新版本", "Launcher is up to date"));
        return ESP_OK;
    }
    uint64_t id, code, bytes, product;
    const char* md5 = String(data, "file_md5");
    if (!Number(data, "firmware_id", &id) || !Number(data, "version_code", &code) ||
        code <= AGENT_LAUNCHER_VERSION_CODE || code > UINT32_MAX ||
        !Number(data, "product_id", &product) || product != AGENT_LAUNCHER_PRODUCT_ID ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_official")) ||
        !Number(data, "file_size", &bytes) || bytes < 512 || bytes > ALBOOT_IMAGE_BYTES ||
        strcmp(String(data, "chip_type"), "esp32-s3") ||
        strcmp(String(data, "firmware_type"), "ota") || strlen(md5) != 32 ||
        !String(data, "version")[0] || strlen(String(data, "version")) >= sizeof value.version)
        return Fail(value, ESP_ERR_INVALID_RESPONSE,
                    DefaultUiText(
                        "更新信息不兼容：需要本产品的官方 ESP32-S3 启动器纯应用镜像，且不超过 3MiB",
                        "Incompatible update: an official ESP32-S3 Launcher app image for this "
                        "product, up to 3 MiB, is required."));
    for (size_t i = 0; i < 32; ++i)
        if (!((md5[i] >= '0' && md5[i] <= '9') || (md5[i] >= 'a' && md5[i] <= 'f') ||
              (md5[i] >= 'A' && md5[i] <= 'F')))
            return Fail(value, ESP_ERR_INVALID_RESPONSE,
                        DefaultUiText("更新信息中的 MD5 格式无效",
                                      "Update metadata contains an invalid MD5"));
    if (refresh && (id != value.firmware_id || code != value.version_code || bytes != value.size ||
                    strcmp(String(data, "version"), value.version) || strcasecmp(md5, value.md5)))
        return Fail(value, ESP_ERR_INVALID_RESPONSE,
                    DefaultUiText("所选固件或校验信息已变化，请重新检查更新后确认",
                                  "The selected firmware or checksum has changed. Check for "
                                  "updates and confirm again."));
    const char* url = String(data, "download_url");
    if (!url[0])
        return Fail(value, ESP_ERR_INVALID_RESPONSE,
                    DefaultUiText("更新接口未返回下载链接，请重新检查更新",
                                  "No download URL returned. Check for updates again."));
    if (strlen(url) >= sizeof value.url)
        return Fail(value, ESP_ERR_INVALID_SIZE,
                    DefaultUiText("更新下载链接过长", "Update download URL is too long"));
    value.firmware_id = id;
    value.version_code = code;
    value.size = bytes;
    snprintf(value.version, sizeof value.version, "%s", String(data, "version"));
    snprintf(value.md5, sizeof value.md5, "%s", md5);
    snprintf(value.changelog, sizeof value.changelog, "%s", String(data, "changelog"));
    value.forced = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_forced"));
    snprintf(value.url, sizeof value.url, "%s", url);
    if (refresh)
        return ESP_OK;
    ResumeRecord saved;
    const bool has_resume = ReadResume(saved) == ESP_OK;
    if (has_resume && saved.rejected && saved.version_code == value.version_code &&
        !strcasecmp(saved.md5, value.md5))
        return Fail(value, ESP_ERR_INVALID_VERSION,
                    DefaultUiText("此版本启动确认失败，已阻止自动重试，请等待修正版",
                                  "This version failed boot confirmation. Automatic retry is "
                                  "blocked; wait for a corrected release."));
    value.resumable = has_resume && !saved.rejected && SameRelease(saved, value);
    value.received = value.resumable ? saved.offset : 0;
    value.state = State::kAvailable;
    snprintf(value.message, sizeof value.message, "%s",
             value.forced
                 ? DefaultUiText("发现重要更新，请及时升级", "An important update is available")
                 : DefaultUiText("发现新版本", "A new version is available"));
    return ESP_OK;
}
bool Cancelled(void*) {
    return cancelled.load() || Pausing();
}
esp_err_t Validate(const esp_partition_t* part, const Status& value) {
    Descriptor descriptor{};
    alboot_capability cap{};
    esp_app_desc_t app{};
    esp_image_metadata_t image{};
    const esp_partition_pos_t position{part->address, part->size};
    if (esp_partition_read(part, 0x120, &descriptor, sizeof descriptor) ||
        esp_partition_read(part, 0x1E4, &cap, sizeof cap) ||
        esp_ota_get_partition_description(part, &app) || memcmp(descriptor.magic, "AGSYS001", 8) ||
        descriptor.schema != 1 || descriptor.layout != 3 ||
        !memchr(descriptor.app_id, 0, sizeof descriptor.app_id) ||
        strcmp(descriptor.app_id, "agent.launcher") ||
        !memchr(descriptor.board, 0, sizeof descriptor.board) ||
        strcmp(descriptor.board, "SDGOODS-ESP32S3") || memcmp(cap.magic, "ALAB0001", 8) ||
        cap.policy != ALPT_POLICY_ID || cap.reserved ||
        cap.product_id != AGENT_LAUNCHER_PRODUCT_ID || cap.version_code != value.version_code ||
        !memchr(app.version, 0, sizeof app.version) || strcmp(app.version, value.version) ||
        esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &image) != ESP_OK ||
        image.image_len != value.size)
        return ESP_ERR_INVALID_VERSION;
    return ESP_OK;
}

esp_err_t VerifyStored(const esp_partition_t* part, const Status& value, uint8_t* buffer) {
    md5_context_t md5;
    esp_rom_md5_init(&md5);
    for (uint32_t at = 0; at < value.size; at += kSector) {
        if (cancelled || Pausing())
            return kCancelled;
        const uint32_t bytes = std::min<uint32_t>(kSector, value.size - at);
        auto error = esp_partition_read(part, at, buffer, bytes);
        if (error != ESP_OK)
            return error;
        esp_rom_md5_update(&md5, buffer, bytes);
        vTaskDelay(pdMS_TO_TICKS(background_task ? 4 : 1) + 1);
    }
    uint8_t digest[16];
    char hex[33];
    esp_rom_md5_final(digest, &md5);
    for (unsigned i = 0; i < 16; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcasecmp(hex, value.md5))
        return ESP_ERR_INVALID_CRC;
    return Validate(part, value);
}
esp_err_t InstallNow(Status& value, bool refreshed = false) {
    if (!refreshed) {
        const auto error = CheckNow(value, true);
        if (error != ESP_OK)
            return error;
    }
    forced_download = value.forced;
    ResumeRecord saved;
    const int running = Running();
    const unsigned target = 1u - unsigned(running);
    const auto* destination = running >= 0 ? Partition(target) : nullptr;
    if (!destination)
        return Fail(
            value, ESP_ERR_INVALID_STATE,
            DefaultUiText("缺少备用启动器分区", "The inactive Launcher partition is missing"));
    const esp_partition_t partition = *destination;
    const auto* part = &partition;
    bool resume =
        ReadResume(saved) == ESP_OK && SameRelease(saved, value) && saved.target == target;
    if (!resume) {
        saved = ResumeRecord{};
        saved.target = target;
        saved.firmware_id = value.firmware_id;
        saved.version_code = value.version_code;
        saved.size = value.size;
        snprintf(saved.version, sizeof saved.version, "%s", value.version);
        snprintf(saved.md5, sizeof saved.md5, "%s", value.md5);
    }
    uint32_t durable = saved.offset;
    saved.forced = value.forced;
    value.received = durable;
    value.resumable = resume;
    const auto started = esp_timer_get_time();
    const auto pause = [&](esp_err_t error, const char* reason, bool owns_operation = false) {
        // Preserve complete sectors even between the periodic 64 KiB checkpoints.
        // Never wait for a foreground owner of flash just to save this receipt.
        if (saved.offset > durable) {
            if (owns_operation) {
                if (SaveResume(saved) == ESP_OK)
                    durable = saved.offset;
            } else {
                std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
                if (operation.owns_lock() && SaveResume(saved) == ESP_OK)
                    durable = saved.offset;
            }
        }
        value.received = durable;
        value.resumable = true;
        value.state = State::kPaused;
        value.error = error;
        snprintf(value.message, sizeof value.message, "%s", reason);
        ESP_LOGI(kTag, "paused bytes=%lu/%lu error=0x%x", (unsigned long)durable,
                 (unsigned long)value.size, unsigned(error));
        return ESP_OK;
    };
    if (cancelled || Pausing())
        return background_task
                   ? pause(ESP_OK, DefaultUiText("后台下载已暂停，保留断点",
                                                 "Background download paused; progress saved"))
                   : Fail(value, kCancelled,
                          DefaultUiText("更新已取消，断点已保留",
                                        "Update cancelled; progress saved"));
    std::unique_ptr<uint8_t, decltype(&free)> buffer(
        static_cast<uint8_t*>(heap_caps_malloc(kSector, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        free);
    if (!buffer)
        return background_task ? pause(ESP_ERR_NO_MEM,
                                       DefaultUiText("内存暂时不足，释放后自动继续下载",
                                                     "Waiting for memory; download will resume"))
                               : Fail(value, ESP_ERR_NO_MEM,
                                      DefaultUiText("下载缓冲分配失败",
                                                    "Could not allocate the download buffer"));
    // No operation lock is held during network waits. Only sector writes and
    // journal commits take it, so foreground app operations can preempt this task.
    community::File file;
    if (saved.offset < value.size) {
        auto error = file.OpenRange(value.url, saved.offset, value.size, background_task, Cancelled,
                                    nullptr);
        value.http_status = file.handle ? esp_http_client_get_status_code(file.handle) : 0;
        if (cancelled || Pausing())
            return background_task
                       ? pause(ESP_OK, DefaultUiText("后台下载已暂停，保留断点",
                                                     "Background download paused; progress saved"))
                       : Fail(value, kCancelled,
                              DefaultUiText("更新已取消，断点已保留",
                                            "Update cancelled; progress saved"));
        if (error == ESP_ERR_NOT_SUPPORTED && saved.offset) {
            // A server ignoring Range returns the whole file. Restart safely;
            // it must never be appended at the saved offset.
            saved.offset = durable = 0;
            resume = false;
            error = file.OpenRange(value.url, 0, value.size, background_task, Cancelled, nullptr);
        }
        if (error != ESP_OK) {
            if (background_task && error != kCertificateError && error != ESP_ERR_INVALID_RESPONSE)
                return pause(error,
                             DefaultUiText("网络暂不可用，稍后继续后台下载",
                                           "Network unavailable; background download will resume"));
            return Fail(value, error,
                        error == kCertificateError
                            ? DefaultUiText("下载服务器证书校验失败",
                                            "Download certificate verification failed")
                            : DefaultUiText(
                                  "固件下载连接失败或续传响应无效，请重试",
                                  "Download connection failed or invalid resume response; retry"));
        }
        if (esp_http_client_get_content_length(file.handle) != int64_t(value.size - saved.offset))
            return Fail(
                value, ESP_ERR_INVALID_SIZE,
                DefaultUiText("下载文件大小不符", "Download size does not match the update"));
    }
    {
        std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
        if (!operation.owns_lock())
            return background_task
                       ? pause(ESP_OK,
                               DefaultUiText(
                                   "正在执行其他操作，后台下载稍后继续",
                                   "Foreground operation active; background download will resume"))
                       : Fail(
                             value, ESP_ERR_INVALID_STATE,
                             DefaultUiText("请等待当前操作完成", "Wait for the current operation"));
        if (cancelled || Pausing())
            return pause(ESP_OK,
                         DefaultUiText("后台下载已暂停，保留断点",
                                       "Background download paused; progress saved"),
                         true);
        alboot_record boot;
        if (Load(boot) || boot.pending != ALBOOT_NO_SLOT || boot.active != running)
            return Fail(
                value, ESP_ERR_INVALID_STATE,
                DefaultUiText("启动器尚未确认正常运行", "Launcher health is not confirmed"));
        if (!resume) {
            boot.valid &= uint8_t(~(1u << target));
            boot.result = 0;
            if (Save(boot) != ESP_OK || SaveResume(saved) != ESP_OK)
                return Fail(value, ESP_FAIL,
                            DefaultUiText("无法保存下载准备状态",
                                          "Could not save download preparation state"));
        }
    }
    value.state = State::kDownloading;
    value.error = ESP_OK;
    value.received = saved.offset;
    snprintf(value.message, sizeof value.message, "%s",
             background_task
                 ? DefaultUiText("低优先级后台下载，高负载任务期间自动暂停",
                                 "Background download; yields only to intensive tasks")
                 : DefaultUiText("正在下载启动器更新", "Downloading the Launcher update"));
    Publish(value);
    int64_t published = 0;
    while (saved.offset < value.size) {
        if (cancelled || Pausing())
            return background_task
                       ? pause(ESP_OK, DefaultUiText("后台下载已暂停，保留断点",
                                                     "Background download paused; progress saved"))
                       : Fail(value, kCancelled,
                              DefaultUiText("更新已取消，断点已保留",
                                            "Update cancelled; progress saved"));
        const uint32_t count = std::min<uint32_t>(kSector, value.size - saved.offset);
        uint32_t filled = 0;
        while (filled < count) {
            if (cancelled || Pausing())
                return pause(ESP_OK, DefaultUiText("下载已暂停，保留断点",
                                                   "Download paused; progress saved"));
            const int bytes = esp_http_client_read(
                file.handle, reinterpret_cast<char*>(buffer.get() + filled), count - filled);
            value.network_errno = esp_http_client_get_errno(file.handle);
            if (bytes <= 0 || uint32_t(bytes) > count - filled) {
                if (background_task)
                    return pause(ESP_ERR_HTTP_CONNECT,
                                 DefaultUiText("网络中断，恢复后继续下载",
                                               "Connection interrupted; download will resume"));
                return Fail(value, ESP_ERR_HTTP_CONNECT,
                            DefaultUiText("下载中断，重试时从断点继续",
                                          "Download interrupted; retry to resume"));
            }
            filled += bytes;
        }
        {
            std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
            if (!operation.owns_lock() || Pausing())
                return pause(ESP_OK,
                             DefaultUiText("正在执行其他操作，下载已暂停",
                                           "Foreground operation active; download paused"),
                             operation.owns_lock());
            if (cancelled)
                return Fail(value, kCancelled, DefaultUiText("更新已取消", "Update cancelled"));
            // Re-erase uncheckpointed sectors after any interruption or power loss.
            auto error = esp_partition_erase_range(part, saved.offset, kSector);
            if (error == ESP_OK)
                error = esp_partition_write(part, saved.offset, buffer.get(), count);
            if (error != ESP_OK)
                return Fail(value, error,
                            DefaultUiText(
                                "备用分区写入失败，旧版仍可使用",
                                "Inactive partition write failed; old Launcher remains available"));
            saved.offset += count;
            if (saved.offset % kCheckpoint == 0 || saved.offset == saved.size) {
                if (SaveResume(saved) != ESP_OK)
                    return Fail(
                        value, ESP_FAIL,
                        DefaultUiText("无法保存下载断点", "Could not save download progress"));
                durable = saved.offset;
            }
        }
        value.received = saved.offset;
        value.elapsed_ms = (esp_timer_get_time() - started) / 1000;
        if (esp_timer_get_time() - published >= 250000) {
            Publish(value);
            published = esp_timer_get_time();
        }
        // At most ~64 KiB/s in background, with bounded cache-disabled writes.
        vTaskDelay(pdMS_TO_TICKS(background_task ? 64 : 1) + 1);
    }
    if (file.handle) {
        char extra;
        if (esp_http_client_read(file.handle, &extra, 1) != 0 ||
            !esp_http_client_is_complete_data_received(file.handle))
            return Fail(value, ESP_ERR_INVALID_SIZE,
                        DefaultUiText("下载长度不符", "Incomplete download or extra data"));
    }
    value.state = State::kVerifying;
    snprintf(value.message, sizeof value.message, "%s",
             DefaultUiText("正在回读校验下载的更新包", "Verifying the downloaded update"));
    Publish(value);
    const auto verified = VerifyStored(part, value, buffer.get());
    if (verified == kCancelled)
        return pause(ESP_OK, DefaultUiText("校验已暂停，更新包已保留",
                                           "Verification paused; downloaded update retained"));
    if (verified != ESP_OK) {
        saved.offset = saved.ready = 0;
        std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
        if (operation.owns_lock())
            (void)SaveResume(saved);
        return Fail(
            value, verified,
            verified == ESP_ERR_INVALID_CRC
                ? DefaultUiText("Flash 回读 MD5 校验失败，请重新下载",
                                "Flash readback MD5 verification failed; download again")
                : DefaultUiText("镜像型号、版本或启动能力校验失败",
                                "Image hardware, version or boot capability verification failed"));
    }
    {
        std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
        if (!operation.owns_lock() || Pausing())
            return pause(ESP_OK,
                         DefaultUiText("更新包已下载，稍后继续校验",
                                       "Update downloaded; verification will resume"),
                         operation.owns_lock());
        if (saved.forced) {
            alboot_record boot;
            if (Load(boot) || boot.active != running || boot.pending != ALBOOT_NO_SLOT)
                return Fail(
                    value, ESP_ERR_INVALID_STATE,
                    DefaultUiText("无法安排强制更新", "Could not schedule the required update"));
            boot.pending = target;
            boot.attempted = 0;
            boot.versions[target] = value.version_code;
            boot.result = 0;
            if (Save(boot) != ESP_OK)
                return Fail(value, ESP_FAIL,
                            DefaultUiText("无法保存强制更新启动记录",
                                          "Could not save required update boot selection"));
        }
        saved.ready = 1;
        const auto saved_ready = SaveResume(saved);
        if (saved_ready != ESP_OK && !saved.forced)
            return Fail(value, saved_ready,
                        DefaultUiText("无法保存更新包状态", "Could not save update readiness"));
        if (saved_ready != ESP_OK)
            ESP_LOGW(kTag, "Required update armed; readiness receipt failed result=0x%x",
                     unsigned(saved_ready));
    }
    // Optional updates wait for explicit consent. Required updates arm a trial
    // without restarting, so Recovery applies them at the next reboot.
    value.state = State::kReady;
    value.resumable = true;
    value.elapsed_ms = (esp_timer_get_time() - started) / 1000;
    snprintf(
        value.message, sizeof value.message, "%s",
        value.forced
            ? DefaultUiText("强制更新已准备好，下次重启自动更新",
                            "Required update ready. It will apply on the next restart.")
            : DefaultUiText("更新包已下载，请确认重启更新；稍后也可继续使用设备",
                            "Update downloaded. Restart to update, or keep using your device."));
    ESP_LOGI(kTag, "staged slot=%c bytes=%lu elapsed_ms=%lu forced=%d", 'A' + target,
             (unsigned long)value.received, (unsigned long)value.elapsed_ms, int(value.forced));
    return ESP_OK;
}
esp_err_t RestartNow(Status& value) {
    ResumeRecord saved;
    alboot_record boot;
    if (ReadResume(saved) != ESP_OK || !saved.ready || saved.target == Running() ||
        !SameRelease(saved, value))
        return Fail(value, ESP_ERR_INVALID_STATE,
                    DefaultUiText("没有已校验的更新包", "No verified update is staged"));
    const auto* part = Partition(saved.target);
    std::unique_ptr<uint8_t, decltype(&free)> buffer(
        static_cast<uint8_t*>(heap_caps_malloc(kSector, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        free);
    if (!part || !buffer)
        return Fail(value, ESP_ERR_NO_MEM,
                    DefaultUiText("无法读取备用更新包", "Could not read the staged update"));
    const auto verified = VerifyStored(part, value, buffer.get());
    if (verified != ESP_OK)
        return Fail(value, verified,
                    DefaultUiText("更新包再次校验失败，未切换启动器",
                                  "Staged update verification failed; Launcher was not switched"));
    std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
    if (!operation.owns_lock() || Load(boot) || boot.active != Running() ||
        (boot.pending != ALBOOT_NO_SLOT && boot.pending != saved.target))
        return Fail(value, ESP_ERR_INVALID_STATE,
                    DefaultUiText("请等待当前操作完成", "Wait for the current operation"));
    boot.pending = saved.target;
    boot.attempted = 0;
    boot.versions[saved.target] = saved.version_code;
    boot.result = 0;
    if (Save(boot) != ESP_OK)
        return Fail(value, ESP_FAIL,
                    DefaultUiText("无法保存升级启动记录", "Could not save update boot selection"));
    auto* recovery = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                              ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    const auto error = recovery ? esp_ota_set_boot_partition(recovery) : ESP_ERR_NOT_FOUND;
    if (error == ESP_OK)
        esp_restart();
    return error;
}
void Worker(void*) {
    for (;;) {
        uint32_t action = 0;
        xTaskNotifyWait(0, UINT32_MAX, &action, portMAX_DELAY);
        vTaskPrioritySet(nullptr, background_task ? 1 : 3);
        Status value = Read();
        value.background = background_task.load();
        const Status staged = value;
        esp_err_t error = ESP_OK;
        if (action == 1 || action == 3) {
            error = CheckNow(value);
            // A boot-time check must not discard a complete offline-staged image.
            if (action == 3 && staged.state == State::kReady &&
                (error != ESP_OK || staged.forced || !value.forced || value.state != State::kAvailable))
                value = staged;
        } else if (action == 2) {
            error = InstallNow(value);
        } else if (action == 4) {
            error = CheckNow(value);
            if (error == ESP_OK && value.state == State::kAvailable)
                error = InstallNow(value, true);
        } else if (action == 5) {
            error = RestartNow(value);
        }
        if ((action == 3 || action == 4) && value.state != State::kReady && error != ESP_OK &&
            (error == ESP_ERR_HTTP_CONNECT || error == ESP_ERR_TIMEOUT ||
             error == ESP_ERR_HTTP_FETCH_HEADER || error == ESP_ERR_NO_MEM ||
             (error == kStoreError && value.http_status >= 500))) {
            value.state = State::kPaused;
            snprintf(value.message, sizeof value.message, "%s",
                     DefaultUiText("更新服务暂不可用，稍后自动重试",
                                   "Update service unavailable; will retry in background"));
        }
        if (error != ESP_OK && value.state != State::kError && value.state != State::kReady &&
            value.state != State::kPaused)
            (void)Fail(value, error,
                       DefaultUiText("更新操作失败，请稍后重试", "Update failed; retry later"));
        Publish(value);
        if (value.state == State::kError || value.state == State::kReady)
            Persist(value);
        // Resource contention resumes as soon as the controller grants access.
        // Failed network/memory attempts back off from completion, not their start.
        next_auto_attempt = value.state == State::kPaused && value.error != ESP_OK
                                ? esp_timer_get_time() + 30000000
                                : 0;
        task_busy = false;
    }
}
}  // namespace
Status Read() {
    std::lock_guard<std::mutex> lock(status_mutex);
    return status;
}
bool Busy() {
    return task_busy.load();
}
bool Background() {
    return Busy() && background_task.load();
}
bool YieldToForeground() {
    idle = false;
    std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
    return operation.owns_lock();
}
bool AutoDownloadEnabled() {
    return auto_download.load();
}
esp_err_t SetAutoDownload(bool enabled) {
    nvs_handle_t handle;
    auto error =
        nvs_open_from_partition(SettingsPartition(), "launcher_update", NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return error;
    error = nvs_set_u8(handle, "auto_dl", enabled ? 1 : 0);
    if (error == ESP_OK)
        error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK)
        auto_download = enabled;
    return error;
}
void Tick(bool connected, bool can_download) {
    online = connected;
    idle = connected && can_download;
    if (!worker || Busy() || !connected)
        return;
    const auto now = esp_timer_get_time();
    const auto value = Read();
    const bool check = !boot_checked;
    const bool download = !check && (auto_download || value.forced) && idle &&
                          now >= next_auto_attempt &&
                          (value.state == State::kAvailable || value.state == State::kPaused);
    if (!check && !download)
        return;
    if (task_busy.exchange(true))
        return;
    if (check)
        boot_checked = true;
    background_task = true;
    cancelled = false;
    xTaskNotify(worker, check ? 3 : 4, eSetValueWithOverwrite);
}
esp_err_t Init() {
    if (worker)
        return ESP_OK;
    Status value;
    value.current_code = AGENT_LAUNCHER_VERSION_CODE;
    snprintf(value.current, sizeof value.current, "%s", AGENT_LAUNCHER_VERSION);
    snprintf(
        value.message, sizeof value.message,
        DefaultUiText("检查 Sugarat Launcher 新版本", "Check for Sugarat Launcher updates"));
    Receipt receipt{};
    size_t bytes = sizeof receipt;
    nvs_handle_t handle;
    if (nvs_open_from_partition(SettingsPartition(), "launcher_update", NVS_READONLY, &handle) ==
        ESP_OK) {
        uint8_t enabled = 1;
        if (nvs_get_u8(handle, "auto_dl", &enabled) == ESP_OK && enabled <= 1)
            auto_download = enabled != 0;
        if (nvs_get_blob(handle, "result", &receipt, &bytes) == ESP_OK && bytes == sizeof receipt &&
            receipt.magic == 0x41425531 && memchr(receipt.message, 0, sizeof receipt.message)) {
            value.error = receipt.error;
            value.http_status = receipt.http;
            snprintf(value.message, sizeof value.message, "%s", receipt.message);
            if (receipt.error != ESP_OK)
                value.state = State::kError;
        }
        nvs_close(handle);
    }
    ResumeRecord saved;
    if (ReadResume(saved) == ESP_OK && !saved.rejected && saved.target != Running()) {
        value.firmware_id = saved.firmware_id;
        value.version_code = saved.version_code;
        value.size = saved.size;
        value.received = saved.offset;
        value.resumable = true;
        value.forced = saved.forced != 0;
        value.error = ESP_OK;
        snprintf(value.version, sizeof value.version, "%s", saved.version);
        snprintf(value.md5, sizeof value.md5, "%s", saved.md5);
        value.state = saved.ready ? State::kReady : State::kPaused;
        snprintf(value.message, sizeof value.message, "%s",
                 saved.ready
                     ? DefaultUiText("更新包已准备好，请确认重启更新",
                                     "Update downloaded. Restart to update when convenient.")
                     : (auto_download || saved.forced)
                         ? DefaultUiText("已保留下载断点，联网空闲时自动继续", "Download progress saved; resumes when online and idle.")
                         : DefaultUiText("下载已暂停，可在更新设置中继续", "Download paused; resume from update settings."));
    }
    alboot_record state;
    if (Load(state) == ESP_OK) {
        if (state.result == ALBOOT_ROLLBACK) {
            if (ReadResume(saved) == ESP_OK && !saved.rejected) {
                saved.ready = 0;
                saved.offset = 0;
                saved.rejected = 1;
                (void)SaveResume(saved);
            }
            value.state = State::kError;
            snprintf(value.message, sizeof value.message,
                     DefaultUiText("新版启动器未通过启动确认，已自动恢复旧版",
                                   "The new Launcher did not pass boot confirmation. The old "
                                   "version was restored."));
        } else if (state.result == ALBOOT_SUCCESS)
            snprintf(value.message, sizeof value.message,
                     DefaultUiText("启动器升级成功，当前版本 %s",
                                   "Launcher update successful. Current version: %s"),
                     value.current);
        if (state.pending < 2 && state.pending != Running()) {
            value.state = State::kReady;
            snprintf(value.message, sizeof value.message,
                     value.forced
                         ? DefaultUiText("必需更新已准备好，下次重启自动应用", "Required update ready. It will apply on the next restart.")
                         : DefaultUiText("新版已准备好，请重启试运行", "The new version is ready. Restart to try it."));
        }
    }
    Publish(value);
    return xTaskCreate(Worker, "launcher_update", 20480, nullptr, 1, &worker) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
esp_err_t Check() {
    if (!worker || Busy() || Read().state == State::kReady)
        return ESP_ERR_INVALID_STATE;
    Status value;
    value.current_code = AGENT_LAUNCHER_VERSION_CODE;
    snprintf(value.current, sizeof value.current, "%s", AGENT_LAUNCHER_VERSION);
    value.state = State::kChecking;
    snprintf(value.message, sizeof value.message,
             DefaultUiText("正在检查新版本", "Checking for updates"));
    task_busy = true;
    background_task = false;
    cancelled = false;
    Publish(value);
    xTaskNotify(worker, 1, eSetValueWithOverwrite);
    return ESP_OK;
}
esp_err_t ArmTrial() {
    alboot_record state;
    auto err = Load(state);
    if (err != ESP_OK)
        return err;
    if (trial_timer || state.pending != Running())
        return ESP_OK;
    esp_timer_create_args_t args{};
    args.name = "launcher_trial";
    args.callback = [](void*) {
        ESP_LOGE(kTag, "Local health confirmation timed out; restarting for rollback");
        esp_restart();
    };
    err = esp_timer_create(&args, &trial_timer);
    return err == ESP_OK ? esp_timer_start_once(trial_timer, 45000000) : err;
}
esp_err_t Install() {
    auto value = Read();
    if (!worker || Busy() || (value.state != State::kAvailable && value.state != State::kPaused))
        return ESP_ERR_INVALID_STATE;
    value.state = State::kDownloading;
    value.error = ESP_OK;
    task_busy = true;
    background_task = false;
    cancelled = false;
    Publish(value);
    xTaskNotify(worker, 2, eSetValueWithOverwrite);
    return ESP_OK;
}
void Cancel() {
    cancelled = true;
}
esp_err_t Restart() {
    auto value = Read();
    if (!worker || Busy() || value.state != State::kReady)
        return ESP_ERR_INVALID_STATE;
    background_task = false;
    cancelled = false;
    task_busy = true;
    value.state = State::kVerifying;
    snprintf(value.message, sizeof value.message, "%s",
             DefaultUiText("正在确认更新包并重启", "Confirming the staged update and restarting"));
    Publish(value);
    xTaskNotify(worker, 5, eSetValueWithOverwrite);
    return ESP_OK;
}
esp_err_t ConfirmHealthy() {
    std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
    if (!operation.owns_lock())
        return ESP_ERR_INVALID_STATE;
    alboot_record state;
    const int running = Running();
    if (running < 0 || Load(state) || alboot_confirm(&state, running, AGENT_LAUNCHER_VERSION_CODE))
        return ESP_ERR_INVALID_STATE;
    auto err = Save(state);
    if (err != ESP_OK)
        return err;
    if (trial_timer)
        (void)esp_timer_stop(trial_timer);
    auto* factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                             ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    err = factory ? esp_ota_set_boot_partition(factory) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK)
        return err;
    ESP_LOGI(kTag, "Launcher %c confirmed version_code=%lu", 'A' + running,
             (unsigned long)AGENT_LAUNCHER_VERSION_CODE);
    auto value = Read();
    if (state.result == ALBOOT_SUCCESS) {
        snprintf(value.message, sizeof value.message,
                 DefaultUiText("启动器升级成功，当前版本 %s",
                               "Launcher update successful. Current version: %s"),
                 value.current);
        value.state = State::kIdle;
        value.error = ESP_OK;
        Publish(value);
        Persist(value);
    }
    return ESP_OK;
}
}  // namespace launcher
}  // namespace agent_system
