#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call paper_hardware_init() before using the codec or PA.
esp_err_t paper_audio_init(void);
esp_err_t paper_audio_speaker_start(void);
esp_err_t paper_audio_speaker_stop(void);
esp_err_t paper_audio_write_mono(const int16_t *pcm, size_t samples);
esp_err_t paper_audio_mic_start(void);
esp_err_t paper_audio_mic_stop(void);
bool paper_audio_mic_active(void);
esp_err_t paper_audio_read_mono(int16_t *pcm, size_t capacity, size_t *samples);

#ifdef __cplusplus
}
#endif
