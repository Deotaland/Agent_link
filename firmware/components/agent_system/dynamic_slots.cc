#include "dynamic_slots.h"
#include "dynamic_layout_config.h"
#include "agent_system.h"
#include "image_identity.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_rom_md5.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "nvs.h"

namespace agent_system::dynamic_slots {
namespace {
EXT_RAM_BSS_ATTR uint8_t current[ALPT_TABLE_BYTES], scratch[ALPT_TABLE_BYTES],
    next_table[ALPT_TABLE_BYTES];
bool writable = false;
int Read(void*, uint32_t offset, void* out, size_t size) {
    return esp_flash_read(esp_flash_default_chip, out, offset, size);
}
int Erase(void*, uint32_t offset, size_t size) {
    return esp_flash_erase_region(esp_flash_default_chip, offset, size);
}
int Write(void*, uint32_t offset, const void* bytes, size_t size) {
    return esp_flash_write(esp_flash_default_chip, bytes, offset, size);
}
int Md5(const void* data, size_t size, uint8_t out[16]) {
    md5_context_t context;
    esp_rom_md5_init(&context);
    esp_rom_md5_update(&context, data, size);
    esp_rom_md5_final(out, &context);
    return 0;
}
const alpt_io io{nullptr, Read, Erase, Write, Md5};
bool MatchingBootloader() {
    const alpt_boot_marker expected{"ALPTGUARD01", 1, ALPT_POLICY_ID};
    uint8_t bytes[1024 + sizeof expected];
    for (uint32_t offset = 0; offset < 0x8000; offset += 1024) {
        const size_t size = std::min<size_t>(sizeof bytes, 0x8000 - offset);
        if (Read(nullptr, offset, bytes, size))
            return false;
        for (size_t i = 0; i + sizeof expected <= size; i += 4)
            if (!memcmp(bytes + i, &expected, sizeof expected))
                return true;
    }
    return false;
}
esp_partition_t Partition(const alpt_entry& entry) {
    const auto* base = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    esp_partition_t part{};
    if (base)
        part = *base;
    part.address = entry.offset;
    part.size = entry.size;
    part.subtype = static_cast<esp_partition_subtype_t>(entry.subtype);
    snprintf(part.label, sizeof part.label, "%s", entry.label);
    return part;
}
esp_err_t Publish(const uint8_t* table) {
    const auto* factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                   ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (!factory || esp_ota_set_boot_partition(factory) != ESP_OK)
        return ESP_FAIL;
    const int result = alpt_commit(&alpt_config, &io, table, scratch);
    writable = false;  // IDF's partition cache must be reloaded even after an uncertain commit.
    if (result != ALPT_OK)
        ESP_LOGW("agent_system.slots", "Layout publication needs recovery: %d", result);
    return kRestartRequired;
}
}  // namespace
bool Writable() {
    return writable;
}
esp_err_t Init(bool launcher) {
    writable = false;
    if (Read(nullptr, alpt_config.table, current, sizeof current))
        return ESP_FAIL;
    const auto* guard =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ALPT_GUARD_PARTITION);
    if (!guard || esp_flash_encryption_enabled() || !MatchingBootloader() ||
        !alpt_validate(&alpt_config, &io, current, 1)) {
        ESP_LOGW("agent_system.slots",
                 "Dynamic writes unavailable; install the matching full USB package");
        return ESP_OK;  // An app-only upgrade may still inspect/launch existing V3 images.
    }
    writable = launcher;
    if (alpt_validate(&alpt_config, &io, current, 0) || !launcher)
        return ESP_OK;
    alpt_entry slots[16];
    for (unsigned i = 0; i < 16; ++i)
        alpt_empty(&alpt_config, i, &slots[i]);
    const auto* initial =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    esp_image_metadata_t metadata{};
    Descriptor identity{};
    ImageFormat format;
    esp_partition_pos_t position{alpt_config.pool_start,
                                 alpt_config.pool_end - alpt_config.pool_start};
    if (initial && ReadIdentity(initial, &identity, &format) &&
        strcmp(identity.app_id, "agent.launcher") &&
        esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) == ESP_OK &&
        metadata.image_len <= initial->size - kCoverReserveBytes) {
        slots[0].offset = initial->address;
        slots[0].size = (metadata.image_len + kCoverReserveBytes + 0xFFFF) & ~0xFFFFu;
        // A USB seed may already carry a slot-tail cover. Copy before shrinking;
        // a power cut leaves the old location intact under the old table.
        if (slots[0].size < initial->size) {
            auto destination = Partition(slots[0]);
            for (size_t offset = 0; offset < kCoverReserveBytes; offset += 4096) {
                esp_err_t err = esp_partition_read(
                    initial, initial->size - kCoverReserveBytes + offset, scratch, 4096);
                if (err != ESP_OK)
                    return err;
                err = esp_partition_erase_range(
                    &destination, destination.size - kCoverReserveBytes + offset, 4096);
                if (err != ESP_OK)
                    return err;
                err = esp_partition_write(
                    &destination, destination.size - kCoverReserveBytes + offset, scratch, 4096);
                if (err != ESP_OK)
                    return err;
            }
        }
        // Separate-file full USB writes preserve NVS. A valid seed replaces any
        // previous first-slot transaction, including a stale unavailable marker.
        nvs_handle_t handle;
        esp_err_t err = nvs_open_from_partition(kMetadataPartition, "al_apps", NVS_READWRITE, &handle);
        if (err != ESP_OK)
            return err;
        err = nvs_set_u8(handle, slots[0].label, 1);
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
        if (err != ESP_OK)
            return err;
    }
    if (alpt_build(&alpt_config, &io, slots, next_table) != ALPT_OK)
        return ESP_ERR_INVALID_STATE;
    return Publish(next_table);
}
esp_err_t Allocate(uint32_t bytes, uint32_t free_mask, unsigned limit, Plan* plan) {
    if (!writable)
        return kNeedsFullPackage;
    if (Read(nullptr, alpt_config.table, current, sizeof current))
        return ESP_FAIL;
    unsigned index;
    alpt_entry entry;
    const int result = alpt_allocate(&alpt_config, &io, current, bytes, kCoverReserveBytes,
                                     free_mask, limit, plan->table, &index, &entry);
    if (result == ALPT_NONE)
        return limit < 16 && !(free_mask & 3) ? kLegacySlotsFull : kNoAppSpace;
    if (result != ALPT_OK)
        return ESP_ERR_INVALID_STATE;
    plan->partition = Partition(entry);
    return ESP_OK;
}
esp_err_t Commit(const Plan& plan) {
    return Publish(plan.table);
}
esp_err_t Release(const char* label) {
    if (!writable)
        return kNeedsFullPackage;
    if (Read(nullptr, alpt_config.table, current, sizeof current) ||
        !alpt_validate(&alpt_config, &io, current, 0))
        return ESP_ERR_INVALID_STATE;
    alpt_entry slots[16];
    if (alpt_slots(current, slots))
        return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 16; ++i)
        if (!strcmp(slots[i].label, label)) {
            if (alpt_config.reserved_mask & (1u << i))
                return ESP_ERR_INVALID_ARG;
            alpt_empty(&alpt_config, i, &slots[i]);
            if (alpt_build(&alpt_config, &io, slots, next_table))
                return ESP_ERR_INVALID_STATE;
            return Publish(next_table);
        }
    return ESP_ERR_NOT_FOUND;
}
}  // namespace agent_system::dynamic_slots
