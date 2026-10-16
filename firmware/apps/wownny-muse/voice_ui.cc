#include "voice_ui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#if VOICE_UI_AVATAR
// Defined by main/CMakeLists.txt when avatar/muse_pixel.c exists.
extern "C" {
#include "avatar/muse_pixel.h"
}
#endif

namespace voice {
namespace {

constexpr const char* TAG = "voice.ui";

constexpr uint16_t kUiW = DISPLAY_WIDTH;
constexpr uint16_t kUiH = DISPLAY_HEIGHT;
constexpr size_t   kFrameBytes = static_cast<size_t>(kUiW) * kUiH * 2u;

// ── Layout ───────────────────────────────────────────────────────────────────────────────────
// Only a circle of UI_VISIBLE_DIAMETER is visible through the enclosure. The ring sits 2 px inside
// it; everything else must stay within kSafeR of the centre (checked by the static_asserts).
constexpr int kCx = kUiW / 2;
constexpr int kCy = kUiH / 2;
constexpr int kRingD = UI_VISIBLE_DIAMETER - 4;
constexpr int kRingW = 3;
constexpr int kSafeR = kRingD / 2 - kRingW - 3;     // 42 for a 100 px window

// Montserrat 12, 15 px line box. Digits use rows 3-11, symbols and the bolt rows 1-13.
constexpr int kLineH = 15, kInkTop = 1, kInkBottom = 14;

// Avatar: 64 px source grid, drawn at 56 px when idle and 48 px otherwise.
constexpr int kAvatarBigPx   = 56;
constexpr int kAvatarBigY    = kCy - kAvatarBigPx / 2 - 3;
constexpr int kAvatarSmallPx = 48;
constexpr int kAvatarSmallY  = kCy - kAvatarSmallPx / 2 - 2;
constexpr int kArtR          = kAvatarBigPx / 2;   // idle art stays inside this radius
constexpr int64_t kMoveUs    = 300 * 1000;         // resize animation
constexpr int kBatteryY    = kAvatarSmallY - 11;
constexpr int kBatteryInkW = 40;                   // "<bolt>100%"
constexpr int kIndY        = kAvatarSmallY + kAvatarSmallPx + 1;
constexpr int kIndH        = 10;
constexpr int kIndLineY    = kIndY + kIndH / 2 - 8;
constexpr int kIndMaxW     = 48;                   // "<speaker> 100%", 7-digit pairing code
constexpr int kMicW = 7, kMicH = 12, kMicY = 104;
// Menu
constexpr int kMenuHeadY = 42, kMenuHeadMaxW = 46;
constexpr int kMenuAboveY = 55, kMenuBelowY = 92, kMenuRowMaxW = 64;
constexpr int kMenuCardY = 71, kMenuCardH = 18, kMenuCardW = 80, kMenuCardGap = 4;   // "Volume 70%" = 77 px
constexpr int kMenuLines = 4;
constexpr int kMenuLineY[kMenuLines]    = {54, 69, 84, 99};
constexpr int kMenuLineMaxW[kMenuLines] = {66, 80, 74, 50};

constexpr int Sq(int v) { return v * v; }
static_assert(Sq(kAvatarSmallPx / 2) + Sq(kCy - kAvatarSmallY) <= Sq(kSafeR), "small avatar top corners outside window");
static_assert(Sq(kAvatarSmallPx / 2) + Sq(kAvatarSmallY + kAvatarSmallPx - kCy) <= Sq(kSafeR), "small avatar bottom corners outside window");
constexpr int kBigOffset = kAvatarBigY + kAvatarBigPx / 2 - kCy;
static_assert((kBigOffset < 0 ? -kBigOffset : kBigOffset) + kArtR <= kSafeR, "big avatar outside window");
static_assert(Sq(kBatteryInkW / 2) + Sq(kCy - (kBatteryY + kInkTop)) <= Sq(kSafeR), "battery outside window");
static_assert(Sq(kIndMaxW / 2) + Sq(kIndLineY + kInkBottom - kCy) <= Sq(kSafeR), "indicator outside window");
static_assert(kIndY + kIndH <= kIndLineY + kInkBottom, "voiceprint taller than the indicator line");
// The last 3 rows of the source grid are always empty.
static_assert(kAvatarBigY + kAvatarBigPx - 3 * kAvatarBigPx / 64 <= kMicY, "mic overlaps the big avatar");
static_assert(Sq(kMicW / 2 + 1) + Sq(kMicY + kMicH - kCy) <= Sq(kSafeR), "mic outside window");
static_assert(Sq(kMenuHeadMaxW / 2) + Sq(kCy - (kMenuHeadY + 3)) <= Sq(kSafeR), "menu heading outside window");
static_assert(Sq(kMenuRowMaxW / 2) + Sq(kCy - (kMenuAboveY + kInkTop)) <= Sq(kSafeR), "menu row outside window");
static_assert(Sq(kMenuRowMaxW / 2) + Sq(kMenuBelowY + kInkBottom - kCy) <= Sq(kSafeR), "menu row outside window");
static_assert(Sq(kMenuCardW / 2) + Sq(kCy - kMenuCardY) <= Sq(kSafeR), "menu card outside window");
static_assert(Sq(kMenuCardW / 2) + Sq(kMenuCardY + kMenuCardH - kCy) <= Sq(kSafeR), "menu card outside window");
constexpr bool MenuLinesFit(int i = 0) {
    return i == kMenuLines ||
           (Sq(kMenuLineMaxW[i] / 2) + Sq(std::max(kCy - (kMenuLineY[i] + kInkTop), kMenuLineY[i] + kInkBottom - kCy)) <= Sq(kSafeR) &&
            MenuLinesFit(i + 1));
}
static_assert(MenuLinesFit(), "menu page line outside window");

// Ring
constexpr int32_t  kRingRange = 1000;
constexpr int64_t  kRingFrameUs = 50 * 1000;
constexpr int64_t  kTalkRingUs = 20LL * 1000 * 1000;      // full ring after 20 s of talking
constexpr int64_t  kSpinDegPerSec = 300;
constexpr int32_t  kSpinArcDeg = 60;

// Indicator
constexpr int      kBars = 7, kBarW = 2, kBarGap = 2, kBarMinH = 2, kBarMaxH = kIndH;
constexpr int      kDots = 3, kDotD = 4, kDotGap = 3;
constexpr int64_t  kIndFrameUs = 50 * 1000;
constexpr float    kPi = 3.14159265f;

#if VOICE_UI_AVATAR
constexpr bool     kHasAvatar = true;
constexpr int64_t  kAvatarFrameUs = 50 * 1000;
static_assert(MUSE_PX_W == MUSE_PX_H && kAvatarBigPx <= MUSE_PX_W, "avatar is never scaled up");
#else
constexpr bool     kHasAvatar = false;
#endif

constexpr int64_t kThinkingTimeoutUs = 75LL * 1000 * 1000;   // the link reports a failure before this
constexpr int64_t kSpeakMinUs = 2LL * 1000 * 1000;
constexpr int64_t kSpeakMaxUs = 20LL * 1000 * 1000;

constexpr uint32_t kColBg     = 0x000000;
constexpr uint32_t kColText   = 0xF2F5FA;
constexpr uint32_t kColMuted  = 0x8A93A6;
constexpr uint32_t kColAccent = 0x2FA8FF;
constexpr uint32_t kColOk     = 0x35D07F;
constexpr uint32_t kColWarn   = 0xFFB020;
constexpr uint32_t kColErr    = 0xFF5C5C;
constexpr uint32_t kColListen = 0xFF4F8B;
constexpr uint32_t kColRingBg = 0x161B26;
constexpr uint32_t kColCard   = 0x1C2333;

enum class Mode { kLink, kIdle, kListening, kThinking, kSpeaking, kMenu };

Gc9d01Panel* s_panel = nullptr;
lv_display_t* s_disp = nullptr;
uint8_t*      s_lv_buf = nullptr;

// Widgets, render task only.
lv_obj_t* s_ring = nullptr;
lv_obj_t* s_battery = nullptr;
lv_obj_t* s_ind_text = nullptr;      // link icon or pairing code
lv_obj_t* s_bars[kBars] = {};        // voiceprint
lv_obj_t* s_dots[kDots] = {};        // thinking
lv_obj_t* s_toast = nullptr;
lv_obj_t* s_orb = nullptr;           // stands in for the avatar when there is none
lv_obj_t* s_mic = nullptr;
lv_obj_t* s_menu_head = nullptr;
lv_obj_t* s_menu_above = nullptr;
lv_obj_t* s_menu_card = nullptr;
lv_obj_t* s_menu_label = nullptr;
lv_obj_t* s_menu_value = nullptr;
lv_obj_t* s_menu_below = nullptr;
lv_obj_t* s_menu_lines[kMenuLines] = {};
#if VOICE_UI_AVATAR
lv_obj_t* s_avatar = nullptr;
uint16_t* s_avatar_buf = nullptr;    // canvas buffer, kAvatarBigPx^2 RGB565 in PSRAM
uint16_t* s_grid = nullptr;          // renderer output at 1:1, MUSE_PX_W^2 RGB565 in PSRAM
#endif

// Written by other tasks, read by the render task. Strings and structs are guarded by s_lock.
struct ToastMsg {
    char     text[32];
    Tone     tone;
    uint32_t ms;
};

struct MenuItem {
    char     heading[16];
    uint32_t heading_color;
    bool     list;
    char     above[20], label[20], value[20], below[20];
    uint32_t value_color;
    char     lines[kMenuLines][32];
    uint32_t line_colors[kMenuLines];
};

portMUX_TYPE          s_lock = portMUX_INITIALIZER_UNLOCKED;
agent_link_status_t   s_status = {};
std::atomic<uint32_t> s_status_rev{1};
ToastMsg              s_toast_msg = {};
std::atomic<uint32_t> s_toast_rev{0};
MenuItem              s_menu_item = {};
std::atomic<uint32_t> s_menu_rev{0};
std::atomic<bool>     s_menu_open{false};

std::atomic<int>      s_batt_pct{-1};
std::atomic<bool>     s_charging{false};
std::atomic<uint32_t> s_batt_rev{1};

std::atomic<bool>     s_listening{false};
std::atomic<int64_t>  s_listen_since{0};
std::atomic<int>      s_level_pct{0};
std::atomic<bool>     s_thinking{false};
std::atomic<int64_t>  s_thinking_since{0};
std::atomic<int64_t>  s_speak_from{0};
std::atomic<int64_t>  s_speak_until{0};
std::atomic<bool>     s_voice_on{false};     // spoken answer playing
std::atomic<int>      s_voice_level{0};      // its loudness, 0-100

bool HasAvatar() {
#if VOICE_UI_AVATAR
    return s_avatar != nullptr;
#else
    return false;
#endif
}

// LVGL redraws an object on every show, even if it is already visible.
void SetHidden(lv_obj_t* o, bool hidden) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Use the avatar's accent colours when it is there.
uint32_t ListenColor() {
#if VOICE_UI_AVATAR
    if (s_avatar) return muse_pixel_accent(MUSE_MODE_LISTENING);
#endif
    return kColListen;
}

uint32_t ThinkColor() {
#if VOICE_UI_AVATAR
    if (s_avatar) return muse_pixel_accent(MUSE_MODE_THINKING);
#endif
    return kColAccent;
}

uint32_t SpeakColor() {
#if VOICE_UI_AVATAR
    if (s_avatar) return muse_pixel_accent(MUSE_MODE_SPEAKING);
#endif
    return kColOk;
}

// Ring colour (and the orb's).
uint32_t MomentColor(Mode mode, agent_link_phase_t phase) {
    switch (mode) {
    case Mode::kLink:
        return phase == AGENT_LINK_PHASE_SETUP || phase == AGENT_LINK_PHASE_PAIRING ? kColAccent
             : phase == AGENT_LINK_PHASE_BLOCKED                                   ? kColErr
             : phase == AGENT_LINK_PHASE_CONNECTED                                 ? kColOk
                                                                                   : kColWarn;
    case Mode::kListening: return ListenColor();
    case Mode::kThinking:  return ThinkColor();
    case Mode::kSpeaking:  return SpeakColor();
    case Mode::kMenu:      return kColAccent;
    default:               return kColMuted;
    }
}

uint32_t ToneColor(Tone tone) {
    switch (tone) {
    case Tone::kOk:    return kColOk;
    case Tone::kError: return kColErr;
    case Tone::kMuted: return kColMuted;
    default:           return kColText;
    }
}

// ── Speaking time for text-only answers ──────────────────────────────────────────────────────

// Decodes one UTF-8 character; *len gets its byte length.
uint32_t Decode(const char* s, size_t* len) {
    const uint8_t b = static_cast<uint8_t>(s[0]);
    size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    uint32_t cp = n == 1 ? b : n == 2 ? (b & 0x1Fu) : n == 3 ? (b & 0x0Fu) : (b & 0x07u);
    for (size_t k = 1; k < n; ++k) {
        if ((static_cast<uint8_t>(s[k]) & 0xC0) != 0x80) { n = k; break; }   // malformed
        cp = cp << 6 | (static_cast<uint8_t>(s[k]) & 0x3Fu);
    }
    *len = n;
    return cp;
}

bool IsCjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

// Rough reading time: ~5 CJK characters/s, ~14 other characters/s.
int64_t SpeakUs(const char* text) {
    int64_t cjk = 0, other = 0;
    for (const char* p = text; p && *p;) {
        size_t len = 0;
        const uint32_t c = Decode(p, &len);
        if (IsCjk(c)) ++cjk;
        else if (c > ' ') ++other;
        p += len;
    }
    const int64_t us = cjk * 1000000 / 5 + other * 1000000 / 14;
    return us < kSpeakMinUs ? kSpeakMinUs : us > kSpeakMaxUs ? kSpeakMaxUs : us;
}

// Fake speech envelope for text-only answers: ~4.7 Hz syllables under a 0.6 Hz phrase.
float SpeechLevel(float t) {
    const float syllable = 0.5f + 0.5f * sinf(t * 2 * kPi * 4.7f);
    const float phrase = 0.55f + 0.45f * sinf(t * 2 * kPi * 0.6f + 1.3f);
    return 0.15f + 0.85f * syllable * phrase;
}

// 0..1. Real playback level if audio is playing, otherwise the fake envelope.
float SpeakingLevel(float t) {
    if (s_voice_on.load(std::memory_order_acquire)) {
        return static_cast<float>(s_voice_level.load(std::memory_order_acquire)) / 100.0f;
    }
    return SpeechLevel(t);
}

// ── Flush ────────────────────────────────────────────────────────────────────────────────────

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    // Partial mode: px_map holds just this area.
    const uint32_t w = static_cast<uint32_t>(lv_area_get_width(area));
    const uint32_t h = static_cast<uint32_t>(lv_area_get_height(area));
    lv_draw_sw_rgb565_swap(px_map, w * h);   // panel expects big-endian
    if (s_panel) {
        (void)s_panel->DrawBitmap(static_cast<uint16_t>(area->x1), static_cast<uint16_t>(area->y1),
                                  static_cast<uint16_t>(w), static_cast<uint16_t>(h), px_map);
    }
    lv_display_flush_ready(disp);   // DrawBitmap is blocking
}

uint32_t TickCb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ── Avatar scaling ───────────────────────────────────────────────────────────────────────────
#if VOICE_UI_AVATAR
// Box filter from the 64 px grid. Nearest-neighbour drops every 4th row/column at 48 px, which
// makes the eyes uneven.
static_assert(2 * kAvatarSmallPx >= MUSE_PX_W, "at most 2 source cells per pixel");

// Source cells covering one output pixel on one axis, weights in 1/px of a cell (sum = MUSE_PX_W).
struct Taps {
    uint8_t first;
    uint8_t n;
    uint8_t w[3];
};
Taps s_taps[kAvatarBigPx];

void InitTaps(int px) {
    for (int o = 0; o < px; ++o) {
        const int lo = o * MUSE_PX_W, hi = (o + 1) * MUSE_PX_W;
        Taps& t = s_taps[o];
        t.first = static_cast<uint8_t>(lo / px);
        t.n = 0;
        for (int c = t.first; c * px < hi; ++c) {
            t.w[t.n++] = static_cast<uint8_t>(std::min(hi, (c + 1) * px) - std::max(lo, c * px));
        }
    }
}

void ShrinkAvatar(int px) {
    if (px == MUSE_PX_W) {
        memcpy(s_avatar_buf, s_grid, static_cast<size_t>(px) * px * 2u);
        return;
    }
    constexpr uint32_t kNorm = MUSE_PX_W * MUSE_PX_W;
    for (int y = 0; y < px; ++y) {
        const Taps& ty = s_taps[y];
        for (int x = 0; x < px; ++x) {
            const Taps& tx = s_taps[x];
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < ty.n; ++j) {
                const uint16_t* row = s_grid + (ty.first + j) * MUSE_PX_W + tx.first;
                for (int i = 0; i < tx.n; ++i) {
                    const uint32_t w = static_cast<uint32_t>(ty.w[j]) * tx.w[i];
                    r += w * (row[i] >> 11);
                    g += w * ((row[i] >> 5) & 0x3F);
                    b += w * (row[i] & 0x1F);
                }
            }
            const uint32_t r5 = (r + kNorm / 2) / kNorm, g6 = (g + kNorm / 2) / kNorm, b5 = (b + kNorm / 2) / kNorm;
            s_avatar_buf[y * px + x] = static_cast<uint16_t>(r5 << 11 | g6 << 5 | b5);
        }
    }
}
#endif

