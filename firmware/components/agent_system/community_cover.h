#pragma once
#include "agent_system.h"
namespace agent_system::community {
// Output is the existing bounded ALCOVER1 representation, ready for CRC/readback.
esp_err_t DecodeCover(const uint8_t* data, size_t size, const char* app_id, uint8_t* output);
esp_err_t FetchCover(const CatalogApp& app, uint8_t* output);
}  // namespace agent_system::community
