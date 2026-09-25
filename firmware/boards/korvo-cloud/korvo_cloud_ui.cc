#include "korvo_cloud_ui.h"

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kc_ui {
namespace {

constexpr const char* TAG = "kc_ui";

constexpr int32_t kUiW = DISPLAY_WIDTH;   // 280 - already the rotated size
constexpr int32_t kUiH = DISPLAY_HEIGHT;  // 240

// LVGL renders in horizontal slices rather than whole frames: these screens are near-static, so a
// partial buffer keeps both the RAM and the per-change SPI traffic down.
constexpr int32_t kBufRows  = UI_DRAW_BUF_ROWS;
constexpr size_t  kBufBytes = static_cast<size_t>(kUiW) * kBufRows * 2u;

constexpr int32_t kBarH = 26;            // status bar
constexpr int32_t kPad  = UI_SAFE_PAD;   // keeps content clear of the rounded corners

// Edge gesture geometry. A press starting inside either band is a candidate swipe and is withheld
// from LVGL for its whole duration, so keep every tap target out of them - see the layout below.
constexpr int32_t kEdgeBand = 40;   // how far in from the top/bottom an edge gesture may start
constexpr int32_t kSwipeMin = 42;   // vertical travel before it counts as a swipe

// Palette
constexpr uint32_t kColBg    = 0x0B0E14;
constexpr uint32_t kColBar   = 0x151A23;
constexpr uint32_t kColCard  = 0x1B2231;
constexpr uint32_t kColText  = 0xF2F5FA;
constexpr uint32_t kColMuted = 0x8A93A6;
constexpr uint32_t kColOk    = 0x35D07F;
constexpr uint32_t kColWarn  = 0xFFB020;
constexpr uint32_t kColErr   = 0xFF5C5C;
constexpr uint32_t kColCode  = 0x2FA8FF;
constexpr uint32_t kColCam   = 0x2FA8FF;
constexpr uint32_t kColCam2  = 0x1C6FD0;

St7789Lcd*       s_lcd   = nullptr;
Cst816Touch*     s_touch = nullptr;
KorvoCloudAudio* s_audio = nullptr;
lv_display_t*    s_disp  = nullptr;
lv_indev_t*      s_indev = nullptr;
uint8_t*         s_buf   = nullptr;

SemaphoreHandle_t s_lcd_mux = nullptr;  // LVGL's flush vs. the preview task's blits

std::atomic<Mode> s_mode{Mode::kHome};  // in effect now - what DrawCameraFrame checks
std::atomic<Mode> s_want{Mode::kHome};  // requested; the render task applies it

// Withhold touch from LVGL for the rest of this press. Set while an edge gesture is possible or
// has fired, and when leaving Camera so the dismissing tap does not land on Home.
std::atomic<bool> s_swallow_touch{false};

std::atomic<bool>     s_connected{false};
std::atomic<uint32_t> s_link_rev{0};

// The link status, copied under a spinlock because a transport task writes it and the render task
// reads it. A revision counter is what tells the render task something actually moved.
portMUX_TYPE          s_status_lock = portMUX_INITIALIZER_UNLOCKED;
agent_link_status_t   s_status = {};
std::atomic<uint32_t> s_status_rev{0};

lv_obj_t* s_scr_home  = nullptr;
lv_obj_t* s_scr_apps  = nullptr;
lv_obj_t* s_scr_shade = nullptr;

// Which page to come back to when Camera or Shade is dismissed. Only ever a real page.
std::atomic<Mode> s_last_page{Mode::kHome};

lv_obj_t* s_link_dot = nullptr;
lv_obj_t* s_title    = nullptr;   // what the board is doing, one line
lv_obj_t* s_big      = nullptr;   // the activation code, or a status word
lv_obj_t* s_hint     = nullptr;   // what the user should do about it

lv_obj_t* s_sh_state = nullptr;   // shade rows
lv_obj_t* s_sh_msg   = nullptr;
lv_obj_t* s_sh_sn    = nullptr;

void Lock()   { if (s_lcd_mux) xSemaphoreTake(s_lcd_mux, portMAX_DELAY); }
void Unlock() { if (s_lcd_mux) xSemaphoreGive(s_lcd_mux); }

uint32_t TickCb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);

    // LVGL's RGB565 is little-endian; the ST7789 wants the high byte first.
    lv_draw_sw_rgb565_swap(px_map, static_cast<uint32_t>(w * h));

    // Nothing reaches the panel while Camera owns it.
    if (s_lcd && s_mode.load(std::memory_order_acquire) != Mode::kCamera) {
        Lock();
        (void)s_lcd->DrawBitmap(static_cast<uint16_t>(area->x1), static_cast<uint16_t>(area->y1),
                                static_cast<uint16_t>(w), static_cast<uint16_t>(h), px_map);
        Unlock();
    }
    lv_display_flush_ready(disp);
}

