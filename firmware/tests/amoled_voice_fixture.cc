#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "amoled_voice_layout.h"
#include "amoled_voice_controls.h"
#include "amoled_pcm_diagnostics.h"
#include "cst9217_frame.h"
#include "voice_playback.h"

enum agent_state_t { AGENT_STATE_DISCONNECTED, AGENT_STATE_CONNECTED, AGENT_STATE_READY };
enum phase_t { AGENT_LINK_PHASE_IDLE, AGENT_LINK_PHASE_SETUP, AGENT_LINK_PHASE_PAIRING,
               AGENT_LINK_PHASE_BLOCKED, AGENT_LINK_PHASE_CONNECTED, AGENT_LINK_PHASE_READY,
               AGENT_LINK_PHASE_CONNECTING };
struct agent_link_status_t { phase_t phase; };
namespace amoled_voice {
struct Screen { Activity activity = Activity::kIdle; bool calling = false, busy = false, local_mode = false; };
void Toast(const char*, unsigned = 2500) {}
void Wake() {}
// INSERT_RESOLVE
}
using esp_err_t = int;
using amoled_voice::Tr;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = -2, ESP_ERR_INVALID_ARG = -3,
    ESP_ERR_NO_MEM = -4, ESP_ERR_INVALID_SIZE = -5;
agent_state_t fixture_state = AGENT_STATE_CONNECTED;
bool running = false;
int ended = 0, grants = 0;
int64_t now_us = 2000000;
agent_state_t agent_link_state() { return fixture_state; }
bool agent_link_realtime_running() { return running; }
int64_t esp_timer_get_time() { return now_us; }
int agent_link_end_realtime() { running = false; ++ended; return ESP_OK; }
int agent_link_request_realtime_session(int, const char*) { running = true; ++grants; return ESP_OK; }
struct Audio {
    bool fault = false, enabled = false;
    bool Fault() const { return fault; }
    void Stop() { enabled = false; }
    void SetContinuous(bool on) { enabled = on; }
};
struct Controller {
    Audio audio_;
    bool call_ = true, active_ = false, ended_ = false;
    int publications = 0;
    void Publish() { assert(!call_ && ended_ && !thinking_.load()); ++publications; }
    int64_t requested_at_ = 0, last_attempt_ = 0;
    int attempts_ = 0, agent_ = 10, errors = 0;
    char session_[80] = "test-conversation";
    std::atomic<bool> thinking_{false};
    bool Error(int) { ++errors; return true; }
    // INSERT_END_CALL
    // INSERT_TICK_CALL
};
enum class Page { kHome, kLauncher, kSettings };
#define ESP_LOGI(...) ((void)0)
constexpr const char* TAG = "test";
struct ButtonController : Controller {
    bool friends_open_ = false;
    Page page_ = Page::kHome;
    int starts = 0;
    void Clear(Page page) { page_ = page; }
    void CloseFriends(Page page) { friends_open_ = false; page_ = page; }
    void Select() { ++starts; call_ = true; }
    // INSERT_BUTTON
};

