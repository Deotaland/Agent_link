// Streaming decoder for MiniMax T2A v2 replies.
//
// The reply is JSON with the MP3 hex-encoded in data.audio. Feed the HTTP body in any chunk sizes:
// the hex is picked out as it arrives, decoded with minimp3 and returned as 16 kHz mono PCM16
// (downmixed / resampled if needed), so playback can start before the download finishes.
// No ESP-IDF dependencies, so it also builds on a PC for testing.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "minimp3.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TTS_PCM_RATE 16000

// `frames` mono samples at TTS_PCM_RATE. Return false to stop decoding.
typedef bool (*tts_pcm_fn)(const int16_t* pcm, size_t frames, void* ctx);

// About 20 KB, allocated by the caller (PSRAM on the device).
typedef struct {
    int      scan;                 // JSON scan state
    size_t   matched;              // chars of the "audio" key matched so far
    int      hi;                   // high hex digit of the current byte, or -1
    uint8_t  in[8192];             // MP3 not decoded yet
    size_t   in_len;
    mp3dec_t mp3;
    int16_t  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];       // decoded frame
    int16_t  out[MINIMP3_MAX_SAMPLES_PER_FRAME + 4];   // resampled frame
    int      src_hz;               // MP3 sample rate, 0 before the first frame
    uint32_t rs_step, rs_pos;      // resampler step and position, 16.16
    int16_t  rs_last;
    size_t   frames;               // PCM frames output
    size_t   mp3_bytes;            // MP3 bytes received
    bool     stopped;
    char     head[160];            // start of the body, logged when no audio came
    size_t   head_len;
} tts_decoder_t;

// Call before each reply.
void tts_decoder_reset(tts_decoder_t* d);
// Next `len` bytes of the body. Returns false once `pcm` has returned false.
bool tts_decoder_feed(tts_decoder_t* d, const char* body, size_t len, tts_pcm_fn pcm, void* ctx);
// End of the body: decodes whatever is left.
void tts_decoder_finish(tts_decoder_t* d, tts_pcm_fn pcm, void* ctx);
// Start of the body as a string, e.g. the server's error message.
const char* tts_decoder_head(tts_decoder_t* d);

#ifdef __cplusplus
}
#endif
