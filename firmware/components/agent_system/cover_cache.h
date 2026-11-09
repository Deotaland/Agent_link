#pragma once
#include "agent_system.h"

namespace agent_system::cover_cache {
void Init();
esp_err_t Update(const char* slot, const CatalogApp& app);
esp_err_t Read(const char* slot, const char* app_id, Cover* cover);
esp_err_t Status(const char* slot, CoverStatus* status);
void NotListed(const char* slot);
void Forget(const char* slot);
}  // namespace agent_system::cover_cache
