// Korvo's home screen and apps, see korvo_ui.h.
//
// LVGL 9 on the ST7789, following the same hand-rolled setup boards/work-badge uses: our own
// display, our own tick callback, our own render task. Three differences worth knowing:
//   - rotation is the panel's (MADCTL), not ours, so LVGL just draws at the rotated 280x240 and
//     nothing here transposes pixels — but the touch controller reports unrotated coordinates, so
//     Cst816Touch is configured to undo the rotation (see config.h);
//   - LVGL keeps RGB565 little-endian while the ST7789 latches big-endian, so every flush is byte
//     swapped. The camera path does NOT need this — its frames already come out big-endian;
//   - the glass has rounded corners, so nothing is placed within UI_SAFE_PAD of one. Full-width
//     bars are fine; it is their *contents* that get inset.
//
// Fonts are Montserrat: Latin and LVGL's symbols only. Labels here stay English on purpose; a
// Chinese label needs a CJK font built in first (same caveat as boards/work-badge).

#include "korvo_ui.h"

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

namespace korvo_ui {
namespace {

constexpr const char* TAG = "korvo_ui";

constexpr int32_t kUiW = DISPLAY_WIDTH;   // 280 — already the rotated size
constexpr int32_t kUiH = DISPLAY_HEIGHT;  // 240

// LVGL renders in horizontal slices rather than whole frames: these screens are mostly static, so
// a partial buffer keeps both the RAM and the per-change SPI traffic down.
constexpr int32_t kBufRows  = UI_DRAW_BUF_ROWS;
constexpr size_t  kBufBytes = static_cast<size_t>(kUiW) * kBufRows * 2u;

constexpr int32_t kBarH  = 26;                     // status bar
constexpr int32_t kDotsH = 20;                     // page indicator
constexpr int32_t kPad   = UI_SAFE_PAD;            // keeps content clear of the rounded corners

// Palette
constexpr uint32_t kColBg     = 0x0B0E14;
constexpr uint32_t kColBar    = 0x151A23;
constexpr uint32_t kColText   = 0xF2F5FA;
constexpr uint32_t kColMuted  = 0x8A93A6;
constexpr uint32_t kColOk     = 0x35D07F;
constexpr uint32_t kColCam    = 0x2FA8FF;
constexpr uint32_t kColCam2   = 0x1C6FD0;
constexpr uint32_t kColAsr  = 0x9B6BFF;
constexpr uint32_t kColAsr2 = 0x6B3FD0;
constexpr uint32_t kColCmd    = 0xFFB020;
constexpr uint32_t kColCmd2   = 0xD98A0B;
constexpr uint32_t kColRec    = 0xFF5C5C;
constexpr uint32_t kColRec2   = 0xC02F2F;

St7789Lcd*    s_lcd   = nullptr;
Cst816Touch*  s_touch = nullptr;
KorvoAudio*   s_audio = nullptr;
lv_display_t* s_disp  = nullptr;
lv_indev_t*   s_indev = nullptr;
uint8_t*      s_buf   = nullptr;

SemaphoreHandle_t s_lcd_mux = nullptr;  // LVGL's flush vs. the preview task's blits

std::atomic<Mode> s_mode{Mode::kLauncher};  // in effect now — what DrawCameraFrame checks
std::atomic<Mode> s_want{Mode::kLauncher};  // requested; the render task applies it

// Set when leaving the Camera app, so the tap that dismissed the preview does not also land on
// whatever control happens to be under the finger. Cleared when the finger comes off the glass.
std::atomic<bool> s_swallow_touch{false};

std::atomic<bool>     s_connected{false};
std::atomic<uint32_t> s_link_rev{0};

// Screens
lv_obj_t* s_scr_home  = nullptr;
lv_obj_t* s_scr_asr = nullptr;
lv_obj_t* s_scr_cmd   = nullptr;
lv_obj_t* s_scr_rec   = nullptr;

lv_obj_t* s_link_label = nullptr;
lv_obj_t* s_tiles      = nullptr;
constexpr int kPages = 4;
lv_obj_t* s_tile[kPages] = {}; // the swipeable pages, in order — kept so the dots don't have to
lv_obj_t* s_dot[kPages]  = {}; //   guess at child indices

// ASR app widgets
lv_obj_t* s_asr_btn   = nullptr;
lv_obj_t* s_asr_state = nullptr;
// Command app widgets
lv_obj_t* s_cmd_btn   = nullptr;
lv_obj_t* s_cmd_state = nullptr;
// Recorder app widgets
lv_obj_t* s_rec_btn   = nullptr;
lv_obj_t* s_rec_time  = nullptr;
lv_obj_t* s_rec_state = nullptr;

void Lock()   { if (s_lcd_mux) xSemaphoreTake(s_lcd_mux, portMAX_DELAY); }
void Unlock() { if (s_lcd_mux) xSemaphoreGive(s_lcd_mux); }

uint32_t TickCb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void FlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);

