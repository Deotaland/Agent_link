// Exercise the production C3 sender without a device or a real network.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "realtime_ws_stream.h"
#include "realtime_ws_frame.h"

constexpr const char* TAG = "fixture";
template <typename... Args> void Log(const char*, const char*, Args...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
using VoiceTls = int;
constexpr int ESP_TLS_ERR_SSL_WANT_READ = -10, ESP_TLS_ERR_SSL_WANT_WRITE = -11;
int64_t now_us = 0;
int64_t esp_timer_get_time() { return now_us; }
std::atomic<uint32_t> s_tx_sent_frames{0}, s_tx_sent_messages{0}, s_tx_bytes{0};
std::atomic<uint32_t> s_tx_write_calls{0}, s_tx_send_ms{0}, s_tx_send_max_ms{0};
std::atomic<uint32_t> s_tx_would_block{0}, s_tx_partial{0}, s_tx_select0_accepted{0};
int select_hint = 0;
bool blocked = false;
unsigned errors = 0, calls = 0;
std::deque<int> results;
std::string accepted;
const uint8_t* retry_pointer = nullptr;
size_t retry_bytes = 0;
int PollWritable(int) { return select_hint; }
bool TlsWouldBlock(int result) {
    return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE;
}
void LogTransportFailure(int, const char*, int, int) { ++errors; }
int esp_tls_conn_write(int, const uint8_t* data, size_t bytes) {
    ++calls;
    if (retry_pointer) { assert(data == retry_pointer && bytes == retry_bytes); }
    int result = blocked ? ESP_TLS_ERR_SSL_WANT_WRITE : static_cast<int>(bytes);
    if (!results.empty()) { result = results.front(); results.pop_front(); }
    if (TlsWouldBlock(result)) { retry_pointer = data; retry_bytes = bytes; }
    else { retry_pointer = nullptr; retry_bytes = 0; }
    if (result > 0) { assert(static_cast<size_t>(result) <= bytes); accepted.append(reinterpret_cast<const char*>(data), result); }
    return result;
}
void esp_fill_random(void* data, size_t bytes) { assert(bytes == 4); std::memcpy(data, "mask", 4); }
// INSERT_SENDER
// INSERT_PUMP_WIRE
// INSERT_WRITE_WIRE
// INSERT_PUMP_CONTROL
void Reset() {
    now_us = 0; select_hint = 0; blocked = false; errors = calls = 0;
    results.clear(); accepted.clear(); retry_pointer = nullptr; retry_bytes = 0;
}
int main() {
    uint8_t wire[256]{};
    auto queue = [&](WsSender& sender, const std::string& frame) {
        assert(!sender.pending.Busy()); std::memcpy(wire, frame.data(), frame.size());
        assert(WriteWire(sender, frame.size()));
    };
    {
        Reset(); WsSender sender{1, wire, sizeof wire};
        const std::string frame = "an immutable masked frame";
        queue(sender, frame); sender.audio_frames = sender.audio_messages = 1; sender.audio_bytes = frame.size();
        const unsigned observed = s_tx_select0_accepted.load();
        assert(PumpWire(sender) && !sender.pending.Busy());
        assert(accepted == frame && s_tx_select0_accepted == observed + 1);
        assert(s_tx_sent_frames == 1 && s_tx_write_calls == 1);
        for (size_t i = 0; i < frame.size(); ++i) assert(wire[i] == 0);
    }
    {
        Reset(); WsSender sender{1, wire, sizeof wire};
        const std::string frame = "a partial frame stays byte-identical across WANT results";
        queue(sender, frame);
        results = {ESP_TLS_ERR_SSL_WANT_WRITE, 3, ESP_TLS_ERR_SSL_WANT_READ, 2};
        assert(PumpWire(sender) && sender.pending.Offset() == 0);
        assert(!WriteWire(sender, 5)); // Never queue another control/audio frame in flight.
        now_us = 2500000; assert(PumpWire(sender) && sender.pending.Offset() == 3);
        now_us = 4000000; assert(PumpWire(sender) && sender.pending.Offset() == 3);
        now_us = 4500000; assert(PumpWire(sender) && sender.pending.Offset() == 5);
        now_us = 7000000; assert(PumpWire(sender) && !sender.pending.Busy());
        assert(accepted == frame && errors == 0);
    }
    {
        Reset(); blocked = true; WsSender sender{1, wire, sizeof wire};
        queue(sender, "a genuinely stalled frame");
        assert(PumpWire(sender)); now_us = 2999000; assert(PumpWire(sender));
        const unsigned before_timeout = calls;
        now_us = 3000000; assert(!PumpWire(sender));
        assert(calls == before_timeout && errors == 1 && accepted.empty());
    }
    {
        Reset(); WsSender sender{1, wire, sizeof wire}; queue(sender, std::string(100, 'd'));
        for (int64_t ms = 2000; ms < 15000; ms += 2000) {
            now_us = ms * 1000; results.push_back(1); assert(PumpWire(sender));
        }
        now_us = 15000000; assert(!PumpWire(sender) && errors == 1);
    }
    {
        Reset(); WsSender sender{1, wire, sizeof wire}; queue(sender, "a failed frame");
        results.push_back(-80); assert(!PumpWire(sender) && errors == 1 && accepted.empty());
    }
    {
        Reset(); WsSender sender{1, wire, sizeof wire};
        const uint8_t pong[] = {'o', 'k'}; bool pong_pending = false; int64_t last_ping = 0;
        now_us = 16000000;
        assert(PumpControl(sender, pong, sizeof pong, &pong_pending, &last_ping, now_us));
        assert(sender.pending.Busy() && (wire[0] & 15) == 9); // Keepalive queued ahead of audio.
        pong_pending = true;
        assert(PumpControl(sender, pong, sizeof pong, &pong_pending, &last_ping, now_us));
        assert(pong_pending && (wire[0] & 15) == 9); // An in-flight frame cannot be overwritten.
        assert(PumpWire(sender));
        assert(PumpControl(sender, pong, sizeof pong, &pong_pending, &last_ping, now_us));
        assert(!pong_pending && (wire[0] & 15) == 10);
        assert(PumpWire(sender) && !sender.pending.Busy());
        const unsigned before_idle = calls;
        assert(PumpControl(sender, pong, sizeof pong, &pong_pending, &last_ping, now_us));
        assert(!sender.pending.Busy() && calls == before_idle);
    }
    std::puts("PASS: production C3 sender: select=false with successful TLS write, exact partial/WANT retries, true stall deadline, bounded slow progress, fatal errors and ping/pong priority");
}