// Reads the snapshot the render task took at the top of this iteration - no I2C from inside LVGL.
void TouchReadCb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->state = LV_INDEV_STATE_RELEASED;
    if (!s_touch || !s_touch->Ready()) return;

    const bool pressed = s_touch->Pressed();
    if (s_swallow_touch.load(std::memory_order_acquire)) {
        if (!pressed) s_swallow_touch.store(false, std::memory_order_release);
        return;
    }
    if (!pressed) return;

    int32_t x = 0, y = 0;
    s_touch->GetPoint(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PRESSED;
}

lv_obj_t* Plain(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, uint32_t color, const lv_font_t* font) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    return l;
}

lv_obj_t* Circle(lv_obj_t* parent, int32_t d, uint32_t color, lv_opa_t opa = LV_OPA_COVER) {
    lv_obj_t* c = Plain(parent);
    lv_obj_set_size(c, d, d);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, opa, 0);
    return c;
}

// The pages you swipe between, in order. Camera is NOT one of them: it is an app you open from
// the icon on the Apps page — the sensor and its two 115KB frame buffers only run while the preview
// is actually on screen. The Shade is not a page either.
constexpr Mode kPages[]   = { Mode::kHome, Mode::kApps };
constexpr int  kPageCount = static_cast<int>(sizeof kPages / sizeof kPages[0]);

// One set per page: an LVGL object belongs to the screen it was created on, so the indicator has
// to exist on both.
lv_obj_t* s_dot_home[kPageCount] = {};
lv_obj_t* s_dot_apps[kPageCount] = {};

int PageIndexOf(Mode m) {
    for (int i = 0; i < kPageCount; ++i) {
        if (kPages[i] == m) return i;
    }
    return -1;
}

// Page indicator, in the bottom band where taps never reach LVGL — decoration, and the only thing
// telling a first-time user that a swipe leads somewhere.
void BuildDots(lv_obj_t* scr, lv_obj_t** out) {
    for (int i = 0; i < kPageCount; ++i) {
        out[i] = Circle(scr, 7, kColText, LV_OPA_30);
        lv_obj_align(out[i], LV_ALIGN_BOTTOM_MID, (2 * i - (kPageCount - 1)) * 8, -32);
    }
}

// The home indicator — says the bottom edge means something.
void BuildPill(lv_obj_t* scr) {
    lv_obj_t* pill = Plain(scr);
    lv_obj_set_size(pill, 90, 4);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(pill, 2, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(kColMuted), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_50, 0);
}

// A lens and a flash on a rounded gradient tile.
void DrawCameraGlyph(lv_obj_t* tile) {
    lv_obj_t* lens = Circle(tile, 36, kColText);
    lv_obj_center(lens);
    lv_obj_t* pupil = Circle(lens, 18, kColCam2);
    lv_obj_center(pupil);
    lv_obj_t* flash = Circle(tile, 9, kColText, LV_OPA_70);
    lv_obj_align(flash, LV_ALIGN_TOP_RIGHT, -10, 10);
}

// Home
//   0..26     status bar
//   40..146   the cloud state block — title, one big line, a wrapped hint
//   200..240  bottom band — page dots and the home indicator, both decoration
void BuildHome() {
    s_scr_home = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr_home);
    lv_obj_set_style_bg_color(s_scr_home, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(s_scr_home, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_home, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bar = Plain(s_scr_home);
    lv_obj_set_size(bar, kUiW, kBarH);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColBar), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    lv_obj_t* name = Label(bar, "korvo-cloud", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, kPad, 0);

    s_link_dot = Circle(bar, 8, kColMuted);
    lv_obj_align(s_link_dot, LV_ALIGN_RIGHT_MID, -kPad, 0);

    s_title = Label(s_scr_home, "Starting", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 40);

    s_big = Label(s_scr_home, "", kColText, &lv_font_montserrat_28);
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, 0, 66);

    // Two lines now that the dock is gone, and that space is what lets a state say what to do
    // about itself instead of just naming itself.
    s_hint = Label(s_scr_home, "", kColMuted, &lv_font_montserrat_14);
    lv_obj_set_width(s_hint, kUiW - 2 * kPad);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 112);

    BuildDots(s_scr_home, s_dot_home);
    BuildPill(s_scr_home);
}

