#include "voice_ui.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stripe_canvas.h"

namespace voice {
namespace {

constexpr const char* TAG = "passport.ui";
constexpr int kWidth = DISPLAY_WIDTH, kHeight = DISPLAY_HEIGHT, kStripeRows = 4;
constexpr uint32_t kBg = 0x0E1118, kPanel = 0x161B26, kText = 0xF2F5FA, kMuted = 0x8A93A6;
constexpr uint32_t kAccent = 0x2FA8FF, kOk = 0x35D07F, kWarn = 0xFFB020, kErr = 0xFF5C5C;
constexpr uint32_t kListen = 0xFF4F8B, kToast = 0x1C2333;
constexpr int64_t kAnswerHoldUs = 120LL * 1000 * 1000;
constexpr const char* kAudioIcon = "\xef\x80\xa8";
constexpr char kEllipsis[] = "...";
enum class Mode { Link, Idle, Listening, Thinking, Speaking, Answer };

struct Input {
    agent_link_status_t status = {};
    char answer[768] = {};
    char toast[96] = {};
    int64_t answer_at = 0;
    uint32_t toast_ms = 0;
    uint32_t status_revision = 1, answer_revision = 0, toast_revision = 0, session_revision = 1;
    uint32_t token = 0;
};

St7789Lcd* s_panel = nullptr;
TaskHandle_t s_task = nullptr;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
Input s_input, s_frame;
std::atomic<uint32_t> s_rendered_token{0};
std::atomic<int> s_level_pct{0};
raster::TextLayout s_answer_layout, s_title_layout, s_hint_layout;
Mode s_mode = Mode::Link;
int s_dots = 0, s_level = 0, s_scroll = 0;
int64_t s_toast_until = 0;
bool s_panel_failed = false;

uint32_t PhaseColor(agent_link_phase_t phase) {
    switch (phase) {
    case AGENT_LINK_PHASE_READY:
    case AGENT_LINK_PHASE_CONNECTED: return kOk;
    case AGENT_LINK_PHASE_BLOCKED: return kErr;
    case AGENT_LINK_PHASE_SETUP:
    case AGENT_LINK_PHASE_PAIRING: return kAccent;
    default: return kWarn;
    }
}

const char* PhaseCaption(agent_link_phase_t phase) {
    switch (phase) {
    case AGENT_LINK_PHASE_SETUP: return "Setup";
    case AGENT_LINK_PHASE_PAIRING: return "Pairing";
    case AGENT_LINK_PHASE_BLOCKED: return "Action needed";
    case AGENT_LINK_PHASE_CONNECTED: return "Connected";
    case AGENT_LINK_PHASE_READY: return "Online";
    default: return "Connecting";
    }
}

Mode CurrentMode(int64_t now) {
    if (s_frame.status.phase != AGENT_LINK_PHASE_READY || TokenState(s_frame.token) == SessionState::Disconnected) return Mode::Link;
    switch (TokenState(s_frame.token)) {
    case SessionState::Listening: return Mode::Listening;
    case SessionState::Thinking: return Mode::Thinking;
    case SessionState::Speaking: return Mode::Speaking;
    default: return s_frame.answer_at && now - s_frame.answer_at < kAnswerHoldUs ? Mode::Answer : Mode::Idle;
    }
}

void Label(raster::Canvas& canvas, const raster_font_t& font, int x, int y,
           const char* text, uint32_t color, int width = 0, bool centered = false) {
    const char* const limit = text + strlen(text);
    const char* end = limit;
    bool ellipsis = false;
    if (width > 0 && raster::TextWidth(font, text, end) > width) {
        ellipsis = true;
        const int dots = raster::TextWidth(font, kEllipsis, kEllipsis + 3);
        const char* p = text;
        end = text;
        while (p < limit) {
            raster::NextCodepoint(p, limit);
            if (raster::TextWidth(font, text, p) + dots > width) break;
            end = p;
        }
    }
    const int measured = raster::TextWidth(font, text, end);
    const int dots_width = ellipsis ? raster::TextWidth(font, kEllipsis, kEllipsis + 3) : 0;
    if (centered) x += (width - measured - dots_width) / 2;
    canvas.Text(font, x, y, text, end, color);
    if (ellipsis) canvas.Text(font, x + measured, y, kEllipsis, kEllipsis + 3, color);
}

void Wrapped(raster::Canvas& canvas, const raster_font_t& font, const raster::TextLayout& layout,
             const char* text, int x, int y, int spacing, uint32_t color) {
    for (unsigned i = 0; i < layout.Count(); ++i) {
        canvas.Text(font, x, y + static_cast<int>(i) * (font.line_height + spacing),
                    text + layout.Start(i), text + layout.End(i), color);
    }
}

const char* LinkTitle() {
    return s_frame.status.phase == AGENT_LINK_PHASE_PAIRING && s_frame.status.code[0] ?
           s_frame.status.code : s_frame.status.title;
}

void Paint(raster::Canvas& canvas) {
    canvas.Fill(kBg);
    canvas.Box({8, 48, 224, 236}, kPanel, 14);
    Label(canvas, raster_montserrat_14, 16, 8, "AI PASSPORT", kText);
    canvas.Box({16, 31, 6, 6}, PhaseColor(s_frame.status.phase), 3);
    const char* caption = s_mode == Mode::Listening ? "Listening" : s_mode == Mode::Thinking ? "Thinking" :
                          s_mode == Mode::Speaking ? "Speaking" : s_mode == Mode::Link ? PhaseCaption(s_frame.status.phase) : "Ready";
    Label(canvas, raster_montserrat_14, 28, 26, caption, kMuted, 196);
    canvas.Clip({24, 64, 192, 204});
    switch (s_mode) {
    case Mode::Link:
        Label(canvas, raster_montserrat_14, 24, 66, "LET'S CONNECT", kAccent);
        Wrapped(canvas, raster_montserrat_28, s_title_layout, LinkTitle(), 24, 96, 0, kText);
        Wrapped(canvas, raster_montserrat_14, s_hint_layout, s_frame.status.hint,
                24, 112 + static_cast<int>(s_title_layout.Count()) * raster_montserrat_28.line_height, 4, kMuted);
        break;
    case Mode::Idle:
        Label(canvas, raster_montserrat_28, 24, 78, kAudioIcon, kOk, 192, true);
        Label(canvas, raster_montserrat_28, 24, 126, s_frame.status.title[0] ? s_frame.status.title : "Ready", kText, 192, true);
        Label(canvas, raster_montserrat_20, 24, 176, "Hold OK to talk", kMuted, 192, true);
        Label(canvas, raster_montserrat_14, 24, 206, "release to send", kMuted, 192, true);
        break;
    case Mode::Listening:
        Label(canvas, raster_montserrat_28, 24, 118, "Listening", kListen, 192, true);
        canvas.Box({24, 166, 192, 14}, 0x262B38, 7);
        canvas.Box({24, 166, 192 * s_level / 100, 14}, kListen, 7);
        Label(canvas, raster_montserrat_14, 24, 186, "Release to send", kMuted, 192, true);
        break;
    case Mode::Thinking: {
        static const char* const dots[] = {"Thinking", "Thinking.", "Thinking..", "Thinking..."};
        Label(canvas, raster_montserrat_28, 24, 132, dots[s_dots], kAccent, 192, true);
        Label(canvas, raster_montserrat_14, 24, 178, "Waiting for your agent", kMuted, 192, true);
        break;
    }
    case Mode::Speaking:
        Label(canvas, raster_montserrat_28, 24, 132, "Speaking", kOk, 192, true);
        Label(canvas, raster_montserrat_14, 24, 178, "Your agent is replying", kMuted, 192, true);
        break;
    case Mode::Answer:
        Wrapped(canvas, raster_montserrat_20, s_answer_layout, s_frame.answer, 24, 64 - s_scroll, 5, kText);
        break;
    }
    canvas.Clip({0, 284, kWidth, 36});
    if (s_toast_until) {
        canvas.Box({16, 291, 208, 22}, kToast, 6);
        Label(canvas, raster_montserrat_14, 24, 294, s_frame.toast, kText, 192, true);
    } else {
        static const char* const footers[] = {"Open the app to connect", "UP / DOWN: volume", "Release OK to send",
                                             "Hold OK to ask again", "Hold OK to interrupt", "Hold OK to ask again"};
        Label(canvas, raster_montserrat_14, 16, 294, footers[static_cast<unsigned>(s_mode)], kMuted, 208, true);
    }
}

bool DrawRows(int top, int bottom, uint32_t revision) {
    if (s_panel_failed) return false;
    top = std::max(0, top / kStripeRows * kStripeRows);
    bottom = std::min(kHeight, (bottom + kStripeRows - 1) / kStripeRows * kStripeRows);
    for (int y = top; y < bottom; y += kStripeRows) {
        taskENTER_CRITICAL(&s_lock);
        const bool stale = s_input.session_revision != revision;
        taskEXIT_CRITICAL(&s_lock);
        if (stale) return false;
        raster::Canvas canvas(s_panel->StripeBuffer(), kWidth, y, kStripeRows);
        Paint(canvas);
        if (s_panel->FlushStripe(static_cast<uint16_t>(y), kStripeRows) != ESP_OK) {
            s_panel_failed = true;
            ESP_LOGE(TAG, "panel flush failed; playback gate stays closed");
            s_rendered_token.store(0, std::memory_order_release);
            return false;
        }
        if ((y / kStripeRows & 7) == 7) vTaskDelay(1);
    }
    return true;
}

void RenderTask(void*) {
    uint32_t seen_status = 0, seen_answer = 0, seen_toast = 0, seen_session = 0, seen_token = 0;
    int64_t dots_at = 0;
    bool redraw = true;
    while (true) {
        const int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_lock);
        s_frame = s_input;
        taskEXIT_CRITICAL(&s_lock);
        const bool status_changed = seen_status != s_frame.status_revision;
        const bool answer_changed = seen_answer != s_frame.answer_revision;
        const bool session_changed = seen_session != s_frame.session_revision;
        const bool toast_changed = seen_toast != s_frame.toast_revision;
        if (status_changed) {
            s_title_layout.Wrap(raster_montserrat_28, LinkTitle(), 192);
            s_hint_layout.Wrap(raster_montserrat_14, s_frame.status.hint, 192);
            seen_status = s_frame.status_revision;
        }
        if (answer_changed) {
            s_answer_layout.Wrap(raster_montserrat_20, s_frame.answer, 192);
            seen_answer = s_frame.answer_revision;
        }
        const Mode mode = CurrentMode(now);
        const Mode previous_mode = s_mode;
        const bool mode_changed = mode != s_mode;
        s_mode = mode;
        const int level = s_level_pct.load(std::memory_order_acquire);
        const bool level_changed = level != s_level;
        s_level = level;
        bool dots_changed = false;
        if (s_mode == Mode::Thinking && now - dots_at >= 400000) {
            dots_at = now;
            s_dots = (s_dots + 1) % 4;
            dots_changed = true;
        }
        const int overflow = std::max(0, static_cast<int>(s_answer_layout.Count()) * (raster_montserrat_20.line_height + 5) - 204);
        const int scroll = s_frame.answer_at ? static_cast<int>(std::min<int64_t>(overflow,
                           std::max<int64_t>(0, now - s_frame.answer_at - 2000000) * 22 / 1000000)) : 0;
        const bool scroll_changed = scroll != s_scroll;
        s_scroll = scroll;
        bool footer_changed = toast_changed;
        if (toast_changed) {
            seen_toast = s_frame.toast_revision;
            s_toast_until = s_frame.toast_ms ? now + static_cast<int64_t>(s_frame.toast_ms) * 1000 : 0;
        } else if (s_toast_until && now >= s_toast_until) {
            s_toast_until = 0;
            footer_changed = true;
        }
        bool drawn = true;
        if (!redraw && !status_changed && session_changed && previous_mode == Mode::Thinking &&
            mode == Mode::Speaking && TokenGeneration(seen_token) == TokenGeneration(s_frame.token)) {
            drawn = DrawRows(24, 44, s_frame.session_revision) &&
                    DrawRows(132, 196, s_frame.session_revision) &&
                    DrawRows(284, kHeight, s_frame.session_revision);
        } else if (redraw || status_changed || session_changed || mode_changed) {
            drawn = DrawRows(0, kHeight, s_frame.session_revision);
        } else {
            if (s_mode == Mode::Answer && (answer_changed || scroll_changed)) drawn = DrawRows(48, 284, s_frame.session_revision);
            if (drawn && s_mode == Mode::Listening && level_changed) drawn = DrawRows(164, 180, s_frame.session_revision);
            if (drawn && s_mode == Mode::Thinking && dots_changed) drawn = DrawRows(132, 164, s_frame.session_revision);
            if (drawn && footer_changed) drawn = DrawRows(284, kHeight, s_frame.session_revision);
        }
        redraw = !drawn;
        if (drawn && session_changed) {
            taskENTER_CRITICAL(&s_lock);
            if (s_input.session_revision == s_frame.session_revision &&
                (TokenState(s_frame.token) != SessionState::Speaking || s_mode == Mode::Speaking)) {
                s_rendered_token.store(s_frame.token, std::memory_order_release);
                seen_session = s_frame.session_revision;
                seen_token = s_frame.token;
            }
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "session frame flushed: token=%u stack_remaining=%u", (unsigned)s_frame.token,
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

void Wake() { if (s_task) xTaskNotifyGive(s_task); }

}  // namespace

Ui& Ui::Instance() { static Ui instance; return instance; }

esp_err_t Ui::Start(St7789Lcd* panel) {
    if (!panel || !panel->Ready() || panel->Width() != kWidth || panel->Height() != kHeight ||
        panel->StripeCapacity() < static_cast<size_t>(kWidth * kStripeRows * 2)) return ESP_ERR_INVALID_STATE;
    s_panel = panel;
    if (xTaskCreate(RenderTask, "passport_ui", 3072, nullptr, 4, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "UI up: %dx%d, direct renderer, shared %u-byte DMA stripe, stack=3072",
             kWidth, kHeight, static_cast<unsigned>(panel->StripeCapacity()));
    return ESP_OK;
}

void Ui::SetStatus(const agent_link_status_t& st) {
    taskENTER_CRITICAL(&s_lock);
    s_input.status = st;
    ++s_input.status_revision;
    taskEXIT_CRITICAL(&s_lock);
    Wake();
}

void Ui::SetSession(uint32_t token) {
    taskENTER_CRITICAL(&s_lock);
    s_rendered_token.store(0, std::memory_order_release);
    s_input.token = token;
    ++s_input.session_revision;
    if (TokenState(token) == SessionState::Listening || TokenState(token) == SessionState::Disconnected) {
        s_input.answer_at = 0;
        s_level_pct.store(0, std::memory_order_release);
    }
    taskEXIT_CRITICAL(&s_lock);
    Wake();
}

bool Ui::SessionRendered(uint32_t token) const { return s_rendered_token.load(std::memory_order_acquire) == token; }

void Ui::SetLevel(int percent) { s_level_pct.store(std::max(0, std::min(100, percent)), std::memory_order_release); }

void Ui::SetAnswer(const char* utf8) {
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_input.answer, utf8 ? utf8 : "", sizeof s_input.answer);
    s_input.answer_at = utf8 && utf8[0] ? esp_timer_get_time() : 0;
    ++s_input.answer_revision;
    taskEXIT_CRITICAL(&s_lock);
    Wake();
}

void Ui::Toast(const char* text, uint32_t ms) {
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_input.toast, text ? text : "", sizeof s_input.toast);
    s_input.toast_ms = ms;
    ++s_input.toast_revision;
    taskEXIT_CRITICAL(&s_lock);
    Wake();
}

}  // namespace voice
