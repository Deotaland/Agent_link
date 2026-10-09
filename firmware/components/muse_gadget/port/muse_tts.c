/*
 * MiniMax T2A v2 text-to-speech for Muse replies (CONFIG_MUSE_TTS_MINIMAX).
 *
 * Same request as the muse-gadget-sdk MiniMax port and the rorolee app: POST /v1/t2a_v2, not
 * streamed, MP3 16 kHz mono 32 kbps. The MP3 comes back hex-encoded in data.audio; it is decoded
 * while it downloads (muse_tts_decode.c) and passed to the sink at most ahead_ms ahead of playback.
 */
#include "muse_gadget.h"

#include "sdkconfig.h"

#if CONFIG_MUSE_TTS_MINIMAX

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "muse_tts_decode.h"

static const char *TAG = "muse_tts";

#define URL "https://api.minimaxi.com/v1/t2a_v2"
#define VOLUME 1.4               /* same as the rorolee app */
#define TEXT_CAP 1500            /* max bytes sent per piece */
#define QUEUE_LEN 4
/* minimp3 needs ~16 KB of stack, TLS the rest. PSRAM is fine: this task never writes flash. */
#define STACK_BYTES (24 * 1024)

typedef struct {
    char *text;
    uint32_t tag;
} piece_t;

typedef struct {
    uint32_t tag, gen;
    bool started;
    int64_t t_request;
} speaking_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static piece_t s_queue[QUEUE_LEN];   /* guarded by s_lock */
static unsigned s_head, s_count;
static volatile uint32_t s_gen;      /* incremented by muse_tts_stop() to cancel everything */

static muse_tts_sink_t s_sink;
static uint32_t s_ahead_ms = 300;
static TaskHandle_t s_task;
static tts_decoder_t *s_dec;         /* worker only */

/* Playback clock: when the sink started playing and how many frames it has been given since. */
static int64_t s_clock_t0;
static uint64_t s_clock_frames;

bool muse_tts_enabled(void)
{
    return CONFIG_MUSE_TTS_MINIMAX_KEY[0] != '\0';
}

void muse_tts_set_sink(const muse_tts_sink_t *sink, uint32_t ahead_ms)
{
    s_sink = *sink;
    s_ahead_ms = ahead_ms;
}

/* Blocks until the sink is at most s_ahead_ms ahead. The clock restarts once the sink has run
 * dry (gap between pieces, slow server). */
static void pace(size_t frames, uint32_t gen)
{
    const int64_t now = esp_timer_get_time();
    const int64_t queued_us = (int64_t)(s_clock_frames * 1000000ULL / MUSE_NOTE_SAMPLE_RATE);
    if (!s_clock_t0 || now - s_clock_t0 >= queued_us) {
        s_clock_t0 = now;
        s_clock_frames = 0;
    }
    for (;;) {
        const int64_t ahead_us = (int64_t)(s_clock_frames * 1000000ULL / MUSE_NOTE_SAMPLE_RATE) -
                                 (esp_timer_get_time() - s_clock_t0);
        if (gen != s_gen || ahead_us <= (int64_t)s_ahead_ms * 1000) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_clock_frames += frames;
}

static bool on_decoded(const int16_t *pcm, size_t frames, void *arg)
{
    speaking_t *sp = arg;
    if (sp->gen != s_gen) return false;
    if (!sp->started) {
        sp->started = true;
        ESP_LOGI(TAG, "speaking, %.2f s after asking", (esp_timer_get_time() - sp->t_request) / 1e6);
        if (s_sink.on_start) s_sink.on_start(sp->tag, s_sink.ctx);
    }
    pace(frames, sp->gen);
    if (sp->gen != s_gen) return false;
    if (s_sink.on_pcm) s_sink.on_pcm(pcm, frames, s_sink.ctx);
    return true;
}

/* Builds the JSON request. Markdown characters are stripped, otherwise they get read out. */
static char *request_body(const char *text)
{
    char *clean = heap_caps_malloc(TEXT_CAP + 1, MALLOC_CAP_SPIRAM);
    if (!clean) return NULL;
    size_t n = 0;
    const char *p = text;
    for (; *p && n < TEXT_CAP; p++) {
        if (*p != '*' && *p != '#' && *p != '`') clean[n++] = *p;
    }
    if (*p) {
        /* Truncated: drop a trailing partial UTF-8 character. */
        size_t lead = n;
        while (lead && ((unsigned char)clean[lead - 1] & 0xC0) == 0x80) lead--;
        if (lead) {
            const unsigned char b = (unsigned char)clean[lead - 1];
            const size_t need = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
            if (n - (lead - 1) < need) n = lead - 1;
        }
    }
    clean[n] = '\0';

    char *body = NULL;
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "model", CONFIG_MUSE_TTS_MINIMAX_MODEL);
        cJSON_AddStringToObject(root, "text", clean);
        cJSON_AddBoolToObject(root, "stream", false);
        cJSON *voice = cJSON_AddObjectToObject(root, "voice_setting");
        cJSON_AddStringToObject(voice, "voice_id", CONFIG_MUSE_TTS_MINIMAX_VOICE);
        cJSON_AddNumberToObject(voice, "speed", 1.0);
        cJSON_AddNumberToObject(voice, "vol", VOLUME);
        cJSON_AddNumberToObject(voice, "pitch", 0);
        cJSON *audio = cJSON_AddObjectToObject(root, "audio_setting");
        cJSON_AddNumberToObject(audio, "sample_rate", MUSE_NOTE_SAMPLE_RATE);
        cJSON_AddNumberToObject(audio, "bitrate", 32000);
        cJSON_AddStringToObject(audio, "format", "mp3");
        cJSON_AddNumberToObject(audio, "channel", 1);
        body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }
    heap_caps_free(clean);
    return body;
}