// Run the production microphone loop with a deterministic PCM source. Only the
// platform boundary is mocked, so capture/muting/publishing/error paths stay real.
constexpr int AUDIO_SAMPLE_RATE = 16000;
struct AmoledAudio { static constexpr size_t kCaptureCapacity = 320; };
constexpr int64_t kDiagnosticIntervalUs = 5000000;
using agent_stream_handle_t = void*;
enum agent_stream_t { AGENT_STREAM_VOICE, AGENT_STREAM_AUDIO };
struct agent_stream_opts_t { const char* name = nullptr; };
struct CaptureFinished {};
int capture_frames = 0, capture_limit = 12, capture_read_error_at = 0;
int capture_write_error_at = 0, capture_write_error = ESP_OK, written_frames = 0;
bool expect_muted = false;
std::atomic<bool>* capture_enabled = nullptr;
int agent_link_stream_open(agent_stream_t, const agent_stream_opts_t*, agent_stream_handle_t* stream) {
    *stream = reinterpret_cast<void*>(1); return ESP_OK;
}
int agent_link_stream_close(agent_stream_handle_t, bool) { return ESP_OK; }
int agent_link_stream_write(agent_stream_handle_t, const void* data, size_t bytes) {
    assert(bytes == 640); // PCM16 mono, 320 samples / 20 ms remains unchanged
    const auto* pcm = static_cast<const int16_t*>(data);
    for (size_t i = 0; i < bytes / 2; ++i) assert(pcm[i] == (expect_muted ? 0 : i % 2 ? 1000 : -1000));
    ++written_frames;
    return capture_frames == capture_write_error_at ? capture_write_error : ESP_OK;
}
unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
void vTaskDelay(unsigned) { throw CaptureFinished{}; }
unsigned uxTaskGetStackHighWaterMark(void* task) { return task ? 2048 : 4500; }
#define taskENTER_CRITICAL(...) ((void)0)
#define taskEXIT_CRITICAL(...) ((void)0)
struct CaptureCodec {
    std::array<kc_audio::PcmLevels, 2> channels;
    bool mic_on = false;
    bool MicOn() const { return mic_on; }
    int StartMic() { mic_on = true; return ESP_OK; }
    int StopMic() { mic_on = false; return ESP_OK; }
    int ReadPcm(int16_t* pcm, size_t frames, size_t* got) {
        assert(frames == 320);
        now_us += 20000; ++capture_frames;
        if (capture_frames == capture_read_error_at) { *got = 0; return ESP_FAIL; }
        for (size_t i = 0; i < frames; ++i) pcm[i] = i % 2 ? 1000 : -1000;
        for (auto& channel : channels) { channel = {}; channel.Add(pcm, frames); }
        *got = frames;
        if (capture_frames >= capture_limit) capture_enabled->store(false);
        return ESP_OK;
    }
    const auto& ChannelLevels() const { return channels; }
};
struct CaptureHardware { CaptureCodec codec_; };
struct CaptureHarness {
    CaptureHardware hw;
    CaptureHardware* hardware_ = &hw;
    std::atomic<int64_t> asr_until_{0};
    std::atomic<bool> asr_{false}, continuous_{true}, fault_{false};
    kc_audio::SpeakerEchoGuard echo_;
    amoled_voice::MicDiagnosticWindow mic_window_, diagnostic_snapshot_;
    uint32_t diagnostic_sequence_ = 0, diagnostic_capture_ = 0;
    void* play_task_ = reinterpret_cast<void*>(1);
    bool diagnostic_active_ = false;
    // INSERT_PCM_PUBLISH
    // INSERT_MIC_LOOP
    void Run() {
        capture_enabled = &continuous_; capture_frames = written_frames = 0;
        fixture_state = AGENT_STATE_READY;
        try { MicLoop(); } catch (const CaptureFinished&) {}
    }
};

