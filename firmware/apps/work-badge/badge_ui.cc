#include "badge_ui.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "badge_data.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace badge {
namespace {

constexpr const char* TAG = "badge.ui";

// UI is laid out landscape and rotated onto the portrait panel at flush time (see config.h).
constexpr uint16_t kUiW = BADGE_UI_WIDTH;    // 240
constexpr uint16_t kUiH = BADGE_UI_HEIGHT;   // 120
constexpr size_t   kFrameBytes = static_cast<size_t>(kUiW) * kUiH * 2u;

// Palette. Dark ground with a single accent: on an AMOLED the black pixels are genuinely off,
// which both looks like a printed card and costs almost nothing to keep lit all day.
constexpr uint32_t kColBg      = 0x05070C;
constexpr uint32_t kColCard    = 0x121826;
constexpr uint32_t kColAccent  = 0x2FA8FF;
constexpr uint32_t kColText    = 0xF2F5FA;
constexpr uint32_t kColMuted   = 0x8A93A6;
constexpr uint32_t kColOk      = 0x35D07F;
constexpr uint32_t kColWarn    = 0xFF5C5C;

// Fonts. These are Montserrat, so the badge renders Latin text and the LVGL symbols only -
// a Chinese name needs a CJK font compiled in first.
#define BADGE_FONT_NAME  (&lv_font_montserrat_28)
#define BADGE_FONT_MID   (&lv_font_montserrat_20)
#define BADGE_FONT_SMALL (&lv_font_montserrat_14)

Sh8501LkPanel* s_panel = nullptr;
lv_display_t* s_disp = nullptr;
uint8_t* s_lv_buf  = nullptr;   // LVGL renders here (landscape)
uint8_t* s_rot_buf = nullptr;   // rotated + byte-swapped copy handed to the panel (portrait)

// Widgets, all owned by the render task.
lv_obj_t* s_card       = nullptr;
lv_obj_t* s_company    = nullptr;
lv_obj_t* s_status     = nullptr;   // battery + link, top right
lv_obj_t* s_name       = nullptr;
lv_obj_t* s_title      = nullptr;
lv_obj_t* s_emp_id     = nullptr;
lv_obj_t* s_note       = nullptr;
lv_obj_t* s_ota_screen = nullptr;   // full-screen overlay, hidden unless upgrading
lv_obj_t* s_ota_bar    = nullptr;
lv_obj_t* s_ota_pct    = nullptr;
lv_obj_t* s_ota_msg    = nullptr;

// Cross-task inputs: written by whoever has the news, consumed by the render task.
std::atomic<int>      s_batt_pct{-1};
std::atomic<bool>     s_charging{false};
std::atomic<bool>     s_connected{false};
std::atomic<uint32_t> s_status_rev{1};      // battery / link changed
char                  s_note_text[64] = {};
std::atomic<uint32_t> s_note_rev{0};

std::atomic<uint8_t>  s_ota_state{AGENT_OTA_IDLE};
std::atomic<uint8_t>  s_ota_pct_val{0};
std::atomic<uint16_t> s_ota_err{0};
std::atomic<uint32_t> s_ota_rev{0};

// Rotate the landscape frame onto the portrait panel and swap to big-endian in the same pass
// (LVGL keeps RGB565 little-endian; the panel wants it the other way round).
void RotateAndSwap(const uint16_t* src, uint16_t* dst) {
    constexpr uint16_t kPw = DISPLAY_WIDTH;   // panel columns == UI height
    for (uint16_t y = 0; y < kUiH; ++y) {
        const uint16_t* row = src + static_cast<size_t>(y) * kUiW;
        for (uint16_t x = 0; x < kUiW; ++x) {
            const uint16_t px = __builtin_bswap16(row[x]);
#if BADGE_ROTATE_CCW
            dst[static_cast<size_t>(kUiW - 1 - x) * kPw + y] = px;
#else
            dst[static_cast<size_t>(x) * kPw + (kUiH - 1 - y)] = px;
#endif
        }
    }
}

void FlushCb(lv_display_t* disp, const lv_area_t* /*area*/, uint8_t* px_map) {
    // LV_DISPLAY_RENDER_MODE_FULL: px_map is always the whole landscape frame.
    RotateAndSwap(reinterpret_cast<const uint16_t*>(px_map), reinterpret_cast<uint16_t*>(s_rot_buf));
    if (s_panel) (void)s_panel->DrawBitmap(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_rot_buf);
    lv_display_flush_ready(disp);   // the LK driver blocks until the pixels are out
}

uint32_t TickCb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ── Screen construction ────────────────────────────────────────────────────────

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void BuildBadgeScreen() {
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // Header strip: company on the left, battery + link on the right.
    s_company = MakeLabel(root, BADGE_FONT_SMALL, kColAccent);
    lv_obj_set_style_text_letter_space(s_company, 2, 0);
    lv_obj_align(s_company, LV_ALIGN_TOP_LEFT, 10, 7);

    s_status = MakeLabel(root, BADGE_FONT_SMALL, kColMuted);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -10, 7);

