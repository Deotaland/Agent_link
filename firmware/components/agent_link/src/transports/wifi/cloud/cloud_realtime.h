#pragma once

#include "esp_err.h"
#include "agent_link_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

// Receives one-use Agent Stack grants from the authenticated device downlink.
// The worker owns TLS and never runs on the MQTT callback task.
esp_err_t cloud_realtime_start(void);
void      cloud_realtime_stop(void);
esp_err_t cloud_realtime_offer(const char* websocket_url, const char* connect_token);

typedef struct {
    void (*state)(bool ready, const char* hint);
    void (*pcm)(const uint8_t* data, size_t bytes);
    void (*text)(const char* utf8);
    void (*audio_end)(void);
    void (*audio_reset)(void);
    void (*finished)(void);
} cloud_realtime_callbacks_t;

void cloud_realtime_set_callbacks(const cloud_realtime_callbacks_t* callbacks);
void cloud_realtime_cancel(void);
bool cloud_realtime_ready(void);
bool cloud_realtime_direct_enabled(void);
esp_err_t cloud_realtime_request_direct(void);
esp_err_t cloud_realtime_request_platform(int agent_id, const char* session_id);
esp_err_t cloud_realtime_stream_start(agent_stream_t type);
esp_err_t cloud_realtime_stream_write(agent_stream_t type, const uint8_t* data, size_t bytes);
esp_err_t cloud_realtime_stream_end(agent_stream_t type, bool complete);

#ifdef __cplusplus
}
#endif
