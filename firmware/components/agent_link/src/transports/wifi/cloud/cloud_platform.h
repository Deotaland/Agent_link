#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "realtime_format.h"

void cloud_platform_set_base_url(const char* base_url);
const char* cloud_platform_base_url(void);
bool cloud_platform_enabled(void);
void cloud_platform_set_bound_agent(const cJSON* auth_data);
esp_err_t cloud_platform_grant(int agent_id, const char* session_id, bool refresh,
                               char* url, size_t url_size, char* token, size_t token_size,
                               agentlink::realtime::AudioFormat* audio, int* http_status);
void cloud_platform_end_grant(int agent_id, const char* session_id);
