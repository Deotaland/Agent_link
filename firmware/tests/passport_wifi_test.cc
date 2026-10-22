#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>
#include "passport_wifi_controls.h"
#include "realtime_audio.h"
#include "realtime_buffer.h"
#include "cloud_limits.h"
#include "voice_playback.h"

namespace {
bool fail_resize = false;
size_t live_buffers = 0;
void* ResizeFixture(void* data, size_t bytes) {
    if (!bytes) { std::free(data); --live_buffers; return nullptr; }
    if (fail_resize) return nullptr;
    void* result = std::realloc(data, bytes);
    if (result && !data) ++live_buffers;
    return result;
}
using TickType_t = uint32_t;
uint32_t tick_hz = 100;
TickType_t last_delay = 0;
void vTaskDelay(TickType_t ticks) { last_delay = ticks; }
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms) * tick_hz / 1000U)
void PlaybackIdleWait() {
    // INSERT_PLAYBACK_IDLE_WAIT
}
#undef pdMS_TO_TICKS
}

class PassportWifiAudio {
public:
    static size_t BufferLevel(void* context);
    static constexpr size_t kBufferBytes = 8192;
    std::atomic<uint32_t> generation_{0}, cleared_{0};
    size_t buffer_ = 1024;
};
size_t xStreamBufferBytesAvailable(size_t bytes) { return bytes; }
// INSERT_BUFFER_LEVEL

