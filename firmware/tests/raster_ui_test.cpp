#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../boards/ai-passport/voice_ui.cc"

static int64_t test_time = 1000000;
static unsigned loops = 1;
static void (*after_frame)() = nullptr;
struct StopRender {};

int64_t esp_timer_get_time() { return test_time; }
unsigned ulTaskNotifyTake(int, unsigned) {
    if (!--loops) throw StopRender{};
    test_time += 50000;
    if (after_frame) after_frame();
    return 0;
}

esp_err_t St7789Lcd::FlushStripe(uint16_t y, uint16_t rows) {
    ++flushes;
    assert(rows == 4 && y + rows <= 320);
    for (unsigned i = 0; i < 16; ++i) assert(stripe[i] == 0xa5 && stripe[1936 + i] == 0xa5);
    if (flushes == fail_at) return ESP_ERR_TIMEOUT;
    std::memcpy(frame.data() + y * 240 * 2, StripeBuffer(), rows * 240 * 2);
    if (flushes == interrupt_at) voice::Ui::Instance().SetSession((99u << 3) | 2u);
    return ESP_OK;
}

static void Reset(St7789Lcd& panel) {
    panel.flushes = panel.fail_at = panel.interrupt_at = 0;
    panel.stripe.fill(0xa5);
    voice::s_input = {};
    voice::s_frame = {};
    voice::s_panel_failed = false;
    voice::s_mode = voice::Mode::Link;
    voice::s_toast_until = 0;
    voice::s_scroll = voice::s_dots = voice::s_level = 0;
    voice::s_rendered_token.store(0);
    test_time = 1000000;
    loops = 1;
    after_frame = nullptr;
    assert(voice::Ui::Instance().Start(&panel) == ESP_OK);
    agent_link_status_t status{};
    status.phase = AGENT_LINK_PHASE_READY;
    strlcpy(status.title, "AIPassport", sizeof status.title);
    strlcpy(status.hint, "Open the Deotaland app and connect to AIPassport", sizeof status.hint);
    voice::Ui::Instance().SetStatus(status);
}

static void Run() {
    try { voice::RenderTask(nullptr); } catch (const StopRender&) {}
}

static void CompareFullFrame(St7789Lcd& panel) {
    std::vector<uint8_t> expected(panel.frame.size());
    raster::Canvas canvas(expected.data(), 240, 0, 320);
    voice::Paint(canvas);
    assert(std::equal(expected.begin(), expected.end(), panel.frame.begin()));
}

static void Save(const St7789Lcd& panel, const char* name) {
    std::string path = std::string("build/ui_") + name + ".ppm";
    FILE* file = nullptr;
    assert(fopen_s(&file, path.c_str(), "wb") == 0);
    std::fprintf(file, "P6\n240 320\n255\n");
    for (size_t i = 0; i < panel.frame.size(); i += 2) {
        const unsigned color = (panel.frame[i] << 8) | panel.frame[i + 1];
        const unsigned char pixel[] = {static_cast<unsigned char>((color >> 11) * 255 / 31),
                                       static_cast<unsigned char>(((color >> 5) & 63) * 255 / 63),
                                       static_cast<unsigned char>((color & 31) * 255 / 31)};
        std::fwrite(pixel, 1, 3, file);
    }
    std::fclose(file);
}

static void TestScreens() {
    St7789Lcd panel;
    const char* names[] = {"link", "ready", "listening", "thinking", "speaking"};
    for (unsigned state = 0; state < 5; ++state) {
        Reset(panel);
        if (!state) {
            agent_link_status_t status{};
            status.phase = AGENT_LINK_PHASE_SETUP;
            strlcpy(status.title, "Open the app", sizeof status.title);
            strlcpy(status.hint, "Open the Deotaland app and connect to AIPassport", sizeof status.hint);
            voice::Ui::Instance().SetStatus(status);
        }
        const uint32_t token = (1u << 3) | state;
        voice::Ui::Instance().SetSession(token);
        voice::Ui::Instance().SetLevel(64);
        Run();
        assert(voice::Ui::Instance().SessionRendered(token));
        assert(panel.flushes == 80);
        CompareFullFrame(panel);
        Save(panel, names[state]);
    }
    Reset(panel);
    voice::Ui::Instance().SetSession((1u << 3) | 1u);
    std::string answer;
    for (unsigned i = 0; i < 20; ++i) answer += "The agent is replying. UTF-8: \xe4\xbd\xa0\xe5\xa5\xbd. ";
    voice::Ui::Instance().SetAnswer(answer.c_str());
    voice::Ui::Instance().Toast("Volume 80", 1200);
    Run();
    CompareFullFrame(panel);
    Save(panel, "answer");
    test_time += 8000000;
    loops = 1;
    Run();
    assert(voice::s_scroll > 0);
    CompareFullFrame(panel);
}

