#include <cstddef>
#include "esp_heap_caps.h"
void* agl_png_malloc(size_t bytes) {
    return bytes <= 4 * 1024 * 1024 ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                    : nullptr;
}
void* agl_png_realloc(void* data, size_t bytes) {
    return bytes <= 4 * 1024 * 1024
               ? heap_caps_realloc(data, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
               : nullptr;
}
void agl_png_free(void* data) {
    heap_caps_free(data);
}
