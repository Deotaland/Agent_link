#pragma once
// Device identity: one UUIDv4 shared by both transports. BLE sends its 16 bytes in the 0x01
// RequestDeviceInfo response; WiFi sends the 36-char string to the platform as device_sn.
//
// Stored in NVS "agent_link"/"dev_uuid". It survives OTA and factory reset: agent_link_forget()
// clears credentials but not this, so the platform keeps recognising the unit.
//
// esp_fill_random() is only a true RNG while RF (WiFi or BT) is running; before that, identical
// units generate identical values. Only mint once a transport has started.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load the identity from NVS, creating it if needed. Idempotent and thread-safe.
 * @param allow_mint Create one if none exists. Pass true only when RF is running.
 * @return ESP_OK when loaded; ESP_ERR_NOT_FOUND if none exists and @p allow_mint is false; an NVS
 *         error if a new identity could not be stored (it is then not used).
 */
esp_err_t dev_identity_load(bool allow_mint);

/** @brief Canonical 36-char lowercase string, or "" if not loaded. */
const char* dev_identity_sn(void);

/** @brief The 16 raw bytes, RFC 4122 order. Returns false if not loaded. */
bool dev_identity_uuid(uint8_t out[16]);

#ifdef __cplusplus
}
#endif