static void TestGates() {
    St7789Lcd panel;
    Reset(panel);
    const uint32_t speaking = (1u << 3) | 4u;
    voice::Ui::Instance().SetSession(speaking);
    panel.interrupt_at = 10;
    Run();
    assert(panel.flushes == 10);
    assert(!voice::Ui::Instance().SessionRendered(speaking));
    assert(!voice::Ui::Instance().SessionRendered((99u << 3) | 2u));
    panel.interrupt_at = 0;
    loops = 1;
    Run();
    assert(voice::Ui::Instance().SessionRendered((99u << 3) | 2u));
    CompareFullFrame(panel);
    Reset(panel);
    voice::Ui::Instance().SetSession(speaking);
    panel.fail_at = 12;
    loops = 3;
    Run();
    assert(panel.flushes == 12);
    assert(!voice::Ui::Instance().SessionRendered(speaking));
    assert(voice::s_panel_failed);
}

static void TestPartialUpdates() {
    St7789Lcd panel;
    Reset(panel);
    voice::Ui::Instance().SetSession((1u << 3) | 2u);
    voice::Ui::Instance().SetLevel(0);
    loops = 2;
    after_frame = [] { voice::Ui::Instance().SetLevel(100); };
    Run();
    assert(panel.flushes == 84);
    CompareFullFrame(panel);
    Reset(panel);
    voice::Ui::Instance().SetSession((1u << 3) | 1u);
    voice::Ui::Instance().Toast("Volume 50", 1);
    loops = 2;
    Run();
    assert(panel.flushes == 89);
    CompareFullFrame(panel);
    Reset(panel);
    voice::Ui::Instance().SetSession((1u << 3) | 3u);
    loops = 2;
    after_frame = [] { voice::Ui::Instance().SetSession((1u << 3) | 4u); };
    Run();
    assert(panel.flushes == 110);
    assert(voice::Ui::Instance().SessionRendered((1u << 3) | 4u));
    CompareFullFrame(panel);
    Reset(panel);
    voice::Ui::Instance().SetSession((1u << 3) | 3u);
    panel.fail_at = 90;
    loops = 3;
    after_frame = [] { if (voice::TokenState(voice::s_input.token) == voice::SessionState::Thinking)
                          voice::Ui::Instance().SetSession((1u << 3) | 4u); };
    Run();
    assert(panel.flushes == 90);
    assert(!voice::Ui::Instance().SessionRendered((1u << 3) | 4u));
}

static void TestWrapAndUtf8() {
    const raster_font_t* fonts[] = {&raster_montserrat_14, &raster_montserrat_20, &raster_montserrat_28};
    for (const auto* font : fonts) {
        for (uint32_t codepoint = 32; codepoint < 127; ++codepoint) assert(raster::GlyphId(*font, codepoint) == codepoint - 31);
        assert(raster::GlyphId(*font, 0xf028) != 0);
        assert(raster::GlyphId(*font, 0x4f60) == 0);
        std::string text;
        for (unsigned i = 0; i < 90; ++i) text += "AVATAR abcdefghijklmnopqrstuvwxyz \xe4\xbd\xa0\xe5\xa5\xbd\n\n";
        raster::TextLayout layout;
        layout.Wrap(*font, text.c_str(), 192);
        assert(layout.Count() == raster::TextLayout::kMaxLines);
        for (unsigned i = 0; i < layout.Count(); ++i) {
            assert(layout.End(i) >= layout.Start(i));
            assert(raster::TextWidth(*font, text.c_str() + layout.Start(i), text.c_str() + layout.End(i)) <= 192);
        }
    }
    for (const char* bad : {"\xf0\x80\x80\x80", "\xe4\xbd", "\xffhello", "\xed\xa0\x80"}) {
        const char* end = bad + strlen(bad), *p = bad;
        while (p < end) { const char* before = p; raster::NextCodepoint(p, end); assert(p > before && p <= end); }
    }
}

int main() {
    TestScreens();
    TestGates();
    TestPartialUpdates();
    TestWrapAndUtf8();
    std::puts("Raster UI: full/stripe equality, partial updates, UTF-8, wrap bounds, DMA guards and stale/error playback gates passed");
}
