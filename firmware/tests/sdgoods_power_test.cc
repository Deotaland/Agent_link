#include "sdgoods_power_stubs.h"
#include "../boards/sdgoods-esp32s3/sdgoods_power.c"
#include <cstdio>

static void ExpectOff(bool allow_resume=true) {
    using namespace power_fixture;
    try {
        (void)sdgoods_power_startup(allow_resume);
        assert(false);
    } catch (const StandbySignal&) {}
    assert(levels.at(7)==0 && levels.at(12)==1 && levels.at(13)==0);
    assert(levels.at(9)==0 && levels.at(5)==0 && levels.at(3)==0);
    assert(holds==std::set<int>({3,5,7,9,12,13}) && deep_hold);
    assert(wake_pin==6 && wake_level==0 && key_pullup);
    assert(!sdgoods_power_take_sleep_resume());
}

int main() {
    using namespace power_fixture;
    Reset(ESP_RST_POWERON); // USB inserted or power supplied without a key.
    ExpectOff(false);
    assert(now==0);
    Reset(ESP_RST_POWERON,true,2990000);
    ExpectOff();
    assert(now==2990000); // Releasing early never latches the supply.
    Reset(ESP_RST_POWERON,true,3010000);
    assert(sdgoods_power_startup(false) && levels.at(7)==1 && now==3050000);
    assert(!sdgoods_power_take_sleep_resume());
    Reset(ESP_RST_POWERON,true,10000);
    bootloader_hold=true; // Existing cold recovery check already held for 3s.
    assert(sdgoods_power_startup(false) && levels.at(7)==1 && now==50000 && !bootloader_hold);
    Reset(ESP_RST_POWERON);
    bootloader_hold=true; // A stale proof without a pressed key cannot power on.
    ExpectOff();

    Reset(ESP_RST_DEEPSLEEP,true,1190000);
    wake_cause=ESP_SLEEP_WAKEUP_EXT0;
    ExpectOff(false); // USB-powered off-state short press remains off.
    Reset(ESP_RST_DEEPSLEEP,true,3010000);
    wake_cause=ESP_SLEEP_WAKEUP_EXT0;
    assert(sdgoods_power_startup(false) && now==3050000 && levels.at(7)==1);
    assert(!sdgoods_power_take_sleep_resume());

    Reset(ESP_RST_DEEPSLEEP);
    wake_cause=ESP_SLEEP_WAKEUP_EXT0;
    sdgoods_power_set_sleep_resume(true);
    assert(!sdgoods_power_startup(true) && now==0 && levels.at(7)==1);
    assert(sdgoods_power_take_sleep_resume() && !sdgoods_power_take_sleep_resume());
    Reset(ESP_RST_DEEPSLEEP);
    wake_cause=ESP_SLEEP_WAKEUP_EXT0;
    ExpectOff(); // A consumed intentional-sleep marker cannot wake twice.
    Reset(ESP_RST_DEEPSLEEP);
    wake_cause=ESP_SLEEP_WAKEUP_EXT0;
    sdgoods_power_set_sleep_resume(true);
    ExpectOff(false); // Recovery never trusts another image's RTC marker.
    Reset(ESP_RST_POWERON);
    sdgoods_power_set_sleep_resume(true);
    ExpectOff(); // Cold power invalidates a stale sleep marker.
    Reset(ESP_RST_DEEPSLEEP);
    sdgoods_power_set_sleep_resume(true);
    ExpectOff(); // Only the intentional EXT0 wake can resume without a hold.
    for (const auto reset : {ESP_RST_USB,ESP_RST_EXT,ESP_RST_BROWNOUT}) {
        Reset(reset);
        ExpectOff(); // Flash/reset and supply resets also default to off.
    }
    for (const auto reset : {ESP_RST_SW,ESP_RST_PANIC,ESP_RST_INT_WDT,ESP_RST_TASK_WDT,ESP_RST_WDT}) {
        Reset(reset);
        sdgoods_power_set_sleep_resume(true);
        assert(!sdgoods_power_startup(true) && now==0 && levels.at(7)==1);
        assert(!sdgoods_power_take_sleep_resume());
    }
    puts("Sugarat power: cold standby, short/long start, retained rails, bootloader proof, USB off wake, one-shot sleep resume and routing restarts passed. No hardware I/O.");
}