int main() {
    PassportWifiAudio audio;
    assert(PassportWifiAudio::BufferLevel(&audio) == 1024);
    audio.generation_ = 1;
    assert(PassportWifiAudio::BufferLevel(&audio) == 8192); // asynchronous reset cannot accept PCM yet
    audio.cleared_ = 1; audio.buffer_ = 3200;
    assert(PassportWifiAudio::BufferLevel(&audio) == 3200);
    assert(agentlink::realtime::PlaybackRoom(5504, 640, 256));
    assert(!agentlink::realtime::PlaybackRoom(5505, 640, 256));
    assert(!agentlink::realtime::PlaybackRoom(8192, 640, 256));
    assert(agentlink::realtime::PlaybackRoom(0, 8192, 256));
    assert(!agentlink::realtime::PlaybackRoom(0, 8193, 256));
    tick_hz = 100;
    PlaybackIdleWait();
    assert(last_delay == 1); // 5 ms must really block at the profile's 100 Hz.
    tick_hz = 1000;
    PlaybackIdleWait();
    assert(last_delay == 5); // Preserve the intended interval at a faster tick rate.

    using namespace passport_wifi;
    Controls keys;
    assert(keys.Update(Key::Ok, 100) == Gesture::None);
    assert(keys.Update(Key::None, 300) == Gesture::Click);
    assert(keys.Update(Key::None, 301) == Gesture::None);
    assert(keys.Update(Key::Ok, 500) == Gesture::None);
    assert(keys.Update(Key::Ok, 1200) == Gesture::Hold);
    assert(keys.Update(Key::Ok, 2000) == Gesture::None);
    assert(keys.Update(Key::None, 2100) == Gesture::None);
    assert(keys.Update(Key::Up, 2200) == Gesture::Up);
    assert(keys.Update(Key::Up, 2699) == Gesture::None);
    assert(keys.Update(Key::Up, 2700) == Gesture::Up);
    assert(keys.Update(Key::Up, 2800) == Gesture::None);
    assert(keys.Update(Key::Up, 2880) == Gesture::Up);
    keys.Update(Key::None, 2900);
    keys.Update(Key::Ok, UINT32_MAX - 300);
    assert(keys.Update(Key::Ok, 400) == Gesture::Hold);
    assert(keys.Update(Key::None, 401) == Gesture::None);
    keys.Update(Key::Ok, 500);
    assert(keys.Update(Key::Down, 600) == Gesture::Down);
    assert(keys.Update(Key::None, 700) == Gesture::None);

    using agentlink::realtime::Message;
    std::vector<uint8_t> buffer(agentlink::cloud_limits::kMessageBytes);
    Message message(buffer.data(), buffer.size());
    const uint8_t first[] = {1, 2, 3};
    assert(message.Append(2, true, true, false, first, 3) == Message::kMore);
    assert(message.Append(0, true, true, true, first, 3) == Message::kComplete);
    assert(message.Opcode() == 2 && message.Size() == 6);
    assert(buffer[0] == 1 && buffer[3] == 1);
    Message oversized(buffer.data(), buffer.size());
    assert(oversized.Append(2, true, true, false, buffer.data(), buffer.size()) == Message::kMore);
    assert(oversized.Append(0, true, true, true, first, 1) == Message::kError);

    // C3 starts small, but fragmented messages retain every byte across a
    // reallocation and still accept the old 16 KB maximum without truncation.
    {
        using agentlink::realtime::MessageBuffer;
        MessageBuffer storage(2048, 16384, ResizeFixture);
        assert(storage.Capacity() == 2048 && live_buffers == 1);
        Message growing(storage.Data(), storage.Capacity());
        const std::vector<uint8_t> part(2048, 0xa5);
        for (unsigned n = 0; n < 8; ++n) {
            assert(storage.Reserve(growing.Size() + part.size()));
            growing.Rebind(storage.Data(), storage.Capacity());
            assert(growing.Append(n ? 0 : 2, true, true, n == 7, part.data(), part.size()) ==
                   (n == 7 ? Message::kComplete : Message::kMore));
        }
        assert(!growing.Active() && growing.Size() == 16384);
        for (size_t n = 0; n < growing.Size(); ++n) assert(storage.Data()[n] == 0xa5);
        assert(!storage.Reserve(16385));
        storage.Trim();
        assert(storage.Capacity() == 2048);
        growing.Rebind(storage.Data(), storage.Capacity());
        assert(growing.Append(1, true, true, true, first, 3) == Message::kComplete);
        fail_resize = true;
        assert(!storage.Reserve(4096));
        assert(storage.Capacity() == 2048 && std::memcmp(storage.Data(), first, 3) == 0);
        fail_resize = false;
    }
    assert(live_buffers == 0);

    // The shared codec worker must keep 20 ms boundaries even when the mic
    // supplies partial packets; ending a turn flushes exactly one padded frame.
    {
        agentlink::realtime::PcmFrames<640> frames;
        std::vector<std::vector<uint8_t>> encoded;
        auto encode = [&](const uint8_t* pcm) { encoded.emplace_back(pcm, pcm + 640); return true; };
        std::vector<uint8_t> pcm(960, 0x31);
        assert(frames.Append(pcm.data(), 320, encode) && encoded.empty());
        assert(frames.Append(pcm.data() + 320, 640, encode) && encoded.size() == 1);
        assert(frames.Flush(encode) && encoded.size() == 2);
        for (size_t n = 0; n < 640; ++n) assert(encoded[0][n] == 0x31);
        for (size_t n = 0; n < 640; ++n) assert(encoded[1][n] == (n < 320 ? 0x31 : 0));
        assert(frames.Flush(encode) && encoded.size() == 2);
        assert(!frames.Append(pcm.data(), 640, [](const uint8_t*) { return false; }));
        frames.Reset();
        assert(frames.Flush(encode) && encoded.size() == 2);
    }
    fail_resize = true;
    { agentlink::realtime::MessageBuffer failed(2048, 16384, ResizeFixture); assert(!failed.Data()); }
    fail_resize = false;
    assert(live_buffers == 0);

    agentlink::realtime::Pcm24To16 converter;
    int16_t out[8]{};
    const uint8_t samples[] = {30,0, 60,0, 90,0, 30,0, 60,0, 90,0};
    assert(converter.Convert(samples, 1, out) == 0);
    assert(converter.Convert(samples + 1, 11, out) == 4);
    assert(out[0] == 40 && out[1] == 80 && out[2] == 40 && out[3] == 80);
    converter.Reset();
    assert(converter.Convert(samples, 2, out) == 0);
    converter.Reset();
    assert(converter.Convert(samples, 6, out) == 2);

    kc_audio::PlaybackGate gate(3200, 4096);
    assert(!gate.Ready(500, false, 0));
    assert(gate.Ready(500, true, 1)); // A short completed reply must not wait forever.
    assert(!gate.Ready(0, true, 2));
    gate.Reset();
    assert(!gate.Ready(0, false, 3));
    assert(gate.Ready(3200, false, 4));
    kc_audio::SpeakerEchoGuard echo;
    assert(!echo.Muted(0));
    echo.OnWrite(1000, 320);
    assert(echo.Muted(21000));
    assert(echo.Drained(121000));
    assert(echo.Muted(200000));
    assert(!echo.Muted(321000));
    std::puts("PASS: playback idle blocks at 100/1000 Hz; short/long/repeated keys and timer rollover; fragmented and oversized voice messages; PCM conversion; short replies and echo tail");
}
