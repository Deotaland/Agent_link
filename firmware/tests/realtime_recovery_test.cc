#include "realtime_recovery.h"
#include "realtime_ws_frame.h"

#include <cassert>
#include <cstdio>
#include <vector>
#include <string>

int main() {
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
