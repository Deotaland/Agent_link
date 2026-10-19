#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int id;
    char name[80];
    bool voice_supported;
} agent_link_platform_agent_t;

typedef struct {
    char id[80];
    char name[96];
    char created_at[32];
} agent_link_platform_conversation_t;

// These calls use the activated device auth_key and perform HTTPS I/O.
// Call them from a worker, never from an LVGL callback.
bool agent_link_platform_available(void);
// Cached from device-gateway/auth; this does not fetch the account's agent list.
esp_err_t agent_link_platform_bound_agent(agent_link_platform_agent_t* agent);
// Legacy account-management APIs are unsupported on devices.
esp_err_t agent_link_platform_agents(int page, agent_link_platform_agent_t* entries,
                                     size_t capacity, size_t* count, int* total);
esp_err_t agent_link_platform_bind_agent(int agent_id);
esp_err_t agent_link_platform_conversations(int agent_id, size_t offset,
                                            agent_link_platform_conversation_t* entries,
                                            size_t capacity, size_t* count, size_t* total);
esp_err_t agent_link_platform_create_conversation(int agent_id, char* id, size_t id_size);
esp_err_t agent_link_platform_delete_conversation(int agent_id, const char* conversation_id);

// Asynchronously request one grant and connect the existing realtime voice worker.
esp_err_t agent_link_request_realtime_session(int agent_id, const char* session_id);

#ifdef __cplusplus
}
#endif