    // The card itself: rounded panel with a left accent edge, like a printed staff badge.
    s_card = lv_obj_create(root);
    lv_obj_set_size(s_card, kUiW - 16, 66);
    lv_obj_align(s_card, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(kColCard), 0);
    lv_obj_set_style_bg_grad_color(s_card, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_grad_dir(s_card, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_radius(s_card, 10, 0);
    lv_obj_set_style_border_side(s_card, LV_BORDER_SIDE_LEFT, 0);   // accent edge, like a printed stripe
    lv_obj_set_style_border_color(s_card, lv_color_hex(kColAccent), 0);
    lv_obj_set_style_border_width(s_card, 3, 0);
    lv_obj_set_style_pad_all(s_card, 0, 0);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_card, LV_SCROLLBAR_MODE_OFF);

    // Name: the one line that has to be readable across a room. Its font is chosen per value
    // (see FontForName) rather than scrolled - a badge that animates all day would keep the
    // SPI bus and the CPU busy for no benefit, and drains a battery that has to last a shift.
    s_name = MakeLabel(s_card, BADGE_FONT_NAME, kColText);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_name, kUiW - 32);
    lv_obj_align(s_name, LV_ALIGN_TOP_LEFT, 10, 6);

    s_title = MakeLabel(s_card, BADGE_FONT_SMALL, kColMuted);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title, kUiW - 32);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 10, 40);

    // Employee id sits below the card, spaced out the way an id number is printed.
    s_emp_id = MakeLabel(root, BADGE_FONT_MID, kColAccent);
    lv_obj_set_style_text_letter_space(s_emp_id, 2, 0);
    lv_obj_align(s_emp_id, LV_ALIGN_BOTTOM_LEFT, 10, -6);

    s_note = MakeLabel(root, BADGE_FONT_SMALL, kColOk);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_note, 130);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_note, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
    lv_label_set_text(s_note, "");
}

