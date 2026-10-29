// Exercise the actual Call implementation. Codec mocks enforce a heap that
// can hold either state, never both; this is a lifecycle regression, not a
// measurement of the vendor library or physical-board acceptance.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include "realtime_audio.h"
#include "realtime_audio_json.h"
#include "realtime_format.h"
#include "realtime_end_log.h"
#include "realtime_ws_frame.h"
#include "realtime_ws_stream.h"
#include "cloud_limits.h"
#include "realtime_codec_memory.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
using namespace agentlink::cloud_limits;
using agentlink::realtime::AudioFormat;
using agentlink::realtime::ReadAudioFormat;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define WS_TRANSPORT_OPCODES_BINARY 2
constexpr size_t kMicBytes = 640, kDecodePcmBytes = 640;
constexpr size_t kUplinkBatchBytes = kMicBytes * kUplinkBatchPackets;
constexpr unsigned kOpusBatchFrames = kEncodedDepth < 3 ? kEncodedDepth : 3;
constexpr char kAudioPrefix[] = "{\"type\":\"input_audio.append\",\"audio\":\"";
constexpr size_t kAudioJsonBytes = sizeof(kAudioPrefix) - 1 + (kUplinkBatchBytes + 2) / 3 * 4 + 3;
// INSERT_TYPES
using TickType_t = unsigned;
struct Queue { size_t depth, item_size; std::deque<std::vector<uint8_t>> items; };
using QueueHandle_t = Queue*;
unsigned live_queues = 0;
QueueHandle_t xQueueCreate(size_t depth, size_t item_size) { ++live_queues; return new Queue{depth, item_size, {}}; }
QueueHandle_t xQueueCreateWithCaps(size_t, size_t, unsigned) { return nullptr; }
void vQueueDelete(QueueHandle_t queue) { --live_queues; delete queue; }
void vQueueDeleteWithCaps(QueueHandle_t) { assert(false); }
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return static_cast<unsigned>(q->items.size()); }
unsigned uxQueueSpacesAvailable(QueueHandle_t q) { return static_cast<unsigned>(q->depth - q->items.size()); }
int xQueueSend(QueueHandle_t q, const void* value, unsigned) {
    if (!uxQueueSpacesAvailable(q)) return 0;
    const auto* p = static_cast<const uint8_t*>(value);
    q->items.emplace_back(p, p + q->item_size); return pdTRUE;
}
int xQueuePeek(QueueHandle_t q, void* value, unsigned) {
    if (q->items.empty()) return 0;
    memcpy(value, q->items.front().data(), q->item_size); return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void* value, unsigned wait) {
    if (!xQueuePeek(q, value, wait)) return 0;
    q->items.pop_front(); return pdTRUE;
}
void xQueueReset(QueueHandle_t q) { q->items.clear(); }
thread_local void* codec_test_task = reinterpret_cast<void*>(1);
size_t largest_system_block = 100000;
unsigned workspace_reservations = 0;
uint8_t* reservation = nullptr;
void* codec_test_caps_malloc(size_t bytes, unsigned caps) {
    if (!(caps & MALLOC_CAP_INTERNAL) || bytes > largest_system_block) return nullptr;
    assert(bytes == agentlink::realtime::CodecWorkspace::kBytes);
    ++workspace_reservations;
    reservation = static_cast<uint8_t*>(malloc(bytes)); return reservation;
}
unsigned uxTaskGetStackHighWaterMark(void*) { return 4096; }
void vTaskDelete(void*) {}
void vTaskDeleteWithCaps(void*) {}
int64_t now_us = 0;
int64_t esp_timer_get_time() { return now_us; }
bool actual_level = false;
size_t queued_pcm = 0;
unsigned playback_waits = 0;
void vTaskDelay(unsigned ticks) {
    now_us += ticks * 1000;
    if (actual_level) {
        ++playback_waits;
        if (playback_waits > 20) queued_pcm = queued_pcm > ticks * 32 ? queued_pcm - ticks * 32 : 0;
    }
}
std::mutex s_mutex;
QueueHandle_t s_tx = nullptr;
std::atomic<bool> s_ready{true}, s_mute{false}, s_recovering{false};
std::atomic<uint32_t> s_reset{0}, s_encoded_pending{0}, s_downlink_bytes{0};
std::atomic<uint32_t> s_media_stack{0}, s_encoder_stack{0}, s_decoded_samples{0}, s_tx_encode_max_us{0};
bool Current(uint32_t epoch) { return epoch == 1; }
const char* cloud_cred_auth_key() { return "fixture-auth-key"; }
// INSERT_RECOVERY_STATE
namespace agentlink::audio {
unsigned BufferMs() { return 256; }
bool BufferLevelBytes(size_t* bytes) { *bytes = queued_pcm; return actual_level; }
}
struct Callbacks {
    void (*state)(bool, const char*) = nullptr;
    void (*text)(const char*) = nullptr;
    void (*pcm)(const uint8_t*, size_t) = nullptr;
    void (*audio_end)() = nullptr;
    void (*audio_reset)() = nullptr;
} s_cb;
struct WsSender {
    int tls = 0;
    uint8_t* wire;
    size_t capacity;
    agentlink::realtime::PendingWrite pending;
    unsigned audio_frames = 0, audio_messages = 0;
    size_t audio_bytes = 0;
};
bool network_writable = true, select_writable = true;
unsigned sent_packets = 0;
std::vector<uint8_t> last_wire;
uint8_t last_audio = 0;
int PollWritable(int) { return select_writable ? 1 : 0; }
void LogTransportFailure(int, const char*, int, int) {}
bool WriteWire(WsSender& ws, size_t bytes) { return ws.pending.Begin(bytes, now_us / 1000); }
bool PumpWire(WsSender& ws) {
    if (!ws.pending.Busy() || !network_writable) return true;
    const size_t bytes = ws.pending.Remaining();
    assert(ws.pending.Advance(static_cast<int>(bytes), false, now_us / 1000));
    last_wire.assign(ws.wire, ws.wire + bytes); ++sent_packets;
    size_t offset = 0;
    while (offset < bytes) {
        const size_t n = ws.wire[offset + 1] & 127;
        const size_t payload = n == 126 ? (size_t(ws.wire[offset + 2]) << 8) | ws.wire[offset + 3] : n;
        const size_t header = n == 126 ? 8 : 6;
        assert(header + payload <= bytes - offset);
        std::string json(payload, '\0');
        for (size_t i = 0; i < payload; ++i)
            json[i] = char(ws.wire[offset + header + i] ^ ws.wire[offset + header - 4 + i % 4]);
        cJSON* body = cJSON_Parse(json.c_str()); assert(body);
        const cJSON* audio = cJSON_GetObjectItemCaseSensitive(body, "audio");
        if (cJSON_IsString(audio)) {
            uint8_t decoded[1275]; size_t got = 0;
            assert(!mbedtls_base64_decode(decoded, sizeof decoded, &got,
                reinterpret_cast<const uint8_t*>(audio->valuestring), strlen(audio->valuestring)) && got);
            last_audio = decoded[0];
        }
        cJSON_Delete(body); offset += header + payload;
    }
    return true;
}
size_t AppendFrame(WsSender& ws, size_t offset, const char* data, size_t bytes) {
    const uint8_t mask[] = {1, 2, 3, 4};
    return agentlink::realtime::EncodeTextFrame(data, bytes, mask, ws.wire + offset, ws.capacity - offset);
}
bool Send(WsSender& ws, const char* text) {
    const size_t n = AppendFrame(ws, 0, text, strlen(text)); return n && WriteWire(ws, n);
}
using esp_audio_err_t = int;
constexpr int ESP_AUDIO_ERR_OK = 0, ESP_OPUS_DEC_FRAME_DURATION_20_MS = 3;
struct esp_opus_dec_cfg_t { int sample_rate, channel, frame_duration; bool self_delimited; };
struct esp_opus_enc_config_t { int sample_rate, channel, bitrate; bool enable_vbr; int complexity; };
#define ESP_OPUS_ENC_CONFIG_DEFAULT() {}
struct esp_audio_enc_in_frame_t { uint8_t* buffer; size_t len; };
struct esp_audio_enc_out_frame_t { uint8_t* buffer; uint32_t len, encoded_bytes; };
struct esp_audio_dec_in_raw_t { uint8_t* buffer; uint32_t len, consumed; };
struct esp_audio_dec_out_frame_t { uint8_t* buffer; uint32_t len, decoded_size; };
struct esp_audio_dec_info_t { int sample_rate, channel; };
bool encoder_live = false, decoder_live = false, fail_encoder = false, fail_decoder = false;
unsigned encodes = 0, decodes = 0, pcm_callbacks = 0;
extern "C" void* media_lib_module_malloc(const char*, size_t);
extern "C" void* media_lib_module_calloc(const char*, size_t, size_t);
extern "C" void media_lib_free(void*);
struct CodecHandle { void* core; };
bool expect_reserved = true;
bool InReservation(void* ptr) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(ptr), begin = reinterpret_cast<uintptr_t>(reservation);
    return value >= begin && value - begin < agentlink::realtime::CodecWorkspace::kBytes;
}
int CreateCodec(void** handle, size_t wrapper, size_t core) {
    auto* memory = static_cast<CodecHandle*>(media_lib_module_calloc("OPUS", 1, wrapper));
    if (!memory) return -1;
    memory->core = media_lib_module_malloc("OPUS", core);
    if (!memory->core) { media_lib_free(memory); return -1; }
    if (expect_reserved) assert(InReservation(memory) && InReservation(memory->core));
    *handle = memory; return 0;
}
void DestroyCodec(void* handle) {
    media_lib_free(static_cast<CodecHandle*>(handle)->core); media_lib_free(handle);
}
int esp_opus_enc_open(void*, size_t, void** handle) {
    assert(!encoder_live); if (decoder_live || fail_encoder) return -1;
    if (CreateCodec(handle, 44, 24524) != 0) return -1;
    encoder_live = true; return 0;
}
void esp_opus_enc_close(void* handle) { assert(encoder_live); DestroyCodec(handle); encoder_live = false; }
int esp_opus_enc_get_frame_size(void*, int* input, int* output) { *input = 640; *output = 1275; return 0; }
int esp_opus_enc_process(void* handle, esp_audio_enc_in_frame_t* input, esp_audio_enc_out_frame_t* output) {
    assert(handle && encoder_live && !decoder_live);
    assert(input->len == 640 && output->len >= 3); ++encodes;
    const bool silence = std::all_of(input->buffer, input->buffer + input->len, [](uint8_t v) { return !v; });
    output->buffer[0] = silence ? 0xf8 : 0xa5;
    output->buffer[1] = 0xff; output->buffer[2] = 0xfe;
    output->encoded_bytes = input->buffer[0] == 0x77 ? 1275 : 3;
    if (output->encoded_bytes > 3) memset(output->buffer + 3, 0x77, output->encoded_bytes - 3);
    return 0;
}
int esp_opus_dec_open(void* config, size_t, void** handle) {
    const auto& cfg = *static_cast<esp_opus_dec_cfg_t*>(config);
    assert(cfg.sample_rate == 16000 && cfg.channel == 1 && cfg.frame_duration == 3);
    assert(!decoder_live); if (encoder_live) return -1;
    if (fail_decoder) {
        // The vendor can fail before publishing a handle, after allocating its
        // wrapper. The production transaction must reclaim such allocations.
        assert(media_lib_module_calloc("OPUS", 1, 8)); return -1;
    }
    if (CreateCodec(handle, 8, 17784) != 0) return -1;
    decoder_live = true; return 0;
}
void esp_opus_dec_close(void* handle) { assert(decoder_live); DestroyCodec(handle); decoder_live = false; }
int esp_opus_dec_decode(void*, esp_audio_dec_in_raw_t* raw, esp_audio_dec_out_frame_t* frame, esp_audio_dec_info_t* info) {
    assert(decoder_live && !encoder_live && frame->len == 640);
    if (raw->len != 3) return -1;
    raw->consumed = raw->len; frame->decoded_size = 640; memset(frame->buffer, 0, 640);
    info->sample_rate = 16000; info->channel = 1; ++decodes; return 0;
}
// INSERT_CALL

