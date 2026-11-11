#include <cstddef>
#include "esp_heap_caps.h"
extern "C" void* agl_webp_malloc(size_t bytes) {
    return bytes <= 4 * 1024 * 1024 ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                    : nullptr;
}
extern "C" void* agl_webp_calloc(size_t count, size_t bytes) {
    return !bytes || count <= 4 * 1024 * 1024 / bytes
               ? heap_caps_calloc(count, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
               : nullptr;
}
extern "C" void agl_webp_free(void* data) {
    heap_caps_free(data);
}
