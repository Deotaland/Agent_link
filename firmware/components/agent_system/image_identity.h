#pragma once
#include <cstdio>
#include <cstring>
#include "agent_system.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

namespace agent_system {
inline bool VendorProject(const char* name, size_t capacity) {
    if (!name[0] || !memchr(name, 0, capacity) || !strcmp(name, "SDGOODS_LAUNCHER") ||
        !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    for (const char* p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
            return false;
    return true;
}
// Internal identity decoder also used by cover publication. Verification of the
// complete ESP image is mandatory at the caller before publishing or launching.
bool DecodeIdentity(const esp_app_desc_t& idf, const Descriptor& raw,
                    Descriptor* out, ImageFormat* format);
inline bool ReadIdentity(const esp_partition_t* part, Descriptor* out, ImageFormat* format) {
    esp_app_desc_t idf{};
    Descriptor raw{};
    return esp_ota_get_partition_description(part, &idf) == ESP_OK &&
           esp_partition_read(part, 0x120, &raw, sizeof raw) == ESP_OK &&
           DecodeIdentity(idf, raw, out, format);
}
}  // namespace agent_system