void Event(Call& call, const char* value) {
    std::vector<uint8_t> message(value, value + strlen(value));
    assert(call.Deliver(1, message.data(), message.size(), now_us, s_reset));
}
void Capture(Call& call, WsSender& sender, TxScratch& scratch, unsigned count, uint8_t value) {
    Tx input{}; input.kind = TxKind::kPcm; input.bytes = 640; input.turn = s_reset;
    memset(input.data, value, sizeof input.data);
    for (unsigned i = 0; i < count; ++i) {
        assert(xQueueSend(s_tx, &input, 0) == pdTRUE);
        assert(call.Pump());
        assert(PumpWire(sender));
        assert(call.FlushTx(sender, &scratch));
        assert(PumpWire(sender)); now_us += 20000;
    }
}
size_t json_allocations = 0;
void* JsonMalloc(size_t bytes) { ++json_allocations; return malloc(bytes); }
void TestBorrowedAudio() {
    for (const std::string& text : {
        std::string(R"({"type":"response.audio.delta","audio":"+P/+"})"),
        std::string(R"({"audio":"+P\u002f+","metadata":{"audio":"wrong","values":[true,false,null,-1.25e+2,{"u":"\ud83d\ude00"}]},"type":"response.audio.delta"})"),
        std::string(R"({"type":"response.audio.delta","audio":"+P\/+","audio":"ignored"})"),
        std::string(R"({"type":"response.audio.delta","audio":""})"),
        std::string(R"({"type":"response.audio.delta","audio":"+P/+","padding":")") + std::string(16000, 'x') + "\"}"
    }) {
        std::vector<uint8_t> copy(text.begin(), text.end());
        uint8_t* audio = nullptr; size_t bytes = 0;
        const size_t before = json_allocations;
        assert(agentlink::realtime::BorrowAudioDelta(copy.data(), copy.size(), &audio, &bytes));
        assert(json_allocations == before && audio >= copy.data() && audio + bytes <= copy.data() + copy.size());
        cJSON* parsed = cJSON_ParseWithLength(text.data(), text.size()); assert(parsed);
        const char* expected = cJSON_GetObjectItemCaseSensitive(parsed, "audio")->valuestring;
        assert(bytes == strlen(expected) && memcmp(audio, expected, bytes) == 0);
        cJSON_Delete(parsed);
    }
    for (const char* value : {
        R"({"type":"transcript.final","text":"hello"})",
        R"({"nested":{"type":"response.audio.delta","audio":"+P/+"}})",
        R"({"type":"other","type":"response.audio.delta","audio":"+P/+"})",
        R"({"type":"response.audio.delta","audio":null,"audio":"+P/+"})",
        R"({"type":"response.audio.delta","audio":"+P/+",})",
        R"({"type":"response.audio.delta","audio":"+P/+"} trailing)",
        R"({"type":"response.audio.delta","audio":"+P/+","n":01})",
        R"({"type":"response.audio.delta","audio":"+P/+","n":1e})",
        R"({"type":"response.audio.delta","audio":"+P/+","n":[true,]})",
        R"({"type":"response.audio.delta","audio":"+P/+","n":"\ud800"})",
        R"({"type":"response.audio.delta","audio":"\u0000+P/+"})",
        R"({"type":"response.audio.delta","audio":"\u4e2d"})",
        R"({"type":"response.audio.delta","au\u0064io":"+P/+"})",
        R"({"type":"response.audio.delta","audio":"\x30"})",
        R"({"type":"response.audio.delta","audio":"+P/+)"
    }) {
        const std::string original(value);
        for (size_t length = 0; length <= original.size(); ++length) {
            std::vector<uint8_t> copy(original.begin(), original.begin() + length);
            copy.push_back(0xa5);
            uint8_t* audio = nullptr; size_t bytes = 0;
            const bool borrowed = agentlink::realtime::BorrowAudioDelta(copy.data(), length, &audio, &bytes);
            if (borrowed) {
                // A valid object followed by garbage has a valid prefix.
                assert(length < original.size());
                cJSON* parsed = cJSON_ParseWithLength(original.data(), length); assert(parsed);
                const char* expected = cJSON_GetObjectItemCaseSensitive(parsed, "audio")->valuestring;
                assert(strlen(expected) == bytes && memcmp(audio, expected, bytes) == 0);
                cJSON_Delete(parsed);
            } else assert(std::equal(original.begin(), original.begin() + length, copy.begin()));
            assert(copy[length] == 0xa5);
        }
    }
}
int main() {
    for (const char* terminal : {
        R"({"type":"error","error":{"code":"upstream_error","message":"upstream unavailable"}})",
        R"({"type":"session.error","code":401,"reason":"token expired"})",
        R"({"type":"session.ended","reason":"duration exceeded"})"}) {
        s_ready = true;
        Call call(1, {true, true}, true);
        Event(call, terminal);
        assert(!call.Healthy() && !call.TransportFailed()); // Still intentionally non-retryable.
    }
    s_ready = true;
    cJSON_Hooks hooks{JsonMalloc, free}; cJSON_InitHooks(&hooks);
    TestBorrowedAudio();
    // Verify in-place decoding against the same mbedTLS base64.c used by IDF,
    // including lengths/padding, whitespace, invalid data and 12 KB audio.
    for (size_t n : {size_t(1), size_t(2), size_t(3), size_t(1275), size_t(12285)}) {
        std::vector<uint8_t> source(n); for (size_t i = 0; i < n; ++i) source[i] = uint8_t(i * 37);
        std::vector<uint8_t> text((n + 2) / 3 * 4 + 1);
        size_t encoded = 0, decoded = 0;
        assert(!mbedtls_base64_encode(text.data(), text.size(), &encoded, source.data(), n));
        assert(!mbedtls_base64_decode(text.data(), encoded, &decoded, text.data(), encoded));
        assert(decoded == n && std::equal(source.begin(), source.end(), text.begin()));
    }
    uint8_t whitespace[] = "YQ==\r\n "; size_t got = 0;
    assert(!mbedtls_base64_decode(whitespace, sizeof whitespace, &got, whitespace, sizeof whitespace - 1) && got == 1);
    uint8_t invalid[] = "a=bc";
    assert(mbedtls_base64_decode(invalid, sizeof invalid, &got, invalid, 4) != 0);
    s_tx = xQueueCreate(kTxDepth, sizeof(Tx));
    TxScratch scratch{};
    WsSender sender{0, scratch.wire, sizeof scratch.wire};
    s_cb.pcm = [](const uint8_t*, size_t bytes) { assert(bytes == 640); ++pcm_callbacks; };
    const unsigned baseline = live_queues;
    {
        Call call(1, {true, true}, true);
        assert(call.Prepare() && encoder_live && !decoder_live && live_queues == baseline + 1);
        assert(!media_lib_module_malloc("OPUS", 4096)); // no fallback if the reservation is exhausted
        assert(!media_lib_module_calloc("OPUS", SIZE_MAX, 2));
        Event(call, "{\"type\":\"session.ready\"}");
        assert(call.Healthy() && encoder_live && !decoder_live);
        // Model the observed failure: the general heap no longer has a block
        // big enough for an encoder. Every transition must use the reservation.
        largest_system_block = 18432;
        std::thread unrelated([] {
            codec_test_task = reinterpret_cast<void*>(2);
            void* memory = media_lib_module_malloc("OTHER", 256);
            assert(memory && !InReservation(memory)); media_lib_free(memory);
        });
        unrelated.join();
        const unsigned reserved_once = workspace_reservations;
        for (unsigned reply = 0; reply < 1000; ++reply) {
            Capture(call, sender, scratch, 10, 0x55);
            assert(encoder_live && !decoder_live && last_audio == 0xa5);
            Event(call, "{\"type\":\"response.started\"}");
            assert(call.Healthy() && !encoder_live && decoder_live && !s_encoded_pending);
            const unsigned before = encodes;
            Capture(call, sender, scratch, 10, 0x7f); // echo must become cached silence
            assert(encodes == before && sent_packets > 0);
            assert(last_audio == 0xf8);
            const size_t before_json = json_allocations;
            Event(call, "{\"type\":\"response.audio.delta\",\"audio\":\"+P/+\"}");
            assert(json_allocations == before_json);
            assert(call.Healthy() && decoder_live);
            Event(call, "{\"type\":\"response.audio.done\"}");
            assert(call.Healthy() && encoder_live && !decoder_live && live_queues == baseline + 1);
            assert(workspace_reservations == reserved_once);
        }
        assert(pcm_callbacks == 1000 && decodes == 1000);
        // A real queue can stay full while prebuffering, despite elapsed wall
        // time. Do not hand PCM to the sink until the queue actually drains.
        Event(call, "{\"type\":\"response.started\"}");
        actual_level = true; queued_pcm = 8192; playback_waits = 0;
        s_cb.pcm = [](const uint8_t*, size_t bytes) {
            assert(queued_pcm + bytes <= 6144); queued_pcm += bytes; ++pcm_callbacks;
        };
        Event(call, "{\"type\":\"response.audio.delta\",\"audio\":\"+P/+\"}");
        assert(call.Healthy() && playback_waits > 20 && queued_pcm <= 6144);
        actual_level = false;
        s_cb.pcm = [](const uint8_t*, size_t bytes) { assert(bytes == 640); ++pcm_callbacks; };
        Event(call, "{\"type\":\"response.audio.done\"}");
        // A legal maximum-size Opus packet still fits even though two such
        // packets cannot share one wire buffer. Leave the second queued.
        Capture(call, sender, scratch, 6, 0x77);
        assert(call.Healthy());
        while (s_encoded_pending) {
            now_us += 40000;
            assert(PumpWire(sender));
            assert(call.FlushTx(sender, &scratch));
            assert(PumpWire(sender));
        }
        // A conservative select() result must not gate a small nonblocking write.
        select_writable = false;
        const unsigned before_low_water = sent_packets;
        Capture(call, sender, scratch, 6, 0x22);
        assert(sent_packets > before_low_water && call.Healthy());
        select_writable = true;
        // Bounded congestion: retain queued PCM and keep decoding while TX is blocked.
        network_writable = false;
        Capture(call, sender, scratch, 6, 0x33);
        assert(sender.pending.Busy() && s_encoded_pending <= kEncodedDepth &&
               uxQueueMessagesWaiting(s_tx) <= kTxDepth);
        Event(call, "{\"type\":\"response.started\"}");
        assert(!s_encoded_pending && !uxQueueMessagesWaiting(s_tx));
        Event(call, "{\"type\":\"response.audio.delta\",\"audio\":\"+P/+\"}");
        assert(call.Healthy()); network_writable = true;
        assert(PumpWire(sender));
        // Cancellation must release decoding state before restarting the encoder.
        ++s_reset; assert(call.Pump());
        assert(encoder_live && !decoder_live);
        // Missing response.started still selects the correct codec on first audio.
        uint8_t packet[] = {0xf8, 0xff, 0xfe};
        assert(call.Deliver(2, packet, sizeof packet, now_us, s_reset));
        assert(call.Healthy() && decoder_live && !encoder_live);
        fail_encoder = true;
        Event(call, "{\"type\":\"response.audio.done\"}");
        assert(!call.Healthy() && !encoder_live && !decoder_live);
        fail_encoder = false;
    }
    assert(!encoder_live && !decoder_live && live_queues == baseline);
    largest_system_block = 100000;
    {
        Call call(1, {true, true}, true); assert(call.Prepare());
        fail_decoder = true; Event(call, "{\"type\":\"response.started\"}");
        assert(!call.Healthy() && !encoder_live && !decoder_live); fail_decoder = false;
    }
    assert(live_queues == baseline);
    {
        // The initial ready event can refine the grant's audio settings. It
        // must release the prepared decoder before selecting encoder-only mode.
        s_ready = false;
        expect_reserved = false;
        Call call(1, {false, true}, true); assert(call.Prepare() && decoder_live && !encoder_live);
        expect_reserved = true;
        Event(call, R"({"type":"session.ready","audio":{"input":{"encoding":"opus","sampleRate":16000,"channels":1},"output":{"encoding":"opus","sampleRate":24000,"channels":1}}})");
        assert(call.Healthy() && s_ready && encoder_live && !decoder_live);
        Event(call, "{\"type\":\"response.started\"}");
        assert(call.Healthy() && decoder_live && !encoder_live);
    }
    assert(!encoder_live && !decoder_live && live_queues == baseline);
    // Valid first-ready refinements away from Opus must also release the
    // previously prepared state; a mixed format cannot occupy both pool states.
    for (const char* ready : {
        R"({"type":"session.ready","audio":{"input":{"encoding":"pcm16","sampleRate":16000,"channels":1},"output":{"encoding":"opus","sampleRate":24000,"channels":1}}})",
        R"({"type":"session.ready","audio":{"input":{"encoding":"opus","sampleRate":16000,"channels":1},"output":{"encoding":"pcm16","sampleRate":24000,"channels":1}}})"
    }) {
        s_ready = false;
        Call call(1, {true, true}, true); assert(call.Prepare());
        Event(call, ready);
        assert(call.Healthy() && s_ready && (encoder_live != decoder_live));
    }
    assert(!encoder_live && !decoder_live && live_queues == baseline);
    // Early admission fails cleanly when the fixed block cannot be reserved.
    largest_system_block = 18432;
    {
        Call call(1, {true, true}, true);
        assert(!call.Prepare() && !encoder_live && !decoder_live);
    }
    largest_system_block = 100000;
    // Exercise the allocator registry while another task allocates/frees and
    // the voice worker repeatedly publishes and destroys its reservation.
    std::atomic<bool> keep_allocating{true}, foreign_started{false};
    std::thread foreign([&] {
        codec_test_task = reinterpret_cast<void*>(2);
        foreign_started.store(true);
        while (keep_allocating.load()) {
            void* memory = media_lib_module_calloc("OTHER", 4, 64);
            assert(memory); media_lib_free(memory);
            std::this_thread::yield();
        }
    });
    while (!foreign_started.load()) std::this_thread::yield();
    for (unsigned call_number = 0; call_number < 50; ++call_number) {
        Call call(1, {true, true}, true);
        assert(call.Prepare() && encoder_live && !decoder_live);
        Event(call, "{\"type\":\"response.started\"}");
        assert(call.Healthy() && decoder_live && !encoder_live);
        Event(call, "{\"type\":\"response.audio.done\"}");
        assert(call.Healthy() && encoder_live && !decoder_live);
    }
    keep_allocating.store(false); foreign.join();
    assert(!encoder_live && !decoder_live && live_queues == baseline);
    vQueueDelete(s_tx);
    printf("buffer payload bytes: mic=%zu encoded=%zu scratch=%zu decode=%zu\n",
           sizeof(Tx), sizeof(EncodedTx), sizeof(TxScratch), kDecodePcmBytes);
    puts("production C3 memory: 1000 replies on one reservation with an 18 KB fragmented heap, 50 call teardowns concurrent with foreign allocations; real-queue pacing, silence, congestion, interruption, codec/admission failures; IDF in-place base64 passed");
}
