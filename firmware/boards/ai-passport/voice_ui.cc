#include "voice_ui.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace voice {
namespace {

constexpr const char* TAG = "passport.ui";

constexpr uint16_t kUiW = DISPLAY_WIDTH;    // 240
constexpr uint16_t kUiH = DISPLAY_HEIGHT;   // 320
// One partial stripe keeps internal RAM available for Bluetooth and audio.
constexpr int      kStripeRows = 16;
constexpr size_t   kStripeBytes = static_cast<size_t>(kUiW) * kStripeRows * 2u;
// One stripe, not two: the C3 has 400 KB of DRAM for NimBLE, the I2S DMA and LVGL together. LVGL
// 9 accepts a single partial buffer and blocks on each flush, which at 30 fps is invisible.
constexpr int      kPad = 16;
constexpr int      kHeaderH = 48;
constexpr int      kFooterH = 36;

// How long the talk-cycle screens hold before falling back to idle.
constexpr int64_t kThinkingTimeoutUs = 75LL * 1000 * 1000;
constexpr int64_t kAnswerHoldUs      = 120LL * 1000 * 1000;

// Dark ground, one accent per state. The C3 panel is not an AMOLED, but the same palette as the
// Muse boards keeps the family recognisable.
constexpr uint32_t kColBg     = 0x0E1118;
constexpr uint32_t kColPanel  = 0x161B26;
constexpr uint32_t kColText   = 0xF2F5FA;
constexpr uint32_t kColMuted  = 0x8A93A6;
constexpr uint32_t kColAccent = 0x2FA8FF;
constexpr uint32_t kColOk     = 0x35D07F;
constexpr uint32_t kColWarn   = 0xFFB020;
constexpr uint32_t kColErr    = 0xFF5C5C;
constexpr uint32_t kColListen = 0xFF4F8B;
constexpr uint32_t kColToast  = 0x1C2333;

enum class Mode { kLink, kIdle, kListening, kThinking, kAnswer };

St7789Lcd*  s_panel = nullptr;
lv_display_t* s_disp = nullptr;
uint8_t*     s_buf_a = nullptr;

// Widgets, owned by the render task.
lv_obj_t* s_dot = nullptr;
lv_obj_t* s_caption = nullptr;
lv_obj_t* s_footer = nullptr;
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

// Cross-task inputs. Strings sit behind one spinlock; the render task copies them out.
portMUX_TYPE        s_lock = portMUX_INITIALIZER_UNLOCKED;
agent_link_status_t s_status = {};
std::atomic<uint32_t> s_status_rev{1};
char                s_answer_text[768] = {};
std::atomic<uint32_t> s_answer_rev{0};
char                s_toast_text[96] = {};
std::atomic<uint32_t> s_toast_rev{0};
std::atomic<uint32_t> s_toast_ms{0};

// The render task copies these out under the lock instead of copying into a stack buffer: 768 bytes
// of answer text on a 4 KB stack was enough to overflow it on the first answer.
char                s_render_answer[sizeof s_answer_text] = {};
char                s_render_toast[sizeof s_toast_text] = {};

std::atomic<bool>     s_listening{false};
std::atomic<int>      s_level_pct{0};
std::atomic<bool>     s_thinking{false};
std::atomic<int64_t>  s_thinking_since{0};
std::atomic<int64_t>  s_answer_at{0};

// ── Flush: hand each finished stripe to the panel driver ─────────────────────────────────────

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const uint32_t w = static_cast<uint32_t>(lv_area_get_width(area));
    const uint32_t h = static_cast<uint32_t>(lv_area_get_height(area));

    // LVGL hands out RGB565 in little-endian byte order; esp_lcd leaves the ST7789 on its
    // big-endian default (RAMCTRL bit3 clear), so the panel wants the high byte first. Without
    // this swap every hue comes out with red and blue exchanged, which is what the other boards
    // (korvo, m5stack-stopwatch, wownny-muse) have always done here.
    lv_draw_sw_rgb565_swap(px_map, w * h);

    // Partial mode: px_map holds exactly the flushed area.
    if (s_panel) (void)s_panel->DrawBitmap(area->x1, area->y1,
                                          static_cast<uint16_t>(w),
                                          static_cast<uint16_t>(h),
                                          px_map);
    lv_display_flush_ready(disp);
}

uint32_t TickCb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ── Construction helpers ─────────────────────────────────────────────────────────────────────

lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// A view under the header; exactly one is visible at a time.
lv_obj_t* View(lv_obj_t* root) {
    lv_obj_t* v = lv_obj_create(root);
    lv_obj_set_size(v, kUiW - 16, kUiH - kHeaderH - kFooterH);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 8, kHeaderH);
    lv_obj_set_style_bg_color(v, lv_color_hex(kColPanel), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_radius(v, 14, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    return v;
}

void Build() {
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(kColBg), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* brand = Label(root, &lv_font_montserrat_14, kColText);
    lv_label_set_text(brand, "AI PASSPORT");
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, kPad, 8);

    // Header: status dot + caption.
    s_dot = lv_obj_create(root);
    lv_obj_set_size(s_dot, 6, 6);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(kColMuted), 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_dot, LV_ALIGN_TOP_LEFT, kPad, 31);

    s_caption = Label(root, &lv_font_montserrat_14, kColMuted);
    lv_label_set_long_mode(s_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_caption, kUiW - 2 * kPad - 16);
    lv_obj_align(s_caption, LV_ALIGN_TOP_LEFT, kPad + 12, 26);

    // Link: what the user has to do to get connected, in the SDK's words.
    s_link_view = View(root);
    lv_obj_t* link_tag = Label(s_link_view, &lv_font_montserrat_14, kColAccent);
    lv_label_set_text(link_tag, "LET'S CONNECT");
    lv_obj_align(link_tag, LV_ALIGN_TOP_LEFT, kPad, 18);
    s_link_title = Label(s_link_view, &lv_font_montserrat_28, kColText);
    lv_label_set_long_mode(s_link_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_link_title, kUiW - 16 - 2 * kPad);
    lv_obj_align(s_link_title, LV_ALIGN_TOP_LEFT, kPad, 48);
    s_link_hint = Label(s_link_view, &lv_font_montserrat_14, kColMuted);
    lv_label_set_long_mode(s_link_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_link_hint, kUiW - 16 - 2 * kPad);
    lv_obj_set_style_text_line_space(s_link_hint, 4, 0);

    // Idle: the agent's name and how to talk to it.
    s_idle_view = View(root);
    lv_obj_t* idle_icon = Label(s_idle_view, &lv_font_montserrat_28, kColOk);
    lv_label_set_text(idle_icon, LV_SYMBOL_AUDIO);
    lv_obj_align(idle_icon, LV_ALIGN_TOP_MID, 0, 30);
    s_idle_name = Label(s_idle_view, &lv_font_montserrat_28, kColText);
    lv_label_set_long_mode(s_idle_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_idle_name, kUiW - 16 - 2 * kPad);
    lv_obj_set_style_text_align(s_idle_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_idle_name, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_t* idle_hint = Label(s_idle_view, &lv_font_montserrat_20, kColMuted);
    lv_label_set_text(idle_hint, "Hold OK to talk");
    lv_obj_set_style_text_align(idle_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(idle_hint, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_t* idle_hint2 = Label(s_idle_view, &lv_font_montserrat_14, kColMuted);
    lv_label_set_text(idle_hint2, "release to send");
    lv_obj_set_style_text_align(idle_hint2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(idle_hint2, LV_ALIGN_TOP_MID, 0, 158);

    // Listening: a level meter while the button is held.
    s_listen_view = View(root);
    lv_obj_t* listen = Label(s_listen_view, &lv_font_montserrat_28, kColListen);
    lv_label_set_text(listen, "Listening");
    lv_obj_set_style_text_align(listen, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(listen, LV_ALIGN_TOP_MID, 0, 70);
    s_level = lv_bar_create(s_listen_view);
    lv_obj_set_size(s_level, kUiW - 16 - 2 * kPad, 14);
    lv_obj_align(s_level, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_color(s_level, lv_color_hex(0x262B38), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level, lv_color_hex(kColListen), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_level, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_level, 7, LV_PART_INDICATOR);
    lv_bar_set_range(s_level, 0, 100);
    lv_obj_t* release = Label(s_listen_view, &lv_font_montserrat_14, kColMuted);
    lv_label_set_text(release, "Release to send");
    lv_obj_set_style_text_align(release, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(release, LV_ALIGN_TOP_MID, 0, 138);

    // Thinking: between release and the answer.
    s_think_view = View(root);
    s_think_label = Label(s_think_view, &lv_font_montserrat_28, kColAccent);
    lv_obj_set_style_text_align(s_think_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_think_label, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_t* think_hint = Label(s_think_view, &lv_font_montserrat_14, kColMuted);
    lv_label_set_text(think_hint, "Waiting for your agent");
    lv_obj_align(think_hint, LV_ALIGN_TOP_MID, 0, 130);

    // Answer: wrapped text that scrolls itself when it is longer than the screen.
    s_answer_view = View(root);
    lv_obj_add_flag(s_answer_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_answer_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_hor(s_answer_view, kPad, 0);
    lv_obj_set_style_pad_ver(s_answer_view, 16, 0);
    s_answer = Label(s_answer_view, &lv_font_montserrat_20, kColText);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_answer, kUiW - 16 - 2 * kPad);
    lv_obj_set_style_text_line_space(s_answer, 5, 0);
    lv_obj_align(s_answer, LV_ALIGN_TOP_LEFT, 0, 0);

    // Keep the footer outside the content card, including when its text changes.
    lv_obj_t* footer = lv_obj_create(root);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, kUiW, kFooterH);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_CLICKABLE);
    s_footer = Label(footer, &lv_font_montserrat_14, kColMuted);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_footer, kUiW - 2 * kPad, lv_font_montserrat_14.line_height);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_footer, "Open rorolee to connect");
    lv_obj_center(s_footer);

    // Toasts share the footer without covering the content card.
    s_toast = Label(footer, &lv_font_montserrat_14, kColText);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(kColToast), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);
    lv_obj_set_style_pad_hor(s_toast, 8, 0);
    lv_obj_set_style_pad_ver(s_toast, 3, 0);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_toast, kUiW - 2 * kPad, lv_font_montserrat_14.line_height + 6);
    lv_obj_set_style_text_align(s_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_toast);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

// ── State helpers ────────────────────────────────────────────────────────────────────────────

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
    case AGENT_LINK_PHASE_SETUP:   return "Setup";
    case AGENT_LINK_PHASE_PAIRING: return "Pairing";
    case AGENT_LINK_PHASE_BLOCKED: return "Action needed";
    case AGENT_LINK_PHASE_CONNECTED: return "Connected";
    case AGENT_LINK_PHASE_READY:    return "Online";
    default:                       return "Connecting";
    }
}

