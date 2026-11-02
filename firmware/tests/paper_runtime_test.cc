#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "esp_err.h"
#include "paper_protocol.h"
#include "paper_refresh.h"
#include "paper_controls.h"
#include "epd.h"

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #value); std::exit(1); } } while (false)
#define CONFIG_PAPER_S31_DISPLAY_DIAGNOSTIC 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr esp_err_t ESP_ERR_TIMEOUT = 0x107;
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;
constexpr int pdPASS = 1;
using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);
#define pdMS_TO_TICKS(ms) (ms)

static bool psram_ok = true, internal_ok = true, task_ok = true;
static unsigned task_calls;
static std::vector<unsigned> allocation_caps;
static std::map<void*, size_t> allocations;
static void* heap_caps_malloc(size_t size, unsigned caps) {
    allocation_caps.push_back(caps);
    if (((caps & MALLOC_CAP_SPIRAM) && !psram_ok) || ((caps & MALLOC_CAP_INTERNAL) && !internal_ok)) return nullptr;
    auto* ptr = static_cast<uint8_t*>(std::malloc(size + 32));
    CHECK(ptr);
    std::memset(ptr + size, 0xa5, 32);
    allocations[ptr] = size;
    return ptr;
}
static void heap_caps_free(void* ptr) {
    CHECK(allocations.count(ptr) == 1);
    const size_t size = allocations.at(ptr);
    for (unsigned i = 0; i < 32; ++i) CHECK(static_cast<uint8_t*>(ptr)[size + i] == 0xa5);
    allocations.erase(ptr);
    std::free(ptr);
}
static int xTaskCreate(TaskFunction_t, const char*, unsigned stack, void*, unsigned, TaskHandle_t* task) {
    ++task_calls;
    CHECK(stack == 6144);
    CHECK(allocations.size() == 1); // The PCM buffer exists before the worker can run.
    *task = reinterpret_cast<void*>(uintptr_t(1));
    return task_ok ? pdPASS : 0;
}

struct LoopFinished {};
static bool music_loop = false, screen_loop = false;
static int64_t now_us, stop_at_us;
static int64_t esp_timer_get_time() { return now_us; }
static void vTaskDelay(unsigned ms) {
    now_us += ms * 1000;
    if ((music_loop && ms == 20) || (screen_loop && now_us >= stop_at_us)) throw LoopFinished{};
}

class PaperWifiAudio {
public:
    bool ready = true, cancel = false, idle = true;
    unsigned ends = 0, stops = 0;
    std::vector<int16_t> samples;
    bool Ready() const { return ready; }
    bool PlaybackIdle() const { return idle; }
    void Stop() { ++stops; }
    void End() { ++ends; }
    bool PlayLocal(const uint8_t* pcm, size_t bytes, const std::atomic<uint32_t>& generation, uint32_t expected) {
        CHECK(generation.load() == expected);
        CHECK(bytes <= 4096 && bytes % 2 == 0);
        const auto* first = reinterpret_cast<const int16_t*>(pcm);
        samples.insert(samples.end(), first, first + bytes / 2);
        if (cancel) const_cast<std::atomic<uint32_t>&>(generation).fetch_add(1);
        return true;
    }
};

static std::vector<uint8_t> wav_data;
static FILE* OpenTrack(const char*, const char*) {
    FILE* file = std::tmpfile();
    CHECK(file);
    CHECK(std::fwrite(wav_data.data(), 1, wav_data.size(), file) == wav_data.size());
    std::rewind(file);
    return file;
}
// INSERT_MUSIC_CLASS
std::vector<std::string> PaperMusic::Tracks() { return {"tone.wav"}; }
// INSERT_MUSIC_WORKER

