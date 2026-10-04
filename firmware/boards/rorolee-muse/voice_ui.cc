#include "voice_ui.h"

#include "cjk_font.h"

#include <atomic>
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

constexpr uint16_t kUiW = UI_WIDTH;    // 240
constexpr uint16_t kUiH = UI_HEIGHT;   // 120
constexpr size_t   kFrameBytes = static_cast<size_t>(kUiW) * kUiH * 2u;
constexpr int      kPad = 8;
constexpr int      kHeaderH = 20;

// With an avatar, the talk-cycle screens put it on the left and their text in the column beside
// it; the link screens keep the full width for their instructions.
#if VOICE_UI_AVATAR
constexpr bool     kHasAvatar = true;
constexpr int      kAvatarPx = 96;                               // the 64x64 grid, x1.5
constexpr int      kAvatarX  = 4;
constexpr int      kAvatarY  = kHeaderH + 2;
constexpr int      kPanelX   = kAvatarX + kAvatarPx + 4;         // 104
constexpr int64_t  kAvatarFrameUs = 50 * 1000;                   // 20 fps
#else
constexpr bool     kHasAvatar = false;
constexpr int      kPanelX   = 0;
#endif
constexpr int      kPanelW   = kUiW - kPanelX;
constexpr int      kTextW    = kPanelW - 2 * kPad;

// How long the talk-cycle screens hold before falling back to idle.
constexpr int64_t kThinkingTimeoutUs = 75LL * 1000 * 1000;   // the link reports its own failure first
constexpr int64_t kAnswerHoldUs      = 120LL * 1000 * 1000;
constexpr int     kScrollPxPerSec    = 22;                    // a little under reading pace
constexpr uint32_t kScrollDelayMs    = 2000;

// Dark ground, one accent per state: black AMOLED pixels are off, so this costs nothing to keep lit.
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

Sh8501LkPanel* s_panel = nullptr;
lv_display_t*  s_disp = nullptr;
uint8_t*       s_lv_buf = nullptr;    // LVGL renders here (landscape)
uint8_t*       s_rot_buf = nullptr;   // rotated + byte-swapped copy for the panel (portrait)

// Widgets, owned by the render task.
lv_obj_t* s_dot = nullptr;
lv_obj_t* s_caption = nullptr;
lv_obj_t* s_battery = nullptr;
lv_obj_t* s_link_view = nullptr;
lv_obj_t* s_link_title = nullptr;
lv_obj_t* s_link_hint = nullptr;
lv_obj_t* s_idle_view = nullptr;
lv_obj_t* s_idle_name = nullptr;
lv_obj_t* s_listen_view = nullptr;
lv_obj_t* s_level = nullptr;
lv_obj_t* s_think_view = nullptr;
lv_obj_t* s_think_label = nullptr;
lv_obj_t* s_answer_view = nullptr;
lv_obj_t* s_answer = nullptr;
lv_obj_t* s_toast = nullptr;
#if VOICE_UI_AVATAR
lv_obj_t* s_avatar = nullptr;
uint16_t* s_avatar_buf = nullptr;     // kAvatarPx^2 RGB565, PSRAM
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
std::atomic<int>      s_level_pct{0};
std::atomic<bool>     s_thinking{false};
std::atomic<int64_t>  s_thinking_since{0};
std::atomic<int64_t>  s_answer_at{0};
std::atomic<uint32_t> s_cycle_rev{1};

// The answer may be in any language. Montserrat is Latin-only; the CJK font from flash (when the
// partition holds one) backs it up glyph by glyph. Loaded by the render task before Build().
lv_font_t* s_cjk = nullptr;

const lv_font_t* TextFont() {
    static lv_font_t with_cjk;
    if (!s_cjk) return &lv_font_montserrat_14;
    if (!with_cjk.fallback) {
        with_cjk = lv_font_montserrat_14;
        with_cjk.fallback = s_cjk;
    }
    return &with_cjk;
}

// CJK glyphs stand taller than Montserrat's 16 px line; give wrapped text room between lines.
void MixedTextLines(lv_obj_t* label) {
    if (s_cjk) lv_obj_set_style_text_line_space(label, 3, 0);
}

// ── Flush: rotate the landscape frame onto the portrait panel, swapping to big-endian ─────────

