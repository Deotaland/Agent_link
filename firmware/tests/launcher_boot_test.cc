#include "agent_launcher_boot.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>

struct Flash {
    std::array<uint8_t, 8192> bytes;
    int operation = 0, fail = -1;
    size_t torn = 0;
    Flash() {
        bytes.fill(255);
    }
    static int Read(void* ctx, uint32_t at, void* out, size_t size) {
        auto& self = *static_cast<Flash*>(ctx);
        assert(at + size <= self.bytes.size());
        memcpy(out, self.bytes.data() + at, size);
        return 0;
    }
    static int Erase(void* ctx, uint32_t at, size_t size) {
        auto& self = *static_cast<Flash*>(ctx);
        const bool cut = ++self.operation == self.fail;
        memset(self.bytes.data() + at, 255, cut ? std::min(size, self.torn) : size);
        return cut ? -1 : 0;
    }
    static int Write(void* ctx, uint32_t at, const void* data, size_t size) {
        auto& self = *static_cast<Flash*>(ctx);
        const bool cut = ++self.operation == self.fail;
        auto* in = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < (cut ? std::min(size, self.torn) : size); ++i)
            self.bytes[at + i] &= in[i];
        return cut ? -1 : 0;
    }
    alpt_io Io() {
        return {this, Read, Erase, Write, nullptr};
    }
};
int main() {
    constexpr uint32_t policy = 0x12345678;
    Flash flash;
    auto io = flash.Io();
    alboot_record state;
    int bank;
    assert(alboot_load(&io, 0, policy, &state, &bank) == ALPT_NONE);
    assert(state.active == 0 && state.pending == 255 && state.valid == 1);
    state.versions[0] = 20000;
    assert(alboot_save(&io, 0, policy, &state) == 0);
    const auto baseline = flash;
    state.pending = 1;
    state.versions[1] = 20001;
    unsigned scenarios = 0;
    for (int step = 1; step <= 3; ++step)
        for (size_t torn : {size_t(0), size_t(1), size_t(3), size_t(4), size_t(16), size_t(31),
                            size_t(35), size_t(36), size_t(39), size_t(40), size_t(4096)}) {
            Flash cut = baseline;
            cut.operation = 0;
            cut.fail = step;
            cut.torn = torn;
            auto cut_io = cut.Io();
            auto pending = state;
            (void)alboot_save(&cut_io, 0, policy, &pending);
            alboot_record recovered;
            assert(alboot_load(&cut_io, 0, policy, &recovered, &bank) == 0);
            assert(recovered.active == 0 && recovered.valid == 1);
            assert(recovered.pending == 255 || recovered.pending == 1);
            ++scenarios;
        }
    assert(alboot_save(&io, 0, policy, &state) == 0);
    assert(alboot_confirm(&state, 1, 20001) == ALPT_INVALID);  // Not attempted yet.
    assert(alboot_attempt(&state, 0) == 0 &&
           !state.attempted);  // Other boots do not consume trial.
    assert(alboot_attempt(&state, 1) == 0 && state.attempted);
    assert(alboot_save(&io, 0, policy, &state) == 0);
    assert(alboot_load(&io, 0, policy, &state, &bank) == 0);
    assert(alboot_attempt(&state, 1) == 1);  // Crash/reset before confirmation.
    assert(state.active == 0 && state.pending == 255 && state.result == ALBOOT_ROLLBACK);
    state.pending = 1;
    state.attempted = 0;
    assert(alboot_attempt(&state, 1) == 0);
    assert(alboot_confirm(&state, 1, 20002) == ALPT_INVALID);
    assert(alboot_confirm(&state, 1, 20001) == 0);
    assert(state.active == 1 && state.valid == 3 && state.pending == 255);
    assert(alboot_save(&io, 0, policy, &state) == 0);
    // The next update goes back to A while the confirmed B remains intact.
    state.pending = 0;
    state.versions[0] = 20002;
    assert(alboot_attempt(&state, 0) == 0);
    assert(alboot_confirm(&state, 0, 20002) == 0 && state.active == 0 && state.valid == 3);
    assert(alboot_load(&io, 0, policy + 1, &state, &bank) == ALPT_NONE);
    printf(
        "Launcher A/B: %u torn-write cases, failed trial rollback, confirmation and alternating "
        "upgrades passed.\n",
        scenarios);
}
