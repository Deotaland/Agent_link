#pragma once
#include <stddef.h>
#include "sdkconfig.h"

namespace agentlink::cloud_limits {
// C3 has no PSRAM. Keep complete-message validation, with bounded queues and
// a smaller HTTP page instead of reserving the S3's external-memory budget.
#if CONFIG_IDF_TARGET_ESP32C3
constexpr size_t kTxDepth = 18;
constexpr size_t kUplinkBatchPackets = 4;
constexpr size_t kReadBytes = 2048;
constexpr size_t kMessageBytes = 16 * 1024;
constexpr size_t kDownlinkBytes = 16 * 1024;
constexpr size_t kDownlinkDepth = 4;
constexpr size_t kListBytes = 12 * 1024;
constexpr unsigned kListPageSize = 3;
#else
constexpr size_t kTxDepth = 102;
constexpr size_t kUplinkBatchPackets = 8;
constexpr size_t kReadBytes = 8192;
constexpr size_t kMessageBytes = 128 * 1024;
constexpr size_t kDownlinkBytes = 256 * 1024;
constexpr size_t kDownlinkDepth = 64;
constexpr size_t kListBytes = 96 * 1024;
constexpr unsigned kListPageSize = 20;
#endif
}  // namespace agentlink::cloud_limits
