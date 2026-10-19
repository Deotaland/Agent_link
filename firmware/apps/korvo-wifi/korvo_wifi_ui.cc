#include "korvo_wifi_ui.h"
#include "agent_link_platform.h"

#include "config.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
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
constexpr uint32_t kColVoice = 0xA075EB;
constexpr uint32_t kColServo = 0x36BFA7;

St7789Lcd*       s_lcd   = nullptr;
Cst816Touch*     s_touch = nullptr;
KorvoCloudAudio* s_audio = nullptr;
ServoRequest     s_servo_request = nullptr;
void*            s_servo_ctx = nullptr;
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
lv_obj_t* s_scr_voice = nullptr;
lv_obj_t* s_scr_servo = nullptr;
lv_obj_t* s_scr_conversations = nullptr;

// Which page to come back to when Camera or Shade is dismissed. Only ever a real page.
std::atomic<Mode> s_last_page{Mode::kHome};

lv_obj_t* s_link_dot = nullptr;
lv_obj_t* s_title    = nullptr;   // what the board is doing, one line
lv_obj_t* s_big      = nullptr;   // the activation code, or a status word
lv_obj_t* s_hint     = nullptr;   // what the user should do about it
lv_obj_t* s_voice_hint = nullptr;
lv_obj_t* s_servo_status = nullptr;
lv_obj_t* s_servo_select_label = nullptr;
lv_obj_t* s_servo_picker = nullptr;
lv_obj_t* s_servo_picker_rows[5] = {};
lv_obj_t* s_servo_angle_label = nullptr;
lv_obj_t* s_servo_arc = nullptr;
uint8_t s_servo_channel = 1;
uint8_t s_servo_picker_channel = 1;
uint8_t s_servo_picker_start_channel = 1;
int32_t s_servo_picker_start_y = 0;
bool s_servo_picker_active = false;
using ServoMode = KorvoCloudServo8::Mode;
using ServoCommand = KorvoCloudServo8::Command;
using ServoStatus = KorvoCloudServo8::Status;
KorvoCloudServo8::ChannelState s_servo_states[9] = {
    {ServoMode::Position180, 90}, {ServoMode::Position180, 90},
    {ServoMode::Position180, 90}, {ServoMode::Position180, 90},
    {ServoMode::Position180, 90}, {ServoMode::Position180, 90},
    {ServoMode::Position180, 90}, {ServoMode::Position180, 90},
    {ServoMode::Position180, 90}};
lv_obj_t* s_servo_mode_label = nullptr;
lv_obj_t* s_servo_unit = nullptr;
lv_obj_t* s_servo_stop = nullptr;
bool s_servo_all_mixed = false;
int64_t s_servo_last_send_us = 0;
portMUX_TYPE s_servo_result_lock = portMUX_INITIALIZER_UNLOCKED;
ServoCommand s_servo_result_command = {};
ServoStatus s_servo_result_status = {};
esp_err_t s_servo_result_error = ESP_OK;
esp_err_t s_servo_read_error = ESP_OK;
std::atomic<uint32_t> s_servo_result_rev{0};
int64_t   s_last_voice_request_us = 0;
int64_t   s_last_voice_attempt_us = 0;
int64_t   s_last_mic_retry_us = 0;
bool      s_voice_autoconnect_pending = false;
unsigned  s_voice_auto_attempts = 0;
lv_obj_t* s_call_status = nullptr;
lv_obj_t* s_call_text = nullptr;
lv_obj_t* s_retry = nullptr;
lv_obj_t* s_conversations_status = nullptr;
lv_obj_t* s_conversation_rows[3] = {};
lv_obj_t* s_delete_rows[3] = {};
int s_selected_agent = 0;
int32_t s_saved_agent = 0;
char s_selected_agent_name[80] = {};
char s_selected_session[80] = {};
char s_selected_conversation_name[96] = {};
EXT_RAM_BSS_ATTR agent_link_platform_conversation_t s_conversations[3];
size_t s_conversation_count = 0, s_conversation_total = 0;
size_t s_conversation_offset = 0;

enum class PlatformOp { kConversations, kCreate, kDelete };
struct PlatformCommand {
    PlatformOp op;
    int agent_id;
    size_t offset;
    char session_id[80];
};
struct PlatformResult {
    PlatformCommand command;
    esp_err_t error;
    size_t count;
    size_t total;
    agent_link_platform_conversation_t conversations[3];
    char created_id[80];
};
QueueHandle_t s_platform_commands = nullptr;
SemaphoreHandle_t s_platform_mutex = nullptr;
EXT_RAM_BSS_ATTR PlatformResult s_platform_result;
std::atomic<uint32_t> s_platform_rev{0};
std::atomic<bool> s_platform_busy{false};
bool s_selection_loaded = false;
int s_delete_confirm_row = -1;
int64_t s_delete_confirm_until_us = 0;

void LoadSelection() {
    if (s_selection_loaded) return;
    const esp_err_t init = nvs_flash_init();
    if (init != ESP_OK) return;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open("kc_voice", NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) { s_selection_loaded = true; return; }
    if (opened != ESP_OK) return;
    int32_t agent = 0;
    size_t session_size = sizeof s_selected_session;
    if (nvs_get_i32(handle, "agent", &agent) == ESP_OK && agent > 0) {
        s_saved_agent = agent;
        size_t conversation_name_size = sizeof s_selected_conversation_name;
        if (nvs_get_str(handle, "session", s_selected_session, &session_size) == ESP_OK)
            (void)nvs_get_str(handle, "session_name", s_selected_conversation_name,
                              &conversation_name_size);
        else s_selected_session[0] = '\0';
    } else {
        s_selected_session[0] = '\0';
    }
    nvs_close(handle);
    s_selection_loaded = true;
}

