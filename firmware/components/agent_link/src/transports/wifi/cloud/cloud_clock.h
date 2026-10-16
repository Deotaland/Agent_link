#pragma once
// Wall-clock time for the WiFi channel, whose every message carries a 13-digit Unix time in ms.
//
// The ESP32 has no battery-backed clock and boots in 1970. Two sources set it: SNTP, started with
// the cloud session, and the platform's own clock, read off each mqtt-token answer (expire_at
// minus expires_in is the platform's "now"). The platform's is used only until SNTP has
// synchronised, which covers networks that block NTP, and it always arrives before MQTT connects.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start SNTP. Idempotent; the network may still be coming up. */
void cloud_clock_start(void);

/** @brief The platform's time, in Unix seconds. Applied only while SNTP has not synchronised. */
void cloud_clock_from_platform(int64_t unix_s);

/** @brief Unix time in milliseconds. Meaningless until cloud_clock_valid(). */
int64_t cloud_clock_now_ms(void);

/** @brief Whether either source has set the clock. */
bool cloud_clock_valid(void);

#ifdef __cplusplus
}
#endif