void RotateAndSwap(const uint16_t* src, uint16_t* dst) {
    constexpr uint16_t kPw = DISPLAY_WIDTH;   // panel columns == UI height
    for (uint16_t y = 0; y < kUiH; ++y) {
        const uint16_t* row = src + static_cast<size_t>(y) * kUiW;
        for (uint16_t x = 0; x < kUiW; ++x) {
            const uint16_t px = __builtin_bswap16(row[x]);
#if UI_ROTATE_CCW
            dst[static_cast<size_t>(kUiW - 1 - x) * kPw + y] = px;
#else
            dst[static_cast<size_t>(x) * kPw + (kUiH - 1 - y)] = px;
#endif
        }
    }
}

void FlushCb(lv_display_t* disp, const lv_area_t* /*area*/, uint8_t* px_map) {
    // LV_DISPLAY_RENDER_MODE_FULL: px_map is always the whole frame.
    RotateAndSwap(reinterpret_cast<const uint16_t*>(px_map), reinterpret_cast<uint16_t*>(s_rot_buf));
    if (s_panel) (void)s_panel->DrawBitmap(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_rot_buf);
    lv_display_flush_ready(disp);   // the LK driver blocks until the pixels are out
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

// A view under the header, x..x+w wide; exactly one is visible at a time.
lv_obj_t* View(lv_obj_t* root, int x, int w) {
    lv_obj_t* v = lv_obj_create(root);
    lv_obj_set_size(v, w, kUiH - kHeaderH);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, x, kHeaderH);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_radius(v, 0, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    return v;
}

void Build() {
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // Header: status dot + caption on the left, battery on the right.
    s_dot = lv_obj_create(root);
    lv_obj_set_size(s_dot, 8, 8);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(kColMuted), 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_dot, LV_ALIGN_TOP_LEFT, kPad, 6);

    s_caption = Label(root, &lv_font_montserrat_14, kColMuted);
    lv_label_set_long_mode(s_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_caption, 150);
    lv_obj_align(s_caption, LV_ALIGN_TOP_LEFT, kPad + 14, 2);

    s_battery = Label(root, &lv_font_montserrat_14, kColMuted);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -kPad, 2);

    // Link: what the user has to do to get connected, in the SDK's words. Always full width.
    s_link_view = View(root, 0, kUiW);
    s_link_title = Label(s_link_view, &lv_font_montserrat_20, kColText);
    lv_label_set_long_mode(s_link_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_link_title, kUiW - 2 * kPad);
    lv_obj_align(s_link_title, LV_ALIGN_TOP_LEFT, kPad, 2);
    s_link_hint = Label(s_link_view, TextFont(), kColMuted);
    MixedTextLines(s_link_hint);
    lv_label_set_long_mode(s_link_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_link_hint, kUiW - 2 * kPad);
    lv_obj_align(s_link_hint, LV_ALIGN_TOP_LEFT, kPad, 30);

#if VOICE_UI_AVATAR
    // The avatar: one canvas the render loop redraws; Muse's renderer writes LVGL-native RGB565.
    s_avatar_buf = static_cast<uint16_t*>(
        heap_caps_malloc(static_cast<size_t>(kAvatarPx) * kAvatarPx * 2u, MALLOC_CAP_SPIRAM));
    if (s_avatar_buf) {
        muse_pixel_set_size(kAvatarPx);
        s_avatar = lv_canvas_create(root);
        lv_canvas_set_buffer(s_avatar, s_avatar_buf, kAvatarPx, kAvatarPx, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_avatar, LV_ALIGN_TOP_LEFT, kAvatarX, kAvatarY);
        lv_obj_remove_flag(s_avatar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_avatar, LV_OBJ_FLAG_HIDDEN);
    } else {
        ESP_LOGW(TAG, "no PSRAM for the avatar; text only");
    }
    const lv_font_t* name_font = &lv_font_montserrat_20;   // the column is narrow
    const char* listening_text = "Listening";
    const char* idle_hint_text = "Hold BOOT\nto talk";
#else
    const lv_font_t* name_font = &lv_font_montserrat_28;
    const char* listening_text = LV_SYMBOL_AUDIO "  Listening";
    const char* idle_hint_text = LV_SYMBOL_AUDIO "  Hold BOOT to talk";
