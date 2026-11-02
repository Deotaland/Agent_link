#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "paper_refresh.h"
#include "paper_controls.h"

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL at line %d\n", __LINE__); std::exit(1); } } while (false)

int main() {
    using paper::Refresh;
    using paper::RefreshGate;
    paper::Button button;
    using paper::Press;
    CHECK(button.Poll(false, 0) == Press::None);
    CHECK(button.Poll(true, 100000) == Press::None);
    CHECK(button.Poll(false, 110000) == Press::None); // Ignore a bouncing contact.
    CHECK(button.Poll(true, 120000) == Press::None);
    CHECK(button.Poll(true, 150000) == Press::None);
    CHECK(button.Poll(false, 200000) == Press::None);
    CHECK(button.Poll(false, 230000) == Press::Tap);
    CHECK(button.Poll(false, 240000) == Press::None);
    CHECK(button.Poll(true, 300000) == Press::None);
    CHECK(button.Poll(true, 330000) == Press::None);
    CHECK(button.Poll(true, 1330000) == Press::Hold);
    CHECK(button.Poll(true, 2330000) == Press::None);
    CHECK(button.Poll(false, 2400000) == Press::None);
    CHECK(button.Poll(false, 2430000) == Press::None); // A hold must not also move/select on release.
    paper::Button held_at_boot;
    CHECK(held_at_boot.Poll(true, 0) == Press::None);
    CHECK(held_at_boot.Poll(true, 2000000) == Press::None);
    CHECK(held_at_boot.Poll(false, 2100000) == Press::None);
    CHECK(held_at_boot.Poll(false, 2130000) == Press::None);
    paper::PendingConfirm confirm;
    confirm.Request(8);
    CHECK(!confirm.Take(7, 8));
    CHECK(confirm.Take(10, 8)); // Header-only revisions do not change the selection.
    CHECK(!confirm.Take(10, 8));
    confirm.Request(11);
    CHECK(!confirm.Take(12, 12)); // The page/list changed while waiting.
    confirm.Request(13); confirm.Cancel();
    CHECK(!confirm.Take(13, 13));
    paper::ProbeRetry probe(1000000);
    CHECK(!probe.Due(5999999) && probe.Due(6000000));
    probe.Attempted(6000000);
    CHECK(!probe.Due(10999999) && probe.Due(11000000));
    probe.Attempted(11000000);
    CHECK(!probe.Exhausted() && probe.Due(16000000));
    probe.Attempted(16000000);
    CHECK(probe.Exhausted() && !probe.Due(3600000000LL));
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
    CHECK(automatic.Observe(online, false, 3600000000LL) == Refresh::kWait);
    CHECK(automatic.Observe(online, true, 3600000000LL) == Refresh::kWait);
    CHECK(automatic.Observe(online, true, 3603000000LL) == Refresh::kDraw);

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
    std::puts("PASS: key debounce/hold, deferred confirmation, background suppression, identical pages, cooldown, activation and failure backoff");
}
