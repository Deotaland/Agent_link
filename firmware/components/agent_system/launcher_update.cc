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
    Publish(value);
    Persist(value);
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
        Publish(value);
        Persist(value);
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
    value.state = State::kAvailable;
    snprintf(value.message, sizeof value.message, "%s",
             value.forced
                 ? DefaultUiText("发现重要更新，请及时升级", "An important update is available")
                 : DefaultUiText("发现新版本", "A new version is available"));
    Publish(value);
    return ESP_OK;
}
bool Cancelled(void*) {
    return cancelled.load();
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
esp_err_t InstallNow(Status& value) {
    std::unique_lock<std::mutex> operation(OperationMutex(), std::try_to_lock);
    if (!operation.owns_lock())
        return Fail(
            value, ESP_ERR_INVALID_STATE,
            DefaultUiText("应用安装或封面同步正在进行，请完成后重试",
                          "An app installation or cover sync is running. Wait for it to finish."));
    alboot_record state;
    const int running = Running();
    if (running < 0 || Load(state) || state.pending != ALBOOT_NO_SLOT || state.active != running)
        return Fail(value, ESP_ERR_INVALID_STATE,
                    DefaultUiText("当前启动器尚未确认正常运行，或分区布局不支持本次更新",
                                  "Launcher health is not confirmed, or this partition layout does "
                                  "not support the update."));
    const unsigned target = 1u - running;
    auto* part = Partition(target);
    if (!part)
        return Fail(value, ESP_ERR_NOT_FOUND,
                    DefaultUiText("缺少专用 A/B 分区，请先通过 USB 烧录完整初始固件",
                                  "Dedicated A/B partitions are missing. Install the full initial "
                                  "USB package first."));
    const auto start = esp_timer_get_time();
    // Recheck the selected release anonymously before touching flash; signed URLs expire.
    auto err = CheckNow(value, true);
    if (err != ESP_OK)
        return err;
    community::File file;
    err = file.OpenDownload(value.url, Cancelled, nullptr);
    value.http_status = file.handle ? esp_http_client_get_status_code(file.handle) : 0;
    if (err != ESP_OK)
        return Fail(
            value, err,
            err == kCertificateError
                ? DefaultUiText("下载服务器证书校验失败，请检查设备时间或更新启动器证书支持",
                                "Download certificate verification failed. Check the device time "
                                "or update Launcher certificate support.")
                : DefaultUiText(
                      "固件下载连接失败或链接已失效，请重新检查更新",
                      "Download connection failed or the URL expired. Check for updates again."));
    const auto length = esp_http_client_get_content_length(file.handle);
    if (length >= 0 && length != value.size)
        return Fail(value, ESP_ERR_INVALID_SIZE,
                    DefaultUiText("下载文件大小与更新信息不一致",
                                  "Download size does not match the update metadata"));
    state.valid &= (uint8_t)~(1u << target);
    if (Save(state))
        return Fail(value, ESP_FAIL,
                    DefaultUiText(
                        "无法保存升级准备状态，旧启动器已保留",
                        "Could not save update preparation state. The old Launcher is preserved."));
    std::unique_ptr<uint8_t, decltype(&free)> buffer(
        static_cast<uint8_t*>(heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), free);
    if (!buffer)
        return Fail(value, ESP_ERR_NO_MEM,
                    DefaultUiText("PSRAM 下载缓冲区分配失败",
                                  "Could not allocate the PSRAM download buffer"));
    md5_context_t md5;
    esp_rom_md5_init(&md5);
    uint32_t erased = 0;
    value.state = State::kDownloading;
    value.received = 0;
    snprintf(value.message, sizeof value.message,
             DefaultUiText("正在下载到启动器 %c，旧版本保持可用",
                           "Downloading to Launcher %c. The old version remains available."),
             'A' + target);
    Publish(value);
    int64_t published = 0, read_us = 0, write_us = 0;
    while (value.received < value.size) {
        if (cancelled)
            return Fail(value, kCancelled,
                        DefaultUiText("更新已取消，旧启动器保持可用；下次更新会重新下载",
                                      "Update cancelled. The old Launcher remains available; the "
                                      "next attempt restarts the download."));
        const auto at = esp_timer_get_time();
        const int bytes =
            esp_http_client_read(file.handle, reinterpret_cast<char*>(buffer.get()),
                                 std::min<uint32_t>(16384, value.size - value.received));
        read_us += esp_timer_get_time() - at;
        value.network_errno = esp_http_client_get_errno(file.handle);
        if (bytes <= 0)
            return Fail(
                value, ESP_ERR_HTTP_CONNECT,
                DefaultUiText(
                    "固件下载中断或提前结束，请检查网络后重试",
                    "Firmware download interrupted or ended early. Check the network and retry."));
        while (erased < value.received + bytes) {
            const uint32_t chunk = std::min<uint32_t>(65536, part->size - erased);
            err = esp_partition_erase_range(part, erased, chunk);
            if (err != ESP_OK)
                return Fail(value, err,
                            DefaultUiText("备用启动器分区擦除失败，旧版本未受影响",
                                          "Could not erase the inactive Launcher partition. The "
                                          "old version is unchanged."));
            erased += chunk;
            vTaskDelay(pdMS_TO_TICKS(5) + 1);
        }
        const auto write_at = esp_timer_get_time();
        err = esp_partition_write(part, value.received, buffer.get(), bytes);
        write_us += esp_timer_get_time() - write_at;
        if (err != ESP_OK)
            return Fail(
                value, err,
                DefaultUiText(
                    "备用启动器写入失败，旧版本未受影响",
                    "Could not write the inactive Launcher. The old version is unchanged."));
        esp_rom_md5_update(&md5, buffer.get(), bytes);
        value.received += bytes;
        value.elapsed_ms = (esp_timer_get_time() - start) / 1000;
        if (esp_timer_get_time() - published >= 250000) {
            Publish(value);
            published = esp_timer_get_time();
        }
    }
    // Prove EOF as well as byte count (also handles chunked responses).
    char extra;
    if (esp_http_client_read(file.handle, &extra, 1) != 0 ||
        !esp_http_client_is_complete_data_received(file.handle))
        return Fail(value, ESP_ERR_INVALID_SIZE,
                    DefaultUiText("下载文件长度不符或传输未完整结束",
                                  "Download length mismatch or incomplete transfer"));
    uint8_t digest[16];
    char hex[33];
    esp_rom_md5_final(digest, &md5);
    for (unsigned i = 0; i < 16; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcasecmp(hex, value.md5))
        return Fail(value, ESP_ERR_INVALID_CRC,
                    DefaultUiText("固件 MD5 校验失败，请重新下载",
                                  "Firmware MD5 verification failed. Download again."));
    value.state = State::kVerifying;
    snprintf(value.message, sizeof value.message,
             DefaultUiText("正在校验已写入镜像、硬件型号和启动能力",
                           "Verifying the written image, hardware and boot capability"));
    Publish(value);
    // Read back flash, not only the network stream, before publishing the trial.
    esp_rom_md5_init(&md5);
    for (uint32_t at = 0; at < value.size; at += 16384) {
        const uint32_t bytes = std::min<uint32_t>(16384, value.size - at);
        err = esp_partition_read(part, at, buffer.get(), bytes);
        if (err != ESP_OK)
            return Fail(value, err,
                        DefaultUiText("已写入固件读取失败", "Could not read the written firmware"));
        esp_rom_md5_update(&md5, buffer.get(), bytes);
        vTaskDelay(1);
    }
    esp_rom_md5_final(digest, &md5);
    for (unsigned i = 0; i < 16; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcasecmp(hex, value.md5))
        return Fail(
            value, ESP_ERR_INVALID_CRC,
            DefaultUiText("Flash 回读 MD5 校验失败", "Flash readback MD5 verification failed"));
    if (Validate(part, value) != ESP_OK)
        return Fail(value, ESP_ERR_INVALID_VERSION,
                    DefaultUiText("镜像不是匹配本板分区布局、版本和产品的 A/B 启动器",
                                  "The image is not an A/B Launcher matching this board, layout, "
                                  "version and product."));
    if (cancelled)
        return Fail(value, kCancelled,
                    DefaultUiText("更新已取消，未切换启动器",
                                  "Update cancelled. Launcher was not switched."));
    state.pending = target;
    state.attempted = 0;
    state.versions[target] = value.version_code;
    state.result = 0;
    if (Save(state))
        return Fail(
            value, ESP_FAIL,
            DefaultUiText("固件校验通过，但升级启动记录保存失败",
                          "Firmware verified, but the update boot record could not be saved."));
    value.state = State::kReady;
    value.elapsed_ms = (esp_timer_get_time() - start) / 1000;
    snprintf(value.message, sizeof value.message,
             DefaultUiText("下载和校验完成，请重启试运行新版；失败会返回旧版",
                           "Download verified. Restart to try the new version; failure returns to "
                           "the old version."));
    ESP_LOGI(kTag, "staged slot=%c bytes=%lu elapsed_ms=%lu read_ms=%lld write_ms=%lld",
             'A' + target, (unsigned long)value.received, (unsigned long)value.elapsed_ms,
             (long long)(read_us / 1000), (long long)(write_us / 1000));
    Publish(value);
    Persist(value);
    return ESP_OK;
}
void Worker(void*) {
    for (;;) {
        uint32_t action = 0;
        xTaskNotifyWait(0, UINT32_MAX, &action, portMAX_DELAY);
        Status value = Read();
        esp_err_t error = ESP_OK;
        if (action == 1)
            error = CheckNow(value);
        else if (action == 2)
            error = InstallNow(value);
        if (error != ESP_OK && value.state != State::kError)
            (void)Fail(value, error,
                       DefaultUiText("更新操作失败，请检查网络后重试",
                                     "Update failed. Check the network and retry."));
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
    alboot_record state;
    if (Load(state) == ESP_OK) {
        if (state.result == ALBOOT_ROLLBACK) {
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
                     DefaultUiText("新版已准备好，请重启试运行",
                                   "The new version is ready. Restart to try it."));
        }
    }
    Publish(value);
    return xTaskCreate(Worker, "launcher_update", 20480, nullptr, 3, &worker) == pdPASS
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
    if (!worker || Busy() || value.state != State::kAvailable)
        return ESP_ERR_INVALID_STATE;
    value.state = State::kDownloading;
    value.error = ESP_OK;
    task_busy = true;
    cancelled = false;
    Publish(value);
    xTaskNotify(worker, 2, eSetValueWithOverwrite);
    return ESP_OK;
}
void Cancel() {
    cancelled = true;
}
esp_err_t Restart() {
    if (Read().state != State::kReady)
        return ESP_ERR_INVALID_STATE;
    auto* factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                             ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    auto err = factory ? esp_ota_set_boot_partition(factory) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK)
        esp_restart();
    return err;
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
