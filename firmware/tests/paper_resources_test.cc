#include "paper_resources_host.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include "esp_partition.h"

namespace {
std::vector<uint8_t> flash;
bool found = true, readable = true, mappable = true;
int maps = 0, unmaps = 0;
esp_partition_t partition{0x230000};
}
const esp_partition_t* esp_partition_find_first(int, int, const char* label) {
    assert(!std::strcmp(label,"font_cjk")); return found ? &partition : nullptr;
}
esp_err_t esp_partition_read(const esp_partition_t* part, size_t offset, void* output, size_t size) {
    assert(part == &partition && offset+size <= flash.size());
    if (!readable) return ESP_FAIL;
    std::memcpy(output,flash.data()+offset,size); return ESP_OK;
}
esp_err_t esp_partition_mmap(const esp_partition_t* part, size_t offset, size_t size, int,
                            const void** data, esp_partition_mmap_handle_t* handle) {
    assert(part == &partition && offset == 0 && size <= flash.size()); ++maps;
    if (!mappable) return ESP_FAIL;
    *data = flash.data(); *handle = 123; return ESP_OK;
}
void esp_partition_munmap(esp_partition_mmap_handle_t handle) { assert(handle == 123); ++unmaps; }
const char* esp_err_to_name(esp_err_t) { return "simulated failure"; }

int main(int argc, char** argv) {
    using namespace paper_resources;
    assert(argc == 2);
    auto bytes = LoadPaperResources(argv[1]);
    const auto original = bytes;
    Header h; std::memcpy(&h,bytes.data(),sizeof h);
    const auto& font = Font();
    assert(h.glyph_count == 7813 && Dictionary().dictionary_count == 9216);
    assert(font.glyph_count == raster_cjk_14.glyph_count && font.sparse_count == raster_cjk_14.sparse_count);
    assert(!std::memcmp(font.bitmap,raster_cjk_14.bitmap,h.bitmap_size));
    assert(!std::memcmp(font.glyphs,raster_cjk_14.glyphs,h.glyph_count*sizeof(raster_glyph_t)));
    assert(!std::memcmp(font.sparse,raster_cjk_14.sparse,h.sparse_count*sizeof(uint16_t)));
    assert(Crc32("123456789",9) == 0xcbf43926u);
    View view; const char* error = nullptr;
    const auto invalid = [&]() { assert(!Parse(bytes.data(),bytes.size(),view,error) && error && !view.dictionary_count); };
    for (size_t size : {size_t(0),sizeof(Header)-1,bytes.size()-1})
        assert(!Parse(bytes.data(),size,view,error));
    assert(!Parse(bytes.data()+1,bytes.size()-1,view,error));
    bytes[0] ^= 1; invalid(); bytes = original;
    bytes[8] = 2; invalid(); bytes = original;
    bytes[96] ^= 1; invalid(); bytes = original;
    const auto patch = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data()+offset,&value,4); };
    const auto crc = [&]() { patch(offsetof(Header,payload_crc32),Crc32(bytes.data()+sizeof h,bytes.size()-sizeof h)); };
    patch(offsetof(Header,glyphs_offset),h.bitmap_offset); invalid(); bytes = original;
    patch(offsetof(Header,dictionary_count),std::numeric_limits<uint32_t>::max()); invalid(); bytes = original;
    patch(offsetof(Header,reserved),1); invalid(); bytes = original;
    patch(h.glyphs_offset,h.bitmap_size+1); crc(); invalid(); bytes = original;
    bytes[h.sparse_offset+2] = bytes[h.sparse_offset]; bytes[h.sparse_offset+3] = bytes[h.sparse_offset+1];
    crc(); invalid(); bytes = original;
    patch(h.dictionary_offset,h.strings_size); crc(); invalid(); bytes = original;
    patch(h.dictionary_offset+4,h.strings_size-1); bytes.back() = 'x'; crc(); invalid(); bytes = original;
    patch(h.dictionary_offset+8,0); crc(); invalid(); bytes = original;
    assert(!Install(nullptr,0) && !Ready() && Dictionary().dictionary_count == 0);
    assert(&Font() == &raster_montserrat_14);
    flash = original;
    found = false; assert(!Init() && !Ready() && maps == 0); found = true;
    readable = false; assert(!Init() && maps == 0); readable = true;
    std::memset(flash.data()+offsetof(Header,total_size),0xff,4);
    assert(!Init() && maps == 0); flash = original;
    mappable = false; assert(!Init() && maps == 1 && unmaps == 0); mappable = true;
    flash[96] ^= 1; assert(!Init() && maps == 2 && unmaps == 1 && !Ready()); flash = original;
    assert(Init() && Ready() && maps == 3 && unmaps == 1);
    assert(Init() && maps == 3 && unmaps == 1);
    const auto entry = Dictionary().Entry(0);
    assert(reinterpret_cast<const uint8_t*>(entry.text) >= flash.data());
    assert(reinterpret_cast<const uint8_t*>(entry.text) < flash.data()+flash.size());
    std::puts("PASS: identical font; malformed resources rejected; missing/read/map/CRC failures; persistent flash mapping and Latin fallback");
}
