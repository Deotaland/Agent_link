#pragma once

#include "nvs.h"
#include "nvs_flash.h"

// A vendor application may erase default NVS. Boards sharing unmodified vendor
// images keep device identity, provisioning and platform credentials separately.
namespace agentlink::storage {
#ifdef AGENT_LINK_NVS_PARTITION
inline esp_err_t Init() { return nvs_flash_init_partition(AGENT_LINK_NVS_PARTITION); }
inline esp_err_t Erase() { return nvs_flash_erase_partition(AGENT_LINK_NVS_PARTITION); }
inline esp_err_t Open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    return nvs_open_from_partition(AGENT_LINK_NVS_PARTITION, name, mode, handle);
}
#else
inline esp_err_t Init() { return nvs_flash_init(); }
inline esp_err_t Erase() { return nvs_flash_erase(); }
inline esp_err_t Open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    return nvs_open(name, mode, handle);
}
#endif
}  // namespace agentlink::storage
