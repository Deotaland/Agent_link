#include "../src/cloud/realtime_audio.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using agentlink::realtime::Message;
using agentlink::realtime::Pcm24To16;

static void TestControlSends() {
    for (const char* text : {
            "{\"type\":\"input_audio.activity\",\"kind\":\"start\"}",
            "{\"type\":\"input_audio.activity\",\"kind\":\"end\"}",
            "{\"type\":\"session.end\"}"}) {
        const std::vector<char> original(text, text + std::strlen(text));
        for (bool success : {true, false}) {
            bool called = false;
            assert(agentlink::realtime::SendControl(text, [&](char* frame, size_t bytes) {
                called = true;
                assert(frame != text);
                assert(bytes == original.size());
                assert(std::equal(original.begin(), original.end(), frame));
                // Model the transport's in-place masking, including a failed send.
                const uint8_t mask[] = {0x37, 0x12, 0x81, 0xff};
                for (size_t i = 0; i < bytes; ++i) frame[i] ^= mask[i % 4];
                return success;
            }) == success);
            assert(called);
            assert(std::equal(original.begin(), original.end(), text));
        }
    }
    char too_long[130];
    std::fill(std::begin(too_long), std::end(too_long) - 1, 'x');
    too_long[129] = '\0';
    bool called = false;
    assert(!agentlink::realtime::SendControl(too_long, [&](char*, size_t) { called = true; return true; }));
    assert(!called);
    assert(!agentlink::realtime::SendControl(nullptr, [](char*, size_t) { return true; }));
}

static void TestResampling() {
    std::vector<uint8_t> input;
    std::vector<int16_t> expected;
    for (int i = 0; i < 24000; i += 3) {
        int16_t triple[3];
        for (int j = 0; j < 3; ++j) {
            triple[j] = static_cast<int16_t>(24000 * std::sin((i + j) * 2 * 3.141592653589793 * 1000 / 24000));
            input.push_back(static_cast<uint8_t>(triple[j]));
            input.push_back(static_cast<uint8_t>(uint16_t(triple[j]) >> 8));
        }
        expected.push_back(static_cast<int16_t>((2 * int32_t(triple[0]) + triple[1]) / 3));
        expected.push_back(static_cast<int16_t>((int32_t(triple[1]) + 2 * int32_t(triple[2])) / 3));
    }
    std::mt19937 rng(20261014);
    for (unsigned max_chunk : {1u, 2u, 3u, 5u, 19u, 640u, 2048u, 65536u}) {
        Pcm24To16 converter;
        std::vector<int16_t> actual;
        size_t at = 0;
        while (at < input.size()) {
            const size_t n = std::min(input.size() - at, size_t(rng() % max_chunk + 1));
            std::vector<int16_t> buffer(n + 2);
            const size_t got = converter.Convert(input.data() + at, n, buffer.data());
            actual.insert(actual.end(), buffer.begin(), buffer.begin() + got);
            at += n;
        }
        assert(actual == expected);
        assert(actual.size() == 16000);
        unsigned crossings = 0;
        for (size_t i = 1; i < actual.size(); ++i) if (actual[i - 1] <= 0 && actual[i] > 0) ++crossings;
        // The first output sample is positive; its initial crossing is outside the interval.
        assert(crossings == 999);
    }

    Pcm24To16 converter;
    int16_t out[4];
    const uint8_t stale[] = {0xff, 0x7f, 0xaa};
    converter.Convert(stale, sizeof stale, out);
    converter.Reset();
    const uint8_t constant[] = {0x00, 0x80, 0x00, 0x80, 0x00, 0x80};
    assert(converter.Convert(constant, sizeof constant, out) == 2);
    assert(out[0] == -32768 && out[1] == -32768);
}

static void TestMessages() {
    const std::vector<uint8_t> payload = {'{', '"', 't', 'y', 'p', 'e', '"', ':', '"', 'x', '"', '}'};
    uint8_t buffer[128];
    // Every possible TCP split and every possible WebSocket continuation boundary.
    for (size_t split = 1; split < payload.size(); ++split) {
        Message message(buffer, sizeof buffer);
        for (size_t i = 0; i < payload.size(); ++i) {
            const bool second_frame = i >= split;
            const bool first = i == 0 || i == split;
            const bool last = i == split - 1 || i == payload.size() - 1;
            const auto result = message.Append(second_frame ? 0 : 1, first, last, second_frame,
                                               payload.data() + i, 1);
            assert(result == (i == payload.size() - 1 ? Message::kComplete : Message::kMore));
        }
        assert(message.Opcode() == 1 && message.Size() == payload.size());
        assert(std::equal(payload.begin(), payload.end(), buffer));
        assert(message.Append(2, true, true, true, payload.data(), payload.size()) == Message::kComplete);
        assert(message.Opcode() == 2);
    }
    Message continuation(buffer, sizeof buffer);
    assert(continuation.Append(0, true, true, true, payload.data(), 1) == Message::kError);
    Message missing(buffer, sizeof buffer);
    assert(missing.Append(1, true, true, false, payload.data(), 1) == Message::kMore);
    assert(missing.Append(2, true, true, true, payload.data(), 1) == Message::kError);
    Message limit(buffer, 2);
    assert(limit.Append(1, true, false, true, payload.data(), 2) == Message::kMore);
    assert(limit.Append(1, false, true, true, payload.data(), 1) == Message::kError);
    Message empty(buffer, sizeof buffer);
    assert(empty.Append(1, true, true, true, nullptr, 0) == Message::kComplete);
}

int main() {
    TestControlSends();
    TestResampling();
    TestMessages();
    std::puts("PASS: writable WS control frames, failed-send isolation, PCM rate/pitch, fragments and bounds");
}
