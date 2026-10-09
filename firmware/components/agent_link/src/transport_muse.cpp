// agent_link Muse transport backend: the device is a Muse gadget (Meta, muse-gadget-sdk).
//
// Meta's Home Link (components/muse_gadget) does everything that makes it one: BLE pairing with
// the Muse app (community pairing v5, a press on the device to confirm), the Wi-Fi the app hands
// over in the same session, the device token, and the encrypted Noise session to the user's Muse
// VM. This file only translates between that and the core:
//
//    Home Link status        -> agent_link_status_t + agent_state_t (READY once registered)
//    AGENT_STREAM_VOICE      -> a voice note to the Muse (muse_note_*), on a task of our own
//    the Muse's answer       -> a 0x33 IoActuate frame on screen0, i.e. the board's on_show_text;
//                               with TTS (muse_tts_*) also on_audio_out / on_audio_end
//    agent_link_confirm()    -> the pairing press;  agent_link_forget() -> Home Link's setup reset
//
// What does not cross: our binary control protocol has no peer here (no App), so events and the
// I/O manifest are refused, and there is no OTA. The Muse's own invokes (link.invoke) are Home
// Link's built-in device commands.
#include "agent_link_transport.h"

#include "sdkconfig.h"

#if CONFIG_AGENT_LINK_TRANSPORT_MUSE

#include "agent_link.h"
#include "audio_downlink.h"
#include "muse_gadget.h"
#include "protocol.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "agent_link.muse";

// Board PCM waiting for the voice task: 8 s of 16 kHz mono. The note goes up as it is spoken and
// waits out a slow uplink (a lost TCP segment holds it for a second or so) instead of failing, so
// this is how far the upload may fall behind before the note is cut short. PSRAM when there is some.
constexpr size_t   kPcmBufBytes   = 256 * 1024;
constexpr size_t   kPcmBytesPerMs = MUSE_NOTE_SAMPLE_RATE * 2 / 1000;
constexpr uint32_t kVoiceStack    = 6144;
constexpr UBaseType_t kVoicePrio  = 5;
constexpr uint8_t  kCmdIoActuate  = 0x33;
constexpr const char* kScreenId   = "screen0";   // the core's synthetic endpoint for on_show_text

enum class VoiceCmd : uint8_t { kBegin, kEnd, kCancel };

void (*s_on_recv)(const uint8_t*, size_t) = nullptr;
void (*s_on_state)(agent_state_t) = nullptr;
void (*s_on_status)(const agent_link_status_t*) = nullptr;
void (*s_on_pcm)(const uint8_t*, size_t) = nullptr;
void (*s_on_audio_end)() = nullptr;

// Home Link reports from its own tasks (the app task, the NimBLE host, the Noise session) and the
// voice task delivers answers; the core is not thread-safe and boards are promised one callback at
// a time. Recursive because a state change reports status on its way through.
std::recursive_mutex s_core_mtx;

std::atomic<muse_gadget_state_t> s_gadget{MUSE_GADGET_BOOT};
std::atomic<agent_state_t>       s_link{AGENT_STATE_DISCONNECTED};
char        s_agent_name[32] = {};   // the Muse's own name, once the session has asked (GET /identity)
portMUX_TYPE s_name_lock = portMUX_INITIALIZER_UNLOCKED;
bool        s_started = false;

QueueHandle_t        s_voice_q = nullptr;
StreamBufferHandle_t s_pcm     = nullptr;
size_t               s_pcm_cap = 0;
std::atomic<bool>    s_voice_open{false};
// From a stream's start until the voice task has handed all of it to the note (or dropped it).
// A new stream waits for that: its speech would land in the old note's buffer.
std::atomic<bool>    s_voice_busy{false};
std::atomic<bool>    s_cut{false};       // the buffer overflowed: the rest of this note is left out
std::atomic<uint32_t> s_dropped{0};

// ── Status ─────────────────────────────────────────────────────────────────────────────────