#endif

    // Idle: the agent's name and how to talk to it.
    s_idle_view = View(root, kPanelX, kPanelW);
    s_idle_name = Label(s_idle_view, name_font, kColText);
    lv_label_set_long_mode(s_idle_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_idle_name, kTextW);
    lv_obj_set_style_text_align(s_idle_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_idle_name, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t* idle_hint = Label(s_idle_view, &lv_font_montserrat_14, kColMuted);
    lv_label_set_text(idle_hint, idle_hint_text);
    lv_obj_set_style_text_align(idle_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(idle_hint, LV_ALIGN_TOP_MID, 0, 54);

    // Listening: a level meter while the button is held.
    s_listen_view = View(root, kPanelX, kPanelW);
    lv_obj_t* listen = Label(s_listen_view, &lv_font_montserrat_20, kColListen);
    lv_label_set_text(listen, listening_text);
    lv_obj_align(listen, LV_ALIGN_TOP_MID, 0, 18);
    s_level = lv_bar_create(s_listen_view);
    lv_obj_set_size(s_level, kTextW - kPad, 8);
    lv_obj_align(s_level, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(s_level, lv_color_hex(0x262B38), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level, lv_color_hex(kColListen), LV_PART_INDICATOR);
    lv_bar_set_range(s_level, 0, 100);
    lv_obj_t* release = Label(s_listen_view, &lv_font_montserrat_14, kColMuted);
    lv_label_set_text(release, "Release to send");
    lv_obj_align(release, LV_ALIGN_TOP_MID, 0, 74);

    // Thinking: between release and the answer.
    s_think_view = View(root, kPanelX, kPanelW);
    s_think_label = Label(s_think_view, &lv_font_montserrat_20, kColAccent);
    lv_obj_align(s_think_label, LV_ALIGN_TOP_MID, 0, 34);

    // Answer: wrapped text that scrolls itself when it is longer than the screen.
    s_answer_view = View(root, kPanelX, kPanelW);
    lv_obj_add_flag(s_answer_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_answer_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_hor(s_answer_view, kPad, 0);
    lv_obj_set_style_pad_bottom(s_answer_view, 4, 0);
    s_answer = Label(s_answer_view, TextFont(), kColText);
    MixedTextLines(s_answer);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_answer, kTextW);
    lv_obj_align(s_answer, LV_ALIGN_TOP_LEFT, 0, 0);

    // Toast: one line over the bottom edge.
    s_toast = Label(root, &lv_font_montserrat_14, kColText);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(kColToast), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);
    lv_obj_set_style_pad_hor(s_toast, 8, 0);
    lv_obj_set_style_pad_ver(s_toast, 3, 0);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(s_toast, kUiW - 2 * kPad, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

// ── Rendering ────────────────────────────────────────────────────────────────────────────────

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
    case AGENT_LINK_PHASE_CONNECTED:  return "Online";
    case AGENT_LINK_PHASE_READY:      return "Online";
    default:                          return "Connecting";
    }
}

void ShowOnly(lv_obj_t* view) {
    lv_obj_t* const views[] = {s_link_view, s_idle_view, s_listen_view, s_think_view, s_answer_view};
    for (lv_obj_t* v : views) {
        if (v == view) lv_obj_remove_flag(v, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    }
#if VOICE_UI_AVATAR
    // The avatar belongs to the talk cycle; the link screens need the room for their words.
    if (s_avatar) {
        if (view == s_link_view) lv_obj_add_flag(s_avatar, LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_remove_flag(s_avatar, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void RefreshStatus(const agent_link_status_t& st) {
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(PhaseColor(st.phase)), 0);
    const bool ready = st.phase == AGENT_LINK_PHASE_READY;
    lv_label_set_text(s_caption, ready ? st.title : PhaseCaption(st.phase));
    lv_label_set_text(s_link_title, st.phase == AGENT_LINK_PHASE_PAIRING && st.code[0] ? st.code : st.title);
    lv_label_set_text(s_link_hint, st.hint);
    lv_label_set_text(s_idle_name, st.title[0] ? st.title : "Ready");
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

#if VOICE_UI_AVATAR
// The avatar's own animation clock: which mode it plays, and since when.
struct AvatarState {
    muse_mode_t mode = MUSE_MODE_IDLE;
    int64_t     since = 0;
    int64_t     frame_at = 0;
    int64_t     speak_until = 0;   // an answer is "being said" for about as long as it takes to read
};

// One frame of the avatar, at most every kAvatarFrameUs, while a talk-cycle screen shows it.
void DrawAvatar(AvatarState& av, Mode mode, int64_t now) {
    if (!s_avatar || mode == Mode::kLink) return;
    muse_mode_t want = MUSE_MODE_IDLE;
    switch (mode) {
    case Mode::kListening: want = MUSE_MODE_LISTENING; break;
    case Mode::kThinking:  want = MUSE_MODE_THINKING;  break;
    case Mode::kAnswer:    want = now < av.speak_until ? MUSE_MODE_SPEAKING : MUSE_MODE_IDLE; break;
    default:               want = MUSE_MODE_IDLE;      break;
    }
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
    s_cjk = LoadCjkFont(CJK_FONT_PX);   // LVGL calls only from this task, and before Build()
    Build();
    RefreshBattery();

    uint32_t seen_status = 0, seen_answer = 0, seen_toast = 0, seen_batt = 0;
    int64_t toast_until = 0;
    bool ready = false;
    Mode shown = Mode::kIdle;
    bool first = true;
    int dots = 0;
    int64_t dots_at = 0;
    agent_link_status_t st = {};
    static char answer[sizeof s_answer_text];   // render task only
#if VOICE_UI_AVATAR
    AvatarState avatar;
#endif

    while (true) {
        const int64_t now = esp_timer_get_time();

        const uint32_t status_rev = s_status_rev.load(std::memory_order_acquire);
        if (status_rev != seen_status) {
            seen_status = status_rev;
            taskENTER_CRITICAL(&s_lock);
            st = s_status;
            taskEXIT_CRITICAL(&s_lock);
            ready = st.phase == AGENT_LINK_PHASE_READY;
            RefreshStatus(st);
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
#if VOICE_UI_AVATAR
            // About reading pace (14 characters a second), between 3 and 20 s.
            int64_t speak_us = static_cast<int64_t>(strlen(answer)) * 1000000 / 14;
            if (speak_us < 3000000) speak_us = 3000000;
            if (speak_us > 20000000) speak_us = 20000000;
            avatar.speak_until = now + speak_us;
#endif
        }

        const Mode mode = CurrentMode(ready, now);
        if (mode != shown || first) {
            first = false;
            shown = mode;
            switch (mode) {
            case Mode::kLink:      ShowOnly(s_link_view); break;
            case Mode::kIdle:      ShowOnly(s_idle_view); break;
            case Mode::kListening: ShowOnly(s_listen_view); break;
            case Mode::kThinking:  ShowOnly(s_think_view); dots_at = 0; break;
            case Mode::kAnswer:    ShowOnly(s_answer_view); break;
            }
        }
        if (mode == Mode::kListening) {
            lv_bar_set_value(s_level, s_level_pct.load(std::memory_order_acquire), LV_ANIM_OFF);
        } else if (mode == Mode::kThinking && now - dots_at > 400 * 1000) {
            dots_at = now;
            dots = (dots + 1) % 4;
            static const char* const kDots[] = {"Thinking", "Thinking.", "Thinking..", "Thinking..."};
            lv_label_set_text(s_think_label, kDots[dots]);
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

esp_err_t Ui::Start(Sh8501LkPanel* panel) {
    if (!panel || !panel->Ready()) return ESP_ERR_INVALID_STATE;
    s_panel = panel;

    // Two full frames in PSRAM (2 x 57.6 KB): internal RAM is what the radios and TLS need.
    s_lv_buf  = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    s_rot_buf = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    if (!s_lv_buf || !s_rot_buf) {
        ESP_LOGE(TAG, "frame buffer alloc failed (need 2 x %u bytes)", static_cast<unsigned>(kFrameBytes));
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    lv_tick_set_cb(TickCb);
    s_disp = lv_display_create(kUiW, kUiH);
    if (!s_disp) return ESP_FAIL;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, FlushCb);
    lv_display_set_buffers(s_disp, s_lv_buf, nullptr, kFrameBytes, LV_DISPLAY_RENDER_MODE_FULL);

    // Internal stack: LVGL's draw path is deep, TinyTTF rasterizes CJK glyphs on it, and a
    // PSRAM stack faults while flash writes (NVS) have the cache disabled. On the last core, as
    // Muse's own UI runs: a frame is mostly CPU spinning on the panel's SPI, and the first core
    // belongs to Wi-Fi and the network. Left unpinned, the avatar's float maths would tie the
    // task to whichever core it first ran on.
    if (xTaskCreatePinnedToCore(RenderTask, "voice_ui", 10240, nullptr, 4, nullptr,
                                portNUM_PROCESSORS - 1) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI up: %ux%u rotated onto the %ux%u panel%s", kUiW, kUiH, DISPLAY_WIDTH,
             DISPLAY_HEIGHT, kHasAvatar ? ", with avatar" : "");
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

}  // namespace voice
