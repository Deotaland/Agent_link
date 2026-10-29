#include "epaper_controls.h"
#include "epaper_frame.h"
#include "epaper_refresh.h"
#include "epaper_requests.h"
#include "../boards/waveshare-epaper-397/epaper_panel_update.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
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
    assert(key.Poll(true, 1400000) == Press::kHold); // hold acts without waiting for release
    assert(key.Poll(false, 1410000) == Press::kNone);
    assert(key.Poll(false, 1440000) == Press::kNone);
    assert(key.Poll(false, 1450000) == Press::kNone);
    epaper::Key boot_held;
    assert(boot_held.Poll(true, 0) == Press::kNone);
    assert(boot_held.Poll(false, 2000000) == Press::kNone);
    assert(boot_held.Poll(false, 2030000) == Press::kNone);
    assert(boot_held.Poll(true, 3000000) == Press::kNone);
    assert(boot_held.Poll(true, 3030000) == Press::kNone);
    assert(boot_held.Poll(false, 3100000) == Press::kNone);
    assert(boot_held.Poll(false, 3130000) == Press::kTap);
    epaper::Key direction;
    assert(direction.Poll(false, 0, true) == Press::kNone);
    assert(direction.Poll(true, 100000, true) == Press::kNone);
    assert(direction.Poll(true, 130000, true) == Press::kTap); // direction fires on press
    assert(direction.Poll(true, 529000, true) == Press::kNone);
    assert(direction.Poll(true, 530000, true) == Press::kTap);
    assert(direction.Poll(true, 680000, true) == Press::kTap);
    assert(direction.Poll(false, 700000, true) == Press::kNone);
    assert(direction.Poll(false, 730000, true) == Press::kNone);
    epaper::Key back;
    assert(back.Poll(false, 0, true, false) == Press::kNone);
    assert(back.Poll(true, 100000, true, false) == Press::kNone);
    assert(back.Poll(true, 130000, true, false) == Press::kTap);
    assert(back.Poll(true, 2000000, true, false) == Press::kNone); // holding BOOT cannot back through several pages
    assert(back.Poll(false, 2100000, true, false) == Press::kNone);
    assert(back.Poll(false, 2130000, true, false) == Press::kNone);
    epaper::PendingConfirm confirm;
    confirm.Request(10);
    assert(!confirm.Take(8, 10) && confirm.Pending()); // quick confirm waits for accumulated navigation
    assert(confirm.Take(10, 10));
    assert(!confirm.Take(10, 10)); // execute once
    confirm.Request(11); confirm.Cancel();
    assert(!confirm.Take(11, 11)); // BOOT cancels pending confirmation
    confirm.Request(12);
    assert(!confirm.Take(12, 13) && !confirm.Pending()); // rebind/page change invalidates it
    epaper::RequestState request;
    const uint32_t first_request = request.Begin();
    assert(first_request && request.Busy() && !request.Begin());
    request.Cancel();
    assert(!request.Busy() && !request.Begin()); // HTTP still owns the single worker
    assert(!request.Complete(first_request)); // a cancelled result cannot reopen the old page
    const uint32_t second_request = request.Begin();
    assert(second_request != first_request && request.Busy());
    assert(!request.Complete(first_request) && request.Busy()); // stale completion cannot consume a new result
    assert(request.Complete(second_request) && !request.Busy());
    epaper::RefreshPolicy refresh;
    const epaper::Rect small{0, 0, 80, 30};
    assert(refresh.Full(epaper::Scene::kSetup, 0, small));
    refresh.Success(true, epaper::Scene::kSetup, 0);
    assert(!refresh.Full(epaper::Scene::kSetup, 0, small) && refresh.Ready(1));
    for (const auto scene : {epaper::Scene::kConnecting, epaper::Scene::kActivation, epaper::Scene::kOnline}) {
        assert(refresh.Full(scene, 0, small)); // every configuration stage establishes a clean base
        refresh.Success(true, scene, 0);
    }
    assert(refresh.Full(epaper::Scene::kOnline, 1, small)); // entering a different app page
    refresh.Success(true, epaper::Scene::kOnline, 1);
    assert(!refresh.Full(epaper::Scene::kOnline, 1, {0, 0, 800, 159}));
    assert(refresh.Full(epaper::Scene::kOnline, 1, {0, 0, 800, 160})); // one third of the panel
    for (int i = 0; i < 64; ++i) {
        assert(!refresh.Full(epaper::Scene::kOnline, 1, small));
        refresh.Success(false, epaper::Scene::kOnline, 1);
    }
    assert(refresh.Full(epaper::Scene::kOnline, 1, small));
    refresh.Failure(1000000);
    assert(refresh.Full(epaper::Scene::kOnline, 1, small) && !refresh.Ready(1499999) && refresh.Ready(1500000));
    refresh.Failure(2000000);
    assert(!refresh.Ready(2999999) && refresh.Ready(3000000));
    refresh.Success(true, epaper::Scene::kOnline, 1);
    assert(refresh.Ready(3000001) && !refresh.Full(epaper::Scene::kOnline, 1, small));
    assert(!epaper::Current(0, 0));
    assert(!epaper::Current(7, 8)); // queued press on old e-paper page must not choose a new item
    assert(epaper::Current(8, 8));
    assert(epaper::NavigationCurrent(8, 8, 10));
    assert(!epaper::NavigationCurrent(7, 8, 10));
    assert(!epaper::Current(8, 10)); // Do not confirm a row that has not appeared yet.
    for (const auto action : {epaper::Action::kUp, epaper::Action::kDown, epaper::Action::kCenter,
                              epaper::Action::kMenu, epaper::Action::kBack}) {
        assert(!epaper::InputAllowed(true, action));
        assert(epaper::InputAllowed(false, action));
    }
    assert(epaper::Volume(0, -1) == 0);
    assert(epaper::Volume(100, 1) == 100);
    assert(epaper::Volume(60, 1) == 65);
    assert(epaper::Volume(60, -1) == 55);
    uint8_t frame[epaper::kStride * epaper::kPanelHeight];
    uint8_t previous[sizeof frame];
    memset(frame, 0xff, sizeof frame); memset(previous, 0xff, sizeof previous);
    epaper::Rect rect{};
    assert(!epaper::ChangedRect(frame, previous, rect));
    assert(!epaper::WrapGlyph(430, 20, 460, 30, 460));
    assert(epaper::WrapGlyph(430, 20, 460, 31, 461));
    assert(epaper::WrapGlyph(430, 20, 460, 10, 461)); // glyph bearing extends past advance
    assert(epaper::InTextBox(20, 78, 20, 78, 460, 140));
    assert(epaper::InTextBox(459, 139, 20, 78, 460, 140));
    assert(!epaper::InTextBox(460, 139, 20, 78, 460, 140));
    assert(!epaper::InTextBox(459, 140, 20, 78, 460, 140));
    epaper::BlackPixel(frame, 0, 0);
    assert(frame[479 * epaper::kStride] == 0x7f); // logical top left -> native bottom left
    assert(epaper::ChangedRect(frame, previous, rect));
    assert(rect.x0 == 0 && rect.x1 == 8 && rect.y0 == 479 && rect.y1 == 480);
    epaper::BlackPixel(frame, 479, 799);
    assert(frame[99] == 0xfe);
    assert(epaper::ChangedRect(frame, previous, rect));
    assert(rect.x0 == 0 && rect.x1 == 800 && rect.y0 == 0 && rect.y1 == 480);
    epaper::ClearBox(frame, 0, 0, 1, 1);
    assert(frame[479 * epaper::kStride] == 0xff && frame[99] == 0xfe);
    memcpy(previous, frame, sizeof frame);
    epaper::BlackPixel(frame, -1, 0); epaper::BlackPixel(frame, 480, 0);
    epaper::BlackPixel(frame, 0, -1); epaper::BlackPixel(frame, 0, 800);
    assert(!epaper::ChangedRect(frame, previous, rect));
    memset(frame, 0xff, sizeof frame); // Clearing pixels also belongs to the changed region.
    assert(epaper::ChangedRect(frame, previous, rect));
    for (size_t i = 0; i < sizeof frame; ++i) frame[i] = static_cast<uint8_t>(i * 37 + i / 100);
    std::vector<uint8_t> commands;
    std::vector<std::vector<uint8_t>> payloads;
    assert(epaper::QueuePartialFrame(frame, [&](uint8_t command, const uint8_t* data, size_t bytes) {
        commands.push_back(command);
        payloads.emplace_back();
        if (bytes) payloads.back().assign(data, data + bytes);
        return true;
    }));
    assert((commands == std::vector<uint8_t>{0x18, 0x3c, 0x44, 0x45, 0x4e, 0x4f, 0x24, 0x22, 0x20}));
    assert((payloads[2] == std::vector<uint8_t>{0, 0, 0x18, 3}));
    assert((payloads[3] == std::vector<uint8_t>{0xdf, 1, 0, 0}));
    assert((payloads[4] == std::vector<uint8_t>{0, 0}) && payloads[4] == payloads[5]);
    assert(payloads[6].size() == sizeof frame && !memcmp(payloads[6].data(), frame, sizeof frame));
    assert((payloads[7] == std::vector<uint8_t>{0xff}));
    commands.clear();
    assert(!epaper::QueuePartialFrame(frame, [&](uint8_t command, const uint8_t*, size_t) {
        commands.push_back(command); return command != 0x24;
    }));
    assert(commands.back() == 0x24 && commands.size() == 7); // Never trigger an incomplete image.
    puts("Waveshare controls: press/repeat, hold, setup lock, deferred confirmation and cancelled-request checks passed");
    puts("Waveshare refresh: configuration transitions, large changes, small updates and recovery checks passed");
    puts("Waveshare frame: rotation, clipping and changed-region checks passed");
    puts("Waveshare transfer: fixed origin, full native frame, vendor waveform and failed-write checks passed");
}
