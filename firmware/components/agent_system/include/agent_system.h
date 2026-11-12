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
// Geometry was published (or needs journal recovery). Restart Factory before
// any further install, removal or launch; IDF caches partition pointers.
constexpr esp_err_t kRestartRequired = 0x7F03;
constexpr esp_err_t kNeedsFullPackage = 0x7F04;
constexpr esp_err_t kNoAppSpace = 0x7F05;
constexpr esp_err_t kLegacySlotsFull = 0x7F06;
constexpr esp_err_t kStoreError = 0x7F07;
constexpr esp_err_t kUnsupportedPackage = 0x7F08;
constexpr esp_err_t kCancelled = 0x7F09;
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
struct SlotCapability {
    char magic[8];
    uint32_t version, slots, flags;
};
static_assert(sizeof(SlotCapability) == 20);
enum class ImageFormat { kAgentLink, kSdgoods };
struct App {
    Descriptor descriptor{};
    char version[32]{};
    char slot[17]{};
    uint32_t size = 0;
    ImageFormat format = ImageFormat::kAgentLink;
    uint64_t community_play = 0, community_version = 0;
    char release_version[32]{};
};
struct CatalogApp {
    char app_id[32]{}, name[64]{}, version[32]{};
    char url[1024]{}, sha256[65]{};
    uint32_t size = 0;
    ImageFormat format = ImageFormat::kAgentLink;
    char cover_url[1024]{}, cover_sha256[65]{};
    char cover_format[32]{};
    uint32_t cover_size = 0;
    uint64_t community_play = 0, community_version = 0;
    enum class Package { kApplication, kFullPackage, kTooLarge } package = Package::kApplication;
    char filename[96]{};
    char release_version[32]{};
    char download_code[7]{};
};
struct CatalogPage {
    unsigned page = 1;
    uint32_t total = 0;
    bool has_more = false;
};
struct StoreError {
    int http_status = 0;
    unsigned retry_after = 0;
    char code[48]{}, message[384]{}, request_id[80]{};
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
enum class InstallStage {
    kWaitCover,
    kInspect,
    kTicket,
    kConnect,
    kHeader,
    kAllocate,
    kErase,
    kDownload,
    kVerify,
    kSave,
    kPublish,
    kDone
};
struct InstallStatus {
    InstallStage stage = InstallStage::kInspect;
    esp_err_t error = ESP_OK;
    uint32_t received = 0, total = 0, elapsed_ms = 0, read_ms = 0, write_ms = 0;
    int http_status = 0;
    int network_errno = 0;
    char slot[17]{}, app_id[32]{};
};
using InstallObserver = void (*)(const InstallStatus&, void*);
using InstallCancel = bool (*)(void*);
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
// HTTPS, private-LAN HTTP, or the board's explicitly configured production origin.
bool ValidServiceUrl(const char* url);
esp_err_t FetchCatalog(CatalogApp* apps, size_t capacity, size_t* count);
esp_err_t FetchCatalogPage(CatalogApp* apps, size_t capacity, size_t* count, unsigned page,
                           CatalogPage* pagination);
bool CommunityCatalog();
// Six ASCII digits, including leading zeroes. Metadata only; no download ticket.
esp_err_t FetchDownloadCode(const char* code, CatalogApp* app);
void LastStoreError(StoreError* error);
esp_err_t SyncStoreCovers();
esp_err_t SyncCovers(const CatalogApp* apps, size_t count);  // Call on a worker after FetchCatalog.
// Observer/cancel run on the install worker, never on the display task. Cancellation
// is checked between I/O operations, before the atomic layout publication begins.
esp_err_t Install(const CatalogApp& app, Progress progress = nullptr, void* context = nullptr,
                  InstallStatus* result = nullptr, InstallObserver observer = nullptr,
                  InstallCancel cancel = nullptr);
esp_err_t StartSettingsServer();  // Port 8081: catalog configuration, no WiFi passwords.
}  // namespace agent_system
