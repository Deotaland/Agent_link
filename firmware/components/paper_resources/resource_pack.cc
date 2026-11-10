#include "paper_resources.h"
#include <cstring>

namespace paper_resources {
namespace {
View current;
bool ready = false;
const char* last_error = "resource pack not initialized";
bool StringAt(const char* strings, size_t size, uint32_t offset, size_t limit, bool pinyin) {
    if (offset >= size) return false;
    const auto* end = static_cast<const char*>(std::memchr(strings+offset,0,size-offset));
    if (!end || end == strings+offset || size_t(end-strings-offset) > limit) return false;
    if (pinyin) for (const char* p = strings+offset; p < end; ++p)
        if (*p < 'a' || *p > 'z') return false;
    return true;
}
}
uint32_t Crc32(const void* bytes, size_t size) {
    uint32_t crc = 0xffffffff;
    const auto* data = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u-(crc&1u)));
    }
    return crc ^ 0xffffffff;
}
DictionaryEntry View::Entry(size_t index) const {
    if (index >= dictionary_count) return {"","",0};
    const auto& record = records[index];
    return {strings+record.pinyin_offset,strings+record.text_offset,record.frequency};
}
bool Parse(const void* bytes, size_t size, View& result, const char*& error) {
    result = {};
    const auto reject = [&](const char* reason) { error = reason; return false; };
    if (!bytes || size < sizeof(Header) || (reinterpret_cast<uintptr_t>(bytes)&3)) return reject("truncated or unaligned header");
    const auto& h = *static_cast<const Header*>(bytes);
    if (std::memcmp(h.magic,"AGLKRSC1",8) || h.version != 1) return reject("unsupported resource format");
    for (auto value : h.reserved) if (value) return reject("unsupported resource flags");
    if (h.total_size < sizeof(Header) || h.total_size > size) return reject("resource length exceeds partition");
    // Canonical, non-overlapping sections, checked in 64-bit arithmetic before pointer access.
    const auto align4 = [](uint64_t n) { return (n+3)&~uint64_t(3); };
    if (h.bitmap_offset != sizeof(Header) || !h.bitmap_size ||
        h.glyph_count < 96 || h.glyph_count > 65535 || h.sparse_count != h.glyph_count-96 ||
        h.glyphs_offset != align4(uint64_t(h.bitmap_offset)+h.bitmap_size) ||
        h.sparse_offset != uint64_t(h.glyphs_offset)+uint64_t(h.glyph_count)*sizeof(raster_glyph_t) ||
        h.dictionary_offset != align4(uint64_t(h.sparse_offset)+uint64_t(h.sparse_count)*2) ||
        !h.dictionary_count || h.strings_offset != uint64_t(h.dictionary_offset)+uint64_t(h.dictionary_count)*12 ||
        !h.strings_size || uint64_t(h.strings_offset)+h.strings_size != h.total_size ||
        !h.line_height || h.line_height > 255 || h.baseline > h.line_height || h.sparse_base > 65535)
        return reject("invalid resource section bounds");
    const auto* data = static_cast<const uint8_t*>(bytes);
    if (Crc32(data+sizeof(Header),h.total_size-sizeof(Header)) != h.payload_crc32) return reject("resource CRC mismatch");
    const auto* glyphs = reinterpret_cast<const raster_glyph_t*>(data+h.glyphs_offset);
    const auto* sparse = reinterpret_cast<const uint16_t*>(data+h.sparse_offset);
    const auto* records = reinterpret_cast<const DictionaryRecord*>(data+h.dictionary_offset);
    const auto* strings = reinterpret_cast<const char*>(data+h.strings_offset);
    for (uint32_t i = 0; i < h.glyph_count; ++i) {
        const auto& g = glyphs[i];
        if (uint64_t(g.bitmap_index)+(uint32_t(g.box_w)*g.box_h+1)/2 > h.bitmap_size)
            return reject("glyph bitmap outside resource");
    }
    for (uint32_t i = 0; i < h.sparse_count; ++i)
        if ((i && sparse[i] <= sparse[i-1]) || uint32_t(sparse[i])+h.sparse_base > 65535)
            return reject("invalid font character index");
    for (uint32_t i = 0; i < h.dictionary_count; ++i)
        if (!StringAt(strings,h.strings_size,records[i].pinyin_offset,32,true) ||
            !StringAt(strings,h.strings_size,records[i].text_offset,64,false) || !records[i].frequency)
            return reject("invalid dictionary string or frequency");
    result.font = {data+h.bitmap_offset,glyphs,sparse,nullptr,nullptr,nullptr,
                   uint16_t(h.sparse_count),uint16_t(h.glyph_count),0,
                   uint8_t(h.line_height),uint8_t(h.baseline),uint16_t(h.sparse_base)};
    result.records = records; result.strings = strings; result.dictionary_count = h.dictionary_count;
    error = nullptr;
    return true;
}
bool Install(const void* bytes, size_t size) {
    ready = Parse(bytes,size,current,last_error);
    return ready;
}
bool Ready() { return ready; }
const char* Error() { return last_error; }
const raster_font_t& Font() { return ready ? current.font : raster_montserrat_14; }
const View& Dictionary() { return current; }
}