// ── Widgets ──────────────────────────────────────────────────────────────────────────────────

lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// Single-line label with a fixed size. Don't use LONG_DOT with SIZE_CONTENT + max_width: LVGL
// wraps the new text at the old width, replaces it all with "..." and then shrinks to fit that.
void FixedLine(lv_obj_t* l, int w) {
    lv_obj_set_size(l, w, kLineH);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

lv_obj_t* Blob(lv_obj_t* parent, int w, int h, int radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

void Build() {
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

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

#if VOICE_UI_AVATAR
    // Size and position are set in DrawAvatar().
    s_grid = static_cast<uint16_t*>(
        heap_caps_malloc(static_cast<size_t>(MUSE_PX_W) * MUSE_PX_H * 2u, MALLOC_CAP_SPIRAM));
    s_avatar_buf = static_cast<uint16_t*>(
        heap_caps_malloc(static_cast<size_t>(kAvatarBigPx) * kAvatarBigPx * 2u, MALLOC_CAP_SPIRAM));
    if (s_grid && s_avatar_buf) {
        muse_pixel_set_size(MUSE_PX_W);   // render at 1:1, ShrinkAvatar() scales
        s_avatar = lv_canvas_create(root);
        lv_obj_remove_flag(s_avatar, LV_OBJ_FLAG_CLICKABLE);
    } else {
        ESP_LOGW(TAG, "no PSRAM for the avatar, using the orb");
        heap_caps_free(s_grid);
        heap_caps_free(s_avatar_buf);
        s_grid = s_avatar_buf = nullptr;
    }
#endif
    if (!HasAvatar()) {
        s_orb = Blob(root, kAvatarSmallPx / 2, kAvatarSmallPx / 2, LV_RADIUS_CIRCLE);
        lv_obj_remove_flag(s_orb, LV_OBJ_FLAG_HIDDEN);
    }

    // Created after the avatar so it draws on top of the canvas' top rows.
    s_battery = Label(root, &lv_font_montserrat_12, kColMuted);
    lv_obj_align(s_battery, LV_ALIGN_TOP_MID, 0, kBatteryY);

    s_ind_text = Label(root, &lv_font_montserrat_12, kColMuted);
    FixedLine(s_ind_text, kIndMaxW);
    lv_obj_set_pos(s_ind_text, kCx - kIndMaxW / 2, kIndLineY);
    lv_obj_add_flag(s_ind_text, LV_OBJ_FLAG_HIDDEN);
    constexpr int kWaveW = kBars * kBarW + (kBars - 1) * kBarGap;
    for (int i = 0; i < kBars; ++i) {
        s_bars[i] = Blob(root, kBarW, kBarMinH, 1);
        lv_obj_set_pos(s_bars[i], kCx - kWaveW / 2 + i * (kBarW + kBarGap), kIndY + kIndH / 2 - kBarMinH / 2);
    }
    constexpr int kDotsW = kDots * kDotD + (kDots - 1) * kDotGap;
    for (int i = 0; i < kDots; ++i) {
        s_dots[i] = Blob(root, kDotD, kDotD, LV_RADIUS_CIRCLE);
        lv_obj_set_pos(s_dots[i], kCx - kDotsW / 2 + i * (kDotD + kDotGap), kIndY + kIndH / 2 - kDotD / 2);
    }

    // Mic icon shown under the avatar when idle.
    {
        static const char* const kArt[kMicH] = {
            "..###..",
            ".#####.",
            ".#####.",
            ".#####.",
            ".#####.",
            "#.###.#",
            "#.....#",
            ".#...#.",
            "..###..",
            "...#...",
            "...#...",
            ".#####.",
        };
        static uint16_t pixels[kMicW * kMicH];
        constexpr uint16_t on565 = ((kColMuted >> 8) & 0xF800) | ((kColMuted >> 5) & 0x07E0) | ((kColMuted >> 3) & 0x001F);
        for (int y = 0; y < kMicH; ++y) {
            for (int x = 0; x < kMicW; ++x) pixels[y * kMicW + x] = kArt[y][x] == '#' ? on565 : 0;
        }
        s_mic = lv_canvas_create(root);
        lv_canvas_set_buffer(s_mic, pixels, kMicW, kMicH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_mic, kCx - kMicW / 2, kMicY);
        lv_obj_remove_flag(s_mic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_mic, LV_OBJ_FLAG_HIDDEN);
    }

    // Menu: heading + list (item above, selected item card, item below), or heading + page lines.
    auto menu_line = [root](int y, int w, uint32_t color) {
        lv_obj_t* l = Label(root, &lv_font_montserrat_12, color);
        FixedLine(l, w);
        lv_obj_set_pos(l, kCx - w / 2, y);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return l;
    };
    s_menu_head = menu_line(kMenuHeadY, kMenuHeadMaxW, kColMuted);
    lv_obj_set_style_text_letter_space(s_menu_head, 1, 0);
    s_menu_above = menu_line(kMenuAboveY, kMenuRowMaxW, kColMuted);
    s_menu_below = menu_line(kMenuBelowY, kMenuRowMaxW, kColMuted);
    s_menu_card = lv_obj_create(root);
    lv_obj_remove_style_all(s_menu_card);
    lv_obj_set_size(s_menu_card, kMenuCardW, kMenuCardH);
    lv_obj_set_pos(s_menu_card, kCx - kMenuCardW / 2, kMenuCardY);
    lv_obj_set_style_radius(s_menu_card, kMenuCardH / 2, 0);
    lv_obj_set_style_bg_color(s_menu_card, lv_color_hex(kColCard), 0);
    lv_obj_set_style_bg_opa(s_menu_card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_menu_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_menu_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_menu_card, LV_OBJ_FLAG_HIDDEN);
    // Name and value centred together; the card is too narrow to align them to the edges.
    lv_obj_set_flex_flow(s_menu_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_menu_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_menu_card, kMenuCardGap, 0);
    s_menu_label = Label(s_menu_card, &lv_font_montserrat_12, kColText);
    s_menu_value = Label(s_menu_card, &lv_font_montserrat_12, kColText);
    for (int i = 0; i < kMenuLines; ++i) s_menu_lines[i] = menu_line(kMenuLineY[i], kMenuLineMaxW[i], kColText);

    // Toast replaces the indicator; the opaque background hides whatever is under it.
    s_toast = Label(root, &lv_font_montserrat_12, kColText);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(s_toast, kIndMaxW, kLineH);
    lv_obj_set_pos(s_toast, kCx - kIndMaxW / 2, kIndLineY);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    // Labels lay out their text against their current size, so get real sizes before any text
    // is set.
    lv_obj_update_layout(root);
}

// ── Rendering ────────────────────────────────────────────────────────────────────────────────

Mode CurrentMode(bool ready, int64_t now) {
    if (s_menu_open.load(std::memory_order_acquire)) return Mode::kMenu;
    if (!ready) return Mode::kLink;
    if (s_listening.load(std::memory_order_acquire)) return Mode::kListening;
    if (s_voice_on.load(std::memory_order_acquire)) return Mode::kSpeaking;
    if (s_thinking.load(std::memory_order_acquire) &&
        now - s_thinking_since.load(std::memory_order_acquire) < kThinkingTimeoutUs) {
        return Mode::kThinking;
    }
    if (now < s_speak_until.load(std::memory_order_acquire)) return Mode::kSpeaking;
    return Mode::kIdle;
}

void RefreshBattery() {
    const int  pct = s_batt_pct.load(std::memory_order_acquire);
    const bool chg = s_charging.load(std::memory_order_acquire);
    char buf[24];
    if (pct >= 0) snprintf(buf, sizeof buf, "%s%d%%", chg ? LV_SYMBOL_CHARGE : "", pct > 100 ? 100 : pct);
    else          snprintf(buf, sizeof buf, "%s", LV_SYMBOL_BATTERY_EMPTY);
    lv_label_set_text(s_battery, buf);
    lv_obj_set_style_text_color(s_battery, lv_color_hex((pct >= 0 && pct < 15 && !chg) ? kColErr : kColMuted), 0);
}

enum class Ind { kNone, kText, kWave, kDots };

void ShowIndicator(Ind which, uint32_t color) {
    SetHidden(s_ind_text, which != Ind::kText);
    for (lv_obj_t* b : s_bars) {
        if (which == Ind::kWave) lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
        SetHidden(b, which != Ind::kWave);
    }
    for (lv_obj_t* d : s_dots) {
        if (which == Ind::kDots) lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
        SetHidden(d, which != Ind::kDots);
    }
}

void IndicatorText(const char* text, uint32_t color) {
    lv_label_set_text(s_ind_text, text);
    lv_obj_set_style_text_color(s_ind_text, lv_color_hex(color), 0);
    ShowIndicator(Ind::kText, color);
}

void SetBar(int i, float level) {
    const float v = level < 0 ? 0 : level > 1 ? 1 : level;
    const int h = kBarMinH + static_cast<int>(v * (kBarMaxH - kBarMinH) + 0.5f);
    lv_obj_set_height(s_bars[i], h);
    lv_obj_set_y(s_bars[i], kIndY + kIndH / 2 - h / 2);
}

// Applies the last ShowMenu() view. Hides every menu widget when !open.
void RefreshMenu(bool open) {
    MenuItem m;
    taskENTER_CRITICAL(&s_lock);
    m = s_menu_item;
    taskEXIT_CRITICAL(&s_lock);
    auto put = [](lv_obj_t* l, const char* text, uint32_t color, bool show) {
        show = show && text[0];
        if (show) {
            lv_label_set_text(l, text);
            lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        }
        SetHidden(l, !show);
    };
    const bool list = open && m.list;
    put(s_menu_head, m.heading, m.heading_color, open);
    put(s_menu_above, m.above, kColMuted, list);
    put(s_menu_below, m.below, kColMuted, list);
    SetHidden(s_menu_card, !list);
    if (list) {
        lv_label_set_text(s_menu_label, m.label);
        put(s_menu_value, m.value, m.value_color, true);   // hidden when empty so the label stays centred
    }
    for (int i = 0; i < kMenuLines; ++i) put(s_menu_lines[i], m.lines[i], m.line_colors[i], open && !m.list);
}

// Everything except the avatar and the ring.
void SetStateViews(Mode mode, const agent_link_status_t& st) {
    const bool menu = mode == Mode::kMenu;
    SetHidden(s_battery, menu);
    RefreshMenu(menu);

    switch (mode) {
    case Mode::kLink:
        switch (st.phase) {
        case AGENT_LINK_PHASE_SETUP:
            IndicatorText(LV_SYMBOL_BLUETOOTH, kColAccent);
            break;
        case AGENT_LINK_PHASE_PAIRING:
            IndicatorText(st.code[0] ? st.code : LV_SYMBOL_BLUETOOTH, st.code[0] ? kColText : kColAccent);
            break;
        case AGENT_LINK_PHASE_BLOCKED:
            IndicatorText(LV_SYMBOL_WARNING, kColErr);
            break;
        case AGENT_LINK_PHASE_CONNECTED:
            IndicatorText(LV_SYMBOL_OK, kColOk);
            break;
        default:
            IndicatorText(LV_SYMBOL_WIFI, kColWarn);
            break;
        }
        break;
    case Mode::kListening:
        ShowIndicator(Ind::kWave, ListenColor());
        break;
    case Mode::kThinking:
        ShowIndicator(Ind::kDots, ThinkColor());
        break;
    case Mode::kSpeaking:
        ShowIndicator(Ind::kWave, SpeakColor());
        break;
    case Mode::kIdle:   // the big avatar uses that space
    case Mode::kMenu:
        ShowIndicator(Ind::kNone, 0);
        break;
    }
}

struct IndicatorState {
    int64_t at = 0;
    float   history[kBars / 2 + 1] = {};   // listening: newest level first
};

void AnimateIndicator(IndicatorState& ind, Mode mode, int64_t now) {
    if (now - ind.at < kIndFrameUs) return;
    ind.at = now;
    const float t = static_cast<float>(now) / 1e6f;
    constexpr int kMid = kBars / 2;
    if (mode == Mode::kListening) {
        // Mic level scrolls outwards from the centre bar.
        for (int i = kMid; i > 0; --i) ind.history[i] = ind.history[i - 1];
        ind.history[0] = static_cast<float>(s_level_pct.load(std::memory_order_acquire)) / 100.0f;
        for (int i = 0; i < kBars; ++i) {
            const int d = i > kMid ? i - kMid : kMid - i;
            SetBar(i, ind.history[d] * (1.0f - 0.12f * static_cast<float>(d)));
        }
    } else if (mode == Mode::kSpeaking) {
        const float level = SpeakingLevel(t);
        for (int i = 0; i < kBars; ++i) {
            const int d = i > kMid ? i - kMid : kMid - i;
            const float shape = 1.0f - 0.16f * static_cast<float>(d);
            SetBar(i, level * shape * (0.7f + 0.3f * sinf(t * 13.0f + static_cast<float>(i) * 2.1f)));
        }
    } else if (mode == Mode::kThinking) {
        for (int i = 0; i < kDots; ++i) {
            const float pulse = 0.5f + 0.5f * sinf(t * 2 * kPi * 1.2f - static_cast<float>(i) * 0.9f);
            lv_obj_set_style_bg_opa(s_dots[i], static_cast<lv_opa_t>(70 + 185 * pulse), 0);
        }
    }
}

// Link: full ring while waiting for the user or blocked, spinner while connecting.
// Listening: fills over kTalkRingUs. Thinking: spinner. Speaking: full while audio plays,
// otherwise fills over the estimated reading time. Menu: full. Idle: track only.
struct RingState {
    uint32_t color = 0;
    int32_t  value = -1;   // -1 after spinning
    int64_t  at = 0;
};

void UpdateRing(RingState& r, Mode mode, agent_link_phase_t phase, int64_t now, bool force) {
    if (!force && now - r.at < kRingFrameUs) return;
    r.at = now;
    const uint32_t color = MomentColor(mode, phase);
    int32_t value = 0;
    bool spin = false;
    switch (mode) {
    case Mode::kLink:
        if (phase == AGENT_LINK_PHASE_SETUP || phase == AGENT_LINK_PHASE_PAIRING ||
            phase == AGENT_LINK_PHASE_BLOCKED || phase == AGENT_LINK_PHASE_CONNECTED) {
            value = kRingRange;
        } else {
            spin = true;
        }
        break;
    case Mode::kListening: {
        const int64_t t = now - s_listen_since.load(std::memory_order_acquire);
        value = t >= kTalkRingUs ? kRingRange : static_cast<int32_t>(t * kRingRange / kTalkRingUs);
        break;
    }
    case Mode::kThinking:
        spin = true;
        break;
    case Mode::kSpeaking: {
        if (s_voice_on.load(std::memory_order_acquire)) {
            value = kRingRange;
            break;
        }
        const int64_t from = s_speak_from.load(std::memory_order_acquire);
        const int64_t span = s_speak_until.load(std::memory_order_acquire) - from;
        value = span > 0 ? static_cast<int32_t>((now - from) * kRingRange / span) : kRingRange;
        if (value > kRingRange) value = kRingRange;
        break;
    }
    case Mode::kMenu:
        value = kRingRange;
        break;
    default:
        break;
    }
    if (color != r.color || force) {
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(color), LV_PART_INDICATOR);
        r.color = color;
    }
    if (spin) {
        // Move the indicator angles instead of rotating the arc; rotating redraws the full screen.
        const int32_t start = static_cast<int32_t>((now / 1000 * kSpinDegPerSec / 1000) % 360);
        lv_arc_set_angles(s_ring, start, start + kSpinArcDeg);
        r.value = -1;
    } else {
        // set_value() returns early if the value is unchanged, so restore the start angle first.
        if (r.value < 0) lv_arc_set_bg_start_angle(s_ring, 0);
        if (value != r.value) {
            lv_arc_set_value(s_ring, value);
            r.value = value;
        }
    }
}

// Size and position of the avatar (or orb), eased between the small and big layout.
struct Middle {
    int     px = kAvatarSmallPx, y = kAvatarSmallY;
    int     from_px = kAvatarSmallPx, from_y = kAvatarSmallY;
    int     to_px = kAvatarSmallPx, to_y = kAvatarSmallY;
    int64_t move_at = 0;
};

void MoveMiddle(Middle& m, bool big, int64_t now) {
    const int px = big ? kAvatarBigPx : kAvatarSmallPx;
    const int y  = big ? kAvatarBigY : kAvatarSmallY;
    if (px != m.to_px || y != m.to_y) {
        m.from_px = m.px;
        m.from_y  = m.y;
        m.to_px   = px;
        m.to_y    = y;
        m.move_at = now;
    }
    float e = static_cast<float>(now - m.move_at) / static_cast<float>(kMoveUs);
    e = e >= 1 ? 1 : 1 - (1 - e) * (1 - e);   // ease out
    m.px = m.from_px + static_cast<int>(lroundf(static_cast<float>(m.to_px - m.from_px) * e));
    m.y  = m.from_y + static_cast<int>(lroundf(static_cast<float>(m.to_y - m.from_y) * e));
}

// Orb used when there is no avatar renderer. Its size follows the mic or speech level.
struct OrbState {
    int      d = 0, cy = 0;
    uint32_t color = 0;
    int64_t  at = 0;
};

void AnimateOrb(OrbState& o, Mode mode, agent_link_phase_t phase, const Middle& m, int64_t now) {
    if (!s_orb) return;
    SetHidden(s_orb, mode == Mode::kMenu);
    if (mode == Mode::kMenu || now - o.at < kIndFrameUs) return;
    o.at = now;
    const float t = static_cast<float>(now) / 1e6f;
    float swell = 0;   // 0..1
    switch (mode) {
    case Mode::kListening:
        swell = static_cast<float>(s_level_pct.load(std::memory_order_acquire)) / 100.0f;
        break;
    case Mode::kSpeaking:
        swell = SpeakingLevel(t);
        break;
    case Mode::kThinking:
        swell = 0.2f + 0.2f * sinf(t * 2 * kPi * 1.2f);
        break;
    default:
        swell = 0.1f + 0.1f * sinf(t * 2 * kPi * 0.25f);
        break;
    }
    const uint32_t color = MomentColor(mode, phase);
    if (color != o.color) {
        lv_obj_set_style_bg_color(s_orb, lv_color_hex(color), 0);
        o.color = color;
    }
    const int d  = m.px / 2 + static_cast<int>(swell * static_cast<float>(m.px / 4) + 0.5f);
    const int cy = m.y + m.px / 2;
    if (d != o.d || cy != o.cy) {
        lv_obj_set_size(s_orb, d, d);
        lv_obj_set_pos(s_orb, kCx - d / 2, cy - d / 2);
        o.d = d;
        o.cy = cy;
    }
}

#if VOICE_UI_AVATAR
struct AvatarState {
    muse_mode_t mode = MUSE_MODE_BOOT;
    int64_t     since = 0;      // when `mode` started
    int64_t     frame_at = 0;
    int         px = 0, y = 0;  // current canvas size and position
};

// Link phases map to BOOT (connecting), ERROR (blocked) or IDLE.
void DrawAvatar(AvatarState& av, Mode mode, agent_link_phase_t phase, const Middle& m, int64_t now) {
    if (!s_avatar) return;
    SetHidden(s_avatar, mode == Mode::kMenu);
    if (mode == Mode::kMenu) return;
    muse_mode_t want = MUSE_MODE_IDLE;
    switch (mode) {
    case Mode::kLink:
        want = phase == AGENT_LINK_PHASE_BLOCKED ? MUSE_MODE_ERROR
             : phase == AGENT_LINK_PHASE_SETUP || phase == AGENT_LINK_PHASE_PAIRING ||
               phase == AGENT_LINK_PHASE_CONNECTED ? MUSE_MODE_IDLE
                                                  : MUSE_MODE_BOOT;
        break;
    case Mode::kListening: want = MUSE_MODE_LISTENING; break;
    case Mode::kThinking:  want = MUSE_MODE_THINKING;  break;
    case Mode::kSpeaking:  want = MUSE_MODE_SPEAKING;  break;
    default:               want = MUSE_MODE_IDLE;      break;
    }
    if (want != av.mode) {
        av.mode = want;
        av.since = now;
    }
    const bool resized = m.px != av.px;
    if (resized) {
        av.px = m.px;
        InitTaps(av.px);
        lv_canvas_set_buffer(s_avatar, s_avatar_buf, av.px, av.px, LV_COLOR_FORMAT_RGB565);
    }
    if (resized || m.y != av.y) {
        av.y = m.y;
        lv_obj_set_pos(s_avatar, kCx - av.px / 2, av.y);
    }
    if (!resized && now - av.frame_at < kAvatarFrameUs) return;
    av.frame_at = now;

    const float t = static_cast<float>(now) / 1e6f;
    muse_pose_t pose = {};
    pose.mode   = av.mode;
    pose.t      = t;
    pose.mode_t = static_cast<float>(now - av.since) / 1e6f;
    pose.level  = want == MUSE_MODE_LISTENING
                      ? static_cast<float>(s_level_pct.load(std::memory_order_acquire)) / 100.0f
                : want == MUSE_MODE_SPEAKING ? SpeakingLevel(t)
                                             : 0.0f;
    muse_pixel_render(&pose);
    muse_pixel_scale(s_grid, MUSE_PX_W, 0, MUSE_PX_W - 1, 0, MUSE_PX_H - 1);
    ShrinkAvatar(av.px);
    lv_obj_invalidate(s_avatar);
}
#endif

void RenderTask(void*) {
    Build();   // all LVGL calls happen on this task
    RefreshBattery();

    uint32_t seen_status = 0, seen_toast = 0, seen_batt = 0, seen_menu = 0;
    int64_t toast_until = 0;
    bool ready = false;
    Mode shown = Mode::kIdle;
    bool first = true;
    agent_link_status_t st = {};
    RingState ring;
    IndicatorState ind;
    OrbState orb;
    Middle middle;
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
            status_changed = true;
        }

        const uint32_t batt_rev = s_batt_rev.load(std::memory_order_acquire);
        if (batt_rev != seen_batt) { seen_batt = batt_rev; RefreshBattery(); }

        const uint32_t menu_rev = s_menu_rev.load(std::memory_order_acquire);
        const bool menu_changed = menu_rev != seen_menu;
        seen_menu = menu_rev;

        const Mode mode = CurrentMode(ready, now);
        const bool mode_changed = mode != shown || first;
        if (mode_changed || status_changed || menu_changed) {
            first = false;
            shown = mode;
            SetStateViews(mode, st);
        }
        if (mode_changed) ind = IndicatorState{};
        UpdateRing(ring, mode, st.phase, now, mode_changed || status_changed);
        AnimateIndicator(ind, mode, now);

        const uint32_t toast_rev = s_toast_rev.load(std::memory_order_acquire);
        if (toast_rev != seen_toast) {
            seen_toast = toast_rev;
            taskENTER_CRITICAL(&s_lock);
            const ToastMsg msg = s_toast_msg;
            taskEXIT_CRITICAL(&s_lock);
            lv_label_set_text(s_toast, msg.text);
            lv_obj_set_style_text_color(s_toast, lv_color_hex(ToneColor(msg.tone)), 0);
            lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            toast_until = now + static_cast<int64_t>(msg.ms) * 1000;
        }
        if (toast_until && (now > toast_until || mode == Mode::kMenu)) {
            toast_until = 0;
            lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        }

        // Big avatar + mic only when idle with no toast.
        const bool waiting = mode == Mode::kIdle && !toast_until;
        MoveMiddle(middle, waiting, now);
        SetHidden(s_mic, !waiting);
        AnimateOrb(orb, mode, st.phase, middle, now);
#if VOICE_UI_AVATAR
        DrawAvatar(avatar, mode, st.phase, middle, now);
#endif

        uint32_t next = lv_timer_handler();
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

    // Full-frame draw buffer in PSRAM (51 KB). Internal RAM is needed by Wi-Fi/BLE/TLS.
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
    lv_display_set_buffers(s_disp, s_lv_buf, nullptr, kFrameBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Stack must be in internal RAM: a PSRAM stack crashes while NVS writes disable the cache.
    // Pinned to the second core, away from Wi-Fi.
    if (xTaskCreatePinnedToCore(RenderTask, "voice_ui", 10240, nullptr, 4, nullptr,
                                portNUM_PROCESSORS - 1) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI up: %ux%u panel, %d px window%s", kUiW, kUiH, UI_VISIBLE_DIAMETER,
             kHasAvatar ? ", with avatar" : "");
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
        s_speak_until.store(0, std::memory_order_release);
        s_voice_on.store(false, std::memory_order_release);
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
    // Ignored while audio plays: playback decides when speaking ends. The text is cumulative, so
    // the timer restarts on every update.
    s_thinking.store(false, std::memory_order_release);
    if (s_voice_on.load(std::memory_order_acquire)) return;
    const int64_t now = esp_timer_get_time();
    s_speak_from.store(now, std::memory_order_release);
    s_speak_until.store(now + SpeakUs(utf8 ? utf8 : ""), std::memory_order_release);
}

void Ui::EndTurn() {
    s_thinking.store(false, std::memory_order_release);
    s_speak_until.store(0, std::memory_order_release);
}

void Ui::SpeechStart() {
    s_voice_level.store(0, std::memory_order_release);
    s_speak_until.store(0, std::memory_order_release);
    s_thinking.store(false, std::memory_order_release);
    s_voice_on.store(true, std::memory_order_release);
}

void Ui::SpeechLevel(int percent) {
    s_voice_level.store(percent < 0 ? 0 : percent > 100 ? 100 : percent, std::memory_order_release);
}

void Ui::SpeechEnd() {
    s_speak_until.store(0, std::memory_order_release);
    s_voice_on.store(false, std::memory_order_release);
}

void Ui::Toast(const char* text, uint32_t ms, Tone tone) {
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_toast_msg.text, text ? text : "", sizeof s_toast_msg.text);
    s_toast_msg.tone = tone;
    s_toast_msg.ms = ms;
    taskEXIT_CRITICAL(&s_lock);
    s_toast_rev.fetch_add(1, std::memory_order_release);
}

void Ui::ShowMenu(const MenuView& view) {
    MenuItem m = {};
    auto copy = [](char* dst, size_t cap, const char* src) { strlcpy(dst, src ? src : "", cap); };
    copy(m.heading, sizeof m.heading, view.heading);
    m.heading_color = view.heading_color;
    m.list = view.list;
    copy(m.above, sizeof m.above, view.above);
    copy(m.label, sizeof m.label, view.label);
    copy(m.value, sizeof m.value, view.value);
    m.value_color = view.value_color;
    copy(m.below, sizeof m.below, view.below);
    for (int i = 0; i < kMenuLines; ++i) {
        copy(m.lines[i], sizeof m.lines[i], view.lines[i]);
        m.line_colors[i] = view.line_colors[i];
    }
    taskENTER_CRITICAL(&s_lock);
    s_menu_item = m;
    taskEXIT_CRITICAL(&s_lock);
    s_menu_open.store(true, std::memory_order_release);
    s_menu_rev.fetch_add(1, std::memory_order_release);
}

void Ui::HideMenu() {
    s_menu_open.store(false, std::memory_order_release);
    s_menu_rev.fetch_add(1, std::memory_order_release);
}

}  // namespace voice
