#include "audio_downlink.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "adpcm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace agentlink {
namespace audio {
namespace {

constexpr const char* TAG = "agent_link.audio";

constexpr uint8_t kEvtAudioFlowControl = 0x20;   // Notify 0xFFC4, payload 4B
constexpr uint8_t kFlowResume = 0;
constexpr uint8_t kFlowPause  = 1;

// PCM16 16kHz mono = 32 bytes per millisecond. Every buffer figure below is in ms.
constexpr uint32_t kBytesPerMs = 32;

// Watermarks: capped by whatever the board can actually hold.
// A board with a genuinely small buffer - no PSRAM, or a different product
// gets the same shape scaled down instead of being told to run seconds ahead of a buffer that would drop every one of those bytes.
constexpr uint32_t kDefaultBufferMs = 8192;             // default (256KB PSRAM)
constexpr uint32_t kProdPauseMs  = 5000;                // pause at 5.0s buffered
constexpr uint32_t kProdResumeMs = 1500;                // resume at 1.5s
constexpr uint32_t kPauseNum  = 5, kPauseDen  = 8;      // fallback cap: 62.5% of capacity
constexpr uint32_t kResumeNum = 3, kResumeDen = 16;     // fallback cap: 18.75% (same ratios)
constexpr uint32_t kNearlyDryMs = 256;  // re-assert RESUME while the buffer is this empty

constexpr uint32_t kTickMs       = 200;   // ticker period; also the flip repeat spacing
constexpr int      kFlipRepeats  = 2;     // extra sends after a state flip (3 total)
constexpr uint32_t kReassertMs   = 2000;  // periodic re-assert, in case a notify was lost

PcmSink   s_pcm = nullptr;
EventSink s_evt = nullptr;

std::mutex s_mtx;                 // guards the flow-control bookkeeping below
std::atomic<uint8_t> s_codec{kCodecPcm16};
std::atomic<bool>    s_armed{false};

// Flow control: how far ahead of real time we have pushed audio at the board.
bool     s_paused = false;
int      s_repeats = 0;
int64_t  s_base_us = 0;           // start of the current delivery accounting window
uint64_t s_delivered = 0;         // PCM bytes handed to the board since s_base_us
int64_t  s_last_send_us = 0;
std::atomic<uint32_t> s_buffer_ms{kDefaultBufferMs};   // board capacity (set once at init)

esp_timer_handle_t s_ticker = nullptr;

// ADPCM reassembly. SDU boundaries have nothing to do with block boundaries, so partial blocks
// must survive across calls. No codec state is kept between blocks - each one is self-contained.
uint8_t* s_block = nullptr;       // kBlockBytes staging
size_t   s_block_len = 0;
uint8_t* s_decoded = nullptr;     // kPcmBytesPerBlock output

int64_t NowUs() { return esp_timer_get_time(); }

// How far ahead of real time we have pushed, in ms: "PCM delivered" minus "wall time elapsed".
// That difference is exactly what the board's play buffer has to hold, and it is the quantity
// the App controls - which makes it the right thing to meter, without the board reporting
// anything. Valid because a speaker drains PCM16/16kHz at a fixed 32 bytes per ms.
// Must be called with s_mtx held.
uint32_t BufferedMsLocked() {
    const int64_t now = NowUs();
    const int64_t elapsed_ms = (now - s_base_us) / 1000;
    const int64_t have_ms    = static_cast<int64_t>(s_delivered / kBytesPerMs);
    int64_t backlog = have_ms - elapsed_ms;
    if (backlog <= 0) {
        // Drained (or the board dropped what we gave it). Re-baseline instead of letting the
        // estimate go permanently negative, which would suppress PAUSE for the rest of the
        // session after any underrun.
        s_base_us = now;
        s_delivered = 0;
        backlog = 0;
    }
    return static_cast<uint32_t>(backlog);
}

// 0x20 AudioFlowControl: state(1) + buffered_ms(2 LE) + reserved(1).
void SendFlow(uint8_t state, uint32_t buffered_ms) {
    if (!s_evt) return;
    const uint16_t ms = (buffered_ms > 0xFFFF) ? 0xFFFF : static_cast<uint16_t>(buffered_ms);
    const uint8_t payload[4] = {
        state,
        static_cast<uint8_t>(ms & 0xFF),
        static_cast<uint8_t>((ms >> 8) & 0xFF),
        0,
    };
    s_evt(kEvtAudioFlowControl, payload, sizeof(payload));
}

// Ticker: decide PAUSE/RESUME and handle the repeat / re-assert policy. Runs on the esp_timer
// task, so it may notify but must stay quick.
void Tick(void*) {
    if (!s_armed.load(std::memory_order_acquire)) return;

    uint8_t  state;
    uint32_t ms;
    bool     send = false;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        ms = BufferedMsLocked();
        const int64_t now = NowUs();
        const int64_t since_ms = (now - s_last_send_us) / 1000;
        const uint32_t cap = s_buffer_ms.load(std::memory_order_acquire);
        const uint32_t cap_pause  = cap * kPauseNum / kPauseDen;
        const uint32_t cap_resume = cap * kResumeNum / kResumeDen;
        const uint32_t pause_ms  = (kProdPauseMs  < cap_pause)  ? kProdPauseMs  : cap_pause;
        const uint32_t resume_ms = (kProdResumeMs < cap_resume) ? kProdResumeMs : cap_resume;

        if (!s_paused && ms >= pause_ms) {
            s_paused = true;  s_repeats = kFlipRepeats;  send = true;
        } else if (s_paused && ms <= resume_ms) {
            s_paused = false; s_repeats = kFlipRepeats;  send = true;
        } else if (s_repeats > 0) {
            --s_repeats;      send = true;               // flips go out 3x, 200ms apart
        } else if (since_ms >= static_cast<int64_t>(kReassertMs)) {
            // Re-assert periodically: a dropped PAUSE notify would let the App flood us, and a
            // dropped RESUME would leave it muted forever. The payload is absolute state, so
            // repeating it is idempotent.
            send = s_paused || (ms < kNearlyDryMs);
        }
        if (send) s_last_send_us = now;
        state = s_paused ? kFlowPause : kFlowResume;
    }
    if (send) SendFlow(state, ms);
}

void EnsureTicker() {
    if (s_ticker) return;
    const esp_timer_create_args_t args = {
        .callback = &Tick,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "agentlink_flow",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_ticker) != ESP_OK) {
        ESP_LOGE(TAG, "flow-control timer create failed; the App will not be throttled");
        s_ticker = nullptr;
    }
}