int main() {
    using namespace amoled_voice;
    // Invalid and release reports cannot retain a stale pressed pointer.
    uint8_t frame[10] = {0x06, 14, 13, 0x92, 70, 1, 0xAB};
    cst9217::Point point;
    assert(cst9217::Decode(frame, sizeof frame, 466, 466, &point));
    assert(point.pressed && point.x == 233 && point.y == 210);
    frame[5] = 0;
    assert(cst9217::Decode(frame, sizeof frame, 466, 466, &point) && !point.pressed);
    frame[5] = 2;
    assert(cst9217::Decode(frame, sizeof frame, 466, 466, &point) && point.pressed);
    frame[5] = 3;
    assert(!cst9217::Decode(frame, sizeof frame, 466, 466, &point) && !point.pressed);
    frame[5] = 1; frame[1] = 29; frame[3] = 0x22;  // x=466 is outside
    assert(!cst9217::Decode(frame, sizeof frame, 466, 466, &point) && !point.pressed);
    frame[1] = 14; frame[3] = 0x92; frame[6] = 0;
    assert(!cst9217::Decode(frame, sizeof frame, 466, 466, &point));
    frame[6] = 0xAB;
    assert(!cst9217::Decode(frame, 6, 466, 466, &point));
    frame[0] = 0;
    assert(cst9217::Decode(frame, sizeof frame, 466, 466, &point) && !point.pressed);
    assert(!cst9217::Decode(nullptr, 0, 466, 466, &point));

    assert(kRingInset > 0 && kRingInset * 2 + kRingDiameter == kDisplaySize);
    assert(kRingDiameter / 2 - kRingWidth - kContentRadius >= 8);
    assert(!Inside(0, 0) && !Inside(465, 465));
    assert(Inside(233, 233) && LayoutFits());
    for (int i = 0; i < 8; ++i) for (int j = i + 1; j < 8; ++j)
        assert(ActivityColor(static_cast<Activity>(i)) != ActivityColor(static_cast<Activity>(j)));
    Screen screen; agent_link_status_t status{AGENT_LINK_PHASE_CONNECTED};
    assert(Resolve(screen, status) == Activity::kIdle);
    screen.calling = true; screen.activity = Activity::kSpeaking;
    assert(Resolve(screen, status) == Activity::kSpeaking);
    status.phase = AGENT_LINK_PHASE_BLOCKED;
    assert(Resolve(screen, status) == Activity::kError);
    status.phase = AGENT_LINK_PHASE_PAIRING;
    assert(Resolve(screen, status) == Activity::kSetup);
    screen.activity = Activity::kError;
    assert(Resolve(screen, status) == Activity::kError);

    Controller call;
    call.TickCall();
    assert(grants == 1 && call.call_ && !call.audio_.enabled); // no mic before ready
    fixture_state = AGENT_STATE_READY; call.TickCall();
    assert(call.active_ && call.audio_.enabled);
    fixture_state = AGENT_STATE_CONNECTED; now_us += 60000000;
    call.TickCall();
    assert(call.call_ && grants == 1 && ended == 0); // recovery retains intent and grant ownership
    now_us += 3600000000LL;
    call.TickCall(); assert(call.call_); // no established-call age limit
    fixture_state = AGENT_STATE_READY; call.TickCall();
    assert(call.call_ && call.audio_.enabled);
    fixture_state = AGENT_STATE_CONNECTED; running = false; call.TickCall();
    assert(!call.call_ && !call.audio_.enabled && ended == 1);
    Controller timeout;
    timeout.TickCall(); assert(timeout.requested_at_ && running);
    now_us += 30000001; timeout.TickCall();
    assert(!timeout.call_ && !running && ended == 2);
    Controller broken;
    broken.audio_.fault = true; broken.TickCall();
    assert(!broken.call_ && broken.errors == 1);
    Controller cancel;
    cancel.TickCall(); cancel.EndCall();
    assert(!cancel.call_ && !running && !cancel.audio_.enabled && cancel.ended_ && cancel.publications == 1);
    cancel.EndCall(); assert(cancel.publications == 1);

    ButtonDebounce key;
    assert(!key.Update(true, 0) && !key.Update(true, 50000)); // BOOT held during boot
    assert(!key.Update(false, 60000) && !key.Update(false, 110000));
    assert(!key.Update(true, 120000));
    assert(!key.Update(false, 130000) && !key.Update(true, 140000)); // contact bounce
    assert(!key.Update(true, 170000) && key.Update(true, 180000));
    assert(!key.Update(true, 5000000)); // holding cannot toggle a second time
    assert(!key.Update(false, 5010000) && !key.Update(false, 5060000));
    assert(!key.Update(true, 5070000) && key.Update(true, 5120000));
    ButtonController button;
    button.call_ = false; button.PhysicalButton();
    assert(button.starts == 1 && button.call_); // selected session starts directly
    button.PhysicalButton(); assert(!button.call_ && button.starts == 1);
    button.page_ = Page::kSettings; button.call_ = true;
    button.PhysicalButton(); assert(!button.call_ && button.page_ == Page::kHome);
    button.page_ = Page::kLauncher;
    button.PhysicalButton(); assert(button.page_ == Page::kHome && button.starts == 1);
    button.call_ = true; button.active_ = false; // connecting call can also be hung up
    button.PhysicalButton(); assert(!button.call_);
    button.friends_open_ = true; button.page_ = Page::kSettings;
    button.PhysicalButton(); assert(!button.friends_open_ && button.page_ == Page::kHome && button.starts == 1);

    language.store(Language::kEnglish); assert(!strcmp(ActivityName(Activity::kListening), "Listening"));
    language.store(Language::kChinese); assert(!strcmp(ActivityName(Activity::kListening), "正在聆听"));

    SwipeTracker swipe;
    assert(swipe.Update(true, 120, 230, 1, 0) == Swipe::kNone);
    assert(swipe.Update(true, 200, 245, 1, 100000) == Swipe::kRight);
    assert(swipe.Update(true, 300, 245, 1, 200000) == Swipe::kNone); // only once
    swipe.Update(false, 0, 0, 1, 210000);
    swipe.Update(true, 300, 230, 1, 300000);
    assert(swipe.Update(true, 210, 230, 1, 400000) == Swipe::kLeft);
    swipe.Update(false, 0, 0, 1, 410000);
    swipe.Update(true, 120, 230, 1, 500000);
    assert(swipe.Update(true, 210, 300, 1, 600000) == Swipe::kNone); // scroll vertically
    assert(swipe.Update(true, 300, 230, 1, 700000) == Swipe::kNone);
    swipe.Update(false, 0, 0, 1, 710000);
    swipe.Update(true, 120, 230, 1, 800000);
    assert(swipe.Update(true, 220, 230, 2, 900000) == Swipe::kNone); // old view

    swipe.Update(false, 0, 0, 2, 1000000);
    swipe.Update(true, 233, 422, 2, 1100000);
    assert(swipe.Update(true, 235, 350, 2, 1200000) == Swipe::kHome);
    assert(swipe.Update(true, 235, 270, 2, 1250000) == Swipe::kNone);
    swipe.Update(false, 0, 0, 2, 1300000);
    swipe.Update(true, 233, 300, 2, 1400000);
    assert(swipe.Update(true, 233, 200, 2, 1500000) == Swipe::kNone); // ordinary scroll
    swipe.Update(false, 0, 0, 2, 1600000);
    swipe.Update(true, 150, 370, 2, 1700000, true);
    assert(swipe.Update(true, 300, 370, 2, 1800000, true) == Swipe::kNone); // slider
    VolumeMailbox volume;
    int desired, saved;
    assert(!volume.Take(desired, saved));
    for (int i = 0; i <= 100; ++i) volume.Submit(i, false);
    assert(volume.Take(desired, saved) && desired == 100 && saved == -1);
    volume.Submit(30, true); volume.Submit(60, false);
    assert(volume.Take(desired, saved) && desired == 60 && saved == 30);
    assert(!volume.Take(desired, saved));
    volume.Submit(-1, true); assert(volume.Take(desired, saved) && desired == 0 && saved == 0);
    volume.Submit(200, true); assert(volume.Take(desired, saved) && desired == 100 && saved == 100);
    assert(FollowReply(24, false) && !FollowReply(25, false) && !FollowReply(0, true));
    assert(WindowScroll(610, 0, 8, kRowHeight) == 82);
    assert(WindowScroll(10, 8, 0, kRowHeight) == 538);
    assert(WindowScroll(10, 0, 8, kRowHeight) == 0);

    kc_audio::SpeakerEchoGuard echo;
    echo.OnWrite(1000000, 1600);
    assert(!echo.Drained(1100000) && echo.Drained(1200000));
    assert(echo.Muted(1300000) && !echo.Muted(1400000));

    PcmLevels levels;
    int16_t zeros[320] = {};
    levels.Add(nullptr, 320); levels.Add(zeros, 0); assert(!levels.frames);
    levels.Add(zeros, 320);
    assert(levels.samples == 320 && levels.zero_frames == 1 && levels.quiet_frames == 1 && levels.Rms() == 0);
    int16_t tiny[] = {-64, 64, -32, 32}; levels.Add(tiny, 4);
    assert(levels.quiet_frames == 2 && levels.zero_frames == 1 && levels.peak == 64);
    levels = {}; int16_t full[] = {-32768, 32767};
    for (int i = 0; i < 40000; ++i) levels.Add(full, 2);
    assert(levels.samples == 80000 && levels.energy == 85896724520000ULL);
    assert(levels.minimum == -32768 && levels.maximum == 32767 && levels.peak == 32768);
    assert(levels.Rms() == 32767 && levels.clipped_samples == 80000);
    levels = {}; int16_t dc[] = {1000,1000,1000,1000}; levels.Add(dc, 4);
    assert(levels.Rms() == 1000 && levels.Mean() == 1000 && levels.minimum == levels.maximum);
    levels = {}; int16_t negative_dc[] = {-1000,-1000}; levels.Add(negative_dc, 2);
    assert(levels.Rms() == 1000 && levels.Mean() == -1000);

    CaptureHarness clear;
    clear.Run();
    auto snapshot = clear.diagnostic_snapshot_;
    assert(snapshot.final && snapshot.raw.frames == 12 && snapshot.uplink.frames == 12);
    assert(snapshot.raw.Rms() == 1000 && snapshot.uplink.Rms() == 1000 && snapshot.raw.Mean() == 0);
    assert(snapshot.voice_frames == 12 && !snapshot.echo_muted_frames && !snapshot.write_errors);
    assert(snapshot.mic_stack_min == 4500 && snapshot.play_stack_min == 2048 && !clear.hw.codec_.MicOn());
    clear.continuous_.store(true); clear.Run();
    assert(clear.diagnostic_snapshot_.capture == 2 && clear.diagnostic_snapshot_.raw.frames == 12);
    CaptureHarness muted;
    muted.echo_.OnWrite(now_us, 1600); expect_muted = true; muted.Run(); expect_muted = false;
    snapshot = muted.diagnostic_snapshot_;
    assert(snapshot.raw.Rms() == 1000 && snapshot.uplink.Rms() == 0 && snapshot.echo_muted_frames == 12);
    CaptureHarness long_call; capture_limit = 260; long_call.Run(); capture_limit = 12;
    snapshot = long_call.diagnostic_snapshot_;
    assert(snapshot.sequence == 2 && snapshot.final && snapshot.raw.frames == 10); // bounded five-second window
    CaptureHarness dropped;
    capture_write_error_at = 4; capture_write_error = ESP_ERR_NO_MEM; dropped.Run();
    assert(dropped.diagnostic_snapshot_.voice_drops == 1 && !dropped.fault_.load());
    CaptureHarness write_error; capture_write_error_at = 1; capture_write_error = ESP_FAIL; write_error.Run();
    assert(write_error.fault_.load() && write_error.diagnostic_snapshot_.write_errors == 1);
    capture_write_error_at = 0;
    CaptureHarness read_error; capture_read_error_at = 3; read_error.Run(); capture_read_error_at = 0;
    assert(read_error.fault_.load() && read_error.diagnostic_snapshot_.read_errors == 1 && written_frames == 2);
    puts("AMOLED voice: circular bounds, ring priority, BOOT debounce/start/hangup, swipes and call recovery passed.");
    puts("PCM diagnostics: numeric bounds, raw/uplink separation, real capture-loop payload preservation, muting, window/reset, drops and read/write faults passed.");
}
