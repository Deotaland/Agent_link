#include "watch_ui.h"

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

namespace watch {
namespace {

constexpr const char* TAG = "watch.ui";

constexpr int32_t kUiW = DISPLAY_WIDTH;    // 466
constexpr int32_t kUiH = DISPLAY_HEIGHT;   // 466
constexpr size_t  kBufBytes = static_cast<size_t>(kUiW) * UI_DRAW_BUF_ROWS * 2u;

// The glass is round: text keeps to a centred column, views fill the band between the header
// (battery, status) and the toast.
constexpr int32_t kTextW = 340;
constexpr int32_t kViewY = 100;
constexpr int32_t kViewH = 280;

// How long the talk-cycle screens hold before falling back to idle.
constexpr int64_t  kThinkingTimeoutUs = 75LL * 1000 * 1000;   // the link reports its own failure first
constexpr int64_t  kAnswerHoldUs      = 120LL * 1000 * 1000;
constexpr int64_t  kDimAfterUs        = 30LL * 1000 * 1000;   // idle screen dims, against burn-in
constexpr int      kScrollPxPerSec    = 30;
constexpr uint32_t kScrollDelayMs     = 2000;

// Voice meter: bars across the middle, tallest in the centre.
constexpr int     kBars    = 9;
constexpr int32_t kBarW    = 14;
constexpr int32_t kBarGap  = 10;
constexpr int32_t kBarMinH = 8;
constexpr int32_t kBarMaxH = 100;
constexpr float   kBarWeight[kBars] = {0.45f, 0.6f, 0.8f, 0.92f, 1.0f, 0.92f, 0.8f, 0.6f, 0.45f};

// Dark ground, one accent per state: black AMOLED pixels are off.
constexpr uint32_t kColBg     = 0x000000;
constexpr uint32_t kColText   = 0xF2F5FA;
constexpr uint32_t kColMuted  = 0x8A93A6;
constexpr uint32_t kColAccent = 0x2FA8FF;
constexpr uint32_t kColOk     = 0x35D07F;
constexpr uint32_t kColWarn   = 0xFFB020;
constexpr uint32_t kColErr    = 0xFF5C5C;
constexpr uint32_t kColListen = 0xFF4F8B;
constexpr uint32_t kColToast  = 0x1C2333;

enum class Mode { kLink, kIdle, kListening, kThinking, kAnswer };

Co5300Panel*  s_panel = nullptr;
lv_display_t* s_disp = nullptr;
uint8_t*      s_buf = nullptr;     // LVGL renders here, PSRAM

// Widgets, owned by the render task.
lv_obj_t* s_battery = nullptr;
lv_obj_t* s_dot = nullptr;
lv_obj_t* s_caption = nullptr;
lv_obj_t* s_link_view = nullptr;
lv_obj_t* s_link_title = nullptr;
lv_obj_t* s_link_code = nullptr;
lv_obj_t* s_link_hint = nullptr;
lv_obj_t* s_idle_view = nullptr;
lv_obj_t* s_idle_mic = nullptr;
lv_obj_t* s_idle_ok = nullptr;
lv_obj_t* s_idle_title = nullptr;
lv_obj_t* s_idle_hint = nullptr;
lv_obj_t* s_listen_view = nullptr;
lv_obj_t* s_bars[kBars] = {};
lv_obj_t* s_think_view = nullptr;
lv_obj_t* s_think_label = nullptr;
lv_obj_t* s_answer_view = nullptr;
lv_obj_t* s_answer = nullptr;
lv_obj_t* s_toast = nullptr;

// Cross-task inputs. Strings sit behind one spinlock; the render task copies them out.
portMUX_TYPE          s_lock = portMUX_INITIALIZER_UNLOCKED;
agent_link_status_t   s_status = {};
std::atomic<uint32_t> s_status_rev{1};
char                  s_answer_text[1024] = {};
std::atomic<uint32_t> s_answer_rev{0};
char                  s_toast_text[96] = {};
std::atomic<uint32_t> s_toast_rev{0};
std::atomic<uint32_t> s_toast_ms{0};
std::atomic<uint32_t> s_wake_rev{0};

std::atomic<int>      s_batt_pct{-1};
std::atomic<bool>     s_charging{false};
std::atomic<uint32_t> s_batt_rev{1};

std::atomic<bool>     s_listening{false};
std::atomic<int>      s_level_pct{0};
std::atomic<bool>     s_thinking{false};
std::atomic<int64_t>  s_thinking_since{0};
std::atomic<int64_t>  s_answer_at{0};

// ── Flush ────────────────────────────────────────────────────────────────────────────────────

// The CO5300 takes only windows that start on an even row and column and have an even size.
void RounderCb(lv_event_t* e) {
    auto* a = static_cast<lv_area_t*>(lv_event_get_param(e));
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(px_map, static_cast<uint32_t>(w * h));   // the panel wants big-endian
    if (s_panel) {
        (void)s_panel->DrawBitmap(static_cast<uint16_t>(area->x1), static_cast<uint16_t>(area->y1),
                                  static_cast<uint16_t>(w), static_cast<uint16_t>(h), px_map);
    }
    lv_display_flush_ready(disp);   // DrawBitmap returns once the pixels are out
}

uint32_t TickCb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ── Construction ─────────────────────────────────────────────────────────────────────────────

lv_obj_t* Plain(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* Block(lv_obj_t* parent, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    lv_obj_t* o = Plain(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// A centred line or paragraph of fixed width.
lv_obj_t* TextBlock(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int32_t w,
                    lv_label_long_mode_t mode) {
    lv_obj_t* l = Label(parent, font, color);
    lv_label_set_long_mode(l, mode);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

// A view across the middle band; exactly one is visible at a time.
lv_obj_t* View(lv_obj_t* root) {
    lv_obj_t* v = Plain(root);
    lv_obj_set_size(v, kUiW, kViewH);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 0, kViewY);
    lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    return v;
}

// A microphone from plain shapes: capsule, cradle, stem, base. 96 x 112.
lv_obj_t* MicIcon(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* box = Plain(parent);
    lv_obj_set_size(box, 96, 112);
    lv_obj_t* capsule = Block(box, 40, 64, 20, color);
    lv_obj_align(capsule, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t* cradle = lv_arc_create(box);
    lv_obj_set_size(cradle, 84, 84);
    lv_obj_align(cradle, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_bg_angles(cradle, 0, 180);   // 0 is 3 o'clock, clockwise: the lower half
    lv_obj_remove_style(cradle, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_opa(cradle, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(cradle, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(cradle, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(cradle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* stem = Block(box, 7, 20, 0, color);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_t* base = Block(box, 44, 8, 4, color);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, 104);
    return box;
}

void Build() {
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // Header: battery on top, then a status dot and caption.
    s_battery = Label(root, &lv_font_montserrat_20, kColMuted);
    lv_obj_align(s_battery, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t* status_row = Plain(root);
    lv_obj_set_size(status_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 10, 0);
    lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, 62);
    s_dot = Block(status_row, 12, 12, LV_RADIUS_CIRCLE, kColMuted);
    s_caption = Label(status_row, &lv_font_montserrat_20, kColMuted);

    // Link: what the user has to do to get connected, in the SDK's words.
    s_link_view = View(root);
    s_link_title = TextBlock(s_link_view, &lv_font_montserrat_28, kColText, 380, LV_LABEL_LONG_DOT);
    lv_obj_align(s_link_title, LV_ALIGN_TOP_MID, 0, 16);
    s_link_code = Label(s_link_view, &lv_font_montserrat_48, kColAccent);
    lv_obj_set_style_text_letter_space(s_link_code, 6, 0);
    lv_obj_align(s_link_code, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_add_flag(s_link_code, LV_OBJ_FLAG_HIDDEN);
    s_link_hint = TextBlock(s_link_view, &lv_font_montserrat_24, kColMuted, kTextW, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_link_hint, LV_ALIGN_TOP_MID, 0, 66);

    // Idle: a microphone when the link carries voice, a tick when it is online without it.
    s_idle_view = View(root);
    s_idle_mic = MicIcon(s_idle_view, kColText);
    lv_obj_align(s_idle_mic, LV_ALIGN_TOP_MID, 0, 0);
    s_idle_ok = Label(s_idle_view, &lv_font_montserrat_48, kColOk);
    lv_label_set_text(s_idle_ok, LV_SYMBOL_OK);
    lv_obj_align(s_idle_ok, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_flag(s_idle_ok, LV_OBJ_FLAG_HIDDEN);
    s_idle_title = TextBlock(s_idle_view, &lv_font_montserrat_28, kColText, 380, LV_LABEL_LONG_DOT);
    lv_obj_align(s_idle_title, LV_ALIGN_TOP_MID, 0, 128);
    s_idle_hint = TextBlock(s_idle_view, &lv_font_montserrat_24, kColMuted, kTextW, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_idle_hint, LV_ALIGN_TOP_MID, 0, 172);

    // Listening: the voice meter while the key is held.
    s_listen_view = View(root);
    lv_obj_t* listen = Label(s_listen_view, &lv_font_montserrat_28, kColListen);
    lv_label_set_text(listen, "Listening");
    lv_obj_align(listen, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t* meter = Plain(s_listen_view);
    constexpr int32_t kMeterW = kBars * kBarW + (kBars - 1) * kBarGap;
    lv_obj_set_size(meter, kMeterW, kBarMaxH);
    lv_obj_align(meter, LV_ALIGN_TOP_MID, 0, 76);
    for (int i = 0; i < kBars; ++i) {
        s_bars[i] = Block(meter, kBarW, kBarMinH, kBarW / 2, kColListen);
        lv_obj_align(s_bars[i], LV_ALIGN_LEFT_MID, i * (kBarW + kBarGap), 0);
    }
    lv_obj_t* release = Label(s_listen_view, &lv_font_montserrat_24, kColMuted);
    lv_label_set_text(release, "Release to send");
    lv_obj_align(release, LV_ALIGN_TOP_MID, 0, 200);

    // Thinking: between release and the answer.
    s_think_view = View(root);
    lv_obj_t* spinner = lv_spinner_create(s_think_view);
    lv_spinner_set_anim_params(spinner, 1000, 270);
    lv_obj_set_size(spinner, 96, 96);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_arc_width(spinner, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x262B38), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(kColAccent), LV_PART_INDICATOR);
    lv_obj_remove_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
    s_think_label = Label(s_think_view, &lv_font_montserrat_28, kColAccent);
    lv_obj_align(s_think_label, LV_ALIGN_TOP_MID, 0, 150);

    // Answer: wrapped text that scrolls itself when it is longer than the screen.
    s_answer_view = View(root);
    lv_obj_set_size(s_answer_view, kTextW, kViewH - 10);
    lv_obj_align(s_answer_view, LV_ALIGN_TOP_MID, 0, kViewY + 5);
    lv_obj_add_flag(s_answer_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_answer_view, LV_SCROLLBAR_MODE_OFF);
    s_answer = TextBlock(s_answer_view, &lv_font_montserrat_24, kColText, kTextW, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_answer, 4, 0);
    lv_obj_align(s_answer, LV_ALIGN_TOP_MID, 0, 0);

    // Toast: a fixed pill near the bottom edge, narrow enough for the circle there.
    s_toast = TextBlock(root, &lv_font_montserrat_20, kColText, 280, LV_LABEL_LONG_DOT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(kColToast), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 20, 0);
    lv_obj_set_style_pad_ver(s_toast, 8, 0);
    lv_obj_set_style_pad_hor(s_toast, 12, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 372);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

// ── Rendering ────────────────────────────────────────────────────────────────────────────────

// Reached the peer: the platform (CONNECTED) or a link that also carries data (READY).
bool Online(agent_link_phase_t p) { return p == AGENT_LINK_PHASE_CONNECTED || p == AGENT_LINK_PHASE_READY; }

uint32_t PhaseColor(agent_link_phase_t p) {
    switch (p) {
    case AGENT_LINK_PHASE_READY:
    case AGENT_LINK_PHASE_CONNECTED: return kColOk;
    case AGENT_LINK_PHASE_BLOCKED:   return kColErr;
    case AGENT_LINK_PHASE_SETUP:
    case AGENT_LINK_PHASE_PAIRING:   return kColAccent;
    default:                         return kColWarn;
    }
}

const char* PhaseCaption(agent_link_phase_t p) {
    switch (p) {
    case AGENT_LINK_PHASE_SETUP:      return "Setup";
    case AGENT_LINK_PHASE_PAIRING:    return "Pairing";
    case AGENT_LINK_PHASE_BLOCKED:    return "Action needed";
    case AGENT_LINK_PHASE_CONNECTED:
    case AGENT_LINK_PHASE_READY:      return "Online";
    case AGENT_LINK_PHASE_IDLE:       return "Starting";
    default:                          return "Connecting";
    }
}

void RefreshStatus(const agent_link_status_t& st) {
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(PhaseColor(st.phase)), 0);
    lv_label_set_text(s_caption, PhaseCaption(st.phase));

    lv_label_set_text(s_link_title, st.title[0] ? st.title : "Starting");
    const bool code = st.phase == AGENT_LINK_PHASE_PAIRING && st.code[0];
    if (code) {
        lv_label_set_text(s_link_code, st.code);
        lv_obj_remove_flag(s_link_code, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_link_code, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_link_hint, st.hint);
    lv_obj_align(s_link_hint, LV_ALIGN_TOP_MID, 0, code ? 140 : 66);

    lv_label_set_text(s_idle_title, st.title[0] ? st.title : "Ready");
}

// Idle shows how to talk only when the link carries voice; otherwise the status hint.
void RefreshIdle(bool can_talk, const agent_link_status_t& st) {
    if (can_talk) {
        lv_obj_remove_flag(s_idle_mic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_idle_ok, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_idle_hint, "Hold the yellow key to talk");
    } else {
        lv_obj_add_flag(s_idle_mic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_idle_ok, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_idle_hint, st.hint);
    }
}

void RefreshBattery() {
    const int  pct = s_batt_pct.load(std::memory_order_acquire);
    const bool chg = s_charging.load(std::memory_order_acquire);
    const char* sym = pct < 0   ? LV_SYMBOL_BATTERY_EMPTY
                    : pct >= 85 ? LV_SYMBOL_BATTERY_FULL
                    : pct >= 60 ? LV_SYMBOL_BATTERY_3
                    : pct >= 35 ? LV_SYMBOL_BATTERY_2
                    : pct >= 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char buf[32];
    if (pct >= 0) snprintf(buf, sizeof buf, "%s%s %d%%", chg ? LV_SYMBOL_CHARGE " " : "", sym, pct > 100 ? 100 : pct);
    else          snprintf(buf, sizeof buf, "%s", sym);
    lv_label_set_text(s_battery, buf);
    lv_obj_set_style_text_color(s_battery, lv_color_hex((pct >= 0 && pct < 15 && !chg) ? kColErr : kColMuted), 0);
}

void ShowOnly(lv_obj_t* view) {
    lv_obj_t* const views[] = {s_link_view, s_idle_view, s_listen_view, s_think_view, s_answer_view};
    for (lv_obj_t* v : views) {
        if (v == view) lv_obj_remove_flag(v, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    }
}

void ScrollExec(void* obj, int32_t y) { lv_obj_scroll_to_y(static_cast<lv_obj_t*>(obj), y, LV_ANIM_OFF); }

// New answer: back to the top, and if it runs past the bottom, roll it up at reading pace.
void ShowAnswer(const char* text) {
    lv_anim_delete(s_answer_view, ScrollExec);
    lv_label_set_text(s_answer, text);
    lv_obj_scroll_to_y(s_answer_view, 0, LV_ANIM_OFF);
    lv_obj_update_layout(s_answer_view);
    const int32_t overflow = lv_obj_get_height(s_answer) + 4 - lv_obj_get_content_height(s_answer_view);
    if (overflow <= 0) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_answer_view);
    lv_anim_set_exec_cb(&a, ScrollExec);
    lv_anim_set_values(&a, 0, overflow);
    lv_anim_set_delay(&a, kScrollDelayMs);
    lv_anim_set_duration(&a, static_cast<uint32_t>(overflow) * 1000u / kScrollPxPerSec);
    lv_anim_start(&a);
}

// The bars follow the mic level, falling back slowly, with a small ripple so they never sit still.
void DrawMeter(float& shown, int64_t now) {
    const float level = static_cast<float>(s_level_pct.load(std::memory_order_acquire)) / 100.0f;
    shown = level > shown ? level : shown * 0.85f;
    const float t = static_cast<float>(now) / 1e6f;
    for (int i = 0; i < kBars; ++i) {
        const float ripple = 0.8f + 0.2f * sinf(t * 9.0f + static_cast<float>(i) * 0.9f);
        const int32_t h = kBarMinH + static_cast<int32_t>(
            static_cast<float>(kBarMaxH - kBarMinH) * shown * kBarWeight[i] * ripple);
        lv_obj_set_height(s_bars[i], h);
    }
}

Mode CurrentMode(bool online, int64_t now) {
    if (!online) return Mode::kLink;
    if (s_listening.load(std::memory_order_acquire)) return Mode::kListening;
    if (s_thinking.load(std::memory_order_acquire) &&
        now - s_thinking_since.load(std::memory_order_acquire) < kThinkingTimeoutUs) {
        return Mode::kThinking;
    }
    const int64_t at = s_answer_at.load(std::memory_order_acquire);
    if (at && now - at < kAnswerHoldUs) return Mode::kAnswer;
    return Mode::kIdle;
}

void RenderTask(void*) {
    Build();
    RefreshBattery();

    uint32_t seen_status = 0, seen_answer = 0, seen_toast = 0, seen_batt = 0, seen_wake = 0;
    int64_t toast_until = 0;
    int64_t last_activity = esp_timer_get_time();
    bool online = false;
    bool can_talk = false;
    bool first = true;
    Mode shown = Mode::kLink;
    int dots = 0;
    int64_t dots_at = 0;
    float meter = 0.0f;
    uint8_t brightness = DISPLAY_BRIGHTNESS;
    agent_link_status_t st = {};
    static char answer[sizeof s_answer_text];   // render task only

    while (true) {
        const int64_t now = esp_timer_get_time();
        bool activity = false;

        const uint32_t status_rev = s_status_rev.load(std::memory_order_acquire);
        if (status_rev != seen_status) {
            seen_status = status_rev;
            taskENTER_CRITICAL(&s_lock);
            st = s_status;
            taskEXIT_CRITICAL(&s_lock);
            online = Online(st.phase);
            RefreshStatus(st);
            RefreshIdle(can_talk, st);
            activity = true;
        }

        // The data plane, not the status: whether a voice stream would open right now.
        const bool talk = agent_link_state() == AGENT_STATE_READY;
        if (talk != can_talk) {
            can_talk = talk;
            RefreshIdle(can_talk, st);
        }

        const uint32_t batt_rev = s_batt_rev.load(std::memory_order_acquire);
        if (batt_rev != seen_batt) { seen_batt = batt_rev; RefreshBattery(); }

        const uint32_t answer_rev = s_answer_rev.load(std::memory_order_acquire);
        if (answer_rev != seen_answer) {
            seen_answer = answer_rev;
            taskENTER_CRITICAL(&s_lock);
            memcpy(answer, s_answer_text, sizeof answer);
            taskEXIT_CRITICAL(&s_lock);
            ShowAnswer(answer);
            activity = true;
        }

        const uint32_t wake_rev = s_wake_rev.load(std::memory_order_acquire);
        if (wake_rev != seen_wake) { seen_wake = wake_rev; activity = true; }

        const Mode mode = CurrentMode(online, now);
        if (mode != shown || first) {
            first = false;
            shown = mode;
            activity = true;
            switch (mode) {
            case Mode::kLink:      ShowOnly(s_link_view); break;
            case Mode::kIdle:      ShowOnly(s_idle_view); break;
            case Mode::kListening: ShowOnly(s_listen_view); meter = 0.0f; break;
            case Mode::kThinking:  ShowOnly(s_think_view); dots_at = 0; break;
            case Mode::kAnswer:    ShowOnly(s_answer_view); break;
            }
        }
        if (mode == Mode::kListening) {
            DrawMeter(meter, now);
            activity = true;
        } else if (mode == Mode::kThinking) {
            if (now - dots_at > 400 * 1000) {
                dots_at = now;
                dots = (dots + 1) % 4;
                static const char* const kDots[] = {"Thinking", "Thinking.", "Thinking..", "Thinking..."};
                lv_label_set_text(s_think_label, kDots[dots]);
            }
            activity = true;
        }

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
            activity = true;
        }
        if (toast_until && now > toast_until) {
            toast_until = 0;
            lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        }

        // Only the idle screen dims: setup instructions and answers stay readable.
        if (activity) last_activity = now;
        const uint8_t want = (mode == Mode::kIdle && now - last_activity > kDimAfterUs)
                                 ? DISPLAY_BRIGHTNESS_DIM : DISPLAY_BRIGHTNESS;
        if (want != brightness && s_panel->SetBrightness(want) == ESP_OK) brightness = want;

        uint32_t next = lv_timer_handler();   // renders + flushes only if something changed
        if (next == LV_NO_TIMER_READY || next > 30) next = 30;
        vTaskDelay(pdMS_TO_TICKS(next < 10 ? 10 : next));
    }
}

}  // namespace

Ui& Ui::Instance() {
    static Ui instance;
    return instance;
}

esp_err_t Ui::Start(Co5300Panel* panel) {
    if (!panel || !panel->Ready()) return ESP_ERR_INVALID_STATE;
    s_panel = panel;

    // Partial buffer in PSRAM: internal RAM is what the radios and TLS need. The flush copies it
    // into the panel's internal DMA stripes.
    s_buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(4, kBufBytes, MALLOC_CAP_SPIRAM));
    if (!s_buf) {
        ESP_LOGE(TAG, "draw buffer alloc failed (%u bytes)", static_cast<unsigned>(kBufBytes));
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    lv_tick_set_cb(TickCb);
    s_disp = lv_display_create(kUiW, kUiH);
    if (!s_disp) return ESP_FAIL;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, FlushCb);
    lv_display_add_event_cb(s_disp, RounderCb, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_set_buffers(s_disp, s_buf, nullptr, kBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Internal stack: LVGL's draw path is deep, and a PSRAM stack faults while flash writes (NVS)
    // have the cache disabled. On the last core: the first belongs to the radio and the network.
    if (xTaskCreatePinnedToCore(RenderTask, "watch_ui", 8192, nullptr, 4, nullptr,
                                portNUM_PROCESSORS - 1) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI up: %ldx%ld, %u-row partial buffer", static_cast<long>(kUiW),
             static_cast<long>(kUiH), static_cast<unsigned>(UI_DRAW_BUF_ROWS));
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
    s_listening.store(on, std::memory_order_release);
    if (on) {
        s_thinking.store(false, std::memory_order_release);
        s_answer_at.store(0, std::memory_order_release);
        s_level_pct.store(0, std::memory_order_release);
    }
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

void Ui::Wake() { s_wake_rev.fetch_add(1, std::memory_order_release); }

}  // namespace watch
