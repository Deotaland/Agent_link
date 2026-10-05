#include "voice_ui.h"

#include "cjk_font.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#if VOICE_UI_AVATAR
// An avatar renderer was dropped into avatar/ (main/CMakeLists.txt defines VOICE_UI_AVATAR then):
// a procedural 64x64 pixel character with one animation per mode, written against Muse's
// muse_pixel.h so any Muse avatar works here unchanged. The renderer is C, its header has no
// linkage block of its own.
extern "C" {
#include "avatar/muse_pixel.h"
}
#endif

namespace voice {
namespace {

constexpr const char* TAG = "voice.ui";

constexpr uint16_t kUiW = DISPLAY_WIDTH;    // 160
constexpr uint16_t kUiH = DISPLAY_HEIGHT;   // 160
constexpr size_t   kFrameBytes = static_cast<size_t>(kUiW) * kUiH * 2u;

// ── The round layout ─────────────────────────────────────────────────────────────────────────
// The glass is the circle inscribed in the 160x160 frame; the corners do not exist. Around its
// edge runs a ring, as on Muse's own round boards: the link state while connecting, a progress
// arc while the user talks, a spinner while the agent thinks. Everything else stays inside a
// circle of radius 71 about the centre (the ring's inner edge less 3 px), so nothing reaches the
// ring or the glass. Each position below is checked against that circle.
constexpr int      kRingD = 156;                // 2 px in from the glass,
constexpr int      kRingW = 4;                  // so the ring spans radius 74..78
constexpr int32_t  kRingRange = 1000;
constexpr int64_t  kRingFrameUs = 50 * 1000;    // ring updates at most this often
constexpr int64_t  kTalkRingUs = 20LL * 1000 * 1000;   // talking fills the ring over 20 s
constexpr int64_t  kSpinDegPerSec = 300;
constexpr int32_t  kSpinArcDeg = 60;

constexpr int kBatteryY = 12;     // the battery, top centre: the circle is ~57 px wide at its glyphs
// The text box for the link screens and the answer: 104x96, corners at radius 70.8.
constexpr int kBoxX = 28;
constexpr int kBoxY = 32;
constexpr int kBoxW = kUiW - 2 * kBoxX;   // 104: six CJK characters a line
constexpr int kBoxH = 96;                 // five lines of mixed text
constexpr int kToastBottom = 120;         // a toast sits over the lower middle, where it is wide
constexpr int kToastMaxW = 116;

#if VOICE_UI_AVATAR
// The avatar in the middle (corners at radius 70.8) and one line of state under it. The art never
// reaches the grid's bottom 3 of 64 rows, so the line may start right below the canvas.
constexpr bool    kHasAvatar = true;
constexpr int     kAvatarPx = 96;                    // the 64x64 grid, x1.5
constexpr int     kAvatarY  = 28;
constexpr int     kStateY   = kAvatarY + kAvatarPx - 4;   // 120: glyphs end at radius 70
constexpr int64_t kAvatarFrameUs = 50 * 1000;        // 20 fps
#else
constexpr bool    kHasAvatar = false;
#endif

// How long the talk-cycle screens hold before falling back to idle.
constexpr int64_t kThinkingTimeoutUs = 75LL * 1000 * 1000;   // the link reports its own failure first
constexpr int64_t kAnswerHoldUs      = 120LL * 1000 * 1000;
constexpr int     kScrollPxPerSec    = 22;                    // a little under reading pace
constexpr uint32_t kScrollDelayMs    = 2000;

// Dark ground, one accent per state.
constexpr uint32_t kColBg     = 0x000000;
constexpr uint32_t kColText   = 0xF2F5FA;
constexpr uint32_t kColMuted  = 0x8A93A6;
constexpr uint32_t kColAccent = 0x2FA8FF;
constexpr uint32_t kColOk     = 0x35D07F;
constexpr uint32_t kColWarn   = 0xFFB020;
constexpr uint32_t kColErr    = 0xFF5C5C;
constexpr uint32_t kColListen = 0xFF4F8B;
constexpr uint32_t kColToast  = 0x1C2333;
constexpr uint32_t kColRingBg = 0x161B26;
constexpr uint32_t kColLevelBg = 0x262B38;

enum class Mode { kLink, kIdle, kListening, kThinking, kAnswer };

Gc9d01Panel* s_panel = nullptr;
lv_display_t* s_disp = nullptr;
uint8_t*      s_lv_buf = nullptr;    // LVGL renders the changed areas here

// Widgets, owned by the render task.
lv_obj_t* s_ring = nullptr;
lv_obj_t* s_battery = nullptr;
lv_obj_t* s_link_view = nullptr;     // the text box: title over hint
lv_obj_t* s_link_title = nullptr;
lv_obj_t* s_link_hint = nullptr;
lv_obj_t* s_talk_view = nullptr;     // idle, listening, thinking
lv_obj_t* s_talk_main = nullptr;     // with the avatar: the line under it; without: the big line
lv_obj_t* s_talk_sub = nullptr;      // without the avatar: the line under the big one
lv_obj_t* s_level = nullptr;         // without the avatar: the mic level while listening
lv_obj_t* s_answer_view = nullptr;   // the text box: the answer
lv_obj_t* s_answer = nullptr;
lv_obj_t* s_toast = nullptr;
#if VOICE_UI_AVATAR
lv_obj_t* s_avatar = nullptr;
uint16_t* s_avatar_buf = nullptr;    // kAvatarPx^2 RGB565, PSRAM
#endif

// Cross-task inputs. Strings sit behind one spinlock; the render task copies them out.
portMUX_TYPE          s_lock = portMUX_INITIALIZER_UNLOCKED;
agent_link_status_t   s_status = {};
std::atomic<uint32_t> s_status_rev{1};
char                  s_answer_text[1024] = {};
std::atomic<uint32_t> s_answer_rev{0};
char                  s_toast_text[96] = {};
std::atomic<uint32_t> s_toast_rev{0};
std::atomic<uint32_t> s_toast_ms{0};

std::atomic<int>      s_batt_pct{-1};
std::atomic<bool>     s_charging{false};
std::atomic<uint32_t> s_batt_rev{1};

std::atomic<bool>     s_listening{false};
std::atomic<int64_t>  s_listen_since{0};
std::atomic<int>      s_level_pct{0};
std::atomic<bool>     s_thinking{false};
std::atomic<int64_t>  s_thinking_since{0};
std::atomic<int64_t>  s_answer_at{0};

// The answer and the agent's name may be in any language. Montserrat is Latin-only; the CJK font
// from flash (when the partition holds one) backs each size up glyph by glyph. Loaded by the
// render task before Build().
lv_font_t* s_cjk = nullptr;

// `base` with the CJK font behind it. Render task only; two sizes are in use.
const lv_font_t* WithCjk(const lv_font_t& base) {
    static lv_font_t copies[2];
    static const lv_font_t* bases[2] = {};
    if (!s_cjk) return &base;
    for (int i = 0; i < 2; ++i) {
        if (bases[i] == &base) return &copies[i];
        if (!bases[i]) {
            copies[i] = base;
            copies[i].fallback = s_cjk;
            bases[i] = &base;
            return &copies[i];
        }
    }
    return &base;
}

const lv_font_t* TextFont()  { return WithCjk(lv_font_montserrat_14); }
const lv_font_t* TitleFont() { return WithCjk(lv_font_montserrat_16); }   // the CJK font's own size

// CJK glyphs stand taller than Montserrat's line; give wrapped text room between lines.
void MixedTextLines(lv_obj_t* label) {
    if (s_cjk) lv_obj_set_style_text_line_space(label, 3, 0);
}

uint32_t ListenColor() {
#if VOICE_UI_AVATAR
    if (s_avatar) return muse_pixel_accent(MUSE_MODE_LISTENING);   // the ring glows as the avatar does
#endif
    return kColListen;
}

uint32_t ThinkColor() {
#if VOICE_UI_AVATAR
    if (s_avatar) return muse_pixel_accent(MUSE_MODE_THINKING);
#endif
    return kColAccent;
}

// ── Text that fits the circle ─────────────────────────────────────────────────────────────────

// One UTF-8 character at `s`: its code point, and its length in *len.
uint32_t Decode(const char* s, size_t* len) {
    const uint8_t b = static_cast<uint8_t>(s[0]);
    size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    uint32_t cp = n == 1 ? b : n == 2 ? (b & 0x1Fu) : n == 3 ? (b & 0x0Fu) : (b & 0x07u);
    for (size_t k = 1; k < n; ++k) {
        if ((static_cast<uint8_t>(s[k]) & 0xC0) != 0x80) { n = k; break; }   // malformed: stop there
        cp = cp << 6 | (static_cast<uint8_t>(s[k]) & 0x3Fu);
    }
    *len = n;
    return cp;
}

bool IsCjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

// Closing punctuation never starts a line in Chinese, an opening bracket never ends one.
bool NoLineStart(uint32_t c) {
    static constexpr uint32_t kSet[] = {0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF01, 0xFF1F,
                                        0xFF09, 0x300B, 0x300D, 0x300F, 0x3011, 0x3015, 0x3009, 0x201D,
                                        0x2019, 0x2026, 0x2014, 0xFF5E, 0x00B7, 0xFF05,
                                        ',', '.', ';', ':', '!', '?', ')', ']', '}', '%'};
    for (uint32_t k : kSet) if (k == c) return true;
    return false;
}

bool NoLineEnd(uint32_t c) {
    static constexpr uint32_t kSet[] = {0xFF08, 0x300A, 0x300C, 0x300E, 0x3010, 0x3014, 0x3008, 0x201C,
                                        0x2018, '(', '[', '{'};
    for (uint32_t k : kSet) if (k == c) return true;
    return false;
}

bool CanBreak(uint32_t before, uint32_t after) {
    if (NoLineStart(after) || NoLineEnd(before)) return false;
    return before == ' ' || IsCjk(before) || IsCjk(after);
}

// `in` broken into lines no wider than `width`: between CJK characters and at Latin spaces, but,
// unlike LVGL's own wrapping, never leaving a "。" or "，" alone at the start of a line - on a
// six-character line that happens to one sentence in six. Every line fits, so a label `width`
// wide keeps them as they are.
void WrapText(const char* in, const lv_font_t* font, int32_t width, char* out, size_t cap) {
    static const char kNewline[] = "\n";
    size_t o = 0;
    auto put = [&](const char* from, const char* to) {
        const size_t n = static_cast<size_t>(to - from);
        if (o + n + 1 < cap) { memcpy(out + o, from, n); o += n; }
    };
    const char* p = in;
    bool first = true;
    while (*p) {
        while (*p == ' ') ++p;                       // no line starts with a space
        if (!*p) break;
        const char* q = p;
        const char* last_ok = nullptr;               // the last place this line may end
        int32_t w = 0;
        uint32_t prev = 0;
        while (*q && *q != '\n') {
            size_t len = 0;
            const uint32_t c = Decode(q, &len);
            size_t next_len = 0;
            const uint32_t next = q[len] ? Decode(q + len, &next_len) : 0;
            if (q > p && CanBreak(prev, c)) last_ok = q;
            const int32_t cw = lv_font_get_glyph_width(font, c, next);
            if (w + cw > width && q > p) break;
            w += cw;
            prev = c;
            q += len;
        }
        const char* end = (!*q || *q == '\n') ? q : (last_ok ? last_ok : q);
        const char* trim = end;
        while (trim > p && trim[-1] == ' ') --trim;
        if (!first) put(kNewline, kNewline + 1);
        put(p, trim);
        first = false;
        p = end;
        if (*p == '\n') ++p;
    }
    out[o] = '\0';
}

// The widest word of `s` at `font`: what decides whether a title fits the box without LVGL
// breaking a word in two ("Reconnecting" is 115 px at 16 px; the box is 104).
int32_t WidestWord(const char* s, const lv_font_t* font) {
    int32_t widest = 0, w = 0;
    while (*s) {
        size_t len = 0;
        const uint32_t c = Decode(s, &len);
        if (c == ' ' || c == '\n' || IsCjk(c)) {
            if (w > widest) widest = w;
            w = IsCjk(c) ? lv_font_get_glyph_width(font, c, 0) : 0;
        } else {
            w += lv_font_get_glyph_width(font, c, 0);
        }
        s += len;
    }
    return w > widest ? w : widest;
}

void ScrollExec(void* obj, int32_t y) { lv_obj_scroll_to_y(static_cast<lv_obj_t*>(obj), y, LV_ANIM_OFF); }

// How tall `box`'s content is, from its top.
int32_t ContentHeight(lv_obj_t* box) {
    lv_area_t b;
    lv_obj_get_coords(box, &b);
    int32_t bottom = b.y1;
    const uint32_t n = lv_obj_get_child_count(box);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(box, static_cast<int32_t>(i));
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        if (a.y2 + 1 > bottom) bottom = a.y2 + 1;
    }
    return bottom - b.y1;
}

// Lays out the text a box now holds: centred top to bottom when it fits, otherwise from the top
// and, after a pause, rolled up at reading pace - once for an answer, back and forth for
// instructions the user may need to read twice.
void Present(lv_obj_t* box, bool loop) {
    lv_anim_delete(box, ScrollExec);
    lv_obj_set_style_pad_top(box, 0, 0);
    lv_obj_scroll_to_y(box, 0, LV_ANIM_OFF);
    lv_obj_update_layout(box);
    const int32_t room = lv_obj_get_content_height(box);
    const int32_t content = ContentHeight(box);
    if (content <= room) {
        lv_obj_set_style_pad_top(box, (room - content) / 2, 0);
        return;
    }
    const int32_t overflow = content - room;
    const uint32_t roll_ms = static_cast<uint32_t>(overflow) * 1000u / kScrollPxPerSec;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, box);
    lv_anim_set_exec_cb(&a, ScrollExec);
    lv_anim_set_values(&a, 0, overflow);
    lv_anim_set_delay(&a, kScrollDelayMs);
    lv_anim_set_duration(&a, roll_ms);
    if (loop) {
        lv_anim_set_playback_delay(&a, kScrollDelayMs);
        lv_anim_set_playback_duration(&a, roll_ms / 4);
        lv_anim_set_repeat_delay(&a, kScrollDelayMs);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    }
    lv_anim_start(&a);
}