    // LVGL's RGB565 is little-endian; the ST7789 wants the high byte first.
    lv_draw_sw_rgb565_swap(px_map, static_cast<uint32_t>(w * h));

    // Nothing reaches the panel while the Camera app owns it.
    if (s_lcd && s_mode.load(std::memory_order_acquire) != Mode::kCamera) {
        Lock();
        (void)s_lcd->DrawBitmap(static_cast<uint16_t>(area->x1), static_cast<uint16_t>(area->y1),
                                static_cast<uint16_t>(w), static_cast<uint16_t>(h), px_map);
        Unlock();
    }
    lv_display_flush_ready(disp);
}

// Reads the snapshot the render task took at the top of this iteration — no I2C from inside LVGL.
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

// ── Small builders ─────────────────────────────────────────────────────────────

// Decoration by default. lv_obj_create() hands out LV_OBJ_FLAG_CLICKABLE, which would make every
// icon, lens and dot its own hit target that swallows the tap and does nothing with it — tapping
// the middle of an app icon would be dead. Strip it here; the few real buttons add it back.
lv_obj_t* Plain(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, uint32_t color, const lv_font_t* font) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);   // same reason: text must not eat the tap
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    return l;
}

// A rounded app-icon tile with a vertical gradient. Decorative (see Plain), so taps fall through to
// the container and the whole icon-plus-label block is one target.
lv_obj_t* IconTile(lv_obj_t* parent, int32_t size, uint32_t c1, uint32_t c2) {
    lv_obj_t* t = Plain(parent);
    lv_obj_set_size(t, size, size);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(t, size / 4, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(c1), 0);
    lv_obj_set_style_bg_grad_color(t, lv_color_hex(c2), 0);
    lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    return t;
}

lv_obj_t* Circle(lv_obj_t* parent, int32_t d, uint32_t color, lv_opa_t opa = LV_OPA_COVER) {
    lv_obj_t* c = Plain(parent);
    lv_obj_set_size(c, d, d);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, opa, 0);
    return c;
}

// ── App icons, drawn rather than bitmapped ─────────────────────────────────────

void DrawCameraGlyph(lv_obj_t* tile) {
    lv_obj_t* lens = Circle(tile, 46, kColText);
    lv_obj_center(lens);
    lv_obj_t* pupil = Circle(lens, 24, kColCam2);
    lv_obj_center(pupil);
    lv_obj_t* flash = Circle(tile, 10, kColText, LV_OPA_70);
    lv_obj_align(flash, LV_ALIGN_TOP_RIGHT, -14, 14);
}

