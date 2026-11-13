#pragma once
#include "agent_system.h"
#include <cstdio>
#include <cstring>
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
#include "dynamic_layout_config.h"
#endif

namespace agent_system {
inline void SlotLabel(char (&label)[16], unsigned index) {
#if defined(AGENT_SYSTEM_DYNAMIC_SLOTS)
    alpt_slot_label(&alpt_config, label, index);
#else
    snprintf(label, sizeof label, "ota_%u", index);
#endif
}
inline int SlotIndex(const char* label) {
    if (!label) return -1;
    for (unsigned i = 0; i < kAppSlotCount; ++i) {
        char expected[16];
        SlotLabel(expected, i);
        if (!strcmp(label, expected)) return static_cast<int>(i);
    }
    return -1;
}
}  // namespace agent_system
