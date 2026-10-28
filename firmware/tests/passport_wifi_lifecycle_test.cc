// Fault-inject the production Passport call controller and microphone loop.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include "passport_wifi_call.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_ERR_NO_MEM = 2, ESP_FAIL = 3;
enum agent_state_t { AGENT_STATE_DISCONNECTED, AGENT_STATE_CONNECTED, AGENT_STATE_READY };
agent_state_t link = AGENT_STATE_CONNECTED;
bool running = false;
int64_t now_us = 0;
unsigned ends = 0, opens = 0, closes = 0, writes = 0;
struct Done {};
std::function<void()> on_delay = [] { throw Done{}; }, on_read = [] {};
std::function<int()> on_open = [] { return ESP_OK; }, on_write = [] { return ESP_OK; };
int64_t esp_timer_get_time() { return now_us; }
const char* esp_err_to_name(esp_err_t) { return "fixture error"; }
agent_state_t agent_link_state() { return link; }
bool agent_link_realtime_running() { return running; }
int agent_link_end_realtime() { ++ends; running = false; return ESP_OK; }
void vTaskDelay(unsigned ticks) { assert(ticks > 0); now_us += ticks * 1000; on_delay(); }
using agent_stream_handle_t = void*;
constexpr int AGENT_STREAM_VOICE = 0;
int agent_link_stream_open(int, void*, agent_stream_handle_t* out) {
    ++opens; const int result = on_open();
    *out = result == ESP_OK ? reinterpret_cast<void*>(uintptr_t(1)) : nullptr;
    return result;
}
int agent_link_stream_close(agent_stream_handle_t stream, bool complete) {
    assert(stream && complete); ++closes; return ESP_OK;
}
int agent_link_stream_write(agent_stream_handle_t stream, const void*, size_t) {
    assert(stream); ++writes; return on_write();
}
struct Codec {
    bool mic = false, fail_start = false;
    unsigned starts = 0, stops = 0;
    bool MicOn() { return mic; }
    int StartMic() { ++starts; if (fail_start) return ESP_FAIL; mic = true; return ESP_OK; }
    int StopMic() { ++stops; mic = false; return ESP_OK; }
    int ReadPcm(int16_t* pcm, unsigned, size_t* got) { pcm[0] = 1; *got = 1; on_read(); return ESP_OK; }
};
struct Hardware { Codec codec; };
struct Echo { bool Muted(int64_t) { return false; } };
class PassportWifiAudio {
public:
    Hardware* hardware_ = nullptr;
    Echo echo_;
    std::atomic<bool> enabled_{false};
    bool Enabled() const { return enabled_.load(); }
    void Enable(bool value) { enabled_ = value; }
    void Stop() { enabled_ = false; }
    void MicLoop();
};
// INSERT_MIC_LOOP
class WifiApp {
public:
    passport_wifi::CallState call_;
    PassportWifiAudio audio_;
    char message_[144]{};
    // INSERT_END_CALL
    // INSERT_UPDATE_CALL
};
void RunMic(PassportWifiAudio& audio) {
    try { audio.MicLoop(); assert(false); } catch (const Done&) {}
}
void TestController() {
    WifiApp app;
    auto start = [&] {
        running = true; link = AGENT_STATE_CONNECTED; now_us += 1000;
        app.call_.Start(now_us); app.UpdateCall(now_us);
        assert(app.call_.Calling() && !app.audio_.Enabled());
        link = AGENT_STATE_READY; app.UpdateCall(now_us);
        assert(app.call_.Active() && app.audio_.Enabled());
    };
    start();
    now_us += 180000000; // A long call must not trip the initial 30 s deadline.
    app.UpdateCall(now_us); assert(ends == 0 && app.call_.Active());
    link = AGENT_STATE_CONNECTED; app.UpdateCall(now_us);
    assert(app.call_.Recovering() && app.audio_.Enabled() && ends == 0);
    for (unsigned i = 0; i < 80; ++i) { now_us += 100000; app.UpdateCall(now_us); }
    assert(app.call_.Recovering() && ends == 0);
    link = AGENT_STATE_READY; app.UpdateCall(now_us);
    assert(app.call_.Active() && app.audio_.Enabled() && ends == 0);
    link = AGENT_STATE_CONNECTED; app.UpdateCall(now_us);
    running = false; app.UpdateCall(now_us); // SDK exhausted its bounded retries.
    assert(!app.call_.Calling() && !app.audio_.Enabled() && ends == 1);
    app.UpdateCall(now_us); assert(ends == 1);
    for (const char* reason : {"user", "menu", "agent_changed"}) {
        start(); link = AGENT_STATE_CONNECTED; app.UpdateCall(now_us);
        app.EndCall(reason); const unsigned ended = ends;
        link = AGENT_STATE_READY; app.UpdateCall(now_us);
        assert(!app.call_.Calling() && !app.audio_.Enabled() && ends == ended);
    }
    start(); app.audio_.Enable(false); app.UpdateCall(now_us);
    assert(!app.call_.Calling() && !app.audio_.Enabled());
    start(); link = AGENT_STATE_CONNECTED; app.UpdateCall(now_us);
    app.audio_.Enable(false); app.UpdateCall(now_us); // Agent stop during backoff.
    assert(!app.call_.Calling());
    running = true; link = AGENT_STATE_CONNECTED; app.call_.Start(now_us);
    app.UpdateCall(now_us + 29999999); assert(app.call_.Calling());
    app.UpdateCall(now_us + 30000000); assert(!app.call_.Calling() && !running);
    running = true; app.call_.Start(now_us); running = false; app.UpdateCall(now_us + 1);
    assert(!app.call_.Calling()); // Initial grant rejection ends promptly.
}
void TestMic() {
    Hardware hardware;
    PassportWifiAudio audio; audio.hardware_ = &hardware;
    // Read races READY -> CONNECTED: preserve intent, close the stale stream,
    // pause the codec, reopen on READY, and obey a later explicit stop.
    unsigned phase = 0;
    audio.Enable(true); link = AGENT_STATE_READY;
    on_read = [&] {
        if (phase == 0) link = AGENT_STATE_CONNECTED;
        else { assert(phase == 2 && hardware.codec.starts == 2); audio.Enable(false); }
    };
    on_write = [&] { return phase == 0 ? ESP_ERR_INVALID_STATE : ESP_OK; };
    on_delay = [&] {
        if (phase == 0) { assert(audio.Enabled() && closes == 1); ++phase; }
        else if (phase == 1) {
            assert(audio.Enabled() && !hardware.codec.MicOn()); link = AGENT_STATE_READY; ++phase;
        } else {
            assert(!audio.Enabled() && !hardware.codec.MicOn() && opens == 2 && closes == 2); throw Done{};
        }
    };
    RunMic(audio);
    // Opening also races transport loss; NO_MEM is bounded backpressure.
    phase = 0; audio.Enable(true); link = AGENT_STATE_READY;
    on_open = [&] { return phase == 0 ? ESP_ERR_INVALID_STATE : phase == 1 ? ESP_ERR_NO_MEM : ESP_OK; };
    on_delay = [&] { assert(audio.Enabled()); ++phase; };
    on_read = [&] { assert(phase == 2 && audio.Enabled()); throw Done{}; };
    RunMic(audio);
    // A real codec failure still stops the call instead of pretending to retry.
    hardware.codec.mic = false; hardware.codec.fail_start = true;
    on_open = [] { return ESP_OK; };
    on_delay = [&] { assert(!audio.Enabled()); throw Done{}; };
    RunMic(audio);
    // A non-transient stream error also remains fatal.
    hardware.codec.fail_start = false; audio.Enable(true);
    on_read = [] {};
    on_write = [] { return ESP_FAIL; };
    RunMic(audio);
}
int main() {
    TestController(); TestMic();
    std::puts("PASS: production call/mic lifecycle: long calls, READY loss, recovery, final failure, explicit cancel, startup deadline, open/write races and hardware faults");
}
