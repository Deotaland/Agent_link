#include "../src/wifi_join.h"
#include <cassert>
#include <cstdio>

using agentlink::wifi::JoinWindow;

int main() {
    JoinWindow join;
    join.Begin(1000);
    assert(join.RemainingSeconds(1000) == 20);
    for (int64_t now = 3000; now < 21000; now += 3000) {
        join.Failed(now);
        assert(!join.TakeRetry(now + 1999));
        if (now + 2000 < 21000) assert(join.TakeRetry(now + 2000));
    }
    assert(join.Expired(21000));
    assert(!join.TakeRetry(21000));

    // Association without DHCP still uses the original deadline.
    join.Begin(50000);
    join.Associated();
    assert(join.WaitingIp());
    assert(!join.Expired(69999) && join.Expired(70000));

    // Cancelling or replacing credentials invalidates the old scheduled retry.
    join.Failed(51000);
    join.Stop();
    assert(!join.Active() && !join.TakeRetry(53000));
    join.Begin(52000);
    assert(!join.TakeRetry(53000) && join.Attempts() == 1);
    assert(join.RemainingSeconds(53000) == 19);

    // A later disconnect gets a fresh bounded window after a successful connection.
    join.Stop();
    join.Begin(200000);
    assert(join.RemainingSeconds(200000) == 20);
    assert(join.Expired(220000));
    std::puts("PASS: WiFi missing AP, DHCP deadline, stale retry, new credentials and later reconnect");
}
