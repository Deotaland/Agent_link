#pragma once
#include <cstdint>
#include "esp_partition.h"
#include "agent_partition_store.h"

namespace agent_system::dynamic_slots {
struct Plan {
    esp_partition_t partition{};
    uint8_t table[ALPT_TABLE_BYTES]{};
};
esp_err_t Init(bool launcher);
bool Writable();
esp_err_t Allocate(uint32_t bytes, uint32_t free_mask, unsigned limit, Plan* plan);
esp_err_t Commit(const Plan& plan);
esp_err_t Release(const char* label);
}  // namespace agent_system::dynamic_slots
