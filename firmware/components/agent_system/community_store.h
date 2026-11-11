#pragma once
#include "agent_system.h"
#include "esp_http_client.h"
#include <string>

namespace agent_system::community {
const char* DefaultCatalog();
bool IsCatalog(const char* url);
bool IsOrigin(const char* url);
esp_err_t Fetch(const char* url, unsigned page, CatalogApp* apps, size_t capacity, size_t* count,
                CatalogPage* pagination);
esp_err_t Detail(uint64_t play, uint64_t version, CatalogApp* app);
esp_err_t PrepareDownload(CatalogApp* app);
esp_err_t SaveBinding(const char* slot, const CatalogApp& app);
void ReadBinding(App* app);
void Error(StoreError* out);
esp_err_t Fail(const char* message, int status = 0);

// Public files never carry account/device headers. Redirects are bounded and
// checked against the same transport policy before opening the next URL.
class File {
   public:
    ~File();
    esp_err_t Open(const char* url);
    esp_http_client_handle_t handle = nullptr;

   private:
    static esp_err_t Event(esp_http_client_event_t* event);
    std::string location_;
    unsigned retry_after_ = 0;
};
}  // namespace agent_system::community