void OnCameraIcon(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "camera icon tapped");
    s_want.store(Mode::kCamera, std::memory_order_release);
}

// Apps
void BuildApps() {
    s_scr_apps = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr_apps);
    lv_obj_set_style_bg_color(s_scr_apps, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(s_scr_apps, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_apps, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bar = Plain(s_scr_apps);
    lv_obj_set_size(bar, kUiW, kBarH);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColBar), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    lv_obj_t* name = Label(bar, "Apps", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, kPad, 0);

    lv_obj_t* icon = Plain(s_scr_apps);
    lv_obj_set_size(icon, 76, 76);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_radius(icon, 20, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(kColCam), 0);
    lv_obj_set_style_bg_grad_color(icon, lv_color_hex(kColCam2), 0);
    lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(icon, OnCameraIcon, LV_EVENT_CLICKED, nullptr);
    DrawCameraGlyph(icon);

    lv_obj_t* cap = Label(s_scr_apps, "Camera", kColText, &lv_font_montserrat_14);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 148);

    lv_obj_t* hint = Label(s_scr_apps, "tap to preview", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 170);

    BuildDots(s_scr_apps, s_dot_apps);
    BuildPill(s_scr_apps);
}

void RefreshDots() {
    const int active = PageIndexOf(s_mode.load(std::memory_order_acquire));
    for (int i = 0; i < kPageCount; ++i) {
        const lv_opa_t opa = (i == active) ? LV_OPA_COVER : LV_OPA_30;
        if (s_dot_home[i]) lv_obj_set_style_bg_opa(s_dot_home[i], opa, 0);
        if (s_dot_apps[i]) lv_obj_set_style_bg_opa(s_dot_apps[i], opa, 0);
    }
}

lv_obj_t* ShadeRow(lv_obj_t* parent, int32_t y, const char* key, lv_obj_t** value_out) {
    lv_obj_t* k = Label(parent, key, kColMuted, &lv_font_montserrat_14);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, kPad, y);
    lv_obj_t* v = Label(parent, "", kColText, &lv_font_montserrat_14);
    lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(v, kUiW - 2 * kPad - 78);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -kPad, y);
    if (value_out) *value_out = v;
    return k;
}

void BuildShade() {
    s_scr_shade = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr_shade);
    lv_obj_set_style_bg_color(s_scr_shade, lv_color_hex(kColCard), 0);
    lv_obj_set_style_bg_opa(s_scr_shade, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_shade, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = Label(s_scr_shade, "Status", kColText, &lv_font_montserrat_20);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);

    ShadeRow(s_scr_shade, 52, "Session",  &s_sh_state);
    ShadeRow(s_scr_shade, 80, "Link",     &s_sh_msg);

    lv_obj_t* sn_key = Label(s_scr_shade, "Device ID", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(sn_key, LV_ALIGN_TOP_LEFT, kPad, 108);

    s_sh_sn = Label(s_scr_shade, "", kColText, &lv_font_montserrat_14);
    lv_obj_set_width(s_sh_sn, kUiW - 2 * kPad);
    lv_label_set_long_mode(s_sh_sn, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_sh_sn, LV_ALIGN_TOP_LEFT, kPad, 128);

    lv_obj_t* hint = Label(s_scr_shade, "swipe up from the bottom to close", kColMuted,
                           &lv_font_montserrat_14);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
}

// Rendering the link
//
// Everything below works from agent_link_status_t alone, so it renders the BLE link and the WiFi link with the same code