void DrawMicGlyph(lv_obj_t* tile) {
    lv_obj_t* body = Plain(tile);                 // capsule
    lv_obj_set_size(body, 26, 42);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_radius(body, 13, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(kColText), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);

    lv_obj_t* stand = Plain(tile);                // stem
    lv_obj_set_size(stand, 4, 14);
    lv_obj_align(stand, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_radius(stand, 2, 0);
    lv_obj_set_style_bg_color(stand, lv_color_hex(kColText), 0);
    lv_obj_set_style_bg_opa(stand, LV_OPA_COVER, 0);

    lv_obj_t* base = Plain(tile);                 // foot
    lv_obj_set_size(base, 30, 4);
    lv_obj_align(base, LV_ALIGN_CENTER, 0, 32);
    lv_obj_set_style_radius(base, 2, 0);
    lv_obj_set_style_bg_color(base, lv_color_hex(kColText), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
}

// "Send what I say to the app." LVGL's built-in symbol font carries this glyph, so it needs no
// drawing and reads unambiguously next to the ASR mic.
void DrawCmdGlyph(lv_obj_t* tile) {
    lv_obj_t* g = Label(tile, LV_SYMBOL_UPLOAD, kColText, &lv_font_montserrat_28);
    lv_obj_center(g);
}

void DrawRecGlyph(lv_obj_t* tile) {
    lv_obj_t* ring = Circle(tile, 52, kColText, LV_OPA_30);
    lv_obj_center(ring);
    lv_obj_t* dot = Circle(tile, 30, kColText);
    lv_obj_center(dot);
}

// ── Home screen ────────────────────────────────────────────────────────────────

void OnTileClicked(lv_event_t* e) {
    OpenApp(static_cast<Mode>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void RefreshDots() {
    if (!s_tiles) return;
    lv_obj_t* active = lv_tileview_get_tile_active(s_tiles);
    for (int i = 0; i < kPages; ++i) {
        if (!s_dot[i]) continue;
        lv_obj_set_style_bg_opa(s_dot[i], active == s_tile[i] ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

void OnTileChanged(lv_event_t* /*e*/) { RefreshDots(); }

// One swipeable page: big icon, app name, one line of hint text.
lv_obj_t* BuildAppTile(lv_obj_t* tv, uint8_t col, Mode mode, uint32_t c1, uint32_t c2,
                       void (*glyph)(lv_obj_t*), const char* name, const char* hint) {
    lv_obj_t* tile = lv_tileview_add_tile(tv, col, 0, LV_DIR_HOR);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* app = Plain(tile);
    lv_obj_set_size(app, kUiW - 2 * kPad, 150);
    lv_obj_center(app);
    lv_obj_add_flag(app, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(app, OnTileClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(mode)));

    lv_obj_t* icon = IconTile(app, 92, c1, c2);
    glyph(icon);

    lv_obj_t* title = Label(app, name, kColText, &lv_font_montserrat_20);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t* sub = Label(app, hint, kColMuted, nullptr);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 126);
    return tile;
}

void BuildHome() {
    s_scr_home = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr_home);
    lv_obj_set_style_bg_color(s_scr_home, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(s_scr_home, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_home, LV_OBJ_FLAG_SCROLLABLE);

    // Status bar. The bar spans the full width, but its text is inset past the rounded corners.
    lv_obj_t* bar = Plain(s_scr_home);
    lv_obj_set_size(bar, kUiW, kBarH);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColBar), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    lv_obj_t* name = Label(bar, "Korvo", kColMuted, nullptr);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, kPad, 0);

    s_link_label = Label(bar, LV_SYMBOL_BLUETOOTH, kColMuted, nullptr);
    lv_obj_align(s_link_label, LV_ALIGN_RIGHT_MID, -kPad, 0);

    // Swipeable app pages.
    s_tiles = lv_tileview_create(s_scr_home);
    lv_obj_remove_style_all(s_tiles);
    lv_obj_set_size(s_tiles, kUiW, kUiH - kBarH - kDotsH);
    lv_obj_align(s_tiles, LV_ALIGN_TOP_MID, 0, kBarH);
    lv_obj_set_style_bg_opa(s_tiles, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_tiles, OnTileChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    s_tile[0] = BuildAppTile(s_tiles, 0, Mode::kCamera,   kColCam,   kColCam2,   DrawCameraGlyph,
                             "Camera",   "Live preview");
    s_tile[1] = BuildAppTile(s_tiles, 1, Mode::kAsr,      kColAsr,   kColAsr2,   DrawMicGlyph,
                             "ASR",      "Live transcription");
    s_tile[2] = BuildAppTile(s_tiles, 2, Mode::kCommand,  kColCmd,   kColCmd2,   DrawCmdGlyph,
                             "Command",  "Send a spoken command");
    s_tile[3] = BuildAppTile(s_tiles, 3, Mode::kRecorder, kColRec,   kColRec2,   DrawRecGlyph,
                             "Recorder", "Record to SD card");

    // Page indicator.
    lv_obj_t* dots = Plain(s_scr_home);
    lv_obj_set_size(dots, kUiW, kDotsH);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (int i = 0; i < kPages; ++i) {
        s_dot[i] = Circle(dots, 7, kColText, LV_OPA_30);
        lv_obj_align(s_dot[i], LV_ALIGN_CENTER, (2 * i - (kPages - 1)) * 8, 0);
    }
    RefreshDots();
}

// ── App screens ────────────────────────────────────────────────────────────────

void OnBackClicked(lv_event_t* /*e*/) { GoHome(); }

// Every app screen gets the same frame: a title at the top and a Back button at the bottom. Both
// are centred or inset by kPad, so the rounded corners never clip them.
lv_obj_t* BuildAppScreen(const char* title) {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = Label(scr, title, kColMuted, nullptr);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* back = Plain(scr);
    lv_obj_set_size(back, 110, 30);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(back, 15, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(kColBar), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* bl = Label(back, LV_SYMBOL_LEFT "  Back", kColText, nullptr);
    lv_obj_center(bl);
    return scr;
}

void OnAsrClicked(lv_event_t* /*e*/) {
    if (!s_audio) return;
    if (s_audio->AsrActive()) s_audio->StopAsr();
    else                      s_audio->StartAsr();
}

void BuildAsrScreen() {
    s_scr_asr = BuildAppScreen("ASR");

    s_asr_btn = Circle(s_scr_asr, 104, kColAsr);
    lv_obj_align(s_asr_btn, LV_ALIGN_CENTER, 0, -14);
    lv_obj_add_flag(s_asr_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_asr_btn, OnAsrClicked, LV_EVENT_CLICKED, nullptr);
    DrawMicGlyph(s_asr_btn);

    s_asr_state = Label(s_scr_asr, "", kColMuted, nullptr);
    lv_obj_align(s_asr_state, LV_ALIGN_CENTER, 0, 52);
}

void OnCmdClicked(lv_event_t* /*e*/) {
    if (!s_audio) return;
    if (s_audio->CommandActive()) s_audio->StopCommand();
    else                          s_audio->StartCommand();
}

void BuildCmdScreen() {
    s_scr_cmd = BuildAppScreen("Command");

    s_cmd_btn = Circle(s_scr_cmd, 104, kColCmd);
    lv_obj_align(s_cmd_btn, LV_ALIGN_CENTER, 0, -14);
    lv_obj_add_flag(s_cmd_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cmd_btn, OnCmdClicked, LV_EVENT_CLICKED, nullptr);
    DrawCmdGlyph(s_cmd_btn);

    s_cmd_state = Label(s_scr_cmd, "", kColMuted, nullptr);
    lv_obj_align(s_cmd_state, LV_ALIGN_CENTER, 0, 52);
}

void OnRecClicked(lv_event_t* /*e*/) {
    if (!s_audio) return;
    if (s_audio->RecordingActive()) s_audio->StopRecording();
    else                            s_audio->StartRecording();
}

void BuildRecScreen() {
    s_scr_rec = BuildAppScreen("Recorder");

    s_rec_btn = Circle(s_scr_rec, 96, kColRec);
    lv_obj_align(s_rec_btn, LV_ALIGN_CENTER, 0, -22);
    lv_obj_add_flag(s_rec_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_rec_btn, OnRecClicked, LV_EVENT_CLICKED, nullptr);
    DrawRecGlyph(s_rec_btn);

    s_rec_time = Label(s_scr_rec, "00:00", kColText, &lv_font_montserrat_20);
    lv_obj_align(s_rec_time, LV_ALIGN_CENTER, 0, 40);

    s_rec_state = Label(s_scr_rec, "", kColMuted, nullptr);
    lv_obj_align(s_rec_state, LV_ALIGN_CENTER, 0, 64);
}

// ── Dynamic refreshes ──────────────────────────────────────────────────────────

void RefreshLink() {
    if (!s_link_label) return;
    const bool up = s_connected.load(std::memory_order_acquire);
    lv_obj_set_style_text_color(s_link_label, lv_color_hex(up ? kColOk : kColMuted), 0);
}

void RefreshAsr() {
    if (!s_asr_btn || !s_asr_state) return;
    if (!s_audio) {
        lv_label_set_text(s_asr_state, "Audio unavailable");
        lv_obj_set_style_bg_color(s_asr_btn, lv_color_hex(kColMuted), 0);
        return;
    }
    const bool on = s_audio->AsrActive();
    lv_obj_set_style_bg_color(s_asr_btn, lv_color_hex(on ? kColOk : kColAsr), 0);
    if (on)                                            lv_label_set_text(s_asr_state, "Listening - tap to stop");
    else if (!s_connected.load(std::memory_order_acquire)) lv_label_set_text(s_asr_state, "App not connected");
    else                                               lv_label_set_text(s_asr_state, "Tap to talk");
}

void RefreshCmd() {
    if (!s_cmd_btn || !s_cmd_state) return;
    if (!s_audio) {
        lv_label_set_text(s_cmd_state, "Audio unavailable");
        lv_obj_set_style_bg_color(s_cmd_btn, lv_color_hex(kColMuted), 0);
        return;
    }
    const bool on = s_audio->CommandActive();
    lv_obj_set_style_bg_color(s_cmd_btn, lv_color_hex(on ? kColOk : kColCmd), 0);
    if (on)                                                lv_label_set_text(s_cmd_state, "Sending - tap to finish");
    else if (!s_connected.load(std::memory_order_acquire)) lv_label_set_text(s_cmd_state, "App not connected");
    else                                                   lv_label_set_text(s_cmd_state, "Tap to send a command");
}

void RefreshRec() {
    if (!s_rec_btn || !s_rec_time || !s_rec_state) return;
    if (!s_audio) {
        lv_label_set_text(s_rec_state, "Audio unavailable");
        lv_obj_set_style_bg_color(s_rec_btn, lv_color_hex(kColMuted), 0);
        return;
    }
    const bool on = s_audio->RecordingActive();
    lv_obj_set_style_bg_color(s_rec_btn, lv_color_hex(on ? kColOk : kColRec), 0);

    const uint32_t s = s_audio->RecordedMs() / 1000u;
    char t[16];
    snprintf(t, sizeof(t), "%02u:%02u", static_cast<unsigned>(s / 60u), static_cast<unsigned>(s % 60u));
    lv_label_set_text(s_rec_time, t);

    if (!s_audio->SdReady())  lv_label_set_text(s_rec_state, "No SD card");
    else if (on)              lv_label_set_text(s_rec_state, "Recording - tap to stop");
    else if (s_audio->LastFile()[0]) lv_label_set_text(s_rec_state, s_audio->LastFile());
    else                      lv_label_set_text(s_rec_state, "Tap to record");
}

// ── Mode switching — all of it on the render task, so every LVGL call stays single-threaded ──

void ApplyMode(Mode want) {
    const Mode from = s_mode.load(std::memory_order_acquire);

    if (want == Mode::kCamera) {
        // Stop LVGL reaching the panel first, then wipe it: the preview is 240 wide on a 280 wide
        // screen, and without this the home screen would stay visible in the bars either side.
        s_mode.store(Mode::kCamera, std::memory_order_release);
        Lock();
        if (s_lcd) (void)s_lcd->FillSolid(rgb565::kBlack);
        Unlock();
        if (s_touch) (void)s_touch->TakePressEdge();  // don't let the opening tap also close it
        ESP_LOGI(TAG, "camera app open - tap anywhere to go back");
        return;
    }

    lv_obj_t* scr = want == Mode::kAsr     ? s_scr_asr
                  : want == Mode::kCommand  ? s_scr_cmd
                  : want == Mode::kRecorder ? s_scr_rec
                                            : s_scr_home;
    if (scr) lv_screen_load(scr);
    s_mode.store(want, std::memory_order_release);

    if (from == Mode::kCamera) {
        // The tap that dismissed the preview must not land on the screen behind it.
        s_swallow_touch.store(true, std::memory_order_release);
    }
    if (want == Mode::kAsr)      RefreshAsr();
    if (want == Mode::kCommand)  RefreshCmd();
    if (want == Mode::kRecorder) RefreshRec();
    if (want == Mode::kLauncher) RefreshDots();
    lv_obj_invalidate(lv_screen_active());   // full redraw, over whatever was there
    ESP_LOGI(TAG, "screen -> %s", want == Mode::kAsr      ? "asr"
                                : want == Mode::kCommand  ? "command"
                                : want == Mode::kRecorder ? "recorder" : "home");
}

void RenderTask(void* /*arg*/) {
    uint32_t seen_link = 0;
    while (true) {
        if (s_touch) (void)s_touch->Poll();

        const Mode want = s_want.load(std::memory_order_acquire);
        if (want != s_mode.load(std::memory_order_acquire)) ApplyMode(want);

        const Mode mode = s_mode.load(std::memory_order_acquire);
        if (mode == Mode::kCamera) {
            // The preview task owns the panel; all we do here is watch for the tap that ends it.
            if (s_touch && s_touch->TakePressEdge()) GoHome();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        const uint32_t link_rev = s_link_rev.load(std::memory_order_acquire);
        if (link_rev != seen_link) {
            seen_link = link_rev;
            RefreshLink();
        }
        // These carry live state from the audio task, so they are polled rather than pushed.
        if (mode == Mode::kAsr)      RefreshAsr();
        if (mode == Mode::kCommand)  RefreshCmd();
        if (mode == Mode::kRecorder) RefreshRec();

        uint32_t next = lv_timer_handler();  // renders + flushes only if something changed
        if (next == LV_NO_TIMER_READY || next > 30) next = 30;
        vTaskDelay(pdMS_TO_TICKS(next < 5 ? 5 : next));
    }
}

}  // namespace

esp_err_t Start(St7789Lcd* lcd, Cst816Touch* touch, KorvoAudio* audio) {
    if (!lcd || !lcd->Ready()) return ESP_ERR_INVALID_STATE;
    s_lcd   = lcd;
    s_touch = (touch && touch->Ready()) ? touch : nullptr;
    s_audio = (audio && audio->Ready()) ? audio : nullptr;

    s_lcd_mux = xSemaphoreCreateMutex();
    if (!s_lcd_mux) return ESP_ERR_NO_MEM;

    // PSRAM, deliberately. LVGL's software renderer would be quicker writing to internal RAM, but
    // internal RAM is what BLE needs to start advertising and there is not enough for both — and
    // these screens are near-static, so the extra render time is invisible. It costs nothing at
    // blit time either: St7789Lcd memcpys into its own internal DMA stripes regardless of source.
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
    BuildAsrScreen();
    BuildCmdScreen();
    BuildRecScreen();
    lv_screen_load(s_scr_home);
    RefreshLink();

    // Internal stack: LVGL's draw path is deep.
    if (xTaskCreate(RenderTask, "korvo_ui", 8192, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "render task create failed");
        return ESP_ERR_NO_MEM;
    }

    if (s_touch) {
        ESP_LOGI(TAG, "UI up: %ldx%ld, swipe between Camera / ASR / Command / Recorder and tap to open",
                 static_cast<long>(kUiW), static_cast<long>(kUiH));
    } else {
        // No touch means nothing can be navigated, so don't strand the user on the home screen.
        ESP_LOGW(TAG, "no touch controller - skipping the home screen, going straight to the camera");
        s_want.store(Mode::kCamera, std::memory_order_release);
    }
    return ESP_OK;
}

Mode CurrentMode() { return s_mode.load(std::memory_order_acquire); }

void OpenApp(Mode m) { s_want.store(m, std::memory_order_release); }
void GoHome()        { s_want.store(Mode::kLauncher, std::memory_order_release); }

esp_err_t DrawCameraFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels) {
    if (!s_lcd || s_mode.load(std::memory_order_acquire) != Mode::kCamera) return ESP_ERR_INVALID_STATE;
    Lock();
    const esp_err_t r = s_lcd->DrawBitmap(x, y, w, h, pixels);
    Unlock();
    return r;
}

void SetLinkState(bool connected) {
    if (connected == s_connected.load(std::memory_order_acquire)) return;
    s_connected.store(connected, std::memory_order_release);
    s_link_rev.fetch_add(1, std::memory_order_release);
}

}  // namespace korvo_ui
