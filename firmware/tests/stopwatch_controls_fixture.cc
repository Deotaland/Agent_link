#include <cassert>
#include <cstdio>
#include <cstring>
#include "cst820_frame.h"
#include "watch_wifi_controls.h"
#include "voice_playback.h"
#include "agent_link_platform.h"

#define ESP_LOGW(...) ((void)0)
constexpr int NVS_READWRITE = 1;
using nvs_handle_t = int;
int nvs_open(const char*, int, int* handle) { *handle = 1; return ESP_OK; }
int nvs_set_i32(int, const char*, int) { return ESP_OK; }
int nvs_set_str(int, const char*, const char*) { return ESP_OK; }
int nvs_commit(int) { return ESP_OK; }
int nvs_erase_all(int) { return ESP_OK; }
void nvs_close(int) {}
enum agent_state_t { AGENT_STATE_CONNECTED, AGENT_STATE_READY, AGENT_STATE_DISCONNECTED };
agent_state_t fixture_state = AGENT_STATE_CONNECTED;
agent_state_t agent_link_state() { return fixture_state; }
int64_t fixture_now = 2000000;
int64_t esp_timer_get_time() { return fixture_now; }
agent_link_platform_agent_t fixture_bound{7, "Test agent", true};
int binding_error = ESP_OK, create_error = ESP_OK, list_error = ESP_OK;
int deletes = 0, resets = 0, grants = 0, ended = 0, lists = 0;
size_t last_offset = 0, total_sessions = 8;
char last_grant[80] = {}, last_deleted[80] = {};
esp_err_t agent_link_platform_bound_agent(agent_link_platform_agent_t* out) { *out = fixture_bound; return binding_error; }
esp_err_t agent_link_platform_conversations(int agent, size_t offset, agent_link_platform_conversation_t* entries,
                                           size_t capacity, size_t* count, size_t* total) {
    assert(agent == fixture_bound.id); ++lists; last_offset = offset;
    *count = 0; *total = total_sessions;
    if (list_error) { entries[0].id[0] = '\0'; return list_error; }
    for (size_t i = offset; i < total_sessions && *count < capacity; ++i) {
        auto& row = entries[(*count)++];
        snprintf(row.id, sizeof row.id, "session-%u", static_cast<unsigned>(i));
        snprintf(row.name, sizeof row.name, "Conversation %u", static_cast<unsigned>(i));
    }
    return ESP_OK;
}
esp_err_t agent_link_platform_create_conversation(int agent, char* id, size_t size) {
    assert(agent == fixture_bound.id); if (!create_error) snprintf(id, size, "created-session"); return create_error;
}
esp_err_t agent_link_platform_delete_conversation(int agent, const char* id) {
    assert(agent == fixture_bound.id); ++deletes; snprintf(last_deleted, sizeof last_deleted, "%s", id); return ESP_OK;
}
esp_err_t agent_link_request_realtime_session(int agent, const char* id) {
    assert(agent == fixture_bound.id); ++grants; snprintf(last_grant, sizeof last_grant, "%s", id); return ESP_OK;
}
int agent_link_end_realtime() { ++ended; return ESP_OK; }
int agent_link_forget() { ++resets; binding_error = ESP_ERR_NOT_FOUND; return ESP_OK; }
namespace watch_wifi {
// INSERT_SCREEN
Screen displayed;
char answer[1024] = {};
void SetScreen(const Screen& screen) { displayed = screen; }
void Toast(const char*, uint32_t = 2500) {}
void SetAnswer(const char* text) { snprintf(answer, sizeof answer, "%s", text); }
}
struct Audio {
    bool continuous = false;
    bool Ready() { return true; }
    void Stop() { continuous = false; }
    void SetContinuous(bool enabled) { continuous = enabled; }
};
// INSERT_DECLARATIONS
class Controller {
public:
    Audio audio_;
    void Vibrate(int) {}
// INSERT_METHODS
// INSERT_MEMBERS
};

