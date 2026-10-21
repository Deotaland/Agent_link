#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
constexpr int MALLOC_CAP_INTERNAL = 4;
inline void* heap_caps_calloc(size_t count, size_t size, int) { return std::calloc(count, size); }
inline void* heap_caps_malloc(size_t size, int) { return std::malloc(size); }
inline size_t heap_caps_get_free_size(int) { return 100000; }
inline size_t heap_caps_get_largest_free_block(int) { return 50000; }
