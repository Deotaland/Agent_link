#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Blocking HTTPS calls. Run on an application worker, never a display/BLE callback.
// All operations are restricted to the currently bound Agent. Returned JSON/text is
// caller-owned; overflow and incomplete SSE streams fail instead of returning partial tools.
esp_err_t agent_link_chat_create(int agent_id, char* conversation_id, size_t capacity);
esp_err_t agent_link_chat_conversations(int agent_id, const char* cursor, char* json, size_t capacity);
esp_err_t agent_link_chat_messages(int agent_id, const char* conversation_id,
                                   const char* cursor, char* json, size_t capacity);
esp_err_t agent_link_chat_send(int agent_id, const char* conversation_id,
                              const char* text, char* reply, size_t capacity);
// Replaces only our marked block, preserving the user's existing Agent prompt.
// Uses device authentication; a platform permission error is returned to the caller.
esp_err_t agent_link_chat_sync_prompt(int agent_id, const char* instructions);

#ifdef __cplusplus
}
#endif