void TestTouch() {
    using cst820::Decode;
    cst820::Point p;
    uint8_t f[7] = {0, 0, 1, 0, 137, 1, 94};  // home call button, x137 y350
    assert(Decode(f, 7, 466, 466, &p) && p.pressed && p.x == 137 && p.y == 350);
    f[3] = 0x81; f[4] = 209; f[5] = 0xA1; f[6] = 209;
    assert(Decode(f, 7, 466, 466, &p) && p.pressed && p.x == 465 && p.y == 465);
    f[4] = 210; assert(!Decode(f, 7, 466, 466, &p) && !p.pressed);
    f[3] = 0x40; assert(Decode(f, 7, 466, 466, &p) && !p.pressed);
    f[2] = 0; f[3] = 0xFF; assert(Decode(f, 7, 466, 466, &p) && !p.pressed);
    f[2] = 1; assert(!Decode(f, 7, 466, 466, &p) && !p.pressed);
    f[2] = 2; assert(!Decode(f, 7, 466, 466, &p));
    assert(!Decode(f, 6, 466, 466, &p));
    assert(!Decode(nullptr, 7, 466, 466, &p));
    f[2] = 1; f[3] = f[4] = f[5] = f[6] = 0;
    assert(Decode(f, 7, 466, 466, &p) && p.pressed && !p.x && !p.y);
    puts("PASS CST820: official down/contact/up, 12-bit coordinates, stale release, malformed/multitouch/out-of-range frames");
}
void TestKeys() {
    watch_wifi::VolumeKey key;
    assert(!key.Poll(true, 0)); assert(!key.Poll(false, 20000));
    assert(!key.Poll(true, 40000)); assert(key.Poll(true, 60000));
    assert(!key.Poll(true, 559999)); assert(key.Poll(true, 560000));
    assert(!key.Poll(true, 700000)); assert(key.Poll(true, 710000));
    assert(!key.Poll(false, 730000)); assert(!key.Poll(false, 750000));
    assert(!key.Down() && !key.Poll(false, 9000000));
    assert(watch_wifi::AdjustVolume(0, -1) == 0);
    assert(watch_wifi::AdjustVolume(100, 1) == 100);
    assert(watch_wifi::AdjustVolume(60, -1) == 55);
    assert(watch_wifi::AdjustVolume(55, 1) == 60);
    watch_wifi::Input input{watch_wifi::TouchAction::kRow, 1, 5, 1000};
    assert(watch_wifi::CurrentInput(input, 5, 1001));
    assert(!watch_wifi::CurrentInput(input, 6, 1001));
    assert(!watch_wifi::CurrentInput(input, 5, 501000));
    assert(!watch_wifi::CurrentInput(input, 5, 999));
    puts("PASS controls: debounce, hold/repeat/release, 0..100 volume, stale/expired/future touch events");
}
void TestController() {
    Controller c;
    assert(c.SyncBoundAgent());
    c.Settings(); c.Publish();
    assert(c.page_ == Page::kSettings && !resets && !deletes);
    assert(!strcmp(watch_wifi::displayed.rows[0], "Choose conversation"));
    c.Select(0); c.Publish();
    assert(c.page_ == Page::kSessions && c.count_ == 3 && watch_wifi::displayed.next && !watch_wifi::displayed.previous);
    c.Sessions(3); c.Publish();
    assert(last_offset == 3 && watch_wifi::displayed.previous && watch_wifi::displayed.next);
    c.Sessions(6); c.Publish(); assert(c.count_ == 2 && !watch_wifi::displayed.next);
    c.Select(3); assert(!c.session_[0]); // row outside displayed range
    c.Select(1); assert(!strcmp(c.session_, "session-7") && c.page_ == Page::kHome);
    c.Select(); assert(c.call_ && !c.audio_.continuous);
    c.TickCall(); assert(grants == 1 && !strcmp(last_grant, "session-7"));
    fixture_state = AGENT_STATE_READY; c.TickCall(); assert(c.active_ && c.audio_.continuous);
    c.Select(); assert(!c.call_ && !c.audio_.continuous);
    fixture_state = AGENT_STATE_CONNECTED;
    c.Settings(); c.Select(2); assert(c.page_ == Page::kDelete && !deletes);
    c.Select(0); assert(!deletes && c.page_ == Page::kSettings); // cancel
    c.Select(2); c.Select(1); assert(deletes == 1 && !strcmp(last_deleted, "session-7") && !c.session_[0]);
    c.Select(0); assert(!strcmp(c.session_, "session-0"));
    c.Settings(); create_error = ESP_FAIL; c.Select(1);
    assert(!strcmp(c.session_, "session-0") && c.page_ == Page::kSettings);
    c.Publish(); assert(strstr(watch_wifi::displayed.detail, "Request failed"));
    create_error = ESP_OK; c.Select(1); assert(!strcmp(c.session_, "created-session") && c.page_ == Page::kHome);
    c.Sessions(0); list_error = ESP_FAIL; c.Sessions(3); list_error = ESP_OK;
    c.Select(0); assert(!strcmp(c.session_, "session-0")); // failed fetch did not corrupt selectable rows
    total_sessions = 2; int before = lists; c.Sessions(3000);
    assert(lists == before + 2 && last_offset == 0 && c.count_ == 2); // bounded fallback
    c.Select(0); fixture_bound.id = 8; c.SyncBoundAgent(); assert(!c.session_[0] && c.agent_ == 8 && !c.call_);
    c.Settings(); c.cursor_ = 3; c.Select(0); assert(c.page_ == Page::kReset && !resets);
    c.Select(0); assert(!resets); // cancel is inert
    c.cursor_ = 3; c.Select(0); c.Select(1); assert(resets == 1 && !c.agent_ && !c.session_[0] && c.page_ == Page::kHome);
    puts("PASS production controller: pagination, failed-fetch preservation, create failure/success, delete cancel/confirm, call ready/end, rebind, reset confirmation");
}
void TestAudio() {
    kc_audio::PlaybackGate gate(16000, 32000);
    assert(!gate.Ready(15999, false, 0)); assert(gate.Ready(16000, false, 1));
    assert(!gate.Ready(0, false, 100)); assert(!gate.Ready(0, false, 180) && gate.Underruns() == 1);
    assert(!gate.Ready(31999, false, 200)); assert(gate.Ready(32000, false, 201));
    gate.Reset(); assert(gate.Ready(100, true, 300)); assert(!gate.Ready(0, true, 301));
    kc_audio::SpeakerEchoGuard echo;
    echo.OnWrite(1000000, 1600);
    assert(!echo.Drained(1199999) && echo.Drained(1200000));
    assert(echo.Muted(1399999) && !echo.Muted(1400000));
    puts("PASS playback: prebuffer, underrun recovery, short-tail drain and microphone echo guard");
}
int main() { TestTouch(); TestKeys(); TestController(); TestAudio(); }
