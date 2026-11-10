#pragma once
#include <cstddef>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
void* heap_caps_malloc(size_t bytes, unsigned caps);
void heap_caps_free(void* pointer);
