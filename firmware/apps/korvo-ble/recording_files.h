#pragma once

#include <cstdint>
#include "esp_err.h"

namespace recording_files {
esp_err_t ScanLastSequence(const char* directory, uint32_t* last);
}
