#include "paper_resources.h"
#include "esp_log.h"
#include "esp_partition.h"

namespace paper_resources {
bool Init() {
    if (Ready()) return true;
    constexpr const char* tag = "paper.resources";
    const auto* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"font_cjk");
    Header header{};
    if (!partition || esp_partition_read(partition,0,&header,sizeof header) != ESP_OK ||
        header.total_size < sizeof header || header.total_size > partition->size) {
        ESP_LOGE(tag,"Shared resources missing: install the Paper 4IN0 USB package");
        return false;
    }
    const void* data = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    const auto error = esp_partition_mmap(partition,0,header.total_size,ESP_PARTITION_MMAP_DATA,&data,&handle);
    if (error != ESP_OK) { ESP_LOGE(tag,"Cannot map shared resources: %s",esp_err_to_name(error)); return false; }
    if (!Install(data,header.total_size)) {
        ESP_LOGE(tag,"Shared resources rejected: %s",Error());
        esp_partition_munmap(handle);
        return false;
    }
    // Keep mapping for the application lifetime; glyphs and candidates reference flash directly.
    ESP_LOGI(tag,"Shared resources: %u bytes, %u glyphs, %u dictionary entries",
             unsigned(header.total_size),unsigned(header.glyph_count),unsigned(header.dictionary_count));
    return true;
}
}