/* Fetches and plays one piece. Returns true if any audio was played. */
static bool speak(const piece_t *p, speaking_t *sp)
{
    char *body = request_body(p->text);
    if (!body) return false;
    const esp_http_client_config_t cfg = {
        .url = URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,   /* the Authorization header can exceed the default 512 */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    tts_decoder_reset(s_dec);
    int status = -1;
    if (!c) goto out;
    esp_http_client_set_header(c, "Authorization", "Bearer " CONFIG_MUSE_TTS_MINIMAX_KEY);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    sp->t_request = esp_timer_get_time();
    const int len = (int)strlen(body);
    if (esp_http_client_open(c, len) != ESP_OK || esp_http_client_write(c, body, len) != len) {
        ESP_LOGW(TAG, "can't reach MiniMax");
        goto out;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "no answer from MiniMax");
        goto out;
    }
    status = esp_http_client_get_status_code(c);
    static char buf[1024];   /* worker only */
    int n;
    while (sp->gen == s_gen && (n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
        if (!tts_decoder_feed(s_dec, buf, (size_t)n, on_decoded, sp)) break;
    }
    if (sp->gen == s_gen) tts_decoder_finish(s_dec, on_decoded, sp);

out:
    if (sp->gen != s_gen) {
        ESP_LOGI(TAG, "cut short after %.1f s", s_dec->frames / (double)TTS_PCM_RATE);
    } else if (s_dec->frames) {
        ESP_LOGI(TAG, "spoke %.1f s (%u bytes of MP3 at %d Hz)", s_dec->frames / (double)TTS_PCM_RATE,
                 (unsigned)s_dec->mp3_bytes, s_dec->src_hz);
    } else {
        ESP_LOGW(TAG, "no speech: HTTP %d, %s", status, tts_decoder_head(s_dec));
    }
    if (c) esp_http_client_cleanup(c);
    cJSON_free(body);
    return s_dec->frames > 0;
}

static bool pop(piece_t *p, uint32_t *gen)
{
    taskENTER_CRITICAL(&s_lock);
    const bool have = s_count > 0;
    if (have) {
        *p = s_queue[s_head];
        s_head = (s_head + 1) % QUEUE_LEN;
        s_count--;
        *gen = s_gen;
    }
    taskEXIT_CRITICAL(&s_lock);
    return have;
}

static bool nothing_waiting(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool empty = s_count == 0;
    taskEXIT_CRITICAL(&s_lock);
    return empty;
}

static void worker(void *arg)
{
    (void)arg;
    uint32_t last_gen = s_gen;
    for (;;) {
        piece_t p;
        uint32_t gen;
        if (!pop(&p, &gen)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (gen != last_gen) {   /* stopped in between: the board flushed its buffer */
            s_clock_t0 = 0;
            last_gen = gen;
        }
        speaking_t sp = { .tag = p.tag, .gen = gen };
        const bool spoke = speak(&p, &sp);
        heap_caps_free(p.text);
        if (gen == s_gen && s_sink.on_end) s_sink.on_end(p.tag, spoke, nothing_waiting(), s_sink.ctx);
    }
}

static bool start_worker(void)
{
    if (s_task) return true;
    s_dec = heap_caps_malloc(sizeof(*s_dec), MALLOC_CAP_SPIRAM);
    if (!s_dec || xTaskCreatePinnedToCoreWithCaps(worker, "muse_tts", STACK_BYTES, NULL, 4, &s_task,
                                                  tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no PSRAM for speech: replies stay text");
        heap_caps_free(s_dec);
        s_dec = NULL;
        s_task = NULL;
        return false;
    }
    ESP_LOGI(TAG, "replies are spoken: MiniMax %s, voice \"%s\"", CONFIG_MUSE_TTS_MINIMAX_MODEL,
             CONFIG_MUSE_TTS_MINIMAX_VOICE);
    return true;
}

bool muse_tts_say(const char *text, uint32_t tag)
{
    if (!muse_tts_enabled() || !text || !text[0] || !start_worker()) return false;
    const size_t n = strnlen(text, 4 * TEXT_CAP);   /* request_body() truncates to TEXT_CAP */
    char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!copy) return false;
    memcpy(copy, text, n);
    copy[n] = '\0';
    taskENTER_CRITICAL(&s_lock);
    const bool room = s_count < QUEUE_LEN;
    if (room) {
        s_queue[(s_head + s_count) % QUEUE_LEN] = (piece_t){ copy, tag };
        s_count++;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!room) {
        heap_caps_free(copy);
        return false;
    }
    xTaskNotifyGive(s_task);
    return true;
}

void muse_tts_stop(void)
{
    piece_t dropped[QUEUE_LEN];
    unsigned n = 0;
    taskENTER_CRITICAL(&s_lock);
    s_gen++;
    while (s_count) {
        dropped[n++] = s_queue[s_head];
        s_head = (s_head + 1) % QUEUE_LEN;
        s_count--;
    }
    taskEXIT_CRITICAL(&s_lock);
    for (unsigned i = 0; i < n; i++) heap_caps_free(dropped[i].text);
}

#else  /* !CONFIG_MUSE_TTS_MINIMAX */

bool muse_tts_enabled(void) { return false; }
void muse_tts_set_sink(const muse_tts_sink_t *sink, uint32_t ahead_ms) { (void)sink; (void)ahead_ms; }
bool muse_tts_say(const char *text, uint32_t tag) { (void)text; (void)tag; return false; }
void muse_tts_stop(void) {}

#endif
