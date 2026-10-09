// See muse_tts_decode.h. Keep this file free of ESP-IDF so it still builds on a PC.
#include "muse_tts_decode.h"

#include <string.h>

// Find "audio", then ':' and '"', then read hex pairs until the closing quote.
enum { SCAN_FIND, SCAN_COLON, SCAN_QUOTE, SCAN_HEX, SCAN_AFTER };
static const char kAudioKey[] = "\"audio\"";

// MP3 bytes kept back until the body ends. minimp3 only takes a frame once it can see the next
// header, and needs two frames in a row to sync (max frame 1441 bytes + 4-byte header).
#define DECODE_AHEAD 2900

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void tts_decoder_reset(tts_decoder_t* d)
{
    d->scan = SCAN_FIND;
    d->matched = 0;
    d->hi = -1;
    d->in_len = 0;
    mp3dec_init(&d->mp3);
    d->src_hz = 0;
    d->rs_step = 0;
    d->rs_pos = 0;
    d->rs_last = 0;
    d->frames = 0;
    d->mp3_bytes = 0;
    d->stopped = false;
    d->head_len = 0;
    d->head[0] = '\0';
}

// Outputs one decoded frame: downmix to mono, resample to TTS_PCM_RATE if needed.
static void emit(tts_decoder_t* d, int samples, int channels, int hz, tts_pcm_fn pcm, void* ctx)
{
    int16_t* x = d->pcm;
    if (channels == 2) {
        for (int i = 0; i < samples; i++) {
            x[i] = (int16_t)(((int32_t)x[2 * i] + x[2 * i + 1]) / 2);
        }
    }
    const int16_t* out = x;
    size_t n = (size_t)samples;
    if (hz != TTS_PCM_RATE) {
        if (hz != d->src_hz) {
            // Rate changed: restart at this frame's first sample.
            d->rs_step = (uint32_t)(((uint64_t)hz << 16) / TTS_PCM_RATE);
            d->rs_pos = 1u << 16;
            d->rs_last = x[0];
        }
        // Linear interpolation. Position 0 is the previous frame's last sample, 1..samples are
        // this frame's.
        const size_t cap = sizeof(d->out) / sizeof(d->out[0]);
        const uint32_t end = (uint32_t)samples << 16;
        n = 0;
        while (n < cap && d->rs_pos < end) {
            const uint32_t j = d->rs_pos >> 16;
            const int32_t a = j ? x[j - 1] : d->rs_last;
            const int32_t b = x[j];
            const int64_t f = d->rs_pos & 0xFFFF;
            d->out[n++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * f) >> 16));
            d->rs_pos += d->rs_step;
        }
        if (d->rs_pos < end) d->rs_pos = end;   // out[] full, only possible below 8 kHz: skip the rest
        d->rs_pos -= end;
        d->rs_last = x[samples - 1];
        out = d->out;
    }
    d->src_hz = hz;
    if (n && !d->stopped) {
        d->frames += n;
        if (!pcm(out, n, ctx)) d->stopped = true;
    }
}

// Decodes the buffered frames, keeping DECODE_AHEAD bytes back unless the body has ended.
static void decode(tts_decoder_t* d, bool final, tts_pcm_fn pcm, void* ctx)
{
    size_t off = 0;
    while (!d->stopped && d->in_len - off > (final ? 0 : DECODE_AHEAD)) {
        mp3dec_frame_info_t info;
        const int samples = mp3dec_decode_frame(&d->mp3, d->in + off, (int)(d->in_len - off), d->pcm, &info);
        if (info.frame_bytes <= 0) break;   // no complete frame (yet)
        off += (size_t)info.frame_bytes;
        if (samples > 0 && info.hz > 0 && (info.channels == 1 || info.channels == 2)) {
            emit(d, samples, info.channels, info.hz, pcm, ctx);
        }
    }
    if (final) {
        d->in_len = 0;   // leftover bytes are not a frame
    } else if (off) {
        memmove(d->in, d->in + off, d->in_len - off);
        d->in_len -= off;
    }
}

bool tts_decoder_feed(tts_decoder_t* d, const char* body, size_t len, tts_pcm_fn pcm, void* ctx)
{
    for (size_t i = 0; i < len && !d->stopped; i++) {
        const char ch = body[i];
        if (d->scan != SCAN_HEX && d->head_len < sizeof(d->head) - 1) d->head[d->head_len++] = ch;
        switch (d->scan) {
        case SCAN_FIND:
            d->matched = ch == kAudioKey[d->matched] ? d->matched + 1 : ch == kAudioKey[0] ? 1 : 0;
            if (d->matched == sizeof(kAudioKey) - 1) d->scan = SCAN_COLON;
            break;
        case SCAN_COLON:
            if (ch == ':') {
                d->scan = SCAN_QUOTE;
            } else if (!is_space(ch)) {
                d->scan = SCAN_FIND;
                d->matched = 0;
            }
            break;
        case SCAN_QUOTE:
            if (ch == '"') {
                d->scan = SCAN_HEX;
            } else if (!is_space(ch)) {
                d->scan = SCAN_FIND;   // e.g. "audio": null
                d->matched = 0;
            }
            break;
        case SCAN_HEX: {
            if (ch == '"') {
                d->scan = SCAN_AFTER;
                break;
            }
            const int v = hex_digit(ch);
            if (v < 0) break;
            if (d->hi < 0) {
                d->hi = v;
                break;
            }
            d->in[d->in_len++] = (uint8_t)(d->hi << 4 | v);
            d->hi = -1;
            d->mp3_bytes++;
            if (d->in_len == sizeof(d->in)) {
                decode(d, false, pcm, ctx);
                if (d->in_len == sizeof(d->in)) d->in_len = 0;   // 8 KB without a frame: not MP3, drop it
            }
            break;
        }
        default:
            break;
        }
    }
    if (!d->stopped) decode(d, false, pcm, ctx);
    return !d->stopped;
}

void tts_decoder_finish(tts_decoder_t* d, tts_pcm_fn pcm, void* ctx)
{
    if (!d->stopped) decode(d, true, pcm, ctx);
}

const char* tts_decoder_head(tts_decoder_t* d)
{
    d->head[d->head_len] = '\0';
    return d->head;
}
