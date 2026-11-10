#pragma once
#include "raster_font.h"
#include <cstddef>
#include <cstdint>

namespace paper_resources {
// AGLKRSC1 is a little-endian resource pack, distinct from the LVGL AGLKTTF1 image.
struct Header {
    char magic[8];
    uint32_t version, total_size, payload_crc32;
    uint32_t bitmap_offset, bitmap_size, glyphs_offset, glyph_count;
    uint32_t sparse_offset, sparse_count, dictionary_offset, dictionary_count;
    uint32_t strings_offset, strings_size, line_height, baseline, sparse_base;
    uint32_t reserved[6];
};
struct DictionaryRecord { uint32_t pinyin_offset, text_offset, frequency; };
struct DictionaryEntry { const char* pinyin; const char* text; uint32_t frequency; };
struct View {
    raster_font_t font{};
    const DictionaryRecord* records = nullptr;
    const char* strings = nullptr;
    size_t dictionary_count = 0;
    DictionaryEntry Entry(size_t index) const;
};
static_assert(sizeof(Header) == 96 && sizeof(DictionaryRecord) == 12 && sizeof(raster_glyph_t) == 12);
uint32_t Crc32(const void* bytes, size_t size);
bool Parse(const void* bytes, size_t size, View& result, const char*& error);
// Call once before starting UI/input tasks. The validated mapping lives until reboot.
bool Init();
bool Ready();
const char* Error();
const raster_font_t& Font();
const View& Dictionary();
// Platform-neutral initialization used by Init and the host harness; caller owns the bytes.
bool Install(const void* bytes, size_t size);
}
