#include "cjk_font.h"

#include <cstring>

#include "esp_log.h"

#if LV_USE_TINY_TTF

#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

namespace {

constexpr const char* TAG = "cjk_font";
constexpr char        kMagic[8] = {'A', 'G', 'L', 'K', 'T', 'T', 'F', '1'};
constexpr uint32_t    kVersion = 1;

// Little-endian, as make_cjk_font.py packs it.
struct ImageHeader {
    char     magic[8];
    uint32_t version;
    uint32_t length;     // TTF bytes after the header
    uint32_t crc32;      // standard CRC-32 of those bytes
    uint8_t  reserved[12];
};
static_assert(sizeof(ImageHeader) == 32, "the image header is 32 bytes");

}  // namespace

lv_font_t* LoadCjkFont(const char* label, int32_t px, uint32_t cache_glyphs) {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!part) {
        ESP_LOGW(TAG, "no '%s' partition: Latin text only", label);
        return nullptr;
    }
    ImageHeader h = {};
    if (esp_partition_read(part, 0, &h, sizeof h) != ESP_OK) return nullptr;
    if (memcmp(h.magic, kMagic, sizeof kMagic) != 0 || h.version != kVersion || h.length == 0 ||
        h.length > part->size - sizeof h) {
        // The partition still holds whatever was there before (on the rorolee PCBs, the
        // production animation pack), not a font image.
        ESP_LOGW(TAG, "no CJK font image in '%s': Latin text only (flash cjk_font.bin there)", label);
        return nullptr;
    }

    const void* map = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    const esp_err_t r = esp_partition_mmap(part, 0, sizeof h + h.length, ESP_PARTITION_MMAP_DATA,
                                           &map, &handle);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "cannot map the CJK font (%u KB): %s", static_cast<unsigned>(h.length / 1024),
                 esp_err_to_name(r));
        return nullptr;
    }
    const uint8_t* ttf = static_cast<const uint8_t*>(map) + sizeof h;

    // A half-written image would send TinyTTF's parser off the end of the mapping; check it whole
    // once (~0.2 s for 2 MB) rather than trust the header.
    const int64_t t0 = esp_timer_get_time();
    if (esp_rom_crc32_le(0, ttf, h.length) != h.crc32) {
        ESP_LOGE(TAG, "CJK font image in '%s' is damaged (CRC mismatch): flash it again", label);
        esp_partition_munmap(handle);
        return nullptr;
    }
    lv_font_t* font = lv_tiny_ttf_create_data_ex(ttf, h.length, px, LV_FONT_KERNING_NONE, cache_glyphs);
    if (!font) {
        ESP_LOGE(TAG, "TinyTTF rejected the CJK font image");
        esp_partition_munmap(handle);
        return nullptr;
    }
    ESP_LOGI(TAG, "CJK font: %u KB from '%s', %d px (checked in %d ms)",
             static_cast<unsigned>(h.length / 1024), label, static_cast<int>(px),
             static_cast<int>((esp_timer_get_time() - t0) / 1000));
    return font;   // the mapping stays: TinyTTF reads glyph outlines from it on demand
}

#else  // !LV_USE_TINY_TTF

lv_font_t* LoadCjkFont(const char* label, int32_t /*px*/, uint32_t /*cache_glyphs*/) {
    ESP_LOGW("cjk_font", "built without LV_USE_TINY_TTF: no CJK font from '%s'", label);
    return nullptr;
}

#endif  // LV_USE_TINY_TTF
