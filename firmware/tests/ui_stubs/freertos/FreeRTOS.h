#pragma once
#include <cstddef>
#include <cstring>
using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0, pdTRUE = 1, pdPASS = 1;
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
inline size_t strlcpy(char* out, const char* in, size_t size) {
    const size_t length = std::strlen(in);
    if (size) { const size_t copy = length < size - 1 ? length : size - 1; std::memcpy(out, in, copy); out[copy] = 0; }
    return length;
}
