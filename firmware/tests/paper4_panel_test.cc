// The runner inserts production C functions, mocking only allocation and panel I/O.
#include "epd.h"
#include "epd_4in0_window.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define EPD_ROW_BYTES (EPD_PANEL_W / 8)
// INSERT_COMMANDS

static bool s_dead = false, s_shadow_valid = false, fail_refresh = false;
static int full_inits = 0;
static uint8_t row_buffer[EPD_ROW_BYTES];
static uint8_t* s_row = row_buffer;
static uint8_t command;
static std::vector<uint8_t> old_data, new_data, panel_window;
static uint8_t* heap_caps_malloc(size_t bytes, unsigned) {
    return static_cast<uint8_t*>(std::malloc(bytes));
}
static void EPD_W21_WriteCMD(uint8_t value) { command = value; }
static void spi_send(int, const uint8_t* data, size_t size) {
    assert(size <= sizeof row_buffer);
    if (command == DTM) old_data.insert(old_data.end(), data, data + size);
    if (command == DTM2) new_data.insert(new_data.end(), data, data + size);
}
static void write_cmd_data(uint8_t value, const uint8_t* data, size_t size) {
    if (value == PTL) panel_window.assign(data, data + size);
}
static void init_regs() {}
static void power_on() {}
static void bw_refresh() { if (fail_refresh) s_dead = true; }
void EPD_init() { ++full_inits; s_dead = false; }
void EPD_sleep() { if (s_dead) s_shadow_valid = false; }
static void send_frame_bw(uint8_t cmd, const unsigned char* picData, uint8_t fill);
static void send_window_bw(uint8_t cmd, const unsigned char* picData, int h0, int h1, int g0, int g1);

// INSERT_SHADOW_AND_PARTIAL
// INSERT_PIXEL_TRANSFER

static int PhysicalColumn(int gate) {
    const int x = gate % 2 ? 300 + gate / 2 : gate / 2;
    return x < 528 ? x : -1;
}
static int LogicalIndex(int source, int column) {
#if EPD_ROTATION == 270
    return (791 - source) * 528 + 527 - column;
#else
    return source * 528 + column;
#endif
}
static void CheckBytes(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& image,
                       int h0, int h1, int g0, int g1) {
    size_t offset = 0;
    for (int gate = g0; gate <= g1; ++gate) {
        const int column = PhysicalColumn(gate);
        for (int source = h0; source <= h1; source += 8) {
            uint8_t expected = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const bool white = column < 0 || image[LogicalIndex(source + bit, column)] == 255;
                if (!white) expected |= 0x80 >> bit; // This panel inverts the logical white bits.
            }
            assert(offset < actual.size() && actual[offset++] == expected);
        }
    }
    assert(offset == actual.size());
}
static void ClearIo() { old_data.clear(); new_data.clear(); panel_window.clear(); }

int main() {
    static_assert(EPD_WIDTH == 528 && EPD_HEIGHT == 792);
    std::vector<uint8_t> base(528 * 792), next(base.size()), visited(base.size(), 0);
    for (int gate = 0; gate < 600; ++gate) {
        const int physical = PhysicalColumn(gate);
        const int row = panel_row(gate);
        if (physical < 0) { assert(row == -1 && epd4_col_of_gate(gate) == -1); continue; }
        for (int source = 0; source < 792; ++source) {
            const auto index = logical_index(source, row);
            assert(index == unsigned(LogicalIndex(source, physical)));
            assert(index < visited.size() && visited[index]++ == 0);
            const int y = epd4_row_of_source(source), x = epd4_col_of_gate(gate);
            assert(index == unsigned(y * 528 + x));
        }
    }
    assert(std::all_of(visited.begin(), visited.end(), [](uint8_t count) { return count == 1; }));
    for (size_t i = 0; i < base.size(); ++i) base[i] = (i * 17 + i / 528) % 7 ? 255 : 0;
    ClearIo(); EPD_init(); PIC_display(base.data()); EPD_sleep();
    assert(s_shadow_valid);
    CheckBytes(new_data, base, 0, 791, 0, 599);
    const int rectangles[][4] = {{0,0,1,1}, {527,791,1,1}, {220,100,20,45},
        {30,150,190,70}, {318,150,190,70}, {-4,-3,12,10}, {500,780,80,40}};
    for (const auto& rect : rectangles) {
        // Change every pixel to detect accidental shadow updates outside the transferred strips.
        for (size_t i = 0; i < base.size(); ++i) next[i] = base[i] ^ 255;
        epd4_window_t window{};
        assert(epd4_window(rect[0], rect[1], rect[2], rect[3], &window));
        assert(window.h0 >= 0 && window.h1 < 792 && window.h0 % 8 == 0 && window.h1 % 8 == 7);
        ClearIo();
        const int before = full_inits;
        epd_partial(next.data(), rect[0], rect[1], rect[2], rect[3]);
        assert(full_inits == before && s_shadow_valid && panel_window.size() == 9);
        assert((panel_window[0] * 256 + panel_window[1]) == window.h0);
        assert((panel_window[2] * 256 + panel_window[3]) == window.h1);
        assert((panel_window[4] * 256 + panel_window[5]) == window.g0);
        assert((panel_window[6] * 256 + panel_window[7]) == window.g1);
        CheckBytes(old_data, base, window.h0, window.h1, window.g0, window.g1);
        CheckBytes(new_data, next, window.h0, window.h1, window.g0, window.g1);
        int covered = 0;
        for (int y = 0; y < 792; ++y) for (int x = 0; x < 528; ++x) {
#if EPD_ROTATION == 270
            const int physical = 527 - x, source = 791 - y;
#else
            const int physical = x, source = y;
#endif
            const int gate = physical < 300 ? 2 * physical : 2 * (physical - 300) + 1;
            const bool touched = source >= window.h0 && source <= window.h1 && gate >= window.g0 && gate <= window.g1;
            covered += touched;
            const size_t index = y * 528 + x;
            assert(s_shadow[index] == (touched ? next[index] : base[index]));
            if (touched) base[index] = next[index];
        }
        assert(covered == epd4_window_area(&window));
    }
    ClearIo(); epd_partial(next.data(), 528, 0, 10, 10);
    assert(old_data.empty() && new_data.empty());
    const std::vector<uint8_t> saved(s_shadow, s_shadow + base.size());
    fail_refresh = true; epd_partial(next.data(), 0, 0, 20, 20);
    assert(!s_shadow_valid && std::memcmp(s_shadow, saved.data(), base.size()) == 0);
    fail_refresh = false;
    const int before = full_inits;
    epd_partial(next.data(), 0, 0, 20, 20);
    assert(full_inits == before + 1 && s_shadow_valid);
    std::free(s_shadow);
    std::printf("PASS: %d-degree full/partial transfer bytes, all 418176 pixels, split-gate windows, shadow boundaries and failure recovery\n", EPD_ROTATION);
}
