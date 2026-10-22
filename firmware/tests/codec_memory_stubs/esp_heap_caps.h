#pragma once
#include <stddef.h>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;
void* codec_test_caps_malloc(size_t, unsigned);
inline void* heap_caps_malloc(size_t bytes, unsigned caps) { return codec_test_caps_malloc(bytes, caps); }
