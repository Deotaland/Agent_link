#pragma once
#include "agent_system.h"
#include "esp_http_client.h"
#include <string>
#if defined(AGENT_SYSTEM_DOWNLOAD_WIFI)
#include "wifi_download.h"
#endif

namespace agent_system::community {
const char* DefaultCatalog();
bool IsCatalog(const char* url);
bool IsOrigin(const char* url);
esp_err_t Fetch(const char* url, unsigned page, CatalogApp* apps, size_t capacity, size_t* count,
                CatalogPage* pagination);
esp_err_t Detail(uint64_t play, uint64_t version, CatalogApp* app,
                 InstallCancel cancel = nullptr, void* context = nullptr);
esp_err_t LookupCode(const char* code, CatalogApp* app,
                     InstallCancel cancel = nullptr, void* context = nullptr);
esp_err_t PrepareDownload(CatalogApp* app, InstallCancel cancel = nullptr, void* context = nullptr);
esp_err_t SaveBinding(const char* slot, const CatalogApp& app);
void ReadBinding(App* app);
void Error(StoreError* out);
esp_err_t Fail(const char* message, int status = 0);
// Certificate errors are permanent for this attempt; transient connection errors
// retain their retry policy. The detailed reason is available through Error().
esp_err_t ConnectionError(esp_http_client_handle_t handle, esp_err_t error);

// Public files never carry account/device headers. Redirects are bounded and
// checked against the same transport policy before opening the next URL.
class File {
   public:
    ~File();
    esp_err_t Open(const char* url);
    // Installer only: wait longer for headers and retry before reading/writing firmware.
    esp_err_t OpenDownload(const char* url, InstallCancel cancel = nullptr, void* context = nullptr);
    esp_http_client_handle_t handle = nullptr;

   private:
    esp_err_t OpenWithTimeout(const char* url, int header_timeout_ms);
    static esp_err_t Event(esp_http_client_event_t* event);
#if defined(AGENT_SYSTEM_DOWNLOAD_WIFI)
    agentlink::wifi::DownloadPower download_power_;
#endif
    std::string location_;
    unsigned retry_after_ = 0;
};
}  // namespace agent_system::community