void Compose(muse_gadget_state_t g, agent_link_status_t* st) {
    *st = {};
    st->transport = AGENT_TRANSPORT_MUSE;
    switch (g) {
    case MUSE_GADGET_BOOT:
        st->phase = AGENT_LINK_PHASE_CONNECTING;
        snprintf(st->title, sizeof st->title, "Starting");
        snprintf(st->hint,  sizeof st->hint,  "Starting Muse");
        break;
    case MUSE_GADGET_ADVERTISING:
        st->phase = AGENT_LINK_PHASE_SETUP;
        snprintf(st->title, sizeof st->title, "Add me in Muse");
        snprintf(st->hint,  sizeof st->hint,
                 "Muse app: Settings > Devices > Developer mode, then Add Device: %s",
                 muse_gadget_ble_name());
        break;
    case MUSE_GADGET_APP_CONNECTED:
        st->phase = AGENT_LINK_PHASE_CONNECTING;
        snprintf(st->title, sizeof st->title, "Pairing");
        snprintf(st->hint,  sizeof st->hint,  "Setting up with the Muse app");
        break;
    case MUSE_GADGET_CONFIRM:
        st->phase = AGENT_LINK_PHASE_SETUP;
        snprintf(st->title, sizeof st->title, "Press to confirm");
        snprintf(st->hint,  sizeof st->hint,  "Press the button on this device to finish pairing");
        break;
    case MUSE_GADGET_WIFI_CONNECTING:
        st->phase = AGENT_LINK_PHASE_CONNECTING;
        snprintf(st->title, sizeof st->title, "Joining Wi-Fi");
        snprintf(st->hint,  sizeof st->hint,  "Joining the Wi-Fi chosen in the Muse app");
        break;
    case MUSE_GADGET_REACHING_MUSE:
        st->phase = AGENT_LINK_PHASE_CONNECTING;
        snprintf(st->title, sizeof st->title, "Connecting");
        snprintf(st->hint,  sizeof st->hint,  "Reaching your Muse");
        break;
    case MUSE_GADGET_ONLINE: {
        st->phase = AGENT_LINK_PHASE_READY;
        taskENTER_CRITICAL(&s_name_lock);
        snprintf(st->title, sizeof st->title, "%s", s_agent_name[0] ? s_agent_name : "Muse");
        taskEXIT_CRITICAL(&s_name_lock);
        snprintf(st->hint,  sizeof st->hint,  "Connected to your Muse");
        break;
    }
    case MUSE_GADGET_RECONNECTING:
        st->phase = AGENT_LINK_PHASE_CONNECTING;
        snprintf(st->title, sizeof st->title, "Reconnecting");
        snprintf(st->hint,  sizeof st->hint,  "Lost the connection to Muse, trying again");
        break;
    case MUSE_GADGET_UNPAIRED:
        st->phase = AGENT_LINK_PHASE_SETUP;
        snprintf(st->title, sizeof st->title, "Removed from Muse");
        snprintf(st->hint,  sizeof st->hint,  "Removed in the Muse app. Restarting for setup");
        break;
    case MUSE_GADGET_ERROR:
        st->phase = AGENT_LINK_PHASE_BLOCKED;
        snprintf(st->title, sizeof st->title, "Can't connect");
        snprintf(st->hint,  sizeof st->hint,
                 "Wi-Fi or Muse sign-in failed. Check the network, or reset the device and pair again");
        break;
    }
}

bool PostVoice(VoiceCmd cmd);

void Report(muse_gadget_state_t g) {
    agent_link_status_t st;
    Compose(g, &st);
    const agent_state_t link = (g == MUSE_GADGET_ONLINE) ? AGENT_STATE_READY : AGENT_STATE_DISCONNECTED;
    // The state only on a change: the core treats every call as a new link and closes the
    // board's open streams, which a status update (the Muse's name arriving) must not do.
    const bool changed = s_link.exchange(link) != link;
    if (changed && link != AGENT_STATE_READY && s_voice_open.exchange(false) &&
        !PostVoice(VoiceCmd::kCancel)) {   // the core has just forgotten the stream; so do we
        s_voice_busy.store(false);
    }
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    if (changed && s_on_state) s_on_state(link);
    if (s_on_status) s_on_status(&st);
}

