#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
// Queue a UTF-8 user message for the desktop's chat worker. Future ASR clients
// call this after transcription; no audio capture or ASR request happens here.
// ESP_OK means queued; replies/errors are shown by the LLM application.
esp_err_t paper_s31_send_text(const char* utf8);
#ifdef __cplusplus
}
#endif