// ── Flush: the changed area straight onto the panel, swapped to big-endian ──────────────────

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    // LV_DISPLAY_RENDER_MODE_PARTIAL: px_map is just this area, rows packed back to back.
    const uint32_t w = static_cast<uint32_t>(lv_area_get_width(area));
    const uint32_t h = static_cast<uint32_t>(lv_area_get_height(area));
    lv_draw_sw_rgb565_swap(px_map, w * h);   // the panel latches the high byte first
    if (s_panel) {
        (void)s_panel->DrawBitmap(static_cast<uint16_t>(area->x1), static_cast<uint16_t>(area->y1),
                                  static_cast<uint16_t>(w), static_cast<uint16_t>(h), px_map);
    }
    lv_display_flush_ready(disp);   // DrawBitmap returns once the pixels are out
}

uint32_t TickCb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ── Construction ─────────────────────────────────────────────────────────────────────────────

lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// A centred line of text `y` from the top.
lv_obj_t* Line(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int y, const char* text) {
    lv_obj_t* l = Label(parent, font, color);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (text) lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// A plain, unstyled rectangle.
lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

// The text box: paragraphs stacked down the middle of the circle, rolled when too long.
lv_obj_t* TextBox(lv_obj_t* parent) {
    lv_obj_t* box = Box(parent, kBoxX, kBoxY, kBoxW, kBoxH);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(box, 4, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    return box;
}

lv_obj_t* Paragraph(lv_obj_t* box, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = Label(box, font, color);
    MixedTextLines(l);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, kBoxW);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

void Build() {
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // The ring around the edge: a dim track, and an indicator in the colour of the moment.
    s_ring = lv_arc_create(root);
    lv_obj_set_size(s_ring, kRingD, kRingD);
    lv_obj_center(s_ring);
    lv_obj_set_style_pad_all(s_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ring, 0, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_rotation(s_ring, 270);   // 0 degrees at 12 o'clock
    lv_arc_set_range(s_ring, 0, kRingRange);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, kRingW, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(kColRingBg), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, kRingW, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_INDICATOR);

    // Top: the battery, on every screen.
    s_battery = Line(root, &lv_font_montserrat_14, kColMuted, kBatteryY, nullptr);

    // Link: what the user has to do to get connected, in the SDK's words.
    s_link_view  = TextBox(root);
    s_link_title = Paragraph(s_link_view, TitleFont(), kColText);
    s_link_hint  = Paragraph(s_link_view, TextFont(), kColMuted);

    // The talk cycle.
    s_talk_view = Box(root, 0, 0, kUiW, kUiH);
#if VOICE_UI_AVATAR
    // The avatar: one canvas the render loop redraws; Muse's renderer writes LVGL-native RGB565.
    s_avatar_buf = static_cast<uint16_t*>(
        heap_caps_malloc(static_cast<size_t>(kAvatarPx) * kAvatarPx * 2u, MALLOC_CAP_SPIRAM));
    if (s_avatar_buf) {
        muse_pixel_set_size(kAvatarPx);
        s_avatar = lv_canvas_create(s_talk_view);
        lv_canvas_set_buffer(s_avatar, s_avatar_buf, kAvatarPx, kAvatarPx, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_avatar, LV_ALIGN_TOP_MID, 0, kAvatarY);
        lv_obj_remove_flag(s_avatar, LV_OBJ_FLAG_CLICKABLE);
        // The line under it; the avatar shows the mic level itself.
        s_talk_main = Line(s_talk_view, &lv_font_montserrat_14, kColMuted, kStateY, nullptr);
    } else {
        ESP_LOGW(TAG, "no PSRAM for the avatar; text only");
    }
#endif
    if (!s_talk_main) {
        // Text only: the state large in the middle, what to do under it.
        s_talk_main = Line(s_talk_view, TitleFont(), kColText, 58, nullptr);
        lv_label_set_long_mode(s_talk_main, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_talk_main, kBoxW);
        s_talk_sub = Line(s_talk_view, &lv_font_montserrat_14, kColMuted, 86, nullptr);
        s_level = lv_bar_create(s_talk_view);
        lv_obj_set_size(s_level, 80, 6);
        lv_obj_align(s_level, LV_ALIGN_TOP_MID, 0, 112);
        lv_obj_set_style_bg_color(s_level, lv_color_hex(kColLevelBg), LV_PART_MAIN);
        lv_bar_set_range(s_level, 0, 100);
    }

    // Answer.
    s_answer_view = TextBox(root);
    s_answer = Paragraph(s_answer_view, TextFont(), kColText);

    // Toast: one short line over the lower middle.
    s_toast = Label(root, &lv_font_montserrat_14, kColText);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(kColToast), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);
    lv_obj_set_style_pad_hor(s_toast, 8, 0);
    lv_obj_set_style_pad_ver(s_toast, 3, 0);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(s_toast, kToastMaxW, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, kToastBottom - kUiH);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

// ── Rendering ────────────────────────────────────────────────────────────────────────────────

void ShowOnly(lv_obj_t* view) {
    lv_obj_t* const views[] = {s_link_view, s_talk_view, s_answer_view};
    for (lv_obj_t* v : views) {
        if (v == view) lv_obj_remove_flag(v, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    }
}

// The talk-cycle words for `mode`; with the avatar, only the line under it.
void SetTalkWords(Mode mode, const char* name) {
    const char* main = "Hold to talk";
    const char* sub  = "Hold BOOT to talk";
    uint32_t color = kColMuted;
    if (mode == Mode::kListening) {
        main = "Listening"; sub = "Release to send"; color = ListenColor();
    } else if (mode == Mode::kThinking) {
        main = "Thinking"; sub = ""; color = ThinkColor();
    } else if (s_talk_sub) {
        main = name; color = kColText;   // text only: the agent's name, big
    }
    lv_label_set_text(s_talk_main, main);
    lv_obj_set_style_text_color(s_talk_main, lv_color_hex(color), 0);
    if (s_talk_sub) lv_label_set_text(s_talk_sub, sub);
    if (s_level) {
        if (mode == Mode::kListening) lv_obj_remove_flag(s_level, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_add_flag(s_level, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_level, lv_color_hex(ListenColor()), LV_PART_INDICATOR);
    }
}

void RefreshLink(const agent_link_status_t& st) {
    const char* title = st.phase == AGENT_LINK_PHASE_PAIRING && st.code[0] ? st.code : st.title;
    // New words only: status updates repeat the same ones, and re-laying them restarts the roll.
    if (strcmp(lv_label_get_text(s_link_title), title) == 0 &&
        strcmp(lv_label_get_text(s_link_hint), st.hint) == 0) {
        return;
    }
    const lv_font_t* font = TitleFont();
    if (WidestWord(title, font) > kBoxW) font = TextFont();   // rather than a word broken in two
    lv_obj_set_style_text_font(s_link_title, font, 0);
    lv_label_set_text(s_link_title, title);
    lv_label_set_text(s_link_hint, st.hint);
    if (st.hint[0]) lv_obj_remove_flag(s_link_hint, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(s_link_hint, LV_OBJ_FLAG_HIDDEN);
    Present(s_link_view, true);
}

void RefreshBattery() {
    const int  pct = s_batt_pct.load(std::memory_order_acquire);
    const bool chg = s_charging.load(std::memory_order_acquire);
    char buf[24];
    if (pct >= 0) snprintf(buf, sizeof buf, "%s%d%%", chg ? LV_SYMBOL_CHARGE " " : "", pct > 100 ? 100 : pct);
    else          snprintf(buf, sizeof buf, "%s", LV_SYMBOL_BATTERY_EMPTY);
    lv_label_set_text(s_battery, buf);
    lv_obj_set_style_text_color(s_battery, lv_color_hex((pct >= 0 && pct < 15 && !chg) ? kColErr : kColMuted), 0);
}

Mode CurrentMode(bool ready, int64_t now) {
    if (!ready) return Mode::kLink;
    if (s_listening.load(std::memory_order_acquire)) return Mode::kListening;
    if (s_thinking.load(std::memory_order_acquire) &&
        now - s_thinking_since.load(std::memory_order_acquire) < kThinkingTimeoutUs) {
        return Mode::kThinking;
    }
    const int64_t at = s_answer_at.load(std::memory_order_acquire);
    if (at && now - at < kAnswerHoldUs) return Mode::kAnswer;
    return Mode::kIdle;
}

// The ring for the moment. Link screens: a full ring in the phase's colour while the user has
// something to do (or it failed), a spinner while it connects. Talking: the ring fills as the user
// speaks. Thinking: a spinner. Idle and the answer: the track alone.
struct RingState {
    uint32_t color = 0;
    int32_t  value = -1;   // -1: the indicator was last placed by hand (spinning)
    int64_t  at = 0;
};

void UpdateRing(RingState& r, Mode mode, agent_link_phase_t phase, int64_t now, bool force) {
    if (!force && now - r.at < kRingFrameUs) return;
    r.at = now;
    uint32_t color = kColRingBg;
    int32_t value = 0;
    bool spin = false;
    switch (mode) {
    case Mode::kLink:
        if (phase == AGENT_LINK_PHASE_SETUP || phase == AGENT_LINK_PHASE_PAIRING) {
            color = kColAccent; value = kRingRange;
        } else if (phase == AGENT_LINK_PHASE_BLOCKED) {
            color = kColErr; value = kRingRange;
        } else if (phase == AGENT_LINK_PHASE_CONNECTED) {
            color = kColOk; value = kRingRange;
        } else {
            color = kColWarn; spin = true;
        }
        break;
    case Mode::kListening: {
        color = ListenColor();
        const int64_t t = now - s_listen_since.load(std::memory_order_acquire);
        value = t >= kTalkRingUs ? kRingRange : static_cast<int32_t>(t * kRingRange / kTalkRingUs);
        break;
    }
    case Mode::kThinking:
        color = ThinkColor(); spin = true;
        break;
    default:
        break;
    }
    if (color != r.color || force) {
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(color), LV_PART_INDICATOR);
        r.color = color;
    }
    if (spin) {
        // Move the indicator rather than rotate the arc: a rotation redraws the whole ring, and
        // with it the whole screen.
        const int32_t start = static_cast<int32_t>((now / 1000 * kSpinDegPerSec / 1000) % 360);
        lv_arc_set_angles(s_ring, start, start + kSpinArcDeg);
        r.value = -1;
    } else {
        if (r.value < 0) lv_arc_set_bg_start_angle(s_ring, 0);   // unchanged, but puts the value's angles back
        if (value != r.value) {
            lv_arc_set_value(s_ring, value);
            r.value = value;
        }
    }
}

#if VOICE_UI_AVATAR
// The avatar's own animation clock: which mode it plays, and since when.
struct AvatarState {
    muse_mode_t mode = MUSE_MODE_IDLE;
    int64_t     since = 0;
    int64_t     frame_at = 0;
};

// One frame of the avatar, at most every kAvatarFrameUs, while the talk view shows it.
void DrawAvatar(AvatarState& av, Mode mode, int64_t now) {
    if (!s_avatar || lv_obj_has_flag(s_talk_view, LV_OBJ_FLAG_HIDDEN)) return;
    const muse_mode_t want = mode == Mode::kListening ? MUSE_MODE_LISTENING
                           : mode == Mode::kThinking  ? MUSE_MODE_THINKING
                                                      : MUSE_MODE_IDLE;
    if (want != av.mode) {
        av.mode = want;
        av.since = now;
    }
    if (now - av.frame_at < kAvatarFrameUs) return;
    av.frame_at = now;

    muse_pose_t pose = {};
    pose.mode   = av.mode;
    pose.t      = static_cast<float>(now) / 1e6f;
    pose.mode_t = static_cast<float>(now - av.since) / 1e6f;
    pose.level  = want == MUSE_MODE_LISTENING
                      ? static_cast<float>(s_level_pct.load(std::memory_order_acquire)) / 100.0f
                      : 0.0f;
    muse_pixel_render(&pose);
    muse_pixel_scale(s_avatar_buf, kAvatarPx, 0, kAvatarPx - 1, 0, kAvatarPx - 1);
    lv_obj_invalidate(s_avatar);
}
#endif

void RenderTask(void*) {
    // LVGL calls only from this task, and before Build()
    s_cjk = LoadCjkFont(CJK_FONT_PARTITION, CJK_FONT_PX, CJK_FONT_CACHE_GLYPHS);
    Build();
    RefreshBattery();

    uint32_t seen_status = 0, seen_answer = 0, seen_toast = 0, seen_batt = 0;
    int64_t toast_until = 0;
    bool ready = false;
    Mode shown = Mode::kIdle;
    bool first = true;
    agent_link_status_t st = {};
    RingState ring;
    // Render task only, and big: kept in PSRAM with the rest of this board's .bss.
    EXT_RAM_BSS_ATTR static char answer[sizeof s_answer_text];
    EXT_RAM_BSS_ATTR static char wrapped[sizeof s_answer_text + 128];
#if VOICE_UI_AVATAR
    AvatarState avatar;
#endif

    while (true) {
        const int64_t now = esp_timer_get_time();

        const uint32_t status_rev = s_status_rev.load(std::memory_order_acquire);
        bool status_changed = false;
        if (status_rev != seen_status) {
            seen_status = status_rev;
            taskENTER_CRITICAL(&s_lock);
            st = s_status;
            taskEXIT_CRITICAL(&s_lock);
            ready = st.phase == AGENT_LINK_PHASE_READY;
            RefreshLink(st);
            status_changed = true;
        }

        const uint32_t batt_rev = s_batt_rev.load(std::memory_order_acquire);
        if (batt_rev != seen_batt) { seen_batt = batt_rev; RefreshBattery(); }

        const uint32_t answer_rev = s_answer_rev.load(std::memory_order_acquire);
        if (answer_rev != seen_answer) {
            seen_answer = answer_rev;
            taskENTER_CRITICAL(&s_lock);
            memcpy(answer, s_answer_text, sizeof answer);
            taskEXIT_CRITICAL(&s_lock);
            WrapText(answer, TextFont(), kBoxW, wrapped, sizeof wrapped);
            lv_label_set_text(s_answer, wrapped);
            Present(s_answer_view, false);
        }

        const Mode mode = CurrentMode(ready, now);
        const bool mode_changed = mode != shown || first;
        if (mode_changed || (status_changed && mode == Mode::kIdle)) {
            first = false;
            shown = mode;
            switch (mode) {
            case Mode::kLink:   ShowOnly(s_link_view); break;
            case Mode::kAnswer: ShowOnly(s_answer_view); break;
            default:
                ShowOnly(s_talk_view);
                SetTalkWords(mode, st.title[0] ? st.title : "Ready");
                break;
            }
        }
        UpdateRing(ring, mode, st.phase, now, mode_changed || status_changed);
        if (mode == Mode::kListening && s_level) {
            lv_bar_set_value(s_level, s_level_pct.load(std::memory_order_acquire), LV_ANIM_OFF);
        }
#if VOICE_UI_AVATAR
        DrawAvatar(avatar, mode, now);
#endif

        const uint32_t toast_rev = s_toast_rev.load(std::memory_order_acquire);
        if (toast_rev != seen_toast) {
            seen_toast = toast_rev;
            char text[sizeof s_toast_text];
            taskENTER_CRITICAL(&s_lock);
            memcpy(text, s_toast_text, sizeof text);
            taskEXIT_CRITICAL(&s_lock);
            lv_label_set_text(s_toast, text);
            lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            toast_until = now + static_cast<int64_t>(s_toast_ms.load()) * 1000;
        }
        if (toast_until && now > toast_until) {
            toast_until = 0;
            lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        }

        uint32_t next = lv_timer_handler();   // renders + flushes only what changed
        if (next == LV_NO_TIMER_READY || next > 30) next = 30;
        vTaskDelay(pdMS_TO_TICKS(next < 10 ? 10 : next));
    }
}

}  // namespace

Ui& Ui::Instance() {
    static Ui instance;
    return instance;
}

esp_err_t Ui::Start(Gc9d01Panel* panel) {
    if (!panel || !panel->Ready()) return ESP_ERR_INVALID_STATE;
    s_panel = panel;

    // One full frame in PSRAM (51 KB): LVGL renders each changed area into it, and the panel
    // copies the area out through its own internal DMA stripes. Internal RAM is what the radios
    // and TLS need.
    s_lv_buf = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    if (!s_lv_buf) {
        ESP_LOGE(TAG, "frame buffer alloc failed (need %u bytes)", static_cast<unsigned>(kFrameBytes));
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    lv_tick_set_cb(TickCb);
    s_disp = lv_display_create(kUiW, kUiH);
    if (!s_disp) return ESP_FAIL;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, FlushCb);
    // Partial: only what changed goes over the 20 MHz SPI - at 20 fps the avatar is 18 KB a frame,
    // the whole screen 51 KB.
    lv_display_set_buffers(s_disp, s_lv_buf, nullptr, kFrameBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Internal stack: LVGL's draw path is deep, TinyTTF rasterizes CJK glyphs on it, and a
    // PSRAM stack faults while flash writes (NVS) have the cache disabled. On the last core, as
    // Muse's own UI runs: the first core belongs to Wi-Fi and the network. Left unpinned, the
    // avatar's float maths would tie the task to whichever core it first ran on.
    if (xTaskCreatePinnedToCore(RenderTask, "voice_ui", 10240, nullptr, 4, nullptr,
                                portNUM_PROCESSORS - 1) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI up: %ux%u round%s", kUiW, kUiH, kHasAvatar ? ", with avatar" : "");
    return ESP_OK;
}

void Ui::SetStatus(const agent_link_status_t& st) {
    taskENTER_CRITICAL(&s_lock);
    s_status = st;
    taskEXIT_CRITICAL(&s_lock);
    s_status_rev.fetch_add(1, std::memory_order_release);
}

void Ui::SetBattery(int percent, bool charging) {
    if (percent == s_batt_pct.load(std::memory_order_acquire) &&
        charging == s_charging.load(std::memory_order_acquire)) {
        return;
    }
    s_batt_pct.store(percent, std::memory_order_release);
    s_charging.store(charging, std::memory_order_release);
    s_batt_rev.fetch_add(1, std::memory_order_release);
}

void Ui::SetListening(bool on) {
    if (on) {
        s_listen_since.store(esp_timer_get_time(), std::memory_order_release);
        s_thinking.store(false, std::memory_order_release);
        s_answer_at.store(0, std::memory_order_release);
        s_level_pct.store(0, std::memory_order_release);
    }
    s_listening.store(on, std::memory_order_release);
}

void Ui::SetLevel(int percent) {
    s_level_pct.store(percent < 0 ? 0 : percent > 100 ? 100 : percent, std::memory_order_release);
}

void Ui::SetThinking() {
    s_thinking_since.store(esp_timer_get_time(), std::memory_order_release);
    s_thinking.store(true, std::memory_order_release);
}

void Ui::SetAnswer(const char* utf8) {
    // strlcpy, not snprintf: nothing that might take a lock inside a critical section.
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_answer_text, utf8 ? utf8 : "", sizeof s_answer_text);
    taskEXIT_CRITICAL(&s_lock);
    s_thinking.store(false, std::memory_order_release);
    s_answer_at.store(esp_timer_get_time(), std::memory_order_release);
    s_answer_rev.fetch_add(1, std::memory_order_release);
}

void Ui::Toast(const char* text, uint32_t ms) {
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_toast_text, text ? text : "", sizeof s_toast_text);
    taskEXIT_CRITICAL(&s_lock);
    s_toast_ms.store(ms, std::memory_order_release);
    s_toast_rev.fetch_add(1, std::memory_order_release);
}

}  // namespace voice