void BuildOtaScreen() {
    // A sibling of the badge, kept hidden. Upgrading takes over the whole screen on purpose:
    // the one thing the wearer must not do is assume the badge is idle and power it off.
    s_ota_screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_ota_screen, kUiW, kUiH);
    lv_obj_align(s_ota_screen, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_ota_screen, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(s_ota_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ota_screen, 0, 0);
    lv_obj_set_style_radius(s_ota_screen, 0, 0);
    lv_obj_remove_flag(s_ota_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_ota_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_ota_screen, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* head = MakeLabel(s_ota_screen, BADGE_FONT_SMALL, kColAccent);
    lv_obj_set_style_text_letter_space(head, 2, 0);
    lv_label_set_text(head, LV_SYMBOL_DOWNLOAD "  FIRMWARE UPDATE");
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 14);

    s_ota_pct = MakeLabel(s_ota_screen, BADGE_FONT_NAME, kColText);
    lv_obj_align(s_ota_pct, LV_ALIGN_CENTER, 0, -6);
    lv_label_set_text(s_ota_pct, "0%");

    s_ota_bar = lv_bar_create(s_ota_screen);
    lv_obj_set_size(s_ota_bar, kUiW - 60, 8);
    lv_obj_align(s_ota_bar, LV_ALIGN_CENTER, 0, 26);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(kColCard), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(kColAccent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ota_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ota_bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_bar_set_value(s_ota_bar, 0, LV_ANIM_OFF);

    s_ota_msg = MakeLabel(s_ota_screen, BADGE_FONT_SMALL, kColMuted);
    lv_obj_align(s_ota_msg, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(s_ota_msg, "Keep the badge powered");
}

// ── Render task ────────────────────────────────────────────────────────────────

// Number of UTF-8 characters (not bytes) - what decides how wide a name draws.
size_t Utf8Chars(const char* s_in) {
    size_t n = 0;
    for (const char* p = s_in; *p; ++p)
        if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) ++n;
    return n;
}

// Step the name down a size or two rather than ellipsizing it: a truncated name defeats the
// point of a badge. The thresholds are what fits across 224px at each Montserrat size.
const lv_font_t* FontForName(const char* name) {
    const size_t n = Utf8Chars(name);
    if (n <= 12) return BADGE_FONT_NAME;    // 28px
    if (n <= 18) return BADGE_FONT_MID;     // 20px
    return BADGE_FONT_SMALL;                // 14px; longer than this does get ellipsized
}

void RefreshBadge() {
    Data& d = Data::Instance();
    lv_label_set_text(s_company, d.Get(Field::kCompany));

    const char* name = d.Get(Field::kName);
    lv_obj_set_style_text_font(s_name, FontForName(name), 0);
    lv_label_set_text(s_name, name);

    // Title and department share a line: "Firmware Engineer - R&D", either half optional.
    const char* title = d.Get(Field::kTitle);
    const char* dept  = d.Get(Field::kDepartment);
    char line[2 * kMaxFieldLen + 16];   // two full fields + the separator, with headroom
    if (*title && *dept)      std::snprintf(line, sizeof(line), "%s  |  %s", title, dept);
    else                      std::snprintf(line, sizeof(line), "%s", *title ? title : dept);
    lv_label_set_text(s_title, line);

    lv_label_set_text(s_emp_id, d.Get(Field::kEmployeeId));
}

void RefreshStatus() {
    const int  pct  = s_batt_pct.load(std::memory_order_acquire);
    const bool chg  = s_charging.load(std::memory_order_acquire);
    const bool link = s_connected.load(std::memory_order_acquire);

    char buf[48];
    const char* batt_sym = LV_SYMBOL_BATTERY_EMPTY;
    if (pct >= 0) {
        batt_sym = (pct >= 85) ? LV_SYMBOL_BATTERY_FULL
                 : (pct >= 60) ? LV_SYMBOL_BATTERY_3
                 : (pct >= 35) ? LV_SYMBOL_BATTERY_2
                 : (pct >= 12) ? LV_SYMBOL_BATTERY_1
                               : LV_SYMBOL_BATTERY_EMPTY;
    }
    // Clamp for the reader (a confused gauge should not print "250%") and size the buffer for
    // any int, so -Wformat-truncation does not have to take the clamp on faith.
    char level[16];
    if (pct >= 0) std::snprintf(level, sizeof(level), "%d%%", pct > 100 ? 100 : pct);
    else          std::snprintf(level, sizeof(level), "--");   // no gauge / read failed
    std::snprintf(buf, sizeof(buf), "%s%s%s %s %s",
                  link ? LV_SYMBOL_BLUETOOTH : "", link ? " " : "",
                  chg ? LV_SYMBOL_CHARGE : "", batt_sym, level);
    lv_label_set_text(s_status, buf);
    // Low battery on a badge you cannot see the back of deserves to be loud.
    lv_obj_set_style_text_color(s_status,
                                lv_color_hex((pct >= 0 && pct < 15 && !chg) ? kColWarn : kColMuted), 0);
}

// How long a failed upgrade stays on screen. The engine drops straight back to IDLE after
// reporting a failure, so without this the wearer would never see why it stopped.
constexpr uint32_t kOtaFailDwellMs = 6000;

agent_ota_state_t s_view_state = AGENT_OTA_IDLE;   // what the overlay is currently showing
uint32_t          s_view_hide_at = 0;              // dwell deadline for a shown failure

void DrawOta(agent_ota_state_t state) {
    lv_obj_remove_flag(s_ota_screen, LV_OBJ_FLAG_HIDDEN);

    const int pct = s_ota_pct_val.load(std::memory_order_acquire);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_ota_pct, buf);
    lv_bar_set_value(s_ota_bar, pct, LV_ANIM_OFF);

    switch (state) {
    case AGENT_OTA_RECEIVING:
        lv_label_set_text(s_ota_msg, "Keep the badge powered");
        lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(kColAccent), LV_PART_INDICATOR);
        break;
    case AGENT_OTA_VERIFYING:
        lv_label_set_text(s_ota_msg, "Verifying...");
        break;
    case AGENT_OTA_SUCCESS:
        lv_label_set_text(s_ota_msg, "Done - restarting");
        lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(kColOk), LV_PART_INDICATOR);
        break;
    case AGENT_OTA_FAILED:
        std::snprintf(buf, sizeof(buf), "Failed (%u)",
                      static_cast<unsigned>(s_ota_err.load(std::memory_order_acquire)));
        lv_label_set_text(s_ota_msg, buf);
        lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(kColWarn), LV_PART_INDICATOR);
        break;
    default:
        break;
    }
}

