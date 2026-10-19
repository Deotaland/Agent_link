#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "paper_refresh.h"

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL at line %d\n", __LINE__); std::exit(1); } } while (false)

int main() {
    using paper::Refresh;
    using paper::RefreshGate;
    paper::Page setup, activation, online;
    std::strcpy(setup.lines[1], "Set up WiFi");
    std::strcpy(activation.lines[1], "Activation: 123456");
    std::strcpy(online.lines[1], "Online");
    RefreshGate gate;
    CHECK(gate.Observe(setup, true, 0) == Refresh::kWait);
    CHECK(gate.Observe(setup, true, RefreshGate::kSettleUs) == Refresh::kDraw);
    gate.Complete(setup, 18000000);
    CHECK(gate.Observe(setup, true, 20000000) == Refresh::kSame);

    // A connection burst returns to the displayed page without another physical refresh.
    CHECK(gate.Observe(online, false, 21000000) == Refresh::kWait);
    CHECK(gate.Observe(setup, true, 23000000) == Refresh::kSame);
    CHECK(gate.Observe(online, false, 24000000) == Refresh::kWait);
    RefreshGate automatic = gate;
    CHECK(automatic.Observe(online, false, 190000000) == Refresh::kWait);
    CHECK(automatic.Observe(online, false, 198000000) == Refresh::kDraw);

    // An activation code bypasses the ordinary three-minute delay, after settling and cooldown.
    CHECK(gate.Observe(activation, true, 25000000) == Refresh::kWait);
    CHECK(gate.Observe(activation, true, 27000000) == Refresh::kWait);
    CHECK(gate.Observe(activation, true, 28000000) == Refresh::kDraw);
    gate.Complete(activation, 43000000);
    CHECK(gate.Observe(activation, true, 90000000) == Refresh::kSame);

    // Streaming text is coalesced; only the stable final page becomes drawable.
    paper::Page partial = activation;
    std::strcpy(partial.lines[11], "First");
    CHECK(gate.Observe(partial, true, 60000000) == Refresh::kWait);
    std::strcpy(partial.lines[11], "First second");
    CHECK(gate.Observe(partial, true, 62000000) == Refresh::kWait);
    CHECK(gate.Observe(partial, true, 64000000) == Refresh::kWait);
    CHECK(gate.Observe(partial, true, 65000000) == Refresh::kDraw);
    gate.Failed(80000000);
    CHECK(gate.Observe(partial, true, 139000000) == Refresh::kWait);
    CHECK(gate.Observe(partial, true, 140000000) == Refresh::kDraw);
    gate.Complete(partial, 155000000);
    CHECK(gate.Observe(partial, true, 160000000) == Refresh::kSame);
    std::puts("PASS: identical pages, transient bursts, cooldown, important activation, text coalescing and failure backoff");
}