// Hand decoded PCM to the board and count it toward the backlog estimate.
void Deliver(const uint8_t* pcm, size_t bytes) {
    if (!s_pcm || !pcm || bytes == 0) return;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_delivered += bytes;
    }
    s_pcm(pcm, bytes);
}

}  // namespace

void SetSinks(PcmSink pcm, EventSink evt) {
    s_pcm = pcm;
    s_evt = evt;
}

bool SetCodec(uint8_t codec) {
    if (codec > kCodecImaAdpcm) return false;
    // Sticky for the connection; a change while a session is live is ignored (the App is told
    // as much), because the decoder cannot switch formats mid-stream.
    if (s_armed.load(std::memory_order_acquire)) {
        if (codec != s_codec.load(std::memory_order_acquire))
            ESP_LOGW(TAG, "codec change to %u ignored mid-session", codec);
        return true;
    }
    if (codec != s_codec.exchange(codec, std::memory_order_acq_rel))
        ESP_LOGI(TAG, "downlink codec = %s", codec == kCodecImaAdpcm ? "IMA-ADPCM" : "PCM16");
    return true;
}

uint8_t CodecValue() { return s_codec.load(std::memory_order_acquire); }

void Arm() {
    if (s_codec.load(std::memory_order_acquire) == kCodecImaAdpcm) {
        // Staging buffers only exist for the ADPCM path. PSRAM if there is any; 1.3KB total.
        if (!s_block) {
            s_block = static_cast<uint8_t*>(heap_caps_malloc(adpcm::kBlockBytes, MALLOC_CAP_SPIRAM));
            if (!s_block) s_block = static_cast<uint8_t*>(malloc(adpcm::kBlockBytes));
        }
        if (!s_decoded) {
            s_decoded = static_cast<uint8_t*>(heap_caps_malloc(adpcm::kPcmBytesPerBlock, MALLOC_CAP_SPIRAM));
            if (!s_decoded) s_decoded = static_cast<uint8_t*>(malloc(adpcm::kPcmBytesPerBlock));
        }
        if (!s_block || !s_decoded) {
            ESP_LOGE(TAG, "ADPCM buffer alloc failed — downlink will be silent");
        }
    }
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_block_len   = 0;
        s_paused      = false;
        s_repeats     = 0;
        s_base_us     = NowUs();
        s_delivered   = 0;
        s_last_send_us = s_base_us;
    }
    s_armed.store(true, std::memory_order_release);

    EnsureTicker();
    if (s_ticker) {
        esp_timer_stop(s_ticker);   // idempotent restart
        esp_timer_start_periodic(s_ticker, kTickMs * 1000);
    }
    ESP_LOGI(TAG, "downlink armed (%s)",
             s_codec.load(std::memory_order_acquire) == kCodecImaAdpcm ? "IMA-ADPCM" : "PCM16");
}

