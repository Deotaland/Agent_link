#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { AGENT_FRIEND_OFF, AGENT_FRIEND_STARTING, AGENT_FRIEND_SCANNING,
    AGENT_FRIEND_WAITING, AGENT_FRIEND_CONFIRM, AGENT_FRIEND_CONNECTED,
    AGENT_FRIEND_STOPPING, AGENT_FRIEND_ERROR } agent_friend_state_t;
typedef enum { AGENT_FRIEND_EVENT_NONE, AGENT_FRIEND_EVENT_REJECTED,
    AGENT_FRIEND_EVENT_TIMEOUT, AGENT_FRIEND_EVENT_LOST, AGENT_FRIEND_EVENT_GREETING_FAILED } agent_friend_event_t;
typedef struct { uint8_t mac[6]; char name[24]; int rssi; } agent_friend_peer_t;
typedef struct {
    agent_friend_state_t state;
    agent_friend_event_t event;
    esp_err_t error;
    char local_name[24], remote_name[24], code[7];
    bool local_confirmed, remote_confirmed;
    uint32_t sent, received;
    int peer_count;
    agent_friend_peer_t peers[6];
} agent_friend_snapshot_t;
// Nonblocking commands. The SDK worker owns the exclusive radio lease and
// pauses/resumes cloud WiFi without changing credentials or platform binding.
esp_err_t agent_link_friends_start(void);
esp_err_t agent_link_friends_stop(void);
esp_err_t agent_link_friends_select(const uint8_t mac[6]);
esp_err_t agent_link_friends_confirm(bool accept);
esp_err_t agent_link_friends_greet(void);
esp_err_t agent_link_friends_disconnect(void);
void agent_link_friends_snapshot(agent_friend_snapshot_t* out);
#ifdef __cplusplus
}
#endif
