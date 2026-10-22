#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "stripe_canvas.h"

static void Text(raster::Canvas& canvas, const raster_font_t& font, const char* text,
                 int y, int lines, uint32_t color) {
    raster::TextLayout layout;
    layout.Wrap(font, text, 208);
    for (unsigned i = 0; i < layout.Count() && i < unsigned(lines); ++i)
        canvas.Text(font, 16, y + i * (font.line_height + 3),
                    text + layout.Start(i), text + layout.End(i), color);
}

int main(int argc, char** argv) {
    assert(argc == 3);
    const auto& cjk = raster_cjk_14;
    assert(cjk.glyph_count == 7813 && cjk.sparse_base == 127);
    FILE* file = fopen(argv[1], "rb"); assert(file);
    std::string hanzi;
    int byte;
    while ((byte = fgetc(file)) != EOF) hanzi += char(byte);
    fclose(file);
    const char* p = hanzi.data(); const char* end = p + hanzi.size();
    unsigned count = 0;
    while (p < end) {
        const uint32_t cp = raster::NextCodepoint(p, end);
        const uint16_t id = raster::GlyphId(cjk, cp);
        assert(id && id < cjk.glyph_count);
        const auto& glyph = cjk.glyphs[id];
        assert(glyph.box_w && glyph.box_h);
        size_t size = (glyph.box_w * glyph.box_h + 1) / 2;
        assert(std::any_of(cjk.bitmap + glyph.bitmap_index, cjk.bitmap + glyph.bitmap_index + size,
                          [](uint8_t v) { return v != 0; }));
        ++count;
    }
    assert(count == 6763);
    for (uint16_t i = 0; i < cjk.sparse_count; ++i)
        assert(raster::GlyphId(cjk, cjk.sparse[i] + cjk.sparse_base) == i + 96);
    for (const char* punctuation = u8"，。！？：“”‘’（）【】《》、；—…"; *punctuation;) {
        const char* stop = punctuation + strlen(punctuation);
        assert(raster::GlyphId(cjk, raster::NextCodepoint(punctuation, stop)));
    }
    assert(!raster::GlyphId(cjk, 0x1f600));
    for (uint32_t cp = 32; cp < 127; ++cp)
        assert(raster::GlyphId(raster_montserrat_14, cp) == cp - 31);
    char clipped[6];
    raster::CopyText(clipped, sizeof clipped, u8"你好");
    assert(std::string(clipped) == u8"你");
    raster::CopyText(clipped, 3, u8"你好"); assert(!*clipped);
    raster::CopyText(clipped, sizeof clipped, "A\xff" "B");
    assert(std::string(clipped) == "A?B");
    const char* mixed = u8"你好，世界！这是中文与 English 123 混排。";
    raster::TextLayout layout; layout.Wrap(cjk, mixed, 56);
    size_t position = 0;
    for (unsigned i = 0; i < layout.Count(); ++i) {
        assert(layout.Start(i) == position);
        position = layout.End(i);
        const char* line = mixed + layout.Start(i);
        while (line < mixed + position) assert(raster::NextCodepoint(line, mixed + position) != 0xfffd);
        // Space wrapping may skip a separator before the next line.
        while (mixed[position] == ' ') ++position;
    }
    assert(position == strlen(mixed));
    std::vector<uint8_t> rgb(240 * 320 * 3);
    uint8_t stripe[240 * 4 * 2];
    for (int y = 0; y < 320; y += 4) {
        raster::Canvas canvas(stripe, 240, y, 4);
        canvas.Fill(0x101820);
        canvas.Box({8, 40, 224, 236}, 0x1D2B36, 12);
        Text(canvas, raster_montserrat_14, "AI PASSPORT / WIFI", 12, 1, 0x54D6B4);
        Text(canvas, raster_montserrat_20, "Voice ready", 54, 1, 0xF4F4EC);
        Text(canvas, cjk, u8"你好，我是你的语音助手。\n中文回复现在可以正常显示。\n会议记录 / English 123", 106, 5, 0xC7D3D9);
        Text(canvas, cjk, u8"请帮我介绍一下北京的历史，\n再推荐几个适合散步的地方。", 213, 2, 0x54D6B4);
        Text(canvas, raster_montserrat_14, "UP / DOWN: volume", 284, 2, 0xC7D3D9);
        for (unsigned i = 0; i < sizeof stripe / 2; ++i) {
            const uint16_t value = uint16_t(stripe[i * 2] << 8) | stripe[i * 2 + 1];
            const size_t offset = (size_t(y) * 240 + i) * 3;
            rgb[offset] = uint8_t(((value >> 11) & 31) * 255 / 31);
            rgb[offset + 1] = uint8_t(((value >> 5) & 63) * 255 / 63);
            rgb[offset + 2] = uint8_t((value & 31) * 255 / 31);
        }
    }
    file = fopen(argv[2], "wb"); assert(file);
    fputs("P6\n240 320\n255\n", file); fwrite(rgb.data(), 1, rgb.size(), file); fclose(file);
    puts("PASS: all 6763 Hanzi renderable, punctuation, legacy ASCII, UTF-8 clipping/wrapping, actual four-row raster preview");
}
