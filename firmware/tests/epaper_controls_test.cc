#include "epaper_controls.h"
#include <cassert>
#include <cstdio>
using epaper::Press;
int main() {
    epaper::Key key;
    assert(key.Poll(false, 0) == Press::kNone);
    assert(key.Poll(true, 100000) == Press::kNone);
    assert(key.Poll(false, 110000) == Press::kNone); // bounce, no action
    assert(key.Poll(true, 120000) == Press::kNone);
    assert(key.Poll(true, 150000) == Press::kNone);
    assert(key.Poll(false, 200000) == Press::kNone);
    assert(key.Poll(false, 230000) == Press::kTap);
    assert(key.Poll(false, 240000) == Press::kNone);
    assert(key.Poll(true, 300000) == Press::kNone);
    assert(key.Poll(true, 330000) == Press::kNone);
    assert(key.Poll(true, 1400000) == Press::kNone); // hold is emitted once, on release
    assert(key.Poll(false, 1410000) == Press::kNone);
    assert(key.Poll(false, 1440000) == Press::kHold);
    assert(key.Poll(false, 1450000) == Press::kNone);
    epaper::Key boot_held;
    assert(boot_held.Poll(true, 0) == Press::kNone);
    assert(boot_held.Poll(false, 2000000) == Press::kNone);
    assert(boot_held.Poll(false, 2030000) == Press::kNone);
    assert(boot_held.Poll(true, 3000000) == Press::kNone);
    assert(boot_held.Poll(true, 3030000) == Press::kNone);
    assert(boot_held.Poll(false, 3100000) == Press::kNone);
    assert(boot_held.Poll(false, 3130000) == Press::kTap);
    assert(!epaper::Current(0, 0));
    assert(!epaper::Current(7, 8)); // queued press on old e-paper page must not choose a new item
    assert(epaper::Current(8, 8));
    assert(epaper::Volume(0, -1) == 0);
    assert(epaper::Volume(100, 1) == 100);
    assert(epaper::Volume(60, 1) == 65);
    assert(epaper::Volume(60, -1) == 55);
    puts("Waveshare controls: debounce, release/hold, boot-held, stale-page and volume checks passed");
}