void ShowOnly(lv_obj_t* view) {
    lv_obj_t* const views[] = {s_link_view, s_idle_view, s_listen_view, s_think_view, s_answer_view};
    for (lv_obj_t* v : views) {
        if (v == view) lv_obj_remove_flag(v, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    }
}

const char* AppText(const char* text, char* buf, size_t cap) {
    const char* old_name = "the Deotaland app";
    const char* match = strstr(text, old_name);
    if (!match) {
        old_name = "the app";
        match = strstr(text, old_name);
    }
    if (!match) return text;
    snprintf(buf, cap, "%.*srorolee%s", static_cast<int>(match - text), text,
             match + strlen(old_name));
    return buf;
}

void RefreshStatus(const agent_link_status_t& st) {
    char title[sizeof st.title], hint[sizeof st.hint];
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(PhaseColor(st.phase)), 0);
    const bool ready = st.phase == AGENT_LINK_PHASE_READY;
    lv_label_set_text(s_caption, ready ? st.title : PhaseCaption(st.phase));
    lv_label_set_text(s_link_title,
                      st.phase == AGENT_LINK_PHASE_PAIRING && st.code[0] ? st.code :
                      AppText(st.title, title, sizeof title));
    lv_label_set_text(s_link_hint, AppText(st.hint, hint, sizeof hint));
    lv_obj_update_layout(s_link_title);
    lv_obj_align_to(s_link_hint, s_link_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_label_set_text(s_idle_name, st.title[0] ? st.title : "Ready");
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
    lv_anim_set_delay(&a, 2000);
    lv_anim_set_duration(&a, static_cast<uint32_t>(overflow) * 1000u / 22);
    lv_anim_start(&a);
}

Mode CurrentMode(bool ready, int64_t now) {
    if (!ready) return Mode::kLink;
    if (s_listening.load(std::memory_order_acquire)) return Mode::kListening;
    if (s_thinking.load(std::memory_order_acquire) &&
        now - s_thinking_since.load(std::memory_order_acquire) < kThinkingTimeoutUs) {
        return Mode::kThinking;
    }
    if (s_answer_at.load(std::memory_order_acquire) && now - s_answer_at.load() < kAnswerHoldUs) {
        return Mode::kAnswer;
    }
    return Mode::kIdle;
}

// ── Render task ──────────────────────────────────────────────────────────────────────────────

void RenderTask(void*) {
    Build();
    // Nothing is shown until the first ShowOnly: Build() hides every view. Start on a value the
    // switch below can never produce, so the first pass always shows the mode it picked.
    Mode mode = static_cast<Mode>(-1);
    uint32_t seen_status = 0, seen_answer = 0, seen_toast = 0;
    int dots = 0;
    int64_t dots_at = 0, toast_until = 0;

    while (true) {
        const int64_t now = esp_timer_get_time();

        const uint32_t srev = s_status_rev.load(std::memory_order_acquire);
        if (srev != seen_status) {
            seen_status = srev;
            agent_link_status_t st;
            taskENTER_CRITICAL(&s_lock);
            st = s_status;
            taskEXIT_CRITICAL(&s_lock);
            RefreshStatus(st);
        }
        const uint32_t arev = s_answer_rev.load(std::memory_order_acquire);
        if (arev != seen_answer) {
            seen_answer = arev;
            taskENTER_CRITICAL(&s_lock);
            memcpy(s_render_answer, s_answer_text, sizeof s_render_answer);
            taskEXIT_CRITICAL(&s_lock);
            ShowAnswer(s_render_answer);
        }

        agent_link_status_t st;
        taskENTER_CRITICAL(&s_lock);
        st = s_status;
        taskEXIT_CRITICAL(&s_lock);
        const bool ready = st.phase == AGENT_LINK_PHASE_READY;
        const Mode next = CurrentMode(ready, now);
        if (next != mode) {
            mode = next;
            static const char* const footers[] = {
                "Open rorolee to connect", "UP / DOWN: volume",
                "Release OK to send", "Hold OK to ask again", "Hold OK to ask again"
            };
            lv_label_set_text(s_footer, footers[static_cast<int>(mode)]);
            switch (mode) {
            case Mode::kLink:      ShowOnly(s_link_view);   break;
            case Mode::kIdle:      ShowOnly(s_idle_view);   break;
            case Mode::kListening: ShowOnly(s_listen_view); break;
            case Mode::kThinking:  ShowOnly(s_think_view);  break;
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

        const uint32_t trev = s_toast_rev.load(std::memory_order_acquire);
        if (trev != seen_toast) {
            seen_toast = trev;
            taskENTER_CRITICAL(&s_lock);
            memcpy(s_render_toast, s_toast_text, sizeof s_render_toast);
            taskEXIT_CRITICAL(&s_lock);
            lv_label_set_text(s_toast, s_render_toast);
            lv_obj_add_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            toast_until = now + static_cast<int64_t>(s_toast_ms.load()) * 1000;
        }
        if (toast_until && now > toast_until) {
            toast_until = 0;
            lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_footer, LV_OBJ_FLAG_HIDDEN);
        }

        uint32_t next_ms = lv_timer_handler();   // renders + flushes only if something changed
        if (next_ms == LV_NO_TIMER_READY || next_ms > 30) next_ms = 30;
        vTaskDelay(pdMS_TO_TICKS(next_ms < 10 ? 10 : next_ms));
    }
}

}  // namespace

Ui& Ui::Instance() {
    static Ui instance;
    return instance;
}

esp_err_t Ui::Start(St7789Lcd* panel) {
    if (!panel || !panel->Ready()) return ESP_ERR_INVALID_STATE;
    s_panel = panel;

    // One 240 x 16 px stripe (7.5 KB) in internal RAM: the C3 has no PSRAM, so a full framebuffer
    // is off the table, and a second stripe does not fit next to NimBLE's pools.
    s_buf_a = static_cast<uint8_t*>(malloc(kStripeBytes));
    if (!s_buf_a) {
        ESP_LOGE(TAG, "stripe buffer alloc failed (need %u bytes)", static_cast<unsigned>(kStripeBytes));
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    lv_tick_set_cb(TickCb);
    s_disp = lv_display_create(kUiW, kUiH);
    if (!s_disp) return ESP_FAIL;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, FlushCb);
    lv_display_set_buffers(s_disp, s_buf_a, nullptr, kStripeBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (xTaskCreate(RenderTask, "passport_ui", 5120, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI up: %ux%u, partial mode, %u-byte stripe",
             kUiW, kUiH, static_cast<unsigned>(kStripeBytes));
    return ESP_OK;
}

void Ui::SetStatus(const agent_link_status_t& st) {
    taskENTER_CRITICAL(&s_lock);
    s_status = st;
    taskEXIT_CRITICAL(&s_lock);
    if (st.phase != AGENT_LINK_PHASE_READY) {
        s_listening.store(false, std::memory_order_release);
        s_thinking.store(false, std::memory_order_release);
        s_answer_at.store(0, std::memory_order_release);
    }
    s_status_rev.fetch_add(1, std::memory_order_release);
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
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_answer_text, utf8 ? utf8 : "", sizeof s_answer_text);
    taskEXIT_CRITICAL(&s_lock);
    s_thinking.store(false, std::memory_order_release);
    s_answer_at.store(utf8 && utf8[0] ? esp_timer_get_time() : 0, std::memory_order_release);
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