void OnGadgetState(muse_gadget_state_t state, void* /*ctx*/) {
    static const char* const kNames[] = {"boot", "advertising", "app connected", "confirm",
                                         "wifi connecting", "reaching muse", "online",
                                         "reconnecting", "unpaired", "error"};
    const unsigned i = static_cast<unsigned>(state);
    ESP_LOGI(TAG, "muse: %s", i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?");
    s_gadget.store(state);
    Report(state);
}

void OnGadgetTitle(const char* name, void* /*ctx*/) {
    taskENTER_CRITICAL(&s_name_lock);
    snprintf(s_agent_name, sizeof s_agent_name, "%s", name ? name : "");
    taskEXIT_CRITICAL(&s_name_lock);
    ESP_LOGI(TAG, "the Muse is called \"%s\"", name ? name : "");
    if (s_gadget.load() == MUSE_GADGET_ONLINE) Report(MUSE_GADGET_ONLINE);
}

// ── The Muse's answers, into the core ─────────────────────────────────────────────────────

// Hand text to the board the way any peer would: an IoActuate command on screen0. The core
// answers it through send_ctrl, which drops the response; there is nobody to read it.
void DeliverText(const char* utf8) {
    if (!utf8 || !s_on_recv) return;
    const size_t id_len = strlen(kScreenId);
    size_t text_len = strlen(utf8);
    const size_t cap = 0xFFFF - 1 - id_len;
    if (text_len > cap) text_len = cap;
    const size_t payload_len = 1 + id_len + text_len;

    std::vector<uint8_t> f;
    f.reserve(agentlink::kHeaderSize + payload_len);
    f.push_back(agentlink::kVersion);
    f.push_back(agentlink::kMsgCommand);
    f.push_back(kCmdIoActuate);
    f.push_back(0);   // sequence: nothing pairs the response with anything
    f.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    f.push_back(static_cast<uint8_t>(payload_len >> 8));
    f.push_back(static_cast<uint8_t>(id_len));
    f.insert(f.end(), kScreenId, kScreenId + id_len);
    f.insert(f.end(), utf8, utf8 + text_len);

    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    s_on_recv(f.data(), f.size());
}

// ── Spoken answers ─────────────────────────────────────────────────────────────────────────
// Each new part of a reply is queued for TTS. The board gets the reply text when that part
// starts playing (right away if TTS produced nothing), the audio as on_audio_out, and
// on_audio_end once nothing is left in the queue.

// Bytes of the reply already queued; EV_REPLY carries the whole reply so far. Voice task only.
size_t s_reply_queued = 0;
// Incremented on every new note; callbacks for older pieces are ignored.
std::atomic<uint32_t> s_reply_tag{0};
// The reply so far, shown when a queued piece starts playing.
char         s_caption[1024];
portMUX_TYPE s_caption_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> s_audio_open{false};   // audio sent to the board since the last on_audio_end

void ShowCaption() {
    static char text[sizeof s_caption];   // TTS task only
    taskENTER_CRITICAL(&s_caption_lock);
    memcpy(text, s_caption, sizeof text);
    taskEXIT_CRITICAL(&s_caption_lock);
    DeliverText(text);
}

void EndAudio() {
    if (!s_audio_open.exchange(false) || !s_on_audio_end) return;
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    s_on_audio_end();
}

void OnSpeechStart(uint32_t tag, void* /*ctx*/) {
    if (tag == s_reply_tag.load()) ShowCaption();
}

void OnSpeechPcm(const int16_t* pcm, size_t frames, void* /*ctx*/) {
    if (!s_on_pcm) return;
    s_audio_open.store(true);
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    s_on_pcm(reinterpret_cast<const uint8_t*>(pcm), frames * sizeof(int16_t));
}

void OnSpeechEnd(uint32_t tag, bool spoke, bool last, void* /*ctx*/) {
    if (tag != s_reply_tag.load()) return;
    if (!spoke) ShowCaption();   // no audio: show the text anyway
    if (last) EndAudio();
}

// Queues the new part of the reply. False if nothing was queued; the caller shows the text then.
bool SpeakReply(const char* reply) {
    if (!muse_tts_enabled()) return false;
    const size_t len = strlen(reply);
    if (len <= s_reply_queued) return false;
    const char* fresh = reply + s_reply_queued;
    while (*fresh == '\n') ++fresh;
    if (!*fresh) return false;
    taskENTER_CRITICAL(&s_caption_lock);
    strlcpy(s_caption, reply, sizeof s_caption);
    taskEXIT_CRITICAL(&s_caption_lock);
    if (!muse_tts_say(fresh, s_reply_tag.load())) return false;
    s_reply_queued = len;
    return true;
}

// New question: stop the previous answer.
void StopSpeech() {
    s_reply_tag.fetch_add(1);
    muse_tts_stop();
    s_reply_queued = 0;
    EndAudio();
}

// ── Voice notes ────────────────────────────────────────────────────────────────────────────

// Board PCM -> the note, in order. False once the buffer is empty.
bool FeedPcm(bool send) {
    static int16_t buf[320];   // 20 ms; the voice task's only, kept off its stack
    const size_t n = xStreamBufferReceive(s_pcm, buf, sizeof buf, 0);
    if (n < sizeof(int16_t)) return false;
    if (send) muse_note_audio(buf, n / sizeof(int16_t));
    return true;
}

void PumpEvents() {
    static char text[1024];   // voice task only
    for (;;) {
        const muse_note_ev_t ev = muse_note_event(text, sizeof text);
        switch (ev) {
        case MUSE_NOTE_EV_NONE:
            return;
        case MUSE_NOTE_EV_SENT:
            ESP_LOGI(TAG, "voice note delivered; waiting for the answer");
            break;
        case MUSE_NOTE_EV_HEARD: {
            // What the Muse understood, shown while it thinks. Quoted, so it does not read as
            // the answer.
            static char quoted[sizeof text + 8];
            snprintf(quoted, sizeof quoted, "\"%s\"", text);
            DeliverText(quoted);
            break;
        }
        case MUSE_NOTE_EV_REPLY:
            if (!SpeakReply(text)) DeliverText(text);
            break;
        case MUSE_NOTE_EV_DONE:
            ESP_LOGI(TAG, "answer complete");
            break;
        case MUSE_NOTE_EV_ERROR:
            ESP_LOGW(TAG, "voice note failed: %s", text);
            DeliverText(text);
            break;
        }
    }
}

// The stream is all handed over (or dropped): say what did not fit, and let the next one start.
void StreamDone() {
    const uint32_t dropped = s_dropped.exchange(0);
    if (dropped) {
        ESP_LOGW(TAG, "the uplink fell %u ms behind: the last %u ms of speech were left out",
                 static_cast<unsigned>(s_pcm_cap / kPcmBytesPerMs),
                 static_cast<unsigned>(dropped / kPcmBytesPerMs));
    }
    s_voice_busy.store(false);
}

void VoiceTask(void*) {
    bool talking = false;
    for (;;) {
        VoiceCmd cmd;
        // One tick while talking (10 ms at this project's 100 Hz; pdMS_TO_TICKS(5) would be 0 and
        // spin), otherwise often enough to keep the answer's events moving.
        if (xQueueReceive(s_voice_q, &cmd, talking ? 1 : pdMS_TO_TICKS(50)) == pdTRUE) {
            switch (cmd) {
            case VoiceCmd::kBegin:
                StopSpeech();
                talking = muse_note_begin();
                break;
            case VoiceCmd::kEnd:
                // Everything the board wrote before closing is already in the buffer; with a slow
                // uplink that can be seconds of it, which go up now.
                while (FeedPcm(talking)) {}
                if (talking) muse_note_end();
                talking = false;
                StreamDone();
                break;
            case VoiceCmd::kCancel:
                while (FeedPcm(false)) {}
                muse_note_cancel();
                talking = false;
                StreamDone();
                break;
            }
        }
        if (talking) {
            while (FeedPcm(true)) {}   // blocks while the uplink is behind; the board's PCM queues up
        }
        PumpEvents();
    }
}

bool PostVoice(VoiceCmd cmd) {
    return s_voice_q && xQueueSend(s_voice_q, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

// ── Operation table ────────────────────────────────────────────────────────────────────────

esp_err_t muse_start(void* /*impl*/) {
    if (s_started) return ESP_OK;
    s_voice_q = xQueueCreate(8, sizeof(VoiceCmd));
    s_pcm_cap = kPcmBufBytes;
    s_pcm = xStreamBufferCreateWithCaps(s_pcm_cap, 1, MALLOC_CAP_SPIRAM);
    if (!s_pcm) {
        s_pcm_cap = 16 * 1024;   // no PSRAM: 0.5 s
        s_pcm = xStreamBufferCreate(s_pcm_cap, 1);
    }
    if (!s_voice_q || !s_pcm ||
        xTaskCreate(VoiceTask, "muse_voice", kVoiceStack, nullptr, kVoicePrio, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the voice task");
        return ESP_ERR_NO_MEM;
    }

    // TTS may run up to 3/4 of the board's play buffer ahead of playback, 2 s at most.
    const uint32_t buffer_ms = agentlink::audio::BufferMs();
    const muse_tts_sink_t sink = {OnSpeechStart, OnSpeechPcm, OnSpeechEnd, nullptr};
    muse_tts_set_sink(&sink, buffer_ms * 3 / 4 < 2000 ? buffer_ms * 3 / 4 : 2000);
    ESP_LOGI(TAG, "%s", muse_tts_enabled() ? "TTS on (MiniMax)" : "TTS off: no MiniMax key in the build");

    Report(MUSE_GADGET_BOOT);
    muse_gadget_config_t cfg = {};
    cfg.on_state = OnGadgetState;
    cfg.on_title = OnGadgetTitle;
    const esp_err_t r = muse_gadget_start(&cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "Home Link did not start: %s", esp_err_to_name(r));
        return r;
    }
    s_started = true;
    ESP_LOGI(TAG, "started as %s", muse_gadget_ble_name());
    return ESP_OK;
}

void muse_stop(void* /*impl*/) {
    // Home Link has no orderly shutdown: upstream it is the whole firmware. Keep it running.
    ESP_LOGW(TAG, "stop ignored: the Muse link runs until restart");
}

esp_err_t muse_send_ctrl(void* /*impl*/, const uint8_t* frame, size_t len) {
    // Responses are the core answering the frames DeliverText made up: nobody waits for them.
    // Events (battery, prompts, the manifest) have no reader on a Muse.
    if (len >= agentlink::kHeaderSize && (frame[1] & 0x7F) == agentlink::kMsgResponse) return ESP_OK;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t muse_stream_start(void* /*impl*/, agent_stream_t type, const uint8_t* /*meta*/, size_t /*meta_len*/) {
    if (type != AGENT_STREAM_VOICE) return ESP_ERR_NOT_SUPPORTED;
    if (!muse_note_ready()) return ESP_ERR_INVALID_STATE;
    if (s_voice_busy.exchange(true)) return ESP_ERR_INVALID_STATE;   // the last note is still going up
    s_cut.store(false);
    s_dropped.store(0);
    if (!PostVoice(VoiceCmd::kBegin)) {
        s_voice_busy.store(false);
        return ESP_ERR_TIMEOUT;
    }
    s_voice_open.store(true);
    return ESP_OK;
}

esp_err_t muse_send_stream(void* /*impl*/, agent_stream_t type, const uint8_t* data, size_t len) {
    if (type != AGENT_STREAM_VOICE) return ESP_ERR_NOT_SUPPORTED;
    if (!s_voice_open.load()) return ESP_ERR_INVALID_STATE;
    // A full buffer means the uplink is a whole buffer behind: the note ends where it got to, as a
    // gap in the middle would garble it. The board's mic task is the only writer, so the space
    // checked here can only grow before the send.
    if (s_cut.load() || xStreamBufferSpacesAvailable(s_pcm) < len) {
        s_cut.store(true);
        s_dropped.fetch_add(static_cast<uint32_t>(len));
        return ESP_ERR_NO_MEM;
    }
    xStreamBufferSend(s_pcm, data, len, 0);
    return ESP_OK;
}

esp_err_t muse_stream_end(void* /*impl*/, agent_stream_t type, bool complete,
                          const uint8_t* /*meta*/, size_t /*meta_len*/) {
    if (type != AGENT_STREAM_VOICE) return ESP_ERR_NOT_SUPPORTED;
    if (!s_voice_open.exchange(false)) return ESP_ERR_INVALID_STATE;
    if (PostVoice(complete ? VoiceCmd::kEnd : VoiceCmd::kCancel)) return ESP_OK;
    s_voice_busy.store(false);   // the voice task will not hear of this stream again
    return ESP_ERR_TIMEOUT;
}

bool muse_is_ready(void* /*impl*/) {
    return muse_gadget_online();
}

agent_transport_t s_muse = {
    .start        = muse_start,
    .stop         = muse_stop,
    .send_ctrl    = muse_send_ctrl,
    .stream_start = muse_stream_start,
    .send_stream  = muse_send_stream,
    .stream_end   = muse_stream_end,
    .is_ready     = muse_is_ready,
    .impl         = nullptr,
};
}  // namespace

extern "C" agent_transport_t* agent_transport_muse(void) { return &s_muse; }
extern "C" void agent_transport_muse_set_recv(void (*cb)(const uint8_t*, size_t)) { s_on_recv = cb; }
extern "C" void agent_transport_muse_set_state(void (*cb)(agent_state_t)) { s_on_state = cb; }
extern "C" void agent_transport_muse_set_status(void (*cb)(const agent_link_status_t*)) { s_on_status = cb; }
extern "C" void agent_transport_muse_set_audio(void (*pcm)(const uint8_t*, size_t), void (*end)(void)) {
    s_on_pcm = pcm;
    s_on_audio_end = end;
}

extern "C" esp_err_t agent_transport_muse_forget(void) {
    if (!s_started) return ESP_ERR_INVALID_STATE;
    muse_gadget_forget();
    return ESP_OK;
}

extern "C" esp_err_t agent_transport_muse_confirm(void) {
    if (!s_started) return ESP_ERR_INVALID_STATE;
    return muse_gadget_confirm_press() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

#else  // !CONFIG_AGENT_LINK_TRANSPORT_MUSE

// Not selected: the core still names these in its transport switch.
extern "C" agent_transport_t* agent_transport_muse(void) { return nullptr; }
extern "C" void agent_transport_muse_set_recv(void (*)(const uint8_t*, size_t)) {}
extern "C" void agent_transport_muse_set_state(void (*)(agent_state_t)) {}
extern "C" void agent_transport_muse_set_status(void (*)(const agent_link_status_t*)) {}
extern "C" void agent_transport_muse_set_audio(void (*)(const uint8_t*, size_t), void (*)(void)) {}
extern "C" esp_err_t agent_transport_muse_forget(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_transport_muse_confirm(void) { return ESP_ERR_INVALID_STATE; }

#endif  // CONFIG_AGENT_LINK_TRANSPORT_MUSE
