#pragma once
// WiFi platform credential: the auth_key bind-status returns once a user claims the device.
// Kept apart from the device identity, which survives a factory reset; this is what
// agent_link_forget() erases.

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOUD_AUTH_KEY_MAX 65   // the platform caps auth_key at 64 chars

/** @brief Read the stored key, if any. Call once at session start. */
esp_err_t cloud_cred_load(void);

/** @brief The stored auth_key, or "" when not bound. */
const char* cloud_cred_auth_key(void);

/** @brief true once the device has been claimed and holds a key. */
bool cloud_cred_is_bound(void);

/** @brief Persist a newly issued auth_key. */
esp_err_t cloud_cred_set_auth_key(const char* key);

/** @brief Erase the key (revoked, or factory reset). The device identity is kept. */
esp_err_t cloud_cred_clear(void);

#ifdef __cplusplus
}
#endif