// The small caption above the headline — which kind of moment this is.
const char* PhaseCaption(const agent_link_status_t& st) {
    switch (st.phase) {
    case AGENT_LINK_PHASE_IDLE:       return "Starting";
    case AGENT_LINK_PHASE_SETUP:      return "Setup";
    case AGENT_LINK_PHASE_CONNECTING: return "Connecting";
    case AGENT_LINK_PHASE_PAIRING:    return st.title;          // "Activation code" — the code is the headline
    case AGENT_LINK_PHASE_BLOCKED:    return "Action needed";
    case AGENT_LINK_PHASE_CONNECTED:  return "Connected";
    case AGENT_LINK_PHASE_READY:      return "Ready";
    }
    return "";
}

// One word for the shade's Session row.
const char* PhaseWord(agent_link_phase_t p) {
    switch (p) {
    case AGENT_LINK_PHASE_IDLE:       return "idle";
    case AGENT_LINK_PHASE_SETUP:      return "setup";
    case AGENT_LINK_PHASE_CONNECTING: return "connecting";
    case AGENT_LINK_PHASE_PAIRING:    return "pairing";
    case AGENT_LINK_PHASE_BLOCKED:    return "blocked";
    case AGENT_LINK_PHASE_CONNECTED:  return "online";
    case AGENT_LINK_PHASE_READY:      return "ready";
    }
    return "?";
}

uint32_t PhaseColor(agent_link_phase_t p) {
    switch (p) {
    case AGENT_LINK_PHASE_PAIRING:    return kColCode;
    case AGENT_LINK_PHASE_BLOCKED:    return kColErr;
    case AGENT_LINK_PHASE_CONNECTED:
    case AGENT_LINK_PHASE_READY:      return kColOk;
    case AGENT_LINK_PHASE_SETUP:
    case AGENT_LINK_PHASE_CONNECTING: return kColWarn;
    default:                          return kColText;
    }
}

void RefreshLink() {
    if (!s_link_dot) return;
    const bool on = s_connected.load(std::memory_order_acquire);
    lv_obj_set_style_bg_color(s_link_dot, lv_color_hex(on ? kColOk : kColMuted), 0);
}

void RefreshStatus() {
    agent_link_status_t st;
    taskENTER_CRITICAL(&s_status_lock);
    st = s_status;
    taskEXIT_CRITICAL(&s_status_lock);

    if (s_title && s_big && s_hint) {
        lv_label_set_text(s_title, PhaseCaption(st));

        // The headline is the thing to act on: the code while pairing, otherwise the SDK's title.
        const bool pairing = (st.phase == AGENT_LINK_PHASE_PAIRING);
        lv_obj_set_style_text_color(s_big, lv_color_hex(PhaseColor(st.phase)), 0);
        lv_label_set_text(s_big, pairing ? (st.code[0] ? st.code : "------") : st.title);

        // The one thing this board adds on its own: the SDK cannot know the codec failed.
        const bool up = (st.phase == AGENT_LINK_PHASE_CONNECTED || st.phase == AGENT_LINK_PHASE_READY);
        lv_label_set_text(s_hint, (up && !s_audio) ? "Audio unavailable on this boot" : st.hint);
    }

    char buf[48];
    if (s_sh_state) lv_label_set_text(s_sh_state, PhaseWord(st.phase));
    if (s_sh_msg) {
        const char* tx = st.transport == AGENT_TRANSPORT_WIFI ? "WiFi" : "BLE";
        if (st.detail != 0) snprintf(buf, sizeof buf, "%s, code %d", tx, st.detail);
        else                snprintf(buf, sizeof buf, "%s", tx);
        lv_label_set_text(s_sh_msg, buf);
    }
    if (s_sh_sn) {
        const char* id = agent_link_device_id();
        lv_label_set_text(s_sh_sn, id ? id : "-");
    }
}