void Disarm() {
    if (!s_armed.exchange(false, std::memory_order_acq_rel)) return;
    if (s_ticker) esp_timer_stop(s_ticker);

    bool was_paused;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        was_paused  = s_paused;
        s_paused    = false;
        s_repeats   = 0;
        s_block_len = 0;
    }
    // If we left the App paused it would stay silent into the next utterance, so send one last
    // RESUME on the way out.
    if (was_paused) SendFlow(kFlowResume, 0);
    ESP_LOGI(TAG, "downlink disarmed");
}

bool IsArmed() { return s_armed.load(std::memory_order_acquire); }

void Feed(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    if (s_codec.load(std::memory_order_acquire) == kCodecPcm16) {
        Deliver(data, len);   // already PCM16: straight through, no copy
        return;
    }

    // IMA-ADPCM: reassemble 256B blocks across SDUs, decode each complete one.
    if (!s_block || !s_decoded) return;
    size_t off = 0;
    while (off < len) {
        const size_t room = adpcm::kBlockBytes - s_block_len;
        const size_t take = (len - off < room) ? (len - off) : room;
        memcpy(s_block + s_block_len, data + off, take);
        s_block_len += take;
        off += take;

        if (s_block_len == adpcm::kBlockBytes) {
            adpcm::DecodeBlock(s_block, reinterpret_cast<int16_t*>(s_decoded));
            s_block_len = 0;
            // Always a whole 1010B (even) block, so sample alignment can never be broken.
            Deliver(s_decoded, adpcm::kPcmBytesPerBlock);
        }
    }
}

void SetBufferMs(uint32_t playable_ms) {
    // Floor it: below ~120ms the pause/resume band collapses and the App would oscillate.
    if (playable_ms < 120) playable_ms = 120;
    s_buffer_ms.store(playable_ms, std::memory_order_release);
    const uint32_t cap_pause  = playable_ms * kPauseNum / kPauseDen;
    const uint32_t cap_resume = playable_ms * kResumeNum / kResumeDen;
    ESP_LOGI(TAG, "downlink flow control: board buffer %ums -> pause %ums / resume %ums",
             static_cast<unsigned>(playable_ms),
             static_cast<unsigned>(kProdPauseMs  < cap_pause  ? kProdPauseMs  : cap_pause),
             static_cast<unsigned>(kProdResumeMs < cap_resume ? kProdResumeMs : cap_resume));
}

void OnLinkState(bool connected) {
    if (connected) return;
    Disarm();
    // Codec is a per-connection sticky setting and resets to raw PCM on disconnect, so an App
    // that reconnects without re-declaring it gets the documented default.
    if (s_codec.exchange(kCodecPcm16, std::memory_order_acq_rel) != kCodecPcm16)
        ESP_LOGI(TAG, "downlink codec reset to PCM16 (link down)");
}

}  // namespace audio
}  // namespace agentlink

// Public API (include/agent_link.h).
extern "C" void agent_link_audio_set_buffer_ms(uint32_t playable_ms) {
    agentlink::audio::SetBufferMs(playable_ms);
}