void SaveSelection() {
    nvs_handle_t handle;
    if (nvs_open("kc_voice", NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t result = nvs_set_i32(handle, "agent", s_selected_agent);
    if (result == ESP_OK) result = nvs_set_str(handle, "session", s_selected_session);
    if (result == ESP_OK) result = nvs_set_str(handle, "agent_name", s_selected_agent_name);
    if (result == ESP_OK) result = nvs_set_str(handle, "session_name", s_selected_conversation_name);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) ESP_LOGW(TAG, "voice selection could not be saved: %s", esp_err_to_name(result));
}

void ShowSelection() {
    if (!s_call_text) return;
    if (s_selected_agent <= 0 || !s_selected_session[0]) {
        lv_label_set_text(s_call_text, s_selected_agent > 0 ? "Choose a conversation in Sessions" :
                          "Bind an agent in the platform console");
        return;
    }
    char caption[192];
    snprintf(caption, sizeof caption, "%s / %s",
             s_selected_agent_name[0] ? s_selected_agent_name : "Agent",
             s_selected_conversation_name[0] ? s_selected_conversation_name : s_selected_session);
    lv_label_set_text(s_call_text, caption);
}

void ShowConversationRows();
void ResetDeleteConfirmation();

bool SyncBoundAgent() {
    LoadSelection();
    agent_link_platform_agent_t bound = {};
    const esp_err_t result = agent_link_platform_bound_agent(&bound);
    if (result != ESP_OK && s_selected_agent == 0) return false;
    const int id = result == ESP_OK ? bound.id : 0;
    if (id != s_selected_agent) {
        s_voice_autoconnect_pending = false;
        if (s_audio) { s_audio->StopContinuous(); s_audio->StopAsr(); s_audio->StopPlayback(); }
        if (s_selected_agent) (void)agent_link_end_realtime();
        s_last_voice_request_us = 0;
        if (s_saved_agent != id) {
            s_selected_session[0] = '\0';
            s_selected_conversation_name[0] = '\0';
        }
        s_selected_agent = id;
        s_saved_agent = id;
        s_conversation_count = s_conversation_total = s_conversation_offset = 0;
        ResetDeleteConfirmation();
        ShowConversationRows();
        if (s_mode.load() == Mode::kConversations) s_want.store(Mode::kVoice);
        snprintf(s_selected_agent_name, sizeof s_selected_agent_name, "%s", bound.name);
        SaveSelection();
        ShowSelection();
        lv_label_set_text(s_call_status, id ? "Choose a conversation in Sessions" :
                          "Bind an agent in the platform console");
    } else if (strcmp(s_selected_agent_name, bound.name) != 0) {
        snprintf(s_selected_agent_name, sizeof s_selected_agent_name, "%s", bound.name);
        ShowSelection();
    }
    return result == ESP_OK && bound.voice_supported;
}

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

void DrawVoiceGlyph(lv_obj_t* tile) {
    lv_obj_t* mic = Plain(tile);
    lv_obj_set_size(mic, 22, 30);
    lv_obj_align(mic, LV_ALIGN_CENTER, 0, -7);
    lv_obj_set_style_radius(mic, 11, 0);
    lv_obj_set_style_bg_color(mic, lv_color_hex(kColText), 0);
    lv_obj_set_style_bg_opa(mic, LV_OPA_COVER, 0);
    lv_obj_t* stem = Plain(tile);
    lv_obj_set_size(stem, 4, 13);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_bg_color(stem, lv_color_hex(kColText), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_t* base = Plain(tile);
    lv_obj_set_size(base, 24, 4);
    lv_obj_align(base, LV_ALIGN_CENTER, 0, 25);
    lv_obj_set_style_bg_color(base, lv_color_hex(kColText), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
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

    lv_obj_t* name = Label(bar, "korvo-wifi", kColMuted, &lv_font_montserrat_14);
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
    ESP_LOGD(TAG, "camera icon tapped");
    s_want.store(Mode::kCamera, std::memory_order_release);
}

bool QueuePlatform(PlatformOp op, int agent_id = 0, size_t offset = 0,
                   const char* session_id = nullptr) {
    if (!s_platform_commands || s_platform_busy.exchange(true)) return false;
    PlatformCommand command{op, agent_id, offset, {}};
    if (session_id) snprintf(command.session_id, sizeof command.session_id, "%s", session_id);
    if (xQueueSend(s_platform_commands, &command, 0) == pdTRUE) return true;
    s_platform_busy.store(false);
    return false;
}

void PlatformWorker(void*) {
    PlatformCommand command;
    while (true) {
        if (xQueueReceive(s_platform_commands, &command, portMAX_DELAY) != pdTRUE) continue;
        PlatformResult result = {};
        result.command = command;
        result.error = ESP_ERR_INVALID_ARG;
        switch (command.op) {
        case PlatformOp::kConversations:
            result.error = agent_link_platform_conversations(command.agent_id, command.offset,
                    result.conversations, sizeof result.conversations / sizeof result.conversations[0],
                    &result.count, &result.total);
            break;
        case PlatformOp::kCreate:
            result.error = agent_link_platform_create_conversation(command.agent_id,
                    result.created_id, sizeof result.created_id);
            break;
        case PlatformOp::kDelete:
            result.error = agent_link_platform_delete_conversation(command.agent_id,
                                                                   command.session_id);
            break;
        }
        xSemaphoreTake(s_platform_mutex, portMAX_DELAY);
        s_platform_result = result;
        xSemaphoreGive(s_platform_mutex);
        s_platform_busy.store(false);
        s_platform_rev.fetch_add(1, std::memory_order_release);
    }
}

lv_obj_t* MenuButton(lv_obj_t* screen, int x, int y, int w, int h,
                     const char* caption, lv_event_cb_t cb, void* user_data = nullptr) {
    lv_obj_t* button = lv_button_create(screen);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_color(button, lv_color_hex(kColCard), 0);
#if LV_FONT_SIMSUN_14_CJK
    lv_obj_t* label = Label(button, caption, kColText, &lv_font_simsun_14_cjk);
#else
    lv_obj_t* label = Label(button, caption, kColText, &lv_font_montserrat_14);
#endif
    lv_obj_set_width(label, w - 12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_center(label);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    return label;
}

void ShowConversationRows() {
    for (size_t i = 0; i < 3; ++i) {
        char caption[110] = {};
        if (i < s_conversation_count) {
            const auto& conversation = s_conversations[i];
            const char* name = conversation.name[0] ? conversation.name : conversation.id;
            snprintf(caption, sizeof caption, "%u. %.95s", (unsigned)(s_conversation_offset + i + 1), name);
        } else snprintf(caption, sizeof caption, "-");
        lv_label_set_text(s_conversation_rows[i], caption);
        lv_obj_t* button = lv_obj_get_parent(s_conversation_rows[i]);
        if (i < s_conversation_count) lv_obj_remove_state(button, LV_STATE_DISABLED);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
        if (s_delete_rows[i]) {
            lv_label_set_text(s_delete_rows[i], s_delete_confirm_row == static_cast<int>(i)
                    ? "OK?" : "Del");
            lv_obj_t* remove = lv_obj_get_parent(s_delete_rows[i]);
            if (i < s_conversation_count) lv_obj_remove_state(remove, LV_STATE_DISABLED);
            else lv_obj_add_state(remove, LV_STATE_DISABLED);
        }
    }
    char status[72];
    snprintf(status, sizeof status, "Sessions %u-%u",
             s_conversation_count ? (unsigned)(s_conversation_offset + 1) : 0,
             (unsigned)(s_conversation_offset + s_conversation_count));
    lv_label_set_text(s_conversations_status, status);
}

void ResetDeleteConfirmation() {
    s_delete_confirm_row = -1;
    s_delete_confirm_until_us = 0;
    for (lv_obj_t* label : s_delete_rows) if (label) lv_label_set_text(label, "Del");
}

void OnConversationRow(lv_event_t* e) {
    if (!SyncBoundAgent()) return;
    const size_t row = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if (row >= s_conversation_count || s_platform_busy) return;
    ResetDeleteConfirmation();
    snprintf(s_selected_session, sizeof s_selected_session, "%s", s_conversations[row].id);
    snprintf(s_selected_conversation_name, sizeof s_selected_conversation_name, "%s",
             s_conversations[row].name);
    SaveSelection();
    ShowSelection();
    s_want.store(Mode::kVoice);
}

void OnConversationDelete(lv_event_t* e) {
    const size_t row = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if (row >= s_conversation_count || s_platform_busy) return;
    const int64_t now = esp_timer_get_time();
    if (s_delete_confirm_row != static_cast<int>(row) || now > s_delete_confirm_until_us) {
        ResetDeleteConfirmation();
        s_delete_confirm_row = static_cast<int>(row);
        s_delete_confirm_until_us = now + 5000000;
        lv_label_set_text(s_delete_rows[row], "OK?");
        lv_label_set_text(s_conversations_status, "Tap OK? again to delete this session");
        return;
    }
    char id[sizeof s_conversations[row].id];
    snprintf(id, sizeof id, "%s", s_conversations[row].id);
    ResetDeleteConfirmation();
    lv_label_set_text(s_conversations_status, "Deleting session...");
    QueuePlatform(PlatformOp::kDelete, s_selected_agent, 0, id);
}

void OnConversationPrev(lv_event_t*) {
    if (s_platform_busy || s_conversation_offset == 0) return;
    ResetDeleteConfirmation();
    s_conversation_offset = s_conversation_offset >= 3 ? s_conversation_offset - 3 : 0;
    lv_label_set_text(s_conversations_status, "Loading...");
    QueuePlatform(PlatformOp::kConversations, s_selected_agent, s_conversation_offset);
}

void OnConversationNext(lv_event_t*) {
    if (s_platform_busy || s_conversation_offset + 3 >= s_conversation_total) return;
    ResetDeleteConfirmation();
    s_conversation_offset += 3;
    lv_label_set_text(s_conversations_status, "Loading...");
    QueuePlatform(PlatformOp::kConversations, s_selected_agent, s_conversation_offset);
}

void OnConversationNew(lv_event_t*) {
    if (s_platform_busy || s_selected_agent <= 0) return;
    ResetDeleteConfirmation();
    lv_label_set_text(s_conversations_status, "Creating voice session...");
    QueuePlatform(PlatformOp::kCreate, s_selected_agent);
}

void OnConversationsBack(lv_event_t*) {
    if (!s_platform_busy) { ResetDeleteConfirmation(); s_want.store(Mode::kVoice); }
}

void ApplyPlatformResult() {
    xSemaphoreTake(s_platform_mutex, portMAX_DELAY);
    const PlatformResult& result = s_platform_result;
    const PlatformOp op = result.command.op;
    const esp_err_t error = result.error;
    agent_link_platform_agent_t bound = {};
    if (agent_link_platform_bound_agent(&bound) != ESP_OK || bound.id != result.command.agent_id ||
        s_selected_agent != result.command.agent_id) {
        xSemaphoreGive(s_platform_mutex);
        return;
    }
    if (error == ESP_OK) {
        if (op == PlatformOp::kConversations) {
            s_conversation_count = result.count;
            s_conversation_total = result.total;
            memcpy(s_conversations, result.conversations, sizeof s_conversations);
            ResetDeleteConfirmation();
            ShowConversationRows();
        } else if (op == PlatformOp::kCreate) {
            s_conversation_offset = 0;
            if (result.created_id[0]) {
                ESP_LOGI(TAG, "voice session created and selected");
                snprintf(s_selected_session, sizeof s_selected_session, "%s", result.created_id);
                s_selected_conversation_name[0] = '\0';
                SaveSelection();
                ShowSelection();
                s_voice_autoconnect_pending = true;
                s_voice_auto_attempts = 0;
                s_want.store(Mode::kVoice);
            } else {
                ESP_LOGW(TAG, "voice session create returned no session ID");
                lv_label_set_text(s_conversations_status, "Loading new session...");
                QueuePlatform(PlatformOp::kConversations, s_selected_agent);
            }
        } else if (op == PlatformOp::kDelete) {
            ESP_LOGI(TAG, "voice session deleted");
            if (strcmp(s_selected_session, result.command.session_id) == 0) {
                s_selected_session[0] = '\0';
                s_selected_conversation_name[0] = '\0';
                SaveSelection();
                ShowSelection();
            }
            s_conversation_offset = 0;
            lv_label_set_text(s_conversations_status, "Session deleted - refreshing...");
            QueuePlatform(PlatformOp::kConversations, s_selected_agent);
        }
    } else {
        char message[96];
        snprintf(message, sizeof message, "%s: %s",
                 op == PlatformOp::kCreate ? "Create failed" : op == PlatformOp::kDelete ? "Delete failed" : "Load failed",
                 esp_err_to_name(error));
        lv_label_set_text(op == PlatformOp::kCreate && s_mode.load() == Mode::kVoice ?
                          s_call_status : s_conversations_status, message);
        if (op == PlatformOp::kCreate && s_mode.load() == Mode::kVoice)
            lv_obj_remove_state(s_retry, LV_STATE_DISABLED);
    }
    xSemaphoreGive(s_platform_mutex);
}

void TryConnectVoice() {
    if (!s_voice_autoconnect_pending || s_mode.load() != Mode::kVoice || s_platform_busy) return;
    if (!SyncBoundAgent()) {
        s_voice_autoconnect_pending = false;
        lv_label_set_text(s_call_status, "Bind a voice agent in the platform console");
        return;
    }
    if (!s_selected_agent || !s_selected_session[0]) {
        s_voice_autoconnect_pending = false;
        lv_label_set_text(s_call_status, "Select a session in Sessions");
        return;
    }
    if (!agent_link_platform_available()) {
        s_voice_autoconnect_pending = false;
        lv_label_set_text(s_call_status, "Platform access not configured");
        return;
    }
    if (agent_link_state() == AGENT_STATE_READY) {
        s_voice_autoconnect_pending = false;
        if (s_audio) s_audio->StartContinuous();
        return;
    }
    if (agent_link_state() != AGENT_STATE_CONNECTED || s_last_voice_request_us) return;
    const int64_t now = esp_timer_get_time();
    if (now - s_last_voice_attempt_us < 1000000) return;
    s_last_voice_attempt_us = now;
    const esp_err_t r = agent_link_request_realtime_session(s_selected_agent, s_selected_session);
    if (r == ESP_OK) {
        s_last_voice_request_us = now;
        s_voice_autoconnect_pending = false;
        lv_label_set_text(s_call_status, "Connecting voice...");
    } else if (r != ESP_ERR_INVALID_STATE || ++s_voice_auto_attempts >= 10) {
        s_voice_autoconnect_pending = false;
        lv_label_set_text(s_call_status, "Voice unavailable - tap Retry");
        ESP_LOGW(TAG, "voice request failed: %s", esp_err_to_name(r));
    }
}

void ConnectVoice(lv_event_t* /*e*/) {
    if (agent_link_state() == AGENT_STATE_READY || s_last_voice_request_us) return;
    s_voice_auto_attempts = 0;
    s_voice_autoconnect_pending = true;
    TryConnectVoice();
}

void EndVoice(lv_event_t* /*e*/) {
    s_want.store(Mode::kApps);
}

void OnVoiceSettings(lv_event_t*) {
    if (s_platform_busy) return;
    const bool bound = SyncBoundAgent();
    ResetDeleteConfirmation();
    s_conversation_offset = 0;
    s_want.store(Mode::kConversations);
    lv_label_set_text(s_conversations_status, bound ? "Loading sessions..." :
                      "Bind a voice agent in the platform console");
    if (bound) QueuePlatform(PlatformOp::kConversations, s_selected_agent);
}

void OnVoiceNew(lv_event_t*) {
    if (s_platform_busy) return;
    if (!SyncBoundAgent()) { OnVoiceSettings(nullptr); return; }
    s_voice_autoconnect_pending = false;
    if (s_audio) s_audio->StopContinuous();
    (void)agent_link_end_realtime();
    s_last_voice_request_us = 0;
    lv_label_set_text(s_call_status, "Creating voice session...");
    lv_obj_add_state(s_retry, LV_STATE_DISABLED);
    QueuePlatform(PlatformOp::kCreate, s_selected_agent);
}

void OnVoiceIcon(lv_event_t* /*e*/) {
    if (!s_voice_hint) return;
    if (!s_audio) {
        lv_label_set_text(s_voice_hint, "no audio");
        return;
    }
    LoadSelection();
    ShowSelection();
    if (SyncBoundAgent() && !s_selected_session[0]) OnVoiceSettings(nullptr);
    else s_want.store(Mode::kVoice);
}

lv_obj_t* VoiceButton(int32_t x, int32_t y, int32_t width, const char* caption) {
    lv_obj_t* button = lv_button_create(s_scr_voice);
    lv_obj_set_size(button, width, 34);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button, lv_color_hex(kColCard), 0);
    lv_obj_t* label = Label(button, caption, kColText, &lv_font_montserrat_14);
    lv_obj_center(label);
    return button;
}

void BuildVoice() {
    s_scr_voice = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr_voice);
    lv_obj_set_style_bg_color(s_scr_voice, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(s_scr_voice, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_voice, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* title = Label(s_scr_voice, "Voice", kColVoice, &lv_font_montserrat_14);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    s_call_status = Label(s_scr_voice, "Connecting...", kColMuted, &lv_font_montserrat_14);
    lv_obj_set_size(s_call_status, kUiW - 2 * kPad, 36);
    lv_obj_align(s_call_status, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_text_align(s_call_status, LV_TEXT_ALIGN_CENTER, 0);
    s_call_text = Label(s_scr_voice, "", kColText, &lv_font_montserrat_14);
#if LV_FONT_SIMSUN_14_CJK
    lv_obj_set_style_text_font(s_call_text, &lv_font_simsun_14_cjk, 0);
#endif
    lv_obj_set_size(s_call_text, kUiW - 2 * kPad, 32);
    lv_obj_align(s_call_text, LV_ALIGN_TOP_MID, 0, 78);
    lv_label_set_long_mode(s_call_text, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_call_text, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t* create = VoiceButton(kPad, 116, 112, "New session");
    lv_obj_set_style_bg_color(create, lv_color_hex(kColVoice), 0);
    lv_obj_add_event_cb(create, OnVoiceNew, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* settings = VoiceButton(kUiW - kPad - 112, 116, 112, "Sessions");
    lv_obj_add_event_cb(settings, OnVoiceSettings, LV_EVENT_CLICKED, nullptr);
    s_retry = VoiceButton(kPad, 164, 112, "Retry");
    lv_obj_add_event_cb(s_retry, ConnectVoice, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* end = VoiceButton(kUiW - kPad - 112, 164, 112, "End call");
    lv_obj_add_event_cb(end, EndVoice, LV_EVENT_CLICKED, nullptr);
    BuildPill(s_scr_voice);
}

void BuildConversations() {
    s_scr_conversations = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr_conversations);
    lv_obj_set_style_bg_color(s_scr_conversations, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(s_scr_conversations, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_conversations, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* title = Label(s_scr_conversations, "Voice sessions", kColVoice, &lv_font_montserrat_20);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 7);
    s_conversations_status = Label(s_scr_conversations, "Bind agent in platform console", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(s_conversations_status, LV_ALIGN_TOP_MID, 0, 35);
    for (uintptr_t i = 0; i < 3; ++i) {
        s_conversation_rows[i] = MenuButton(s_scr_conversations, kPad, 60 + static_cast<int>(i) * 35,
                kUiW - 2 * kPad - 54, 31, "-", OnConversationRow, reinterpret_cast<void*>(i));
        s_delete_rows[i] = MenuButton(s_scr_conversations, kUiW - kPad - 50,
                60 + static_cast<int>(i) * 35, 50, 31, "Del", OnConversationDelete,
                reinterpret_cast<void*>(i));
        lv_obj_add_state(lv_obj_get_parent(s_conversation_rows[i]), LV_STATE_DISABLED);
        lv_obj_add_state(lv_obj_get_parent(s_delete_rows[i]), LV_STATE_DISABLED);
    }
    MenuButton(s_scr_conversations, kPad, 170, 54, 28, "Back", OnConversationsBack);
    MenuButton(s_scr_conversations, kPad + 58, 170, 54, 28, "Prev", OnConversationPrev);
    MenuButton(s_scr_conversations, kPad + 116, 170, 54, 28, "Next", OnConversationNext);
    MenuButton(s_scr_conversations, kPad + 174, 170, 54, 28, "New", OnConversationNew);
    BuildPill(s_scr_conversations);
}

void OnServoIcon(lv_event_t* /*e*/) {
    s_want.store(Mode::kServo, std::memory_order_release);
}

void ShowServoAngle(int16_t value) {
    if (!s_servo_angle_label) return;
    char caption[12];
    snprintf(caption, sizeof caption, "%d", value);
    lv_label_set_text(s_servo_angle_label, caption);
}

uint8_t ServoTarget() {
    return s_servo_channel == 9 ? KorvoCloudServo8::kAll : s_servo_channel;
}

void UpdateServoControls() {
    const auto& state = s_servo_states[s_servo_channel - 1];
    const bool mixed = s_servo_channel == 9 && s_servo_all_mixed;
    lv_label_set_text(s_servo_mode_label, mixed ? "Mode" : state.mode == ServoMode::Continuous ? "Speed" :
        state.mode == ServoMode::Position360 ? "360 deg" :
        state.mode == ServoMode::Released ? "Released" : "180 deg");
    lv_label_set_text(s_servo_unit, state.mode == ServoMode::Continuous ? "% speed" : "deg");
    lv_arc_set_range(s_servo_arc, state.mode == ServoMode::Continuous ? -100 : 0,
                    state.mode == ServoMode::Continuous ? 100 :
                    state.mode == ServoMode::Position360 ? 360 : 180);
    lv_arc_set_value(s_servo_arc, state.value);
    ShowServoAngle(state.value);
    if (!s_servo_request || mixed || state.mode == ServoMode::Released)
        lv_obj_add_state(s_servo_arc, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_servo_arc, LV_STATE_DISABLED);
    if (!s_servo_request || mixed || state.mode != ServoMode::Continuous)
        lv_obj_add_state(s_servo_stop, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_servo_stop, LV_STATE_DISABLED);
}

bool SendServo(const ServoCommand& command) {
    if (!s_servo_request || !s_servo_request(command, s_servo_ctx)) {
        lv_label_set_text(s_servo_status, "Queue busy; retry");
        lv_obj_set_style_text_color(s_servo_status, lv_color_hex(kColErr), 0);
        return false;
    }
    lv_label_set_text(s_servo_status, command.read_only ? "Reading..." : "Sending...");
    lv_obj_set_style_text_color(s_servo_status, lv_color_hex(kColMuted), 0);
    return true;
}

void RequestServoAngle(int16_t value, bool force) {
    const int64_t now = esp_timer_get_time();
    if (!force && s_servo_last_send_us && now - s_servo_last_send_us < 40000) return;
    if (SendServo({ServoTarget(), s_servo_states[s_servo_channel - 1].mode, value}))
        s_servo_last_send_us = now;
}

void SelectServoChannel(uint8_t channel) {
    if (channel == s_servo_channel) return;
    s_servo_channel = channel;
    char caption[8];
    if (channel == 9) snprintf(caption, sizeof caption, "ALL");
    else snprintf(caption, sizeof caption, "SS%u", channel);
    lv_label_set_text(s_servo_select_label, caption);
    UpdateServoControls();
    lv_label_set_text(s_servo_status, channel == 9 ?
        (s_servo_all_mixed ? "Mixed; pick mode" : "ALL: same mode") : "Turn dial to move");
    lv_obj_set_style_text_color(s_servo_status, lv_color_hex(kColMuted), 0);
}

void OnServoMode(lv_event_t*) {
    auto& state = s_servo_states[s_servo_channel - 1];
    state.mode = (s_servo_channel == 9 && s_servo_all_mixed) ||
        state.mode == ServoMode::Released || state.mode == ServoMode::Continuous ?
        ServoMode::Position180 : static_cast<ServoMode>(static_cast<unsigned>(state.mode) + 1);
    if (s_servo_channel == 9) s_servo_all_mixed = false;
    state.value = state.mode == ServoMode::Continuous ? 0 : 90;
    UpdateServoControls();
    lv_label_set_text(s_servo_status, "Mode selected; turn dial");
}

void OnServoStop(lv_event_t*) {
    // Neutral pulses stop a continuous servo; release disables its pulses.
    if (SendServo({ServoTarget(), ServoMode::Continuous, 0})) {
        s_servo_states[s_servo_channel - 1] = {ServoMode::Continuous, 0};
        UpdateServoControls();
    }
}

void OnServoRelease(lv_event_t*) {
    if (SendServo({ServoTarget(), ServoMode::Released, 0})) {
        s_servo_states[s_servo_channel - 1] = {ServoMode::Released, 0};
        UpdateServoControls();
    }
}

void OnServoRead(lv_event_t*) {
    SendServo({ServoTarget(), ServoMode::Released, 0, true});
}

void RefreshServoPicker() {
    for (int i = 0; i < 5; ++i) {
        const int channel = static_cast<int>(s_servo_picker_channel) + i - 2;
        char caption[8] = {};
        if (channel == 9) snprintf(caption, sizeof caption, "ALL");
        else if (channel >= 1 && channel <= 8) snprintf(caption, sizeof caption, "SS%d", channel);
        lv_label_set_text(s_servo_picker_rows[i], caption);
    }
}

void OnServoPicker(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        s_servo_picker_start_y = point.y;
        s_servo_picker_start_channel = s_servo_channel;
        s_servo_picker_channel = s_servo_channel;
        s_servo_picker_active = true;
        RefreshServoPicker();
        lv_obj_move_to_index(s_servo_picker, -1);
        lv_obj_remove_flag(s_servo_picker, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_PRESSING && s_servo_picker_active) {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        // Sliding down walks SS1 -> SS8; the highlighted choice stays centered.
        int channel = static_cast<int>(s_servo_picker_start_channel) +
                      (point.y - s_servo_picker_start_y) / 16;
        if (channel < 1) channel = 1;
        if (channel > 9) channel = 9;
        if (channel != s_servo_picker_channel) {
            s_servo_picker_channel = static_cast<uint8_t>(channel);
            RefreshServoPicker();
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
               s_servo_picker_active) {
        s_servo_picker_active = false;
        lv_obj_add_flag(s_servo_picker, LV_OBJ_FLAG_HIDDEN);
        if (code == LV_EVENT_RELEASED) SelectServoChannel(s_servo_picker_channel);
    }
}

void OnServoArc(lv_event_t* e) {
    const int16_t angle = lv_arc_get_value(s_servo_arc);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        s_servo_states[s_servo_channel - 1].value = angle;
        ShowServoAngle(angle);
        RequestServoAngle(angle, false);
    } else {
        RequestServoAngle(angle, true);
    }
}

void BuildServo() {
    s_scr_servo = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_scr_servo);
    lv_obj_set_style_bg_color(s_scr_servo, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(s_scr_servo, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_servo, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = Label(s_scr_servo, "Servo control", kColServo, &lv_font_montserrat_20);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    s_servo_select_label = MenuButton(s_scr_servo, kPad, 42, 72, 34,
                                      "SS1", nullptr);
    s_servo_mode_label = MenuButton(s_scr_servo, 94, 42, 86, 34, "180 deg", OnServoMode);
    MenuButton(s_scr_servo, 188, 42, 76, 34, "Read", OnServoRead);
    s_servo_stop = lv_obj_get_parent(MenuButton(s_scr_servo, kPad, 84, 94, 34, "Stop", OnServoStop));
    lv_obj_add_state(s_servo_stop, LV_STATE_DISABLED);
    MenuButton(s_scr_servo, kPad, 126, 94, 34, "Release", OnServoRelease);
    lv_obj_t* select_button = lv_obj_get_parent(s_servo_select_label);
    lv_obj_add_flag(select_button, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(select_button, OnServoPicker, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(select_button, OnServoPicker, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(select_button, OnServoPicker, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(select_button, OnServoPicker, LV_EVENT_PRESS_LOST, nullptr);

    s_servo_picker = Plain(s_scr_servo);
    lv_obj_set_size(s_servo_picker, 84, 124);
    lv_obj_set_pos(s_servo_picker, kPad - 1, 62);
    lv_obj_set_style_radius(s_servo_picker, 12, 0);
    lv_obj_set_style_bg_color(s_servo_picker, lv_color_hex(kColCard), 0);
    lv_obj_set_style_bg_opa(s_servo_picker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_servo_picker, 1, 0);
    lv_obj_set_style_border_color(s_servo_picker, lv_color_hex(kColServo), 0);
    lv_obj_t* picked = Plain(s_servo_picker);
    lv_obj_set_size(picked, 76, 24);
    lv_obj_align(picked, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(picked, 8, 0);
    lv_obj_set_style_bg_color(picked, lv_color_hex(kColServo), 0);
    lv_obj_set_style_bg_opa(picked, LV_OPA_COVER, 0);
    for (int i = 0; i < 5; ++i) {
        s_servo_picker_rows[i] = Label(s_servo_picker, "", i == 2 ? kColBg : kColText,
                                       &lv_font_montserrat_14);
        lv_obj_set_width(s_servo_picker_rows[i], 76);
        lv_obj_set_style_text_align(s_servo_picker_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_servo_picker_rows[i], LV_ALIGN_CENTER, 0, (i - 2) * 24);
    }
    lv_obj_add_flag(s_servo_picker, LV_OBJ_FLAG_HIDDEN);

    s_servo_arc = lv_arc_create(s_scr_servo);
    lv_obj_set_size(s_servo_arc, 116, 116);
    lv_obj_set_pos(s_servo_arc, 136, 82);
    lv_arc_set_range(s_servo_arc, 0, 180);
    lv_arc_set_rotation(s_servo_arc, 135);
    lv_arc_set_bg_angles(s_servo_arc, 0, 270);
    lv_arc_set_change_rate(s_servo_arc, 1440);
    lv_arc_set_value(s_servo_arc, s_servo_states[0].value);
    lv_obj_set_style_arc_color(s_servo_arc, lv_color_hex(kColCard), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_servo_arc, lv_color_hex(kColServo), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_servo_arc, lv_color_hex(kColText), LV_PART_KNOB);
    lv_obj_add_event_cb(s_servo_arc, OnServoArc, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_servo_arc, OnServoArc, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_servo_arc, OnServoArc, LV_EVENT_PRESS_LOST, nullptr);

    s_servo_angle_label = Label(s_scr_servo, "90", kColText, &lv_font_montserrat_28);
    lv_obj_align_to(s_servo_angle_label, s_servo_arc, LV_ALIGN_CENTER, 0, -10);
    s_servo_unit = Label(s_scr_servo, "deg", kColMuted, &lv_font_montserrat_14);
    lv_obj_align_to(s_servo_unit, s_servo_arc, LV_ALIGN_CENTER, 0, 18);

    s_servo_status = Label(s_scr_servo, s_servo_request ? "Hold SS & slide" : "Servo unavailable",
                           s_servo_request ? kColMuted : kColErr, &lv_font_montserrat_14);
    lv_obj_set_size(s_servo_status, 104, 38);
    lv_label_set_long_mode(s_servo_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_servo_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_servo_status, kPad - 1, 164);

    if (!s_servo_request) {
        lv_obj_add_state(lv_obj_get_parent(s_servo_select_label), LV_STATE_DISABLED);
        lv_obj_add_state(s_servo_arc, LV_STATE_DISABLED);
    }

    lv_obj_t* back = Label(s_scr_servo, "Swipe up to return", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -28);
    if (!s_servo_request) {
        const uint32_t count = lv_obj_get_child_count(s_scr_servo);
        for (uint32_t i = 0; i < count; ++i)
            lv_obj_add_state(lv_obj_get_child(s_scr_servo, i), LV_STATE_DISABLED);
    }
    BuildPill(s_scr_servo);
}

void RefreshServoResult() {
    if (!s_servo_status) return;
    ServoCommand command;
    ServoStatus status;
    esp_err_t err, read_error;
    taskENTER_CRITICAL(&s_servo_result_lock);
    command = s_servo_result_command;
    status = s_servo_result_status;
    err = s_servo_result_error;
    read_error = s_servo_read_error;
    taskEXIT_CRITICAL(&s_servo_result_lock);
    if (read_error == ESP_OK && !lv_obj_has_state(s_servo_arc, LV_STATE_PRESSED)) {
        for (unsigned i = 0; i < 8; ++i) s_servo_states[i] = status.channels[i];
        bool same = true;
        for (unsigned i = 1; i < 8; ++i)
            same &= status.channels[i].mode == status.channels[0].mode &&
                    status.channels[i].value == status.channels[0].value;
        if (same) s_servo_states[8] = status.channels[0];
        s_servo_all_mixed = !same;
        UpdateServoControls();
    }
    const bool relevant = command.read_only || command.channel == ServoTarget() ||
        command.channel == KorvoCloudServo8::kAll;
    if (!relevant) return;
    lv_label_set_text(s_servo_status, err != ESP_OK ? "I2C write failed" :
        read_error != ESP_OK ? (command.read_only ? "Read failed" : "Sent; read failed") :
        (s_servo_channel == 9 && s_servo_all_mixed ? "Mixed; pick mode" : "Board state read"));
    lv_obj_set_style_text_color(s_servo_status,
        lv_color_hex(err == ESP_OK && read_error == ESP_OK ? kColOk : kColErr), 0);
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
    lv_obj_set_size(icon, 68, 68);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, -88, 64);
    lv_obj_set_style_radius(icon, 20, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(kColCam), 0);
    lv_obj_set_style_bg_grad_color(icon, lv_color_hex(kColCam2), 0);
    lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(icon, OnCameraIcon, LV_EVENT_CLICKED, nullptr);
    DrawCameraGlyph(icon);

    lv_obj_t* cap = Label(s_scr_apps, "Camera", kColText, &lv_font_montserrat_14);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, -88, 143);

    lv_obj_t* hint = Label(s_scr_apps, "preview", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, -88, 166);

    lv_obj_t* voice = Plain(s_scr_apps);
    lv_obj_set_size(voice, 68, 68);
    lv_obj_align(voice, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_radius(voice, 20, 0);
    lv_obj_set_style_bg_color(voice, lv_color_hex(kColVoice), 0);
    lv_obj_set_style_bg_opa(voice, LV_OPA_COVER, 0);
    lv_obj_add_flag(voice, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(voice, OnVoiceIcon, LV_EVENT_CLICKED, nullptr);
    DrawVoiceGlyph(voice);
    lv_obj_t* voice_cap = Label(s_scr_apps, "Voice", kColText, &lv_font_montserrat_14);
    lv_obj_align(voice_cap, LV_ALIGN_TOP_MID, 0, 143);
    s_voice_hint = Label(s_scr_apps, "request", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(s_voice_hint, LV_ALIGN_TOP_MID, 0, 166);

    lv_obj_t* servo = Plain(s_scr_apps);
    lv_obj_set_size(servo, 68, 68);
    lv_obj_align(servo, LV_ALIGN_TOP_MID, 88, 64);
    lv_obj_set_style_radius(servo, 20, 0);
    lv_obj_set_style_bg_color(servo, lv_color_hex(kColServo), 0);
    lv_obj_set_style_bg_opa(servo, LV_OPA_COVER, 0);
    lv_obj_add_flag(servo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(servo, OnServoIcon, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* angle = Label(servo, "90", kColBg, &lv_font_montserrat_28);
    lv_obj_center(angle);
    lv_obj_t* servo_cap = Label(s_scr_apps, "Servo", kColText, &lv_font_montserrat_14);
    lv_obj_align(servo_cap, LV_ALIGN_TOP_MID, 88, 143);
    lv_obj_t* servo_hint = Label(s_scr_apps, "SS1", kColMuted, &lv_font_montserrat_14);
    lv_obj_align(servo_hint, LV_ALIGN_TOP_MID, 88, 166);

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

    if (s_retry) {
        const bool ready = st.phase == AGENT_LINK_PHASE_READY;
        if (ready) {
            s_last_voice_request_us = 0;
            if (s_mode.load() == Mode::kVoice && s_audio) s_audio->StartContinuous();
            lv_obj_add_state(s_retry, LV_STATE_DISABLED);
        } else {
            if (strstr(st.hint, "Tap Connect to retry")) s_last_voice_request_us = 0;
            if (s_audio) { s_audio->StopContinuous(); s_audio->StopAsr(); }
            if (s_last_voice_request_us || s_voice_autoconnect_pending)
                lv_obj_add_state(s_retry, LV_STATE_DISABLED);
            else
                lv_obj_remove_state(s_retry, LV_STATE_DISABLED);
        }
        if (s_selected_agent <= 0 || !s_selected_session[0])
            lv_label_set_text(s_call_status, "Select a session in Sessions");
        else if (s_mode.load() == Mode::kVoice && s_voice_autoconnect_pending)
            lv_label_set_text(s_call_status, "Waiting for platform...");
        else
            lv_label_set_text(s_call_status, st.hint[0] ? st.hint : st.title);
        lv_label_set_text(s_voice_hint, ready ? "live" : "tap");
    }

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
        if (st.detail != 0) snprintf(buf, sizeof buf, "Link, code %d", st.detail);
        else                snprintf(buf, sizeof buf, "Link status");
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
    if (from == Mode::kVoice) {
        s_voice_autoconnect_pending = false;
        if (s_audio) s_audio->StopContinuous();
        if (s_audio) { s_audio->StopAsr(); s_audio->StopPlayback(); }
        s_last_voice_request_us = 0;
        (void)agent_link_end_realtime();
    }

    if (want == Mode::kCamera) {
        // Stop LVGL reaching the panel first, then wipe it: the preview is 240 wide on a 280 wide
        // screen, and without this Home would stay visible in the bars either side.
        s_mode.store(Mode::kCamera, std::memory_order_release);
        Lock();
        if (s_lcd) (void)s_lcd->FillSolid(rgb565::kBlack);
        Unlock();
        ESP_LOGD(TAG, "camera open");
        return;
    }

    lv_obj_t* scr = want == Mode::kShade ? s_scr_shade
                  : want == Mode::kApps  ? s_scr_apps
                  : want == Mode::kServo ? s_scr_servo
                  : want == Mode::kVoice ? s_scr_voice
                  : want == Mode::kConversations ? s_scr_conversations
                                         : s_scr_home;
    if (want == Mode::kServo && s_servo_request)
        SendServo({ServoTarget(), ServoMode::Released, 0, true});
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
    if (want == Mode::kVoice) {
        s_voice_auto_attempts = 0;
        s_voice_autoconnect_pending = true;
        if (s_audio) s_audio->StopAsr();
        ShowSelection();
    }
    RefreshStatus();
    if (want == Mode::kVoice) TryConnectVoice();
    RefreshLink();
    RefreshDots();
    lv_obj_invalidate(lv_screen_active());
    ESP_LOGI(TAG, "screen -> %s", want == Mode::kShade ? "shade"
                                : want == Mode::kApps  ? "apps"
                                : want == Mode::kServo ? "servo"
                                : want == Mode::kVoice ? "voice"
                                : want == Mode::kConversations ? "sessions" : "home");
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
    ESP_LOGD(TAG, "swipe up from the bottom -> %s", back == Mode::kApps ? "apps" : "home");
    s_want.store(back, std::memory_order_release);
}

void OnSwipeDownFromTop() {
    if (s_mode.load(std::memory_order_acquire) == Mode::kShade) return;
    ESP_LOGD(TAG, "swipe down from the top -> shade");
    s_want.store(Mode::kShade, std::memory_order_release);
}

// Turn a page
void OnSwipeHorizontal(int dx) {
    const int idx = PageIndexOf(s_mode.load(std::memory_order_acquire));
    if (idx < 0) return;   // Camera and Shade are not pages; leave them with the bottom-edge swipe

    const int next = dx < 0 ? (idx + 1) % kPageCount
                            : (idx + kPageCount - 1) % kPageCount;
    ESP_LOGD(TAG, "swipe %s -> page %d", dx < 0 ? "left" : "right", next);
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

    // Keep a drag on the dial with LVGL. Only the top/bottom edge gestures own a
    // press on this screen; the ordinary page-swipe recognizer would cancel it.
    if (s_mode.load(std::memory_order_acquire) == Mode::kServo &&
        !s_sw.from_top && !s_sw.from_bot) return;

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
    uint32_t seen_platform = 0;
    uint32_t seen_servo = 0;
    while (true) {
        (void)SyncBoundAgent();
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

        const uint32_t platform_rev = s_platform_rev.load(std::memory_order_acquire);
        if (platform_rev != seen_platform) { seen_platform = platform_rev; ApplyPlatformResult(); }
        const uint32_t servo_rev = s_servo_result_rev.load(std::memory_order_acquire);
        if (servo_rev != seen_servo) { seen_servo = servo_rev; RefreshServoResult(); }
        if (s_delete_confirm_row >= 0 && esp_timer_get_time() > s_delete_confirm_until_us) {
            ResetDeleteConfirmation();
            if (s_mode.load() == Mode::kConversations) ShowConversationRows();
        }

        if (s_voice_autoconnect_pending) TryConnectVoice();
        if (s_mode.load() == Mode::kVoice && agent_link_state() == AGENT_STATE_READY &&
            s_audio && !s_audio->CommandActive() &&
            esp_timer_get_time() - s_last_mic_retry_us >= 1000000) {
            s_last_mic_retry_us = esp_timer_get_time();
            s_audio->StartContinuous();
        }

        if (s_last_voice_request_us && esp_timer_get_time() - s_last_voice_request_us > 30000000) {
            s_last_voice_request_us = 0;
            (void)agent_link_end_realtime();
            lv_label_set_text(s_call_status, "Voice timed out - tap Retry");
        }
        uint32_t next = lv_timer_handler();  // renders + flushes only if something changed
        if (next == LV_NO_TIMER_READY || next > 30) next = 30;
        const TickType_t delay = pdMS_TO_TICKS(next);
        vTaskDelay(delay ? delay : 1);
    }
}

}  // namespace

esp_err_t Start(St7789Lcd* lcd, Cst816Touch* touch, KorvoCloudAudio* audio,
                ServoRequest servo_request, void* servo_ctx) {
    if (!lcd || !lcd->Ready()) return ESP_ERR_INVALID_STATE;
    s_lcd   = lcd;
    s_touch = (touch && touch->Ready()) ? touch : nullptr;
    s_audio = (audio && audio->Ready()) ? audio : nullptr;
    s_servo_request = servo_request;
    s_servo_ctx = servo_ctx;

    s_lcd_mux = xSemaphoreCreateMutex();
    if (!s_lcd_mux) return ESP_ERR_NO_MEM;
    s_platform_mutex = xSemaphoreCreateMutex();
    s_platform_commands = xQueueCreateWithCaps(4, sizeof(PlatformCommand),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_platform_commands) s_platform_commands = xQueueCreate(4, sizeof(PlatformCommand));
    if (!s_platform_mutex || !s_platform_commands) return ESP_ERR_NO_MEM;

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
    BuildServo();
    BuildVoice();
    BuildConversations();
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
    if (xTaskCreate(PlatformWorker, "kc_platform", 12288, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "platform worker create failed");
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

void SetServoResult(const KorvoCloudServo8::Command& command, esp_err_t result,
                    const KorvoCloudServo8::Status* status, esp_err_t read_result) {
    taskENTER_CRITICAL(&s_servo_result_lock);
    s_servo_result_command = command;
    if (status) s_servo_result_status = *status;
    s_servo_result_error = result;
    s_servo_read_error = read_result;
    taskEXIT_CRITICAL(&s_servo_result_lock);
    s_servo_result_rev.fetch_add(1, std::memory_order_release);
}

void SetVoiceText(const char* utf8) {
    (void)utf8;
}

}  // namespace kc_ui
