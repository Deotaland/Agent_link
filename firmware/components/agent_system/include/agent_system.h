#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "agent_cover.h"

namespace agent_system {
#ifndef AGENT_SYSTEM_SLOT_COUNT
#define AGENT_SYSTEM_SLOT_COUNT 3
#endif
constexpr int kAppSlotCount = AGENT_SYSTEM_SLOT_COUNT;
static_assert(kAppSlotCount > 0 && kAppSlotCount <= 16);
constexpr uint32_t kLayout = AGENT_SYSTEM_LAYOUT;
constexpr size_t kMaxApps = 8;
constexpr esp_err_t kSlotsFull = 0x7F01;
constexpr esp_err_t kAlreadyInstalled = 0x7F02;
constexpr const char* kBoard = AGENT_SYSTEM_BOARD_MODEL;
constexpr const char* kSdk = "0.1.0";
// Stored immediately after esp_app_desc at image offset 0x120. Keep this ABI stable.
struct Descriptor {
    char magic[8];
    uint32_t schema;
    uint32_t layout;
    char app_id[32];
    char name[64];
    char board[48];
    char sdk[16];
};
static_assert(sizeof(Descriptor) == 176);
enum class ImageFormat { kAgentLink, kSdgoods };
struct App {
    Descriptor descriptor{};
    char version[32]{};
    char slot[17]{};
    uint32_t size = 0;
    ImageFormat format = ImageFormat::kAgentLink;
};
struct CatalogApp {
    char app_id[32]{}, name[64]{}, version[32]{};
    char url[256]{}, sha256[65]{};
    uint32_t size = 0;
    ImageFormat format = ImageFormat::kAgentLink;
    char cover_url[256]{}, cover_sha256[65]{};
    char cover_format[32]{};
    uint32_t cover_size = 0;
};
enum class CoverState { kPending, kDownloading, kReady, kNoImage, kNotListed, kFailed };
enum class CoverStage {
    kNone,
    kMount,
    kCatalog,
    kMetadata,
    kConnect,
    kHeaders,
    kReceive,
    kValidate,
    kPublish,
    kBinding,
    kRead
};
struct CoverStatus {
    CoverState state = CoverState::kPending;
    CoverStage stage = CoverStage::kNone;
    esp_err_t error = ESP_OK;
    bool cached = false;  // An older usable image may remain after a failed update.
};
using Progress = void (*)(uint32_t received, uint32_t total, void* context);
const Descriptor& Current();
uint32_t Revision();  // Changes after an installed-app list mutation.
const char* SettingsPartition();
// Initializes system settings. SDGOODS compatibility may reinitialize invalid
// vendor default NVS; Agent Link settings remain in their separate partition.
esp_err_t Init();
// USB apps run independently in the factory boot partition. OTA-slot apps
// confirm rollback readiness and arm a verified launcher when one is present.
esp_err_t Ready();
bool IsLauncher();
bool CanReturnToLauncher();  // Cached by Ready; false before readiness or in standalone mode.
esp_err_t ReturnToLauncher();
esp_err_t List(App* apps, size_t capacity, size_t* count);
esp_err_t ReadCover(const char* slot, Cover* cover);
esp_err_t GetCoverStatus(const char* slot, CoverStatus* status);  // Nonblocking network-wise.
esp_err_t Launch(const char* slot);
esp_err_t Remove(const char* slot);
esp_err_t CatalogUrl(char* out, size_t capacity);
esp_err_t SetCatalogUrl(const char* url);
bool ValidServiceUrl(const char* url);  // HTTPS, or explicit private-LAN HTTP test service.
esp_err_t FetchCatalog(CatalogApp* apps, size_t capacity, size_t* count);
esp_err_t SyncCovers(const CatalogApp* apps, size_t count);  // Call on a worker after FetchCatalog.
esp_err_t Install(const CatalogApp& app, Progress progress = nullptr, void* context = nullptr);
esp_err_t StartSettingsServer();  // Port 8081: catalog configuration, no WiFi passwords.
}  // namespace agent_system