// Decide what the overlay should show right now. Called when the engine reports something and
// again while a failure is serving out its dwell.
void UpdateOtaView() {
    const auto state = static_cast<agent_ota_state_t>(s_ota_state.load(std::memory_order_acquire));
    const uint32_t now = TickCb();

    if (state != AGENT_OTA_IDLE) {
        s_view_state   = state;
        s_view_hide_at = (state == AGENT_OTA_FAILED) ? now + kOtaFailDwellMs : 0;
        DrawOta(state);
        return;
    }
    // Engine finished. Keep a failure up until its dwell expires, then go back to the badge.
    // (Success never gets here - the device reboots first.)
    if (s_view_state == AGENT_OTA_FAILED && now < s_view_hide_at) return;
    s_view_hide_at = 0;   // dwell served (or never started): stop polling this
    if (s_view_state != AGENT_OTA_IDLE) {
        s_view_state = AGENT_OTA_IDLE;
        lv_obj_add_flag(s_ota_screen, LV_OBJ_FLAG_HIDDEN);
    }
}

void RenderTask(void*) {
    BuildBadgeScreen();
    BuildOtaScreen();
    RefreshBadge();
    RefreshStatus();

    uint32_t seen_data = 0, seen_status = 0, seen_note = 0, seen_ota = 0;
    while (true) {
        const uint32_t data_rev = Data::Instance().Revision();
        if (data_rev != seen_data) { seen_data = data_rev; RefreshBadge(); }

        const uint32_t status_rev = s_status_rev.load(std::memory_order_acquire);
        if (status_rev != seen_status) { seen_status = status_rev; RefreshStatus(); }

        const uint32_t note_rev = s_note_rev.load(std::memory_order_acquire);
        if (note_rev != seen_note) { seen_note = note_rev; lv_label_set_text(s_note, s_note_text); }

        const uint32_t ota_rev = s_ota_rev.load(std::memory_order_acquire);
        // Also re-run while a failure is on screen, so its dwell can expire without the engine
        // having anything more to say.
        if (ota_rev != seen_ota || s_view_hide_at) { seen_ota = ota_rev; UpdateOtaView(); }

        uint32_t next = lv_timer_handler();          // renders + flushes only if something changed
        if (next == LV_NO_TIMER_READY || next > 30) next = 30;
        vTaskDelay(pdMS_TO_TICKS(next < 5 ? 5 : next));
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

    // Two full frames: one LVGL draws into (landscape), one holding the rotated copy the panel
    // reads (portrait). PSRAM - 2 x 57.6KB would be a painful bite out of internal RAM.
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

    // Internal stack: LVGL's draw path is deep, and it must stay reachable while flash writes
    // during an OTA disable the cache (a PSRAM stack would fault there).
    if (xTaskCreate(RenderTask, "badge_ui", 8192, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UI up: %ux%u logical -> %ux%u panel (%s)", kUiW, kUiH, DISPLAY_WIDTH,
             DISPLAY_HEIGHT, BADGE_ROTATE_CCW ? "rotated CCW" : "rotated CW");
    return ESP_OK;
}

void Ui::SetBattery(int percent, bool charging) {
    if (percent == s_batt_pct.load(std::memory_order_acquire) &&
        charging == s_charging.load(std::memory_order_acquire)) {
        return;
    }
    s_batt_pct.store(percent, std::memory_order_release);
    s_charging.store(charging, std::memory_order_release);
    s_status_rev.fetch_add(1, std::memory_order_release);
}

void Ui::SetLinkState(bool connected) {
    if (connected == s_connected.load(std::memory_order_acquire)) return;
    s_connected.store(connected, std::memory_order_release);
    s_status_rev.fetch_add(1, std::memory_order_release);
}

void Ui::SetNote(const char* utf8) {
    std::snprintf(s_note_text, sizeof(s_note_text), "%s", utf8 ? utf8 : "");
    s_note_rev.fetch_add(1, std::memory_order_release);
}

void Ui::SetOtaStatus(const agent_ota_status_t* st) {
    if (!st) return;
    const uint32_t pct = st->total_bytes ? (st->received_bytes * 100ull / st->total_bytes) : 0;
    s_ota_state.store(static_cast<uint8_t>(st->state), std::memory_order_release);
    s_ota_pct_val.store(static_cast<uint8_t>(st->state == AGENT_OTA_SUCCESS ? 100 : pct),
                        std::memory_order_release);
    s_ota_err.store(st->error_code, std::memory_order_release);
    s_ota_rev.fetch_add(1, std::memory_order_release);
}

}  // namespace badge
