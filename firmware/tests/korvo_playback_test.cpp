#include "../boards/korvo-cloud/voice_playback.h"
#include <cassert>
#include <cstdio>

int main() {
    kc_audio::PlaybackGate gate(16000, 32000);
    assert(!gate.Ready(8000, false, 0));
    assert(gate.Ready(8000, true, 1)); // A short reply must play through its tail.
    assert(!gate.Ready(0, true, 251));
    gate.Reset();
    assert(gate.Ready(16000, false, 1000));
    assert(!gate.Ready(0, false, 1500));
    assert(!gate.Ready(0, false, 1580));
    assert(gate.Underruns() == 1);
    assert(!gate.Ready(1000, false, 1600));
    assert(gate.Ready(1000, true, 1601)); // End bypasses the resume threshold too.

    kc_audio::SpeakerEchoGuard guard;
    assert(!guard.Muted(1000000));
    guard.OnWrite(1000000, 256);
    assert(guard.Muted(1000000));
    assert(!guard.Drained(1115999) && guard.Drained(1116000));
    assert(guard.Muted(1315999) && !guard.Muted(1316000));
    guard.OnWrite(1016000, 256);
    assert(guard.Muted(1316000) && !guard.Muted(1332000));
    // No speaker writes during thinking: real uploads stay enabled.
    assert(!guard.Muted(10000000));
    std::puts("PASS: short-reply tail, underrun recovery, DMA drain and echo-guard release");
}