// Mode switching - all of it on the render task, so every LVGL call stays single-threaded.
void ApplyMode(Mode want) {
    const Mode from = s_mode.load(std::memory_order_acquire);
    if (want == from) return;

    if (want == Mode::kCamera) {
        // Stop LVGL reaching the panel first, then wipe it: the preview is 240 wide on a 280 wide
        // screen, and without this Home would stay visible in the bars either side.
        s_mode.store(Mode::kCamera, std::memory_order_release);
        Lock();
        if (s_lcd) (void)s_lcd->FillSolid(rgb565::kBlack);
        Unlock();
        ESP_LOGI(TAG, "camera open - swipe up from the bottom to go back");
        return;
    }

    lv_obj_t* scr = want == Mode::kShade ? s_scr_shade
                  : want == Mode::kApps  ? s_scr_apps
                                         : s_scr_home;
    if (scr) lv_screen_load(scr);
    s_mode.store(want, std::memory_order_release);

    // Remember where to come back to. Camera and Shade are things you drop into; the page under
    // them is what a swipe up from the bottom should restore.
    if (want == Mode::kHome || want == Mode::kApps) {
        s_last_page.store(want, std::memory_order_release);
    }
    if (from == Mode::kCamera) {
        // Whatever ended the preview must not also land on the screen behind it.
        s_swallow_touch.store(true, std::memory_order_release);
    }
    RefreshStatus();
    RefreshLink();
    RefreshDots();
    lv_obj_invalidate(lv_screen_active());
    ESP_LOGI(TAG, "screen -> %s", want == Mode::kShade ? "shade"
                                : want == Mode::kApps  ? "apps" : "home");
}

// Edge gestures, read straight from the touch stream because Camera is not an LVGL screen.
//
// A press that starts in either band is withheld from LVGL for its whole duration - that is what
// stops a swipe from also registering as a tap, and it is why the layout keeps every tap target
// between the bands.
struct Swipe {
    bool    tracking = false;
    bool    fired    = false;
    bool    from_top = false;
    bool    from_bot = false;
    int32_t x0 = 0, y0 = 0;
};
Swipe s_sw;

void OnSwipeUpFromBottom() {
    const Mode m = s_mode.load(std::memory_order_acquire);
    if (PageIndexOf(m) >= 0) return;
    const Mode back = s_last_page.load(std::memory_order_acquire);
    ESP_LOGI(TAG, "swipe up from the bottom -> %s", back == Mode::kApps ? "apps" : "home");
    s_want.store(back, std::memory_order_release);
}

void OnSwipeDownFromTop() {
    if (s_mode.load(std::memory_order_acquire) == Mode::kShade) return;
    ESP_LOGI(TAG, "swipe down from the top -> shade");
    s_want.store(Mode::kShade, std::memory_order_release);
}

// Turn a page
void OnSwipeHorizontal(int dx) {
    const int idx = PageIndexOf(s_mode.load(std::memory_order_acquire));
    if (idx < 0) return;   // Camera and Shade are not pages; leave them with the bottom-edge swipe

    const int next = dx < 0 ? (idx + 1) % kPageCount
                            : (idx + kPageCount - 1) % kPageCount;
    ESP_LOGI(TAG, "swipe %s -> page %d", dx < 0 ? "left" : "right", next);
    s_want.store(kPages[next], std::memory_order_release);
}

void Fire() {
    s_sw.fired = true;
    s_swallow_touch.store(true, std::memory_order_release);
    if (s_indev) lv_indev_wait_release(s_indev);
}

void PollGestures() {
    if (!s_touch) return;

    const bool pressed = s_touch->Pressed();
    if (!pressed) {
        s_sw = Swipe{};
        return;
    }

    int32_t x = 0, y = 0;
    s_touch->GetPoint(&x, &y);

    if (!s_sw.tracking) {
        s_sw.tracking = true;
        s_sw.fired    = false;
        s_sw.x0       = x;
        s_sw.y0       = y;
        s_sw.from_top = (y <= kEdgeBand);
        s_sw.from_bot = (y >= kUiH - kEdgeBand);
        // Claim the press up front rather than once the finger has moved far enough: by then LVGL
        // would already have seen it go down, and a slow swipe off the dock would fire the icon.
        if (s_sw.from_top || s_sw.from_bot) {
            s_swallow_touch.store(true, std::memory_order_release);
        }
        return;
    }

    if (s_sw.fired) return;

    const int32_t dx = x - s_sw.x0;
    const int32_t dy = y - s_sw.y0;

    // Whichever axis the finger committed to first wins, so a page turn that drifts and a back
    // gesture that drifts cannot both fire from one press.
    if (abs(dy) >= kSwipeMin && abs(dy) > abs(dx)) {
        // Vertical, and these two are the only ones with a start-zone requirement: a drag up from
        // the middle of the screen is not a back gesture, it is somebody resting a finger.
        if (dy < 0 && s_sw.from_bot)      { Fire(); OnSwipeUpFromBottom(); }
        else if (dy > 0 && s_sw.from_top) { Fire(); OnSwipeDownFromTop(); }
        else                              { Fire(); }  // vertical but nowhere to go — still not a tap
        return;
    }
    if (abs(dx) >= kSwipeMin && abs(dx) > abs(dy)) {
        Fire();
        OnSwipeHorizontal(dx);   // page turns work anywhere on the glass
    }
}

