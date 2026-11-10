#include "paper4_state.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace paper4;
    Button confirm;
    assert(confirm.Poll(true, 0, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(true, 2000000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(false, 2010000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(false, 2040000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(true, 2100000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(true, 2130000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(false, 2200000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(false, 2230000, Action::Confirm, true) == Action::Confirm);
    assert(confirm.Poll(true, 2300000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(true, 2330000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(true, 3330000, Action::Confirm, true) == Action::Back);
    assert(confirm.Poll(false, 3340000, Action::Confirm, true) == Action::None);
    assert(confirm.Poll(false, 3370000, Action::Confirm, true) == Action::None);

    Confirm target;
    target.Request(7);
    assert(!target.Take(6, 7));
    assert(target.Take(7, 7));
    assert(!target.Take(7, 7));
    target.Request(7);
    assert(!target.Take(7, 8));
    assert(!target.Pending());

    std::tm date{};
    date.tm_year = 124; date.tm_mon = 1; date.tm_mday = 31;
    ClampDate(date); assert(date.tm_mday == 29);
    date.tm_year = 125; ClampDate(date); assert(date.tm_mday == 28);
    int year = 2026, month = 1;
    MoveMonth(year, month, -1); assert(year == 2025 && month == 12);
    year = 2000; month = 1; MoveMonth(year, month, -1); assert(year == 2000 && month == 1);
    year = 2099; month = 12; MoveMonth(year, month, 1); assert(year == 2099 && month == 12);

    Alarm alarm;
    date.tm_year = 126; date.tm_mon = 10; date.tm_mday = 9; date.tm_hour = 7; date.tm_min = 30;
    assert(!alarm.Due(date, false, 7, 30));
    assert(alarm.Due(date, true, 7, 30));
    assert(!alarm.Due(date, true, 7, 30));
    ++date.tm_mday; assert(alarm.Due(date, true, 7, 30));

    Refresh refresh;
    assert(!refresh.Due(0, 1));
    assert(!refresh.Due(200000, 2));
    assert(!refresh.Due(499999, 2));
    assert(refresh.Due(500000, 2));
    refresh.Complete(1000000);
    assert(!refresh.Due(1500000, 3));
    assert(refresh.Due(2000000, 3));
    refresh.Failed(2000000);
    assert(!refresh.Due(61999999, 3));
    assert(refresh.Due(62000000, 3));
    assert(ActivationDetail("123456", "600 seconds") == ActivationDetail("123456", "595 seconds"));
    assert(ActivationDetail("", "Network failed") == "Network failed");
    HeaderCache metadata;
    assert(metadata.Read("12:30 80%", 0, false) == "12:30 80%");
    assert(metadata.Read("12:30 79%", 1000000, false) == "12:30 80%");
    assert(metadata.Read("12:31 79%", 60000000, false) == "12:31 79%");
    assert(metadata.Read("setup", 61000000, true) == "setup");
    assert(metadata.Read("changed", 600000000, true) == "setup");
    assert(metadata.Read("online", 601000000, false) == "online");
    std::puts("Paper 4IN0 controls, visible confirmation, calendar, alarm and refresh checks passed");
}
