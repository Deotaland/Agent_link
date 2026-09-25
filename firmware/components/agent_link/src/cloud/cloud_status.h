#pragma once
// Cloud session states, internal to the WiFi backend. transport_wifi.cpp translates them into
// agent_link_status_t before anything outside the component sees them.
//
//   no IP -> claim-code -> show code -> poll bind-status -> (user claims it in the console)
//         -> auth_key saved -> auth -> online

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Where the device is in the claim/auth lifecycle. */
typedef enum {
    AGENT_CLOUD_IDLE = 0,     ///< Not started
    AGENT_CLOUD_NO_NETWORK,   ///< No usable network (joining, or the portal is up)
    AGENT_CLOUD_CLAIMING,     ///< Showing an activation code, waiting for it to be claimed
    AGENT_CLOUD_BOUND,        ///< Has an auth_key, authenticating
    AGENT_CLOUD_ONLINE,       ///< Authenticated, heartbeat running
    AGENT_CLOUD_FAULT,        ///< Refused or unreachable; see api_code

    // Refusals that retrying cannot clear; they need action in the console.
    AGENT_CLOUD_ALREADY_BOUND,  ///< SN is claimed by an account; unbind it before re-claiming
    AGENT_CLOUD_NO_AGENT,       ///< Authenticated, but no agent is attached
} agent_cloud_state_t;

/** @brief Snapshot of the cloud session. */
typedef struct {
    agent_cloud_state_t state;
    char                code[8];      ///< Activation code (CLAIMING only)
    uint32_t            expires_s;    ///< Seconds left on the code
    int                 api_code;     ///< Last platform business code; 0 ok, <0 no response
    char                message[96];  ///< Last platform message, for logging (not display)
} agent_cloud_status_t;

/**
 * @brief Called on every state change, and on each countdown tick while CLAIMING.
 * @note Runs on the session task; keep it short.
 */
typedef void (*agent_cloud_state_cb_t)(const agent_cloud_status_t* st, void* ctx);

#ifdef __cplusplus
}
#endif