void RenderTask(void*) {
    uint32_t seen_link  = 0;
    uint32_t seen_status = 0;
    while (true) {
        if (s_touch) {
            (void)s_touch->Poll();
            PollGestures();
        }

        const Mode want = s_want.load(std::memory_order_acquire);
        if (want != s_mode.load(std::memory_order_acquire)) ApplyMode(want);

        if (s_mode.load(std::memory_order_acquire) == Mode::kCamera) {
            // The preview task owns the panel; all this task does here is watch for the swipe.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const uint32_t link_rev = s_link_rev.load(std::memory_order_acquire);
        if (link_rev != seen_link) { seen_link = link_rev; RefreshLink(); }

        const uint32_t status_rev = s_status_rev.load(std::memory_order_acquire);
        if (status_rev != seen_status) { seen_status = status_rev; RefreshStatus(); }

        uint32_t next = lv_timer_handler();  // renders + flushes only if something changed
        if (next == LV_NO_TIMER_READY || next > 30) next = 30;
        vTaskDelay(pdMS_TO_TICKS(next < 5 ? 5 : next));
    }
}

}  // namespace

esp_err_t Start(St7789Lcd* lcd, Cst816Touch* touch, KorvoCloudAudio* audio) {
    if (!lcd || !lcd->Ready()) return ESP_ERR_INVALID_STATE;
    s_lcd   = lcd;
    s_touch = (touch && touch->Ready()) ? touch : nullptr;
    s_audio = (audio && audio->Ready()) ? audio : nullptr;

    s_lcd_mux = xSemaphoreCreateMutex();
    if (!s_lcd_mux) return ESP_ERR_NO_MEM;

    s_buf = static_cast<uint8_t*>(heap_caps_malloc(kBufBytes, MALLOC_CAP_SPIRAM));
    if (!s_buf) s_buf = static_cast<uint8_t*>(heap_caps_malloc(kBufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
    lv_display_set_buffers(s_disp, s_buf, nullptr, kBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (s_touch) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, TouchReadCb);
        lv_indev_set_display(s_indev, s_disp);
    }

    BuildHome();
    BuildApps();
    BuildShade();
    lv_screen_load(s_scr_home);
    RefreshStatus();
    RefreshLink();
    RefreshDots();

    // Internal stack: LVGL's draw path is deep.
    if (xTaskCreate(RenderTask, "kc_ui", 8192, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UI up: %ldx%ld%s", static_cast<long>(kUiW), static_cast<long>(kUiH),
             s_touch ? ", swipe sideways to turn pages, up from the bottom to go back, "
                       "down from the top for status"
                     : ", no touch - home screen only");
    return ESP_OK;
}

Mode CurrentMode() { return s_mode.load(std::memory_order_acquire); }

void OpenCamera() { s_want.store(Mode::kCamera, std::memory_order_release); }
void GoHome()     { s_want.store(Mode::kHome,   std::memory_order_release); }

esp_err_t DrawCameraFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels) {
    if (!s_lcd || s_mode.load(std::memory_order_acquire) != Mode::kCamera) return ESP_ERR_INVALID_STATE;
    Lock();
    const esp_err_t r = s_lcd->DrawBitmap(x, y, w, h, pixels);
    Unlock();
    return r;
}

void SetLinkStatus(const agent_link_status_t& st) {
    taskENTER_CRITICAL(&s_status_lock);
    s_status = st;
    taskEXIT_CRITICAL(&s_status_lock);
    s_status_rev.fetch_add(1, std::memory_order_release);
}

void SetLinkState(bool connected) {
    if (connected == s_connected.load(std::memory_order_acquire)) return;
    s_connected.store(connected, std::memory_order_release);
    s_link_rev.fetch_add(1, std::memory_order_release);
}

}  // namespace kc_ui
