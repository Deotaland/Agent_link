#include "cover_cache.h"
#if defined(AGENT_SYSTEM_COMMUNITY)
#include "community_cover.h"
#endif
#include "image_identity.h"
#include "slot_identity.h"
#include "sha256.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace agent_system::cover_cache {
namespace {
constexpr const char* kTag = "agent_system.cover";
constexpr char kMagic[8] = {'A', 'L', 'C', 'O', 'V', 'E', 'R', '1'};
std::mutex mutex, update_mutex;
CoverStatus statuses[kAppSlotCount];
std::shared_ptr<const CoverPixels> images[kAppSlotCount];
uint32_t revisions[kAppSlotCount]{};
int Index(const char* slot) {
    return SlotIndex(slot);
}
const esp_partition_t* Partition(const char* slot) {
    const int index = Index(slot);
    return index < 0
               ? nullptr
               : esp_partition_find_first(
                     ESP_PARTITION_TYPE_APP,
                     static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0 + index),
                     slot);
}
uint32_t Crc(const uint8_t* bytes, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFF;
}
const CoverHeader& Header(const CoverPixels& image) {
    return *reinterpret_cast<const CoverHeader*>(image.blob.get());
}
bool ValidHeader(const CoverHeader& h, const char* app_id) {
    bool magic = !memcmp(h.magic, kMagic, 8);
#if defined(AGENT_SYSTEM_SDGOODS_COMPAT)
    magic = magic || !memcmp(h.magic, "SDGCOVER", 8);
#endif
    return magic && h.version == 1 && h.format == 1 &&
           h.width == kCoverEdge && h.height == kCoverEdge && !h.reserved &&
           h.data_len == kCoverPixelBytes && memchr(h.app_id, 0, sizeof h.app_id) &&
           !strcmp(h.app_id, app_id);
}
bool Hash(const CoverPixels& image, const char* expected) {
    if (strnlen(expected, 65) != 64)
        return false;
    agentlink::ota::Sha256 hash;
    uint8_t digest[32];
    char hex[65]{};
    if (!hash.Start() || !hash.Update(image.blob.get(), kCoverBytes) || !hash.Finish(digest))
        return false;
    for (int i = 0; i < 32; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return !strcmp(hex, expected);
}
std::shared_ptr<CoverPixels> Allocate() {
    auto* raw =
        static_cast<uint8_t*>(heap_caps_malloc(kCoverBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw)
        return {};
    auto* pixels = new (std::nothrow) CoverPixels;
    if (!pixels) {
        heap_caps_free(raw);
        return {};
    }
    pixels->blob = std::shared_ptr<const uint8_t>(
        raw, [](const uint8_t* p) { heap_caps_free(const_cast<uint8_t*>(p)); });
    return std::shared_ptr<CoverPixels>(pixels);
}
esp_err_t Load(const esp_partition_t* part, const char* app_id,
               std::shared_ptr<const CoverPixels>& out) {
    if (!part || part->size < kCoverReserveBytes)
        return ESP_ERR_INVALID_SIZE;
    const size_t offset = part->size - kCoverReserveBytes;
    CoverHeader header{};
    esp_err_t err = esp_partition_read(part, offset, &header, sizeof header);
    if (err != ESP_OK)
        return err;
    if (!ValidHeader(header, app_id))
        return ESP_ERR_NOT_FOUND;
    auto image = Allocate();
    if (!image)
        return ESP_ERR_NO_MEM;
    auto* data = const_cast<uint8_t*>(image->blob.get());
    err = esp_partition_read(part, offset, data, kCoverBytes);
    if (err != ESP_OK)
        return err;
    if (!ValidHeader(Header(*image), app_id) ||
        Crc(image->Data(), kCoverPixelBytes) != Header(*image).crc32)
        return ESP_ERR_INVALID_CRC;
#if defined(AGENT_SYSTEM_SDGOODS_COMPAT)
    if (!memcmp(header.magic, "SDGCOVER", 8)) {
        // Vendor LVGL8 covers store RGB565 byte-swapped. Normalize the PSRAM
        // snapshot for our LVGL9 renderer; keep on-flash vendor bytes untouched.
        for (size_t i = kCoverHeaderBytes; i < kCoverHeaderBytes + kCoverEdge*kCoverEdge*2; i += 2)
            std::swap(data[i], data[i + 1]);
        auto* normalized = reinterpret_cast<CoverHeader*>(data);
        memcpy(normalized->magic, kMagic, 8);
        normalized->crc32 = Crc(image->Data(), kCoverPixelBytes);
    }
#endif
    out = std::move(image);
    return ESP_OK;
}
// Call with mutex held. Old shared snapshots stay alive until LVGL releases them.
void Publish(int index, std::shared_ptr<const CoverPixels> image) {
    if (!images[index] || memcmp(images[index]->blob.get(), image->blob.get(), kCoverBytes)) {
        images[index] = std::move(image);
        if (++revisions[index] == 0)
            ++revisions[index];
    }
    statuses[index].cached = true;
}
void SetStatus(int index, CoverState state, CoverStage stage, esp_err_t error) {
    std::lock_guard<std::mutex> lock(mutex);
    statuses[index].state = state;
    statuses[index].stage = stage;
    statuses[index].error = error;
}
esp_err_t CheckApp(const esp_partition_t* part, const char* id) {
    Descriptor d{};
    ImageFormat format;
    if (!ReadIdentity(part, &d, &format) || strcmp(d.app_id, id))
        return ESP_ERR_INVALID_VERSION;
    esp_partition_pos_t pos{part->address, part->size};
    esp_image_metadata_t meta{};
    const esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (err != ESP_OK)
        return err;
    // Older applications may already extend into the proposed cover region. Never erase their
    // bytes.
    return meta.image_len <= part->size - kCoverReserveBytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
struct Client {
    esp_http_client_handle_t value = nullptr;
    ~Client() {
        if (value) {
            esp_http_client_close(value);
            esp_http_client_cleanup(value);
        }
    }
};
}  // namespace

void Init() {
    ESP_LOGI(kTag, "slot-tail covers: 132x132 RGB565LE+A8, 56 KiB reserved per app");
}
esp_err_t Update(const char* slot, const CatalogApp& app) {
    const int index = Index(slot);
    const auto* part = Partition(slot);
    if (index < 0 || !part)
        return ESP_ERR_INVALID_ARG;
    if (!IsLauncher())
        return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> updating(update_mutex);
    const auto fail = [&](CoverStage stage, esp_err_t error) {
        SetStatus(index, CoverState::kFailed, stage, error);
        ESP_LOGW(kTag, "slot=%s stage=%d failed: %s", slot, static_cast<int>(stage),
                 esp_err_to_name(error));
        return error;
    };
    if (!app.cover_url[0]) {
        SetStatus(index, CoverState::kNoImage, CoverStage::kMetadata, ESP_OK);
        return ESP_OK;
    }
    const bool community_image =
        (app.community_play || app.firmware_id) && !strcmp(app.cover_format, "community-image");
    if ((!community_image &&
         (app.cover_size != kCoverBytes || strcmp(app.cover_format, kCoverFormat))) ||
        !ValidServiceUrl(app.cover_url))
        return fail(CoverStage::kMetadata, ESP_ERR_INVALID_ARG);
    const esp_err_t app_error = CheckApp(part, app.app_id);
    if (app_error != ESP_OK)
        return fail(CoverStage::kPublish, app_error);
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::shared_ptr<const CoverPixels> saved;
        if (Load(part, app.app_id, saved) == ESP_OK &&
            (community_image || Hash(*saved, app.cover_sha256))) {
            Publish(index, std::move(saved));
            statuses[index].state = CoverState::kReady;
            statuses[index].error = ESP_OK;
            statuses[index].stage = CoverStage::kRead;
            ESP_LOGD(kTag, "slot=%s verified cover hit", slot);
            return ESP_OK;
        }
    }
    auto image = Allocate();
    if (!image)
        return fail(CoverStage::kReceive, ESP_ERR_NO_MEM);
    ESP_LOGI(kTag, "download start: slot=%s app=%s bytes=%lu", slot, app.app_id,
             static_cast<unsigned long>(kCoverBytes));
    SetStatus(index, CoverState::kDownloading, CoverStage::kConnect, ESP_OK);
    auto* bytes = const_cast<uint8_t*>(image->blob.get());
    esp_err_t err;
#if defined(AGENT_SYSTEM_COMMUNITY)
    if (community_image) {
        err = community::FetchCover(app, bytes);
        if (err != ESP_OK)
            return fail(CoverStage::kValidate, err);
    } else
#endif
    {
        esp_http_client_config_t config{};
        config.url = app.cover_url;
        config.timeout_ms = 15000;
        config.user_agent = "Agent-Link-Development/1.0";
        config.buffer_size = 2048;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.disable_auto_redirect = true;
        Client client;
        client.value = esp_http_client_init(&config);
        if (!client.value)
            return fail(CoverStage::kConnect, ESP_ERR_NO_MEM);
        err = esp_http_client_open(client.value, 0);
        if (err != ESP_OK) {
            int tls = 0, flags = 0;
            (void)esp_http_client_get_and_clear_last_tls_error(client.value, &tls, &flags);
            ESP_LOGW(kTag, "slot=%s connect/TLS error=0x%x flags=0x%x", slot, tls, flags);
            return fail(CoverStage::kConnect, err);
        }
        SetStatus(index, CoverState::kDownloading, CoverStage::kHeaders, ESP_OK);
        const auto headers = esp_http_client_fetch_headers(client.value);
        const int status = esp_http_client_get_status_code(client.value);
        const auto length = esp_http_client_get_content_length(client.value);
        ESP_LOGI(kTag, "slot=%s HTTP=%d length=%lld expected=%lu", slot, status,
                 static_cast<long long>(length), static_cast<unsigned long>(kCoverBytes));
        if (headers < 0 || status != 200)
            return fail(CoverStage::kHeaders, ESP_ERR_INVALID_RESPONSE);
        if (length != kCoverBytes)
            return fail(CoverStage::kHeaders, ESP_ERR_INVALID_SIZE);
        SetStatus(index, CoverState::kDownloading, CoverStage::kReceive, ESP_OK);
        uint32_t received = 0;
        while (received < kCoverBytes) {
            const int count =
                esp_http_client_read(client.value, reinterpret_cast<char*>(bytes + received),
                                     std::min<uint32_t>(2048, kCoverBytes - received));
            if (count <= 0) {
                ESP_LOGW(kTag, "slot=%s receive stopped at %lu", slot,
                         static_cast<unsigned long>(received));
                return fail(CoverStage::kReceive, ESP_FAIL);
            }
            received += count;
        }
        if (!esp_http_client_is_complete_data_received(client.value))
            return fail(CoverStage::kReceive, ESP_ERR_INVALID_SIZE);
        SetStatus(index, CoverState::kDownloading, CoverStage::kValidate, ESP_OK);
        if (memcmp(Header(*image).magic, kMagic, 8) || !ValidHeader(Header(*image), app.app_id) ||
            Crc(image->Data(), kCoverPixelBytes) != Header(*image).crc32 ||
            !Hash(*image, app.cover_sha256))
            return fail(CoverStage::kValidate, ESP_ERR_INVALID_CRC);
    }
    SetStatus(index, CoverState::kDownloading, CoverStage::kPublish, ESP_OK);
    {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t offset = part->size - kCoverReserveBytes;
        // Download/validate before touching flash. The magic is the final commit;
        // power loss during erase/write leaves an unusable cover, never an invalid app.
        images[index].reset();
        statuses[index].cached = false;
        err = esp_partition_erase_range(part, offset, kCoverReserveBytes);
        if (err == ESP_OK)
            err = esp_partition_write(part, offset + 8, bytes + 8, kCoverBytes - 8);
        if (err == ESP_OK)
            err = esp_partition_write(part, offset, bytes, 8);
        std::shared_ptr<const CoverPixels> back;
        if (err == ESP_OK)
            err = Load(part, app.app_id, back);
        if (err == ESP_OK && memcmp(back->blob.get(), bytes, kCoverBytes))
            err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK)
            Publish(index, std::move(image));
    }
    if (err != ESP_OK)
        return fail(CoverStage::kPublish, err);
    SetStatus(index, CoverState::kReady, CoverStage::kPublish, ESP_OK);
    ESP_LOGI(kTag, "slot=%s cover saved and read back at 0x%lx", slot,
             static_cast<unsigned long>(part->address + part->size - kCoverReserveBytes));
    return ESP_OK;
}
esp_err_t Read(const char* slot, const char* app_id, Cover* cover) {
    const int index = Index(slot);
    if (index < 0 || !cover)
        return ESP_ERR_INVALID_ARG;
    *cover = {};
    std::lock_guard<std::mutex> lock(mutex);
    if (!images[index] || strcmp(Header(*images[index]).app_id, app_id)) {
        std::shared_ptr<const CoverPixels> image;
        const esp_err_t err = Load(Partition(slot), app_id, image);
        if (err != ESP_OK) {
            statuses[index].cached = false;
            if (err != ESP_ERR_NOT_FOUND) {
                statuses[index].state = CoverState::kFailed;
                statuses[index].stage = CoverStage::kRead;
                statuses[index].error = err;
            }
            return err;
        }
        Publish(index, std::move(image));
    }
    statuses[index].cached = true;
    if (statuses[index].state == CoverState::kPending)
        statuses[index].state = CoverState::kReady;
    cover->slot = index;
    cover->revision = revisions[index];
    return ESP_OK;
}
std::shared_ptr<const CoverPixels> Acquire(const Cover& cover) {
    std::lock_guard<std::mutex> lock(mutex);
    return cover.slot >= 0 && cover.slot < kAppSlotCount && revisions[cover.slot] == cover.revision
               ? images[cover.slot]
               : nullptr;
}
esp_err_t Status(const char* slot, CoverStatus* status) {
    std::lock_guard<std::mutex> lock(mutex);
    const int index = Index(slot);
    if (index < 0 || !status)
        return ESP_ERR_INVALID_ARG;
    *status = statuses[index];
    return ESP_OK;
}
void NotListed(const char* slot) {
    const int index = Index(slot);
    if (index >= 0)
        SetStatus(index, CoverState::kNotListed, CoverStage::kCatalog, ESP_ERR_NOT_FOUND);
}
void Forget(const char* slot) {
    std::lock_guard<std::mutex> lock(mutex);
    const int index = Index(slot);
    if (index >= 0) {
        images[index].reset();
        ++revisions[index];
        statuses[index] = {};
    }
}
}  // namespace agent_system::cover_cache
namespace agent_system {
std::shared_ptr<const CoverPixels> AcquireCover(const Cover& cover) {
    return cover_cache::Acquire(cover);
}
}  // namespace agent_system
