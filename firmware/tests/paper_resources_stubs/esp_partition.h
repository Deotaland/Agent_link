#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
struct esp_partition_t { size_t size; };
using esp_partition_mmap_handle_t = uint32_t;
enum { ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ESP_PARTITION_MMAP_DATA };
const esp_partition_t* esp_partition_find_first(int, int, const char*);
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
esp_err_t esp_partition_mmap(const esp_partition_t*, size_t, size_t, int, const void**, esp_partition_mmap_handle_t*);
void esp_partition_munmap(esp_partition_mmap_handle_t);
const char* esp_err_to_name(esp_err_t);
