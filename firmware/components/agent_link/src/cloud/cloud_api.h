#pragma once
// Client for the platform's device endpoints (/api/device-gateway/*), one function per endpoint.
// The only code that knows the wire format; callers get typed structs.
//
// Protocol notes:
//   1. Every response is HTTP 200, errors included; success is body.code / body.success.
//   2. body.data is an object on success, {} on a business error and null on a validation error,
//      so every read of it is type-checked.
//   3. Code 1002103 means the auth_key is invalid or revoked; the session clears it and re-claims.
//   4. A 401 "Missing required header" comes from the auth gateway in front of /api/, not from the
//      endpoint.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Business codes from body.code (not HTTP status).
#define CLOUD_API_OK            0        // success
#define CLOUD_API_BAD_FIELD     1        // validation failed; message is "body.<field>:<reason>;"
#define CLOUD_API_BAD_BODY      400      // request body could not be parsed
#define CLOUD_API_NO_HEADER     401      // rejected by the auth gateway (note 4)
#define CLOUD_API_ALREADY_BOUND 1002     // claim-code: the SN is already claimed by an account
#define CLOUD_API_KEY_REVOKED   1002103  // auth_key invalid or revoked
#define CLOUD_API_NO_AGENT      1002104  // auth: the device is bound but has no agent

/** @brief The platform's reply, on success or failure. */
typedef struct {
    int  code;          ///< body.code
    bool success;       ///< body.success
    char message[96];   ///< body.message, truncated
} cloud_api_err_t;

/** @brief claim-code result. Idempotent: the same SN gets the same code while it is valid. */
typedef struct {
    char     code[8];      ///< 6 digits
    uint32_t expires_in;   ///< seconds (600)
} cloud_claim_t;

/** @brief bind-status result. */
typedef enum {
    CLOUD_BIND_PENDING = 0,  ///< not claimed yet; keep polling
    CLOUD_BIND_EXPIRED,      ///< code expired; request a new one
    CLOUD_BIND_BOUND,        ///< claimed; auth_key is set
} cloud_bind_state_t;

typedef struct {
    cloud_bind_state_t state;
    uint32_t           remaining_ttl;  ///< seconds left (PENDING)
    char               auth_key[65];   ///< set when BOUND
} cloud_bind_t;

/**
 * @brief mqtt-token result: where the broker is, how to log in to it, and the device's topics.
 *
 * Kept in RAM only. The platform issues one against the auth_key whenever asked, and NVS is stored
 * in plain text. Strings arrive whole or the call fails: a truncated password or topic would only
 * show up later, as a refusal from the broker.
 */
typedef struct {
    char     protocol[8];         ///< "wss" (MQTT over WebSocket over TLS) or "mqtts"
    char     host[64];            ///< broker_url: a host name, not a URL
    uint16_t port;                ///< broker_port; 0 = the protocol's default
    char     path[32];            ///< WebSocket path, e.g. "/mqtt"
    char     client_id[48];       ///< the device_sn
    char     username[64];        ///< opaque
    char     password[640];       ///< a JWT. CONNECT carries it with the id and username in esp-mqtt's 1KB buffer
    uint32_t expires_in;          ///< seconds the password stays valid from issue; 0 = not given
    uint32_t expire_at;           ///< the same as Unix time; minus expires_in, the platform's clock
    char     topic_down[96];      ///< channels.down.default: messages to the device
    char     topic_ota_down[96];  ///< channels.down.ota: firmware updates
    char     topic_up[96];        ///< channels.up.default
    char     topic_event_up[96];  ///< channels.up.event
} cloud_mqtt_token_t;

/**
 * @brief Set the platform root and allocate the response buffer.
 * @param base_url No trailing slash, e.g. "https://api.example.com".
 */
esp_err_t cloud_api_init(const char* base_url);
void      cloud_api_deinit(void);

/**
 * @brief Close the kept-alive connection. Call when the network goes away, so the next request
 *        reconnects at once instead of timing out on a dead socket.
 */
void      cloud_api_drop_connection(void);

/** @brief POST claim-code: request an activation code (see cloud_claim_t). */
esp_err_t cloud_api_claim_code(int product_id, const char* sn, const char* mac,
                               const char* chip_type, const char* fw_version,
                               cloud_claim_t* out, cloud_api_err_t* err);

/**
 * @brief POST bind-status: has the code been claimed?
 * @note Returns immediately (no long poll); callers poll every few seconds.
 */
esp_err_t cloud_api_bind_status(const char* sn, const char* code,
                                cloud_bind_t* out, cloud_api_err_t* err);

/**
 * @brief POST auth: verify the auth_key and fetch the agent's runtime config.
 * @param agent_json Receives body.data as raw JSON, NUL-terminated and truncated to @p cap. It is
 *                   not parsed here yet, as the platform has not fixed its format.
 */
esp_err_t cloud_api_auth(const char* auth_key, const char* mac, const char* fw_version,
                         char* agent_json, size_t cap, cloud_api_err_t* err);

/** @brief POST heartbeat: keep the device marked online. */
esp_err_t cloud_api_heartbeat(const char* auth_key, cloud_api_err_t* err);

/**
 * @brief POST mqtt-token: exchange the auth_key for MQTT credentials.
 * @note Independent of auth and safe to repeat. Refuses as auth does: CLOUD_API_KEY_REVOKED,
 *       CLOUD_API_NO_AGENT.
 */
esp_err_t cloud_api_mqtt_token(const char* auth_key, cloud_mqtt_token_t* out, cloud_api_err_t* err);

#ifdef __cplusplus
}
#endif