static void ResetMemory() {
    CHECK(allocations.empty());
    psram_ok = internal_ok = task_ok = true;
    task_calls = 0;
    allocation_caps.clear();
}
static void Wav(unsigned rate, unsigned channels, unsigned frames) {
    wav_data.clear();
    auto word = [](uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) wav_data.push_back(static_cast<uint8_t>(value >> (8 * i)));
    };
    auto tag = [](const char* text) { wav_data.insert(wav_data.end(), text, text + 4); };
    tag("RIFF"); word(36 + frames * channels * 2, 4); tag("WAVE");
    tag("fmt "); word(16, 4); word(1, 2); word(channels, 2); word(rate, 4);
    word(rate * channels * 2, 4); word(channels * 2, 2); word(16, 2);
    tag("data"); word(frames * channels * 2, 4);
    for (unsigned frame = 0; frame < frames; ++frame) {
        word(1000, 2);
        if (channels == 2) word(3000, 2);
    }
}
static void RunMusic(PaperMusic& music) {
    music_loop = true;
    try { music.Loop(); } catch (LoopFinished&) {}
    music_loop = false;
    CHECK(!music.Playing());
}
static void MusicTests() {
    ResetMemory();
    PaperWifiAudio audio;
    PaperMusic music(audio);
    CHECK(music.Play("../tone.wav") == ESP_ERR_NOT_FOUND);
    CHECK(task_calls == 0 && allocation_caps.empty());
    audio.ready = false;
    CHECK(music.Play("tone.wav") == ESP_ERR_INVALID_STATE);
    audio.ready = true;
    CHECK(music.Play("tone.wav") == ESP_OK);
    CHECK(allocation_caps == std::vector<unsigned>{MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT});
    CHECK(allocations.at(music.buffers_) == 6144);
    Wav(8000, 1, 1024); // Largest output: every input sample produces two output samples.
    RunMusic(music);
    CHECK(music.Error() == ESP_OK && audio.samples.size() == 2048);
    CHECK(std::all_of(audio.samples.begin(), audio.samples.end(), [](int16_t s) { return s == 1000; }));
    auto* reused = music.buffers_;
    CHECK(music.Play("tone.wav") == ESP_OK && music.buffers_ == reused);
    CHECK(task_calls == 1 && allocation_caps.size() == 1);
    audio.samples.clear();
    Wav(48000, 2, 1024); // Conversion phase must survive a 2048-byte input chunk boundary.
    RunMusic(music);
    CHECK(music.Error() == ESP_OK && audio.samples.size() == 341);
    CHECK(std::all_of(audio.samples.begin(), audio.samples.end(), [](int16_t s) { return s == 2000; }));
    CHECK(music.Play("tone.wav") == ESP_OK);
    audio.samples.clear(); audio.cancel = true;
    Wav(8000, 1, 4096);
    RunMusic(music);
    CHECK(audio.samples.size() == 2048 && audio.ends == 3);
    audio.cancel = false;
    CHECK(music.Play("tone.wav") == ESP_OK);
    wav_data = {'b', 'a', 'd'};
    RunMusic(music);
    CHECK(music.Error() == ESP_ERR_NOT_SUPPORTED);
    heap_caps_free(music.buffers_);

    ResetMemory();
    PaperMusic fallback(audio);
    psram_ok = false;
    CHECK(fallback.Play("tone.wav") == ESP_OK);
    CHECK(allocation_caps.size() == 2);
    CHECK(allocation_caps[0] == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    CHECK(allocation_caps[1] == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    heap_caps_free(fallback.buffers_);

    ResetMemory();
    PaperMusic unavailable(audio);
    psram_ok = internal_ok = false;
    CHECK(unavailable.Play("tone.wav") == ESP_ERR_NO_MEM);
    CHECK(unavailable.Error() == ESP_ERR_NO_MEM && !unavailable.buffers_ && !unavailable.task_);
    CHECK(task_calls == 0 && allocations.empty());
    psram_ok = internal_ok = true; task_ok = false;
    CHECK(unavailable.Play("tone.wav") == ESP_ERR_NO_MEM);
    CHECK(!unavailable.buffers_ && !unavailable.task_ && allocations.empty());
    task_ok = true;
    CHECK(unavailable.Play("tone.wav") == ESP_OK); // A failed allocation/task create is recoverable.
    Wav(16000, 1, 32);
    RunMusic(unavailable);
    CHECK(unavailable.Error() == ESP_OK);
    heap_caps_free(unavailable.buffers_);
    ResetMemory();
    std::puts("PASS: PSRAM preference, internal fallback, allocation/task failure cleanup, buffer reuse, WAV boundaries and cancellation");
}

constexpr uint32_t AGENT_CAP_BUTTON = 1, AGENT_CAP_HAPTIC = 2, AGENT_CAP_LED = 4,
    AGENT_CAP_SCREEN = 8, AGENT_CAP_BATTERY = 16, AGENT_CAP_MIC = 32, AGENT_CAP_SPEAKER = 64;
// INSERT_PAGE_KEY
enum agent_link_phase_t { AGENT_LINK_PHASE_IDLE, AGENT_LINK_PHASE_SETUP, AGENT_LINK_PHASE_PAIRING,
    AGENT_LINK_PHASE_BLOCKED, AGENT_LINK_PHASE_CONNECTING, AGENT_LINK_PHASE_CONNECTED, AGENT_LINK_PHASE_READY };
struct agent_link_status_t { agent_link_phase_t phase = AGENT_LINK_PHASE_IDLE; char code[8]{}; };
class Application {
public:
    virtual uint32_t Capabilities() const = 0;
    virtual void ShowText(const char*) = 0;
    virtual void OnLinkStatus(const agent_link_status_t&) {}
    virtual void AudioEnd() {}
    virtual ~Application() = default;
};
static unsigned probe_calls;
static unsigned recover_on_probe;
static std::vector<int64_t> display_times;
static bool fail_first_frame;
extern "C" bool epd_probe() { return ++probe_calls == recover_on_probe; }
static esp_err_t paper_display_lines(const char* const* lines, size_t count) {
    CHECK(lines && count == 14);
    display_times.push_back(now_us);
    return fail_first_frame && display_times.size() == 1 ? ESP_FAIL : ESP_OK;
}
class PaperS31Wifi : public Application {
public:
    PaperWifiAudio audio_;
    PaperMusic music_{audio_};
    std::atomic<bool> screen_ready_{false}, voice_output_{false}, voice_changed_{false};
    std::atomic<bool> refresh_paused_{false}, voice_final_{false}, status_changed_{false}, status_attention_{false};
    std::atomic<int64_t> voice_text_at_{0};
    bool battery_present_ = false, screen_text_pending_ = false;
    bool ringing_ = false, call_ = false, active_ = false;
    std::string pending_screen_text_, voice_text_, displayed_voice_text_, message_text_, notice_, tool_results_, screen_tool_text_;
    std::string clock_text_;
    std::vector<int> messages_;
    Page page_ = Page::Home, sessions_parent_ = Page::Chat;
    int scroll_ = 9, calendar_month_ = 1, calendar_year_ = 2026, edit_alarm_hour_ = 7, edit_alarm_minute_ = 0;
    unsigned cursor_ = 0;
    std::tm edit_time_{};
    std::mutex mutex_;
    agent_link_status_t status_;
    paper::Page screen_;
    paper::PendingConfirm confirm_;
    uint32_t view_revision_ = 0, epoch_ = 1;
    std::atomic<uint32_t> revision_{1}, important_revision_{1}, displayed_{0};
    unsigned renders = 0, activations = 0, voice_calls = 0, motor_ms = 0;
    esp_err_t failure = ESP_OK;
    const char* T(const char*, const char* en) const { return en; }
    void RunMotor(unsigned ms) { motor_ms = ms; }
    void Activate() { ++activations; }
    void Open(Page page) {
        confirm_.Cancel(); refresh_paused_.store(false);
        page_ = page; cursor_ = 0; scroll_ = 0; notice_.clear(); ++epoch_; Render();
    }
    void EndCall() { call_ = active_ = false; voice_output_.store(false); audio_.Stop(); }
    void Voice() { ++voice_calls; call_ = !call_; }
    void Notice(const char*, const char* en) { notice_ = en; Render(); }
    void Failure(esp_err_t error) { failure = error; }
    unsigned Count() const { return 7; }
    void Render(bool important = true) {
        ++renders;
        paper::Page output;
        std::snprintf(output.lines[1], sizeof output.lines[1], "%s", clock_text_.c_str());
        std::snprintf(output.lines[2], sizeof output.lines[2], "%d", int(page_));
        std::snprintf(output.lines[3], sizeof output.lines[3], "%u", cursor_);
        std::snprintf(output.lines[4], sizeof output.lines[4], "%s", message_text_.c_str());
        std::snprintf(output.lines[5], sizeof output.lines[5], "%s", displayed_voice_text_.c_str());
        std::snprintf(output.lines[6], sizeof output.lines[6], "%s", notice_.c_str());
        // INSERT_SCREEN_PUBLISH
    void ConsumeConfirm() {
        // INSERT_CONFIRM
    }
    // INSERT_CAPABILITIES
    // INSERT_STATUS
    // INSERT_SHOW_TEXT
    // INSERT_AUDIO_END
    // INSERT_APPLY_TEXT
    // INSERT_VOICE_TEXT
    // INSERT_NAVIGATE
    // INSERT_SHORTCUT
    // INSERT_SCREEN_LOOP
};
static void RunScreen(PaperS31Wifi& app, int64_t duration_us, unsigned recovery, bool fail_frame = false) {
    now_us = 0; stop_at_us = duration_us;
    probe_calls = 0; recover_on_probe = recovery;
    display_times.clear(); fail_first_frame = fail_frame;
    screen_loop = true;
    try { app.ScreenLoop(); } catch (LoopFinished&) {}
    screen_loop = false;
}
static void ScreenTests() {
    PaperS31Wifi app;
    CHECK(app.Capabilities() & AGENT_CAP_SCREEN); // The callback must still be wired after a failed boot probe.
    char text[] = "outside voice";
    app.ShowText(text);
    text[0] = '!';
    CHECK(app.renders == 0 && app.page_ == Page::Home);
    app.ApplyScreenText();
    CHECK(app.message_text_ == "outside voice" && app.page_ == Page::Message && app.scroll_ == 0 && app.renders == 1);
    app.ShowText("old"); app.ShowText("你好，世界"); app.ApplyScreenText();
    CHECK(app.message_text_ == "你好，世界" && app.renders == 2);
    app.ShowText(nullptr); app.ApplyScreenText();
    CHECK(app.message_text_.empty() && app.renders == 3);
    app.ShowText("deferred");
    app.voice_output_.store(true); app.page_ = Page::Voice;
    app.ShowText("voice reply"); app.ApplyScreenText();
    CHECK(app.page_ == Page::Voice && app.voice_text_ == "voice reply" && app.voice_changed_.load());
    CHECK(app.renders == 3 && app.screen_text_pending_);
    app.voice_output_.store(false); app.ApplyScreenText();
    CHECK(app.page_ == Page::Message && app.message_text_ == "deferred");
    RunScreen(app, 30000000, 2);
    CHECK(app.screen_ready_.load() && probe_calls == 2 && display_times.size() == 1);
    CHECK(display_times[0] >= 13000000 && app.displayed_.load() == app.revision_.load());

    PaperS31Wifi absent;
    absent.ShowText("retained while offline"); absent.ApplyScreenText();
    RunScreen(absent, 300000000, 0);
    CHECK(!absent.screen_ready_.load() && probe_calls == paper::ProbeRetry::kAttempts && display_times.empty());
    CHECK(absent.message_text_ == "retained while offline");
    PaperS31Wifi last_retry;
    RunScreen(last_retry, 30000000, paper::ProbeRetry::kAttempts);
    CHECK(last_retry.screen_ready_.load() && probe_calls == paper::ProbeRetry::kAttempts && display_times.size() == 1);
    PaperS31Wifi ready;
    ready.screen_ready_.store(true);
    RunScreen(ready, 70000000, 0, true);
    CHECK(probe_calls == 0 && display_times.size() == 2);
    CHECK(display_times[1] - display_times[0] >= paper::RefreshGate::kRetryUs);
    PaperS31Wifi loading;
    loading.Render(); loading.refresh_paused_.store(true);
    RunScreen(loading, 30000000, 1);
    CHECK(loading.screen_ready_.load() && display_times.empty());
    loading.refresh_paused_.store(false);
    RunScreen(loading, 30000000, 0);
    CHECK(display_times.size() == 1);
    std::puts("PASS: copied/coalesced desktop text, voice routing, empty text, bounded boot recovery and refresh failure backoff");
}
static void InteractionTests() {
    PaperS31Wifi app;
    app.Render(); app.screen_ready_.store(true);
    app.Navigate(Key::Down); app.Navigate(Key::Down); app.Navigate(Key::Ok);
    app.ConsumeConfirm(); CHECK(app.activations == 0);
    app.displayed_.store(app.revision_.load()); app.ConsumeConfirm();
    CHECK(app.activations == 1 && app.cursor_ == 2);
    app.ConsumeConfirm(); CHECK(app.activations == 1);
    app.Navigate(Key::Down); app.Navigate(Key::Ok); app.Navigate(Key::Back);
    app.displayed_.store(app.revision_.load()); app.ConsumeConfirm();
    CHECK(app.activations == 1); // Returning cancels the old selection.
    app.Navigate(Key::Down); app.Navigate(Key::Ok);
    app.clock_text_ = "12:34 99%"; app.Render(false);
    app.displayed_.store(app.revision_.load()); app.ConsumeConfirm();
    CHECK(app.activations == 2); // A changed clock cannot invalidate the chosen row.
    app.Navigate(Key::Down); app.Navigate(Key::Ok);
    app.message_text_ = "a different list"; app.Render();
    app.displayed_.store(app.revision_.load()); app.ConsumeConfirm();
    CHECK(app.activations == 2); // A changed body must cancel it.
    app.displayed_.store(0); app.refresh_paused_.store(true);
    app.Navigate(Key::Voice);
    CHECK(app.voice_calls == 1 && app.call_ && !app.refresh_paused_.load());
    const uint32_t old_epoch = app.epoch_;
    app.ringing_ = true; app.Navigate(Key::Stop);
    CHECK(!app.call_ && !app.ringing_ && app.audio_.stops > 0 && app.epoch_ > old_epoch);
    CHECK(app.failure == ESP_OK && app.notice_ == "Audio stopped");
    app.music_.playing_.store(true); app.Navigate(Key::Stop);
    CHECK(app.failure == ESP_ERR_TIMEOUT);
    app.music_.playing_.store(false);
    app.clock_text_ = "12:35 98%"; app.Render(false);
    CHECK(app.important_revision_.load() != app.revision_.load());
    app.Navigate(Key::Refresh);
    CHECK(app.important_revision_.load() == app.revision_.load());

    app.page_ = Page::Voice; app.voice_output_.store(true); now_us = 1000000;
    const unsigned before_voice = app.renders;
    app.ShowText("first fragment"); app.UpdateVoiceText(2000000);
    app.ShowText("complete reply"); app.Render(false); // A status update must not publish fragments.
    CHECK(app.displayed_voice_text_.empty());
    app.AudioEnd(); app.UpdateVoiceText(2100000);
    CHECK(app.displayed_voice_text_ == "complete reply" && !app.voice_changed_.load());
    CHECK(app.renders == before_voice + 2);
    now_us = 3000000; app.ShowText("quiet reply"); app.audio_.idle = false;
    app.UpdateVoiceText(9000000); CHECK(app.displayed_voice_text_ == "complete reply");
    app.audio_.idle = true; app.UpdateVoiceText(9000000);
    CHECK(app.displayed_voice_text_ == "quiet reply");

    agent_link_status_t status; status.phase = AGENT_LINK_PHASE_READY;
    app.OnLinkStatus(status); CHECK(!app.status_attention_.exchange(false));
    status.phase = AGENT_LINK_PHASE_PAIRING; std::strcpy(status.code, "123456");
    app.OnLinkStatus(status); CHECK(app.status_attention_.exchange(false));
    app.OnLinkStatus(status); CHECK(!app.status_attention_.exchange(false));
    std::strcpy(status.code, "654321"); app.OnLinkStatus(status); CHECK(app.status_attention_.exchange(false));
    status.phase = AGENT_LINK_PHASE_READY; app.OnLinkStatus(status); CHECK(app.status_attention_.exchange(false));
    std::puts("PASS: queued/cancelled confirmation, shortcuts without a displayed page, deferred metadata and final voice replies");
}
// INSERT_ROTATION
static void RotationTests() {
    CHECK(EPD_ROTATION == 180 && EPD_WIDTH == 400 && EPD_HEIGHT == 600);
    CHECK(logical_index(0, 0) == EPD_WIDTH * EPD_HEIGHT - 1);
    CHECK(logical_index(EPD_PANEL_W - 1, EPD_PANEL_H - 1) == 0);
    CHECK(logical_index(EPD_PANEL_W - 1, 0) == (EPD_HEIGHT - 1) * EPD_WIDTH);
    CHECK(logical_index(0, EPD_PANEL_H - 1) == EPD_WIDTH - 1);
    std::vector<bool> visited(EPD_WIDTH * EPD_HEIGHT);
    for (int y = 0; y < EPD_PANEL_H; ++y) for (int x = 0; x < EPD_PANEL_W; ++x) {
        const auto index = logical_index(x, y);
        CHECK(index < visited.size() && !visited[index]); visited[index] = true;
    }
    std::puts("PASS: production 180-degree pixel mapping, corners and all 240000 unique pixels");
}
int main() { MusicTests(); ScreenTests(); InteractionTests(); RotationTests(); }
