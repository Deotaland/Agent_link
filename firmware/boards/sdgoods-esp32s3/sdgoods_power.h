#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Run before asserting the battery latch or initializing peripherals. Recovery
// never accepts an application's RTC sleep marker, and waits for key release
// before routing through a software restart. Returns true for a new,
// qualified power-key start, so it cannot also select the backup Launcher.
bool sdgoods_power_startup(bool allow_sleep_resume);
void sdgoods_power_set_sleep_resume(bool resume);
bool sdgoods_power_take_sleep_resume(void);

#ifdef __cplusplus
}
#endif
