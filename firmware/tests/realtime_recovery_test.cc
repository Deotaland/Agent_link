#include "realtime_recovery.h"
#include "realtime_ws_frame.h"
#include "realtime_ws_stream.h"
#include "realtime_ws_upgrade.h"
#include "realtime_audio.h"
#include "realtime_buffer.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

int main() {
    {
        // Match C3's retained 16 KB message alongside the fixed TLS record.
        // A full message may cross many 1 KB TLS reads and repeat indefinitely
        // without introducing another receive-buffer realloc during playback.
        static unsigned resizes = 0;
        const auto resize = [](void* data, size_t bytes) -> void* {
            ++resizes;
            if (!bytes) { free(data); return nullptr; }
            return realloc(data, bytes);
        };
        agentlink::realtime::MessageBuffer storage(2048, 16384, resize);
        assert(storage.Reserve(16384));
        const unsigned reserved = resizes;
        const uint8_t* original = storage.Data();
        agentlink::realtime::FrameReader reader(16384);
        agentlink::realtime::Message message(storage.Data(), storage.Capacity());
        std::vector<uint8_t> wire{0x82, 126, 0x40, 0x00};
        wire.resize(16388, 0x5a);
        unsigned delivered = 0;
        for (unsigned repeat = 0; repeat < 1000; ++repeat) {
            for (size_t offset = 0; offset < wire.size(); offset += 1024) {
                assert(reader.Consume(wire.data() + offset, std::min(size_t(1024), wire.size() - offset),
                    [&](unsigned opcode, bool first, bool end, bool fin, const uint8_t* data, size_t bytes) {
                        const size_t buffered = message.Active() ? message.Size() : 0;
                        assert(storage.Reserve(buffered + bytes));
                        const auto result = message.Append(opcode, first, end, fin, data, bytes);
                        assert(result != agentlink::realtime::Message::kError);
                        if (result == agentlink::realtime::Message::kComplete) {
                            assert(message.Size() == 16384 && storage.Data()[16383] == 0x5a); ++delivered;
                        }
                        return true;
                    }));
            }
            assert(storage.Data() == original && resizes == reserved);
        }
        assert(delivered == 1000 && !storage.Reserve(16385));
    }
    // A server may split even its frame header across arbitrary TLS records.
    // A ping between data fragments must preserve the incomplete message.
    {
        const std::vector<uint8_t> wire = {0x01, 3, 'a', 'b', 'c', 0x89, 2, 'h', 'i',
                                          0x80, 3, 'd', 'e', 'f', 0x82, 0};
        for (size_t step : {size_t(1), size_t(2), size_t(3), size_t(7), wire.size()}) {
            agentlink::realtime::FrameReader reader(16384);
            uint8_t buffer[64]{};
            agentlink::realtime::Message message(buffer, sizeof buffer);
            std::vector<std::string> messages;
            std::string ping;
            unsigned pings = 0;
            for (size_t offset = 0; offset < wire.size(); offset += step) {
                const size_t size = std::min(step, wire.size() - offset);
                assert(reader.Consume(wire.data() + offset, size,
                    [&](unsigned opcode, bool first, bool end, bool fin, const uint8_t* data, size_t bytes) {
                        if (opcode == 9) {
                            if (first) ping.clear();
                            ping.append(reinterpret_cast<const char*>(data), bytes);
                            if (end) { assert(fin && ping == "hi"); ++pings; }
                        } else {
                            const auto result = message.Append(opcode, first, end, fin, data, bytes);
                            assert(result != agentlink::realtime::Message::kError);
                            if (result == agentlink::realtime::Message::kComplete)
                                messages.emplace_back(reinterpret_cast<char*>(buffer), message.Size());
                        }
                        return true;
                    }));
            }
            assert(pings == 1 && (messages == std::vector<std::string>{"abcdef", ""}));
        }
        // Extended headers, empty control frames and invalid server frames.
        for (size_t length : {size_t(126), size_t(65536)}) {
            std::vector<uint8_t> packet{0x82, uint8_t(length == 126 ? 126 : 127)};
            const unsigned width = length == 126 ? 2 : 8;
            for (unsigned i = width; i; --i) packet.push_back(uint8_t(uint64_t(length) >> ((i - 1) * 8)));
            packet.resize(packet.size() + length, 0xa5);
            agentlink::realtime::FrameReader reader(65536);
            size_t consumed = 0; unsigned starts = 0, ends = 0;
            for (const uint8_t byte : packet)
                assert(reader.Consume(&byte, 1, [&](unsigned op, bool first, bool end, bool fin,
                                                   const uint8_t* data, size_t size) {
                    assert(op == 2 && fin); starts += first; ends += end;
                    if (size) { assert(*data == 0xa5); consumed += size; }
                    return true;
                }));
            assert(consumed == length && starts == 1 && ends == 1);
        }
        const std::vector<std::vector<uint8_t>> invalid = {
            {0xc1, 0}, {0x83, 0}, {0x81, 0x80}, {0x09, 0}, {0x89, 126, 0, 126},
            {0x82, 126, 0, 1}, {0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0},
            {0x82, 126, 0x40, 1}
        };
        for (const auto& packet : invalid) {
            agentlink::realtime::FrameReader reader(16384);
            assert(!reader.Consume(packet.data(), packet.size(), [](auto...) { return true; }));
        }
        agentlink::realtime::FrameReader empty_ping(16384);
        const uint8_t empty[] = {0x89, 0};
        unsigned callbacks = 0;
        assert(empty_ping.Consume(empty, sizeof empty, [&](unsigned op, bool first, bool end,
                                                          bool fin, const uint8_t*, size_t bytes) {
            assert(op == 9 && first && end && fin && bytes == 0); ++callbacks; return true;
        }));
        assert(callbacks == 1);
    }
    {
        // Known RFC 6455 accept value; check status, tokens, protocol and
        // duplicates without accepting a proxy response or an extension.
        const std::string header = "HTTP/1.1 101 Switching Protocols\r\n"
            "uPgRaDe: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
            "Sec-WebSocket-Protocol: tidb-link.realtime.v1\r\n\r\n";
        int status = 0;
        const char* accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
        assert(agentlink::realtime::ValidUpgrade(header.c_str(), accept, &status) && status == 101);
        assert(!agentlink::realtime::ValidUpgrade(header.c_str(), "wrong", &status));
        std::string duplicate = header;
        duplicate.insert(duplicate.size() - 2, "Upgrade: websocket\r\n");
        assert(!agentlink::realtime::ValidUpgrade(duplicate.c_str(), accept, &status));
        std::string extension = header;
        extension.insert(extension.size() - 2, "Sec-WebSocket-Extensions: permessage-deflate\r\n");
        assert(!agentlink::realtime::ValidUpgrade(extension.c_str(), accept, &status));
        for (const char* rejected : {"", "HTTP/1.1 ", "HTTP/1.1 403 Forbidden\r\n\r\n",
                                     "HTTP/1.1 101 Switching Protocols\r\n\r\n"})
            assert(!agentlink::realtime::ValidUpgrade(rejected, accept, &status));
    }
    {
        agentlink::realtime::PendingWrite pending;
        const std::string frame = "immutable masked frame";
        std::string delivered;
        assert(pending.Begin(frame.size(), 0));
        assert(!pending.Begin(3, 1)); // A ping/audio frame must not overwrite it.
        assert(pending.Advance(-1, true, 100) && pending.Offset() == 0);
        delivered.append(frame.data() + pending.Offset(), 3);
        assert(pending.Advance(3, false, 200));
        assert(pending.Advance(-1, true, 250) && pending.Offset() == 3);
        delivered.append(frame.data() + pending.Offset(), pending.Remaining());
        assert(pending.Advance(static_cast<int>(pending.Remaining()), false, 300));
        assert(!pending.Busy() && delivered == frame);
        assert(pending.Begin(10, 400));
        unsigned audio_ticks = 0;
        // Backpressure yields to audio scheduling rather than blocking for 3s.
        for (int64_t now = 400; now < 3400; now += 20) {
            assert(pending.Advance(-1, true, now)); ++audio_ticks;
        }
        assert(audio_ticks == 150 && pending.Expired(3400));
        assert(!pending.Advance(-1, true, 3400));
        agentlink::realtime::PendingWrite failed;
        assert(failed.Begin(4, 0));
        assert(!failed.Advance(-1, false, 1));
        assert(!failed.Advance(5, false, 2));
    }
    // A coalesced TLS record is still three independently masked, ordered
    // WebSocket messages, including the 125/126-byte header boundary.
    {
        std::vector<uint8_t> wire(1024);
        const std::vector<std::string> messages = {"first", std::string(125, 'b'), std::string(126, 'c')};
        size_t used = 0;
        for (unsigned i = 0; i < messages.size(); ++i) {
            const uint8_t mask[4] = {static_cast<uint8_t>(i + 1), 0x82, 0x19, 0xa4};
            const size_t added = agentlink::realtime::EncodeTextFrame(messages[i].data(), messages[i].size(),
                mask, wire.data() + used, wire.size() - used);
            assert(added);
            used += added;
        }
        size_t offset = 0;
        for (const auto& message : messages) {
            assert(wire[offset++] == 0x81);
            const uint8_t length_byte = wire[offset++];
            assert(length_byte & 0x80);
            size_t length = length_byte & 0x7f;
            if (length == 126) { length = (size_t(wire[offset]) << 8) | wire[offset + 1]; offset += 2; }
            const size_t mask_at = offset;
            offset += 4;
            std::string decoded;
            for (size_t i = 0; i < length; ++i) decoded += char(wire[offset++] ^ wire[mask_at + i % 4]);
            assert(decoded == message);
        }
        assert(offset == used);
    }
    {
        agentlink::realtime::OpusBatchGate gate;
        assert(!gate.Ready(1, 0));
        assert(!gate.Ready(2, 39));
        assert(gate.Ready(2, 40));  // a short tail cannot wait indefinitely
        gate.Reset();
        assert(gate.Ready(3, 100)); // a full batch adds no extra delay
        assert(!gate.Ready(0, 101));
        assert(!gate.Ready(1, 1000)); // no inherited timer after an empty queue
        assert(gate.Ready(1, 1040));
        agentlink::realtime::OpusBatchGate c3(2);
        assert(!c3.Ready(1, 0));
        assert(c3.Ready(2, 20)); // the C3's two-slot queue cannot wait for three
        c3.Reset();
        assert(!c3.Ready(1, 50) && c3.Ready(1, 90));
        agentlink::realtime::WriteStall stall;
        stall.Observe(false, 0);
        assert(stall.Blocked() && !stall.Expired(2999));
        assert(stall.Expired(3000));
        stall.Observe(true, 3000);
        assert(!stall.Blocked() && !stall.Expired(9000));
        stall.Observe(false, 10000);
        stall.Observe(false, 12000); // polling must not keep extending the deadline
        assert(stall.Expired(13000));
    }
    // Independently decode the actual wire format at short/extended-length
    // boundaries; preserve the input and emit exactly one transport write.
    for (size_t bytes : {size_t(0), size_t(1), size_t(125), size_t(126), size_t(127), size_t(4096), size_t(65535)}) {
        const std::string input(bytes, 'x');
        const uint8_t mask[4] = {0x12, 0xa3, 0x56, 0x78};
        std::vector<uint8_t> frame(bytes + 8);
        unsigned writes = 0;
        const bool ok = agentlink::realtime::WriteTextFrame(input.data(), bytes, mask, frame.data(), frame.size(),
            [&](const uint8_t* wire, size_t size) {
                ++writes;
                assert(wire[0] == 0x81 && (wire[1] & 0x80));
                const bool extended = (wire[1] & 0x7f) == 126;
                const size_t length = extended ? (size_t(wire[2]) << 8) | wire[3] : wire[1] & 0x7f;
                const size_t mask_at = extended ? 4 : 2;
                assert(length == bytes && size == bytes + mask_at + 4);
                for (size_t i = 0; i < bytes; ++i)
                    assert((wire[mask_at + 4 + i] ^ wire[mask_at + i % 4]) == 'x');
                return static_cast<int>(size);
            });
        assert(ok && writes == 1 && input == std::string(bytes, 'x'));
        // Zero, negative and partial writes must fail without a second write.
        for (int result : {-1, 0, 3}) {
            writes = 0;
            assert(!agentlink::realtime::WriteTextFrame(input.data(), bytes, mask, frame.data(), frame.size(),
                [&](const uint8_t*, size_t) { ++writes; return result; }));
            assert(writes == 1);
        }
        writes = 0;
        assert(!agentlink::realtime::WriteTextFrame(input.data(), bytes, mask, frame.data(), bytes,
            [&](const uint8_t*, size_t) { ++writes; return 0; }));
        assert(writes == 0);
    }
    using agentlink::realtime::RecoverTransport;
    // Persistent transport failure exhausts a call-wide budget, even if an
    // attempt reached READY before its next failure. There is no infinite loop.
    unsigned attempts = 0;
    std::vector<unsigned> waits;
    RecoverTransport([&](bool may_retry) {
        ++attempts;
        assert(may_retry == (attempts < 3));
        return true;
    }, [&](unsigned retry, unsigned delay_ms) {
        assert(retry == waits.size() + 1);
        waits.push_back(delay_ms);
        return true;
    });
    assert(attempts == 3);
    assert((waits == std::vector<unsigned>{2000, 4000}));

    // Recovery succeeds on the second attempt; no later grant is requested.
    attempts = 0;
    waits.clear();
    RecoverTransport([&](bool) { return ++attempts == 1; },
                     [&](unsigned, unsigned ms) { waits.push_back(ms); return true; });
    assert(attempts == 2 && waits.size() == 1);

    // User exits during backoff: never launch another network request.
    attempts = 0;
    RecoverTransport([&](bool) { ++attempts; return true; },
                     [](unsigned, unsigned) { return false; });
    assert(attempts == 1);

    // Explicit server rejection, invalid media or unsupported grant source
    // returns false from the attempt and must not enter the backoff callback.
    attempts = 0;
    RecoverTransport([&](bool) { ++attempts; return false; },
                     [](unsigned, unsigned) { assert(false); return true; });
    assert(attempts == 1);
    std::puts("Realtime recovery fault scenarios passed");
}
