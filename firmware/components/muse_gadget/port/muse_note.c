/*
 * Voice notes on Home Link's own Noise session: muse_note_* in muse_gadget.h.
 *
 * Ported from components/muse/muse_chat_link.c in facebookincubator/muse-gadget-sdk (Apache-2.0,
 * Copyright (c) Meta Platforms, Inc. and affiliates), which runs the same turn on Muse boards
 * without PSRAM. The wire format is unchanged:
 *   speech -> POST /chat/stream, body {"message":"","output_modality":"text","items":[{"type":
 *             "file","mime_type":"audio/wav",...,"data_base64":"<WAV>"}]} streamed as it is
 *             spoken; the ack carries the note's message_id
 *   reply  <- POST /chat/subscribe, NDJSON events: the transcript as message.user, the answer
 *             as delta.text_append / delta.message_done / message.assistant
 * What changed: the caption paging and reading-pace scroll are gone (the board lays text out
 * itself), REPLY carries the whole answer so far rather than one caption page, the calls
 * that went through Muse's muse_link_req_* ops go straight to noise_ctrl_req_*, and a slow
 * uplink no longer ends the note. Upstream gives up when the session's queue stays full for
 * SEND_WAIT_MS, since a board without PSRAM has nowhere to keep the speech meanwhile; here the
 * caller holds it in PSRAM, so a chunk waits as long as the session is up and still taking
 * chunks (STALL_US). One lost TCP segment can hold the upload for a second.
 */
#include "muse_gadget.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config_store.h"
#include "noise_control.h"
#include "wifi_mgr.h"

static const char *TAG = "muse_note";

#define STAGE_BYTES 1536                /* PCM per body chunk: 2 KB of base64, 48 ms */
#define CHUNK_BYTES (STAGE_BYTES / 3 * 4)
#define ACK_MAX 2048
#define ROW_MAX 12288                   /* largest subscription line gathered across frames */
#define TEXT_MAX 1024                   /* reply text kept for the board */
#define EV_TEXT 160
#define EV_QUEUE 8
#define SEND_WAIT_MS 200
#define STALL_US 10000000               /* a session that takes nothing for this long is stuck */
#define PCM_BYTES_PER_MS (MUSE_NOTE_SAMPLE_RATE * 2 / 1000)
#define SETTLE_US 3000000               /* quiet after a reply before the turn ends */
#define REPLY_TIMEOUT_US 60000000

/* A voice note is a POST /chat/stream body: NOTE_HEAD, a base64 WAV, NOTE_TAIL. */
#define NOTE_HEAD \
    "{\"message\":\"\",\"output_modality\":\"text\",\"items\":[{\"type\":\"file\"," \
    "\"mime_type\":\"audio/wav\",\"filename\":\"voice_note.wav\",\"data_base64\":\""
#define NOTE_TAIL "\"}]}"
#define WAV_HEADER 44

typedef struct {
    uint32_t gen;       /* bumped per request; frames of older ones are dropped */
    int status;         /* HTTP status, -1 if the request died */
    bool done;
    bool overflow;
    size_t len;
    size_t cap;         /* fixed note ACK buffer; 0: subscription line, gathered only if split */
    char *body;
} rx_t;

/* A subscription event, decoded without allocating a JSON tree. */
typedef struct {
    char type[16];
    bool ready;         /* display_text_ready */
    uint64_t seq;
    char event[24];
    char msg[80];
    char reply_to[80];
    char text[TEXT_MAX];
} row_t;

enum { RX_NOTE, RX_SUB, RX_COUNT };

typedef enum { T_IDLE, T_TALKING, T_ACK, T_REPLY } phase_t;

typedef struct {
    muse_note_ev_t type;
    char text[EV_TEXT];
} ev_t;

static SemaphoreHandle_t s_rx_lock;
static rx_t s_rx[RX_COUNT];
static int64_t s_stream[RX_COUNT];      /* open request per slot, 0 none */
/* Subscription callback state, always under s_rx_lock. Keep the two newest
 * completed messages until the turn's task drains them, including before ACK. */
static row_t s_row, s_delta, s_pending[2];
static unsigned s_pending_count;
static uint64_t s_last_seq;
static bool s_early_evicted, s_skipped_big;
static char s_note_id[80], s_parent_id[80]; /* ACK IDs shared under s_rx_lock */

/* Events, on the turn's task only. */
static ev_t s_ev[EV_QUEUE];
static unsigned s_ev_head, s_ev_count;

/* The turn's task only. */
static struct {
    phase_t phase;
    uint8_t *stage;                     /* PCM waiting for the next chunk */
    size_t stage_len;
    char *chunk;
    char note_id[80], parent_id[80], seen[2][80];
    unsigned seen_count;
    size_t pcm_bytes;                   /* speech taken so far */
    int64_t longest_wait;               /* the longest a chunk waited for room on the session */
    int64_t t_begin, t_end, t_reply;
    bool heard, replied;
    bool after_note;
    char text[TEXT_MAX];
} s_turn;

/* ---- Voice note encoding ---- */

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* The note's length isn't known until the release, so the sizes are the streaming "unknown". */
static void wav_header(uint8_t h[WAV_HEADER], uint32_t rate)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, UINT32_MAX, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);             /* PCM */
    put_le(h + 22, 1, 2);             /* mono */
    put_le(h + 24, rate, 4);
    put_le(h + 28, rate * 2, 4);
    put_le(h + 32, 2, 2);
    put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, UINT32_MAX, 4);
}

/* Writes 4 characters per 3 bytes of `in`, padded; returns the length. */
static size_t base64(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = out;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        *o++ = A[v >> 18];
        *o++ = A[v >> 12 & 63];
        *o++ = A[v >> 6 & 63];
        *o++ = A[v & 63];
    }
    if (i < n) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0);
        *o++ = A[v >> 18];
        *o++ = A[v >> 12 & 63];
        *o++ = i + 1 < n ? A[v >> 6 & 63] : '=';
        *o++ = '=';
    }
    return o - out;
}

/* ---- Subscription rows, read in place ---- */

typedef struct {
    const char *p, *end;
} scan_t;

static void skip_ws(scan_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) {
        s->p++;
    }
}

static bool expect(scan_t *s, char c)
{
    skip_ws(s);
    if (s->p < s->end && *s->p == c) {
        s->p++;
        return true;
    }
    return false;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v << 4 | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

/* Reads the string at s->p into out (may be NULL), cut to fit on a character boundary. */
static bool read_string(scan_t *s, char *out, size_t cap)
{
    if (s->p >= s->end || *s->p != '"') {
        return false;
    }
    size_t n = 0;
    bool clipped = false;
    for (s->p++; s->p < s->end && *s->p != '"'; s->p++) {
        char buf[4];
        size_t len = 1;
        buf[0] = *s->p;
        if (*s->p == '\\' && ++s->p < s->end) {
            switch (*s->p) {
            case 'n': buf[0] = '\n'; break;
            case 't': buf[0] = ' '; break;
            case 'r': buf[0] = ' '; break;
            case 'b': case 'f': buf[0] = ' '; break;
            case 'u': {
                if (s->end - s->p < 5) {
                    return false;
                }
                uint32_t c = hex4(s->p + 1);
                s->p += 4;
                if (c >= 0xD800 && c < 0xDC00) {
                    if (s->end - s->p < 7 || s->p[1] != '\\' || s->p[2] != 'u') return false;
                    uint32_t low = hex4(s->p + 3);
                    if (low < 0xDC00 || low > 0xDFFF) return false;
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    s->p += 6;
                } else if (c >= 0xDC00 && c <= 0xDFFF) {
                    return false;
                }
                if (c < 0x80) {
                    buf[0] = (char)c;
                } else if (c < 0x800) {
                    buf[0] = (char)(0xC0 | c >> 6);
                    buf[1] = (char)(0x80 | (c & 63));
                    len = 2;
                } else if (c < 0x10000) {
                    buf[0] = (char)(0xE0 | c >> 12);
                    buf[1] = (char)(0x80 | (c >> 6 & 63));
                    buf[2] = (char)(0x80 | (c & 63));
                    len = 3;
                } else {
                    buf[0] = (char)(0xF0 | c >> 18);
                    buf[1] = (char)(0x80 | (c >> 12 & 63));
                    buf[2] = (char)(0x80 | (c >> 6 & 63));
                    buf[3] = (char)(0x80 | (c & 63));
                    len = 4;
                }
                break;
            }
            default: buf[0] = *s->p; break;   /* \" \\ \/ */
            }
        }
        if (out && !clipped && n + len < cap) {
            memcpy(out + n, buf, len);
            n += len;
        } else if (out) {
            clipped = true;
        }
    }
    if (out && cap) {
        if (clipped && n) {
            size_t start = n - 1;
            while (start && ((unsigned char)out[start] & 0xC0) == 0x80) start--;
            unsigned char lead = (unsigned char)out[start];
            size_t need = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            if (n - start < need) n = start;
        }
        out[n] = '\0';
    }
    if (s->p >= s->end) {
        return false;
    }
    s->p++;
    return true;
}

/* Steps over one value: a string, number, literal, or a whole object or array. */
static bool skip_value(scan_t *s)
{
    int depth = 0;
    skip_ws(s);
    while (s->p < s->end) {
        char c = *s->p;
        if (c == '"') {
            if (!read_string(s, NULL, 0)) {
                return false;
            }
        } else if (c == '{' || c == '[') {
            depth++;
            s->p++;
        } else if (c == '}' || c == ']') {
            if (!depth) {
                return true;
            }
            depth--;
            s->p++;
        } else if (c == ',' && !depth) {
            return true;
        } else {
            s->p++;
        }
        if (!depth && (c == '"' || c == '}' || c == ']')) {
            return true;
        }
    }
    return false;
}

/* Reads a string member's value into out; anything else (null) is skipped. */
static bool read_field(scan_t *s, char *out, size_t cap)
{
    skip_ws(s);
    return s->p < s->end && *s->p == '"' ? read_string(s, out, cap) : skip_value(s);
}

static bool parse_row(scan_t *s, row_t *r, scan_t *payload)
{
    if (!expect(s, '{')) {
        return false;
    }
    if (expect(s, '}')) {
        return true;
    }
    unsigned text_rank = 0;
    bool have_id = false, have_parent = false;
    do {
        char key[24];
        skip_ws(s);
        if (!read_string(s, key, sizeof(key)) || !expect(s, ':')) {
            return false;
        }
        skip_ws(s);
        bool ok;
        bool string = s->p < s->end && *s->p == '"';
        if (payload && !strcmp(key, "payload")) {
            *payload = *s;
            ok = skip_value(s);
        } else if (payload && !strcmp(key, "type")) {
            ok = read_field(s, r->type, sizeof(r->type));
        } else if (payload && !strcmp(key, "seq")) {
            for (r->seq = 0; s->p < s->end && *s->p >= '0' && *s->p <= '9'; s->p++) {
                r->seq = r->seq * 10 + (*s->p - '0');
            }
            ok = skip_value(s);
        } else if (!strcmp(key, "display_text_ready")) {
            r->ready = s->p < s->end && *s->p == 't';
            ok = skip_value(s);
        } else if (payload && (!strcmp(key, "event_name") || !strcmp(key, "event"))) {
            ok = read_field(s, r->event, sizeof(r->event));
        } else if (!strcmp(key, "message_id") || (!have_id && !strcmp(key, "id"))) {
            ok = read_field(s, r->msg, sizeof(r->msg));
            have_id |= string && !strcmp(key, "message_id");
        } else if (!strcmp(key, "reply_to_message_id") || (!have_parent && !strcmp(key, "parent_message_id"))) {
            ok = read_field(s, r->reply_to, sizeof(r->reply_to));
            have_parent |= string && !strcmp(key, "reply_to_message_id");
        } else if (!strcmp(key, "display_text") || !strcmp(key, "content") || !strcmp(key, "text")) {
            unsigned rank = !strcmp(key, "display_text") ? 3 : !strcmp(key, "content") ? 2 : 1;
            ok = string && rank >= text_rank ? read_field(s, r->text, sizeof(r->text)) : skip_value(s);
            if (string && rank > text_rank) text_rank = rank;
        } else {
            ok = skip_value(s);
        }
        if (!ok) {
            return false;
        }
    } while (expect(s, ','));
    return expect(s, '}');
}

static bool parse_event(const char *data, size_t len)
{
    scan_t scan = { data, data + len }, payload = {0};
    memset(&s_row, 0, sizeof(s_row));
    s_row.ready = true;
    if (!parse_row(&scan, &s_row, &payload)) return false;
    skip_ws(&scan);
    if (scan.p != scan.end || strcmp(s_row.type, "event")) return false;
    return !payload.p || parse_row(&payload, &s_row, NULL);
}

static bool related(const row_t *row)
{
    if (!strcmp(row->event, "message.user")) {
        return !s_note_id[0] || !strcmp(row->msg, s_note_id) || !strcmp(row->msg, s_parent_id);
    }
    return !s_note_id[0] || !row->reply_to[0]
        || !strcmp(row->reply_to, s_note_id) || !strcmp(row->reply_to, s_parent_id);
}

/* Append whole UTF-8 characters even when the buffer is already nearly full. */
static void text_append(char *out, size_t cap, const char *text)
{
    size_t used = strlen(out), n = strlen(text);
    if (n >= cap - used) {
        n = cap - used - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    }
    memcpy(out + used, text, n);
    out[used + n] = 0;
}

/* Called with the receive lock held; never waits on the turn's task. */
static void subscription_event(void)
{
    if (s_row.seq && s_row.seq <= s_last_seq) return;
    if (s_row.seq) s_last_seq = s_row.seq;
    bool append = !strcmp(s_row.event, "delta.text_append");
    bool start = !strcmp(s_row.event, "delta.message_start");
    bool done = !strcmp(s_row.event, "delta.message_done");
    bool full = !strcmp(s_row.event, "message.assistant");
    bool user = !strcmp(s_row.event, "message.user");
    if ((!append && !start && !done && !full && !user) || !s_row.msg[0]) return;
    if (!related(&s_row)) return;
    if (start || append) {
        if (strcmp(s_delta.msg, s_row.msg)) {
            memset(&s_delta, 0, sizeof(s_delta));
            strlcpy(s_delta.msg, s_row.msg, sizeof(s_delta.msg));
        }
        if (s_row.reply_to[0]) strlcpy(s_delta.reply_to, s_row.reply_to, sizeof(s_delta.reply_to));
        if (append) {
            text_append(s_delta.text, sizeof(s_delta.text), s_row.text);
        }
        return;
    }
    if (!done && !s_row.ready) return;
    if (!strcmp(s_row.msg, s_delta.msg)) {
        if (!s_row.text[0]) strlcpy(s_row.text, s_delta.text, sizeof(s_row.text));
        if (!s_row.reply_to[0]) strlcpy(s_row.reply_to, s_delta.reply_to, sizeof(s_row.reply_to));
    }
    if (!related(&s_row) || !s_row.text[0]) return;
    if (!user) strlcpy(s_row.event, "message.assistant", sizeof(s_row.event));
    s_row.ready = true;
    /* Coalesce the done event and persisted full message, including before ACK. */
    for (unsigned i = 0; i < s_pending_count; i++) {
        if (!strcmp(s_pending[i].msg, s_row.msg)) { s_pending[i] = s_row; return; }
    }
    /* Keep the two newest finals if the turn's task has not drained the queue.
     * Before ACK they are provisional: unrelated traffic must not fail a turn
     * or grow memory without bound. Remember eviction for a useful timeout. */
    if (s_pending_count == 2) {
        if (!s_note_id[0]) s_early_evicted = true;
        s_pending[0] = s_pending[1];
        s_pending_count--;
    }
    s_pending[s_pending_count++] = s_row;
}

/* Parse complete lines in Link's buffer; gather only a line split across frames.
 * An oversized line is skipped through its newline, so later events still work. */
static void row_data(rx_t *rx, const uint8_t *data, size_t len, bool end)
{
    if (rx->status != 200) return;
    while (len) {
        const uint8_t *newline = memchr(data, '\n', len);
        size_t n = newline ? (size_t)(newline - data) : len;
        if (!rx->overflow) {
            if (n > ROW_MAX - rx->len) {
                rx->overflow = s_skipped_big = true;
            } else if (!rx->body && newline) {
                if (parse_event((const char *)data, n)) subscription_event();
            } else if (n) {
                char *grown = realloc(rx->body, rx->len + n);
                if (!grown) {
                    rx->overflow = s_skipped_big = true;
                } else {
                    rx->body = grown;
                    memcpy(rx->body + rx->len, data, n);
                    rx->len += n;
                }
            }
        }
        if (rx->overflow) {
            free(rx->body);
            rx->body = NULL;
            rx->len = 0;
        }
        if (newline) {
            if (rx->body && !rx->overflow && parse_event(rx->body, rx->len)) subscription_event();
            free(rx->body);
            rx->body = NULL;
            rx->len = 0;
            rx->overflow = false;
            n++;
        }
        data += n;
        len -= n;
    }
    if (end) {
        if (rx->len && !rx->overflow && parse_event(rx->body, rx->len)) subscription_event();
        free(rx->body);
        rx->body = NULL;
        rx->len = 0;
    }
}

static void rx_clear(rx_t *rx)
{
    rx->status = 0;
    rx->done = rx->overflow = false;
    rx->len = 0;
    if (!rx->cap) {
        free(rx->body);
        rx->body = NULL;
        s_skipped_big = false;
        s_pending_count = 0;
        s_last_seq = 0;
        s_early_evicted = false;
        s_note_id[0] = s_parent_id[0] = 0;
        memset(&s_delta, 0, sizeof(s_delta));
    }
}

/* Runs on the Noise session task. */
static void on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    uintptr_t token = (uintptr_t)ctx;
    rx_t *rx = &s_rx[token & 1];
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    if ((uint32_t)(token >> 1) == rx->gen && !rx->done) {
        if (status) {
            rx->status = status;
        }
        if (!rx->cap) {
            row_data(rx, data, len, end && status >= 0);
        } else if (len && rx->len + len >= rx->cap) {
            rx->overflow = true;
        } else if (len) {
            memcpy(rx->body + rx->len, data, len);
            rx->len += len;
            rx->body[rx->len] = '\0';
        }
        rx->done = end || status < 0;
    }
    xSemaphoreGive(s_rx_lock);
}

/* Starts a request whose reply lands in slot `slot`. False if the session is down. */
static bool request(int slot, const char *verb, const char *path, bool json, bool end_body)
{
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const char *headers[] = { "x-request-id", req_id, "x-app-id", "hatch-web",
                              json ? "Content-Type" : NULL, "application/json",
                              "Accept", slot == RX_SUB ? "application/x-ndjson" : "application/json", NULL };
    rx_t *rx = &s_rx[slot];
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    rx->gen++;
    rx_clear(rx);
    if (rx->cap) {
        rx->body[0] = '\0';
    }
    uintptr_t token = (uintptr_t)rx->gen << 1 | slot;
    xSemaphoreGive(s_rx_lock);
    s_stream[slot] = noise_ctrl_req_open(verb, path, headers, end_body, on_frame, (void *)token);
    return s_stream[slot] != 0;
}

/* request(), waiting for room in the session's queue while the session is up. */
static bool open_request(int slot, const char *path)
{
    int64_t t0 = esp_timer_get_time();
    while (!request(slot, "POST", path, true, false)) {
        if (!noise_ctrl_is_connected() || esp_timer_get_time() - t0 > STALL_US) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

/* False once slot `slot`'s request is gone, e.g. with the session it was opened on. */
static bool alive(int slot)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    bool dead = s_rx[slot].status < 0;
    xSemaphoreGive(s_rx_lock);
    return s_stream[slot] && !dead;
}

/* Queues a body chunk on slot `slot`'s request, waiting while the session is up and taking
 * chunks: what the session has not taken yet waits in the caller's buffer. */
static bool send_body(int slot, const void *data, size_t len, bool last)
{
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        if (!alive(slot) || !noise_ctrl_is_connected() || esp_timer_get_time() - t0 > STALL_US) {
            return false;
        }
        if (noise_ctrl_req_send(s_stream[slot], data, len, last, SEND_WAIT_MS)) {
            break;
        }
        vTaskDelay(1);
    }
    int64_t waited = esp_timer_get_time() - t0;
    if (waited > s_turn.longest_wait) {
        s_turn.longest_wait = waited;
    }
    return true;
}

/* Why a chunk of the note could not be queued. */
static const char *send_failure(void)
{
    return noise_ctrl_is_connected() && alive(RX_NOTE) ? "CAN'T KEEP UP" : "LOST CONNECTION TO MUSE";
}

/* Drops slot `slot`'s request, if any, and whatever of its reply is still coming. */
static void drop(int slot)
{
    if (s_stream[slot]) {
        noise_ctrl_req_cancel(s_stream[slot]);
        s_stream[slot] = 0;
    }
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    s_rx[slot].gen++;
    rx_clear(&s_rx[slot]);
    xSemaphoreGive(s_rx_lock);
}

/* True once slot `slot`'s reply is complete (or the request died). */
static bool received(int slot)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    bool done = s_stream[slot] && s_rx[slot].done;
    xSemaphoreGive(s_rx_lock);
    if (done) {
        s_stream[slot] = 0;
    }
    return done;
}

static void emit(muse_note_ev_t type, const char *text)
{
    if (s_ev_count == EV_QUEUE) {
        /* Keep the newest: a stale REPLY is superseded by the next one anyway. */
        s_ev_head = (s_ev_head + 1) % EV_QUEUE;
        s_ev_count--;
    }
    ev_t *ev = &s_ev[(s_ev_head + s_ev_count++) % EV_QUEUE];
    ev->type = type;
    strlcpy(ev->text, text ? text : "", sizeof(ev->text));
}

static void end_turn(void)
{
    drop(RX_NOTE);
    drop(RX_SUB);
    free(s_turn.stage);
    free(s_turn.chunk);
    s_turn.stage = NULL;
    s_turn.chunk = NULL;
    s_turn.phase = T_IDLE;
}

static void fail(const char *why)
{
    ESP_LOGW(TAG, "turn failed: %s", why);
    end_turn();
    emit(MUSE_NOTE_EV_ERROR, why);
}

/* Sends the staged PCM as one body chunk; whole chunks are a multiple of 3 bytes, so only the last pads. */
static bool send_stage(bool last)
{
    size_t n = base64(s_turn.stage, s_turn.stage_len, s_turn.chunk);
    if (last) {
        memcpy(s_turn.chunk + n, NOTE_TAIL, sizeof(NOTE_TAIL) - 1);
        n += sizeof(NOTE_TAIL) - 1;
    }
    s_turn.stage_len = 0;
    return send_body(RX_NOTE, s_turn.chunk, n, last);
}

static void on_ack(void)
{
    rx_t *rx = &s_rx[RX_NOTE];
    if (rx->status != 200 || rx->overflow) {
        ESP_LOGW(TAG, "chat/stream: %d", rx->status);
        fail(rx->status < 0 ? "LOST CONNECTION TO MUSE" : "MUSE DIDN'T TAKE IT");
        return;
    }
    cJSON *root = cJSON_Parse(rx->body);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_IsObject(result) ? result : root, "message_id"));
    strlcpy(s_turn.note_id, id ? id : "", sizeof(s_turn.note_id));
    const char *parent = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_IsObject(result) ? result : root, "reply_to_message_id"));
    strlcpy(s_turn.parent_id, parent ? parent : "", sizeof(s_turn.parent_id));
    cJSON_Delete(root);
    if (!s_turn.note_id[0]) {
        ESP_LOGW(TAG, "chat/stream ack without a message id");
        fail("MUSE DIDN'T TAKE IT");
        return;
    }
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    strlcpy(s_note_id, s_turn.note_id, sizeof(s_note_id));
    strlcpy(s_parent_id, s_turn.parent_id, sizeof(s_parent_id));
    unsigned kept = 0;
    for (unsigned i = 0; i < s_pending_count; i++) {
        if (related(&s_pending[i])) s_pending[kept++] = s_pending[i];
    }
    s_pending_count = kept;
    if (!related(&s_delta)) memset(&s_delta, 0, sizeof(s_delta));
    xSemaphoreGive(s_rx_lock);
    s_turn.phase = T_REPLY;
    ESP_LOGI(TAG, "note acknowledged after %.2fs", (esp_timer_get_time() - s_turn.t_end) / 1e6);
    emit(MUSE_NOTE_EV_SENT, NULL);
}

/* Handles a completed subscription row on the turn's task. */
static void on_row(row_t *r)
{
    if (!strcmp(r->event, "message.user")) {
        s_turn.after_note = !strcmp(r->msg, s_turn.note_id) || !strcmp(r->msg, s_turn.parent_id);
    }
    if (s_turn.after_note && !strcmp(r->event, "message.user")) {
        /* The row reads "<transcript>\n[file:audio/wav ...]", or "[Voice note]" before transcription. */
        char *heard = r->text;
        char *att = strstr(heard, "\n[file:");
        if (att) {
            *att = '\0';
        }
        if (heard[0] && strcmp(heard, "[Voice note]") && !s_turn.heard) {
            ESP_LOGI(TAG, "heard: %.120s", heard);
            emit(MUSE_NOTE_EV_HEARD, heard);
            s_turn.heard = true;
        }
        return;
    }
    /* Explicit parents must match the ACK. Unparented live replies follow the note. */
    if (strcmp(r->event, "message.assistant")
        || (r->reply_to[0] && strcmp(r->reply_to, s_turn.note_id) && strcmp(r->reply_to, s_turn.parent_id))) {
        return;
    }
    if (!r->ready) {
        return;
    }
    for (unsigned i = 0; i < s_turn.seen_count; i++) {
        if (!strcmp(r->msg, s_turn.seen[i])) return;
    }
    if (r->text[0]) {
        if (s_turn.seen_count == 2) {
            memcpy(s_turn.seen[0], s_turn.seen[1], sizeof(s_turn.seen[0]));
            s_turn.seen_count = 1;
        }
        strlcpy(s_turn.seen[s_turn.seen_count++], r->msg, sizeof(s_turn.seen[0]));
        if (s_turn.text[0]) text_append(s_turn.text, sizeof(s_turn.text), "\n");
        text_append(s_turn.text, sizeof(s_turn.text), r->text);
        ESP_LOGI(TAG, "reply after %.2fs: %.80s", (esp_timer_get_time() - s_turn.t_end) / 1e6, r->text);
        s_turn.replied = true;
        s_turn.t_reply = esp_timer_get_time();
        emit(MUSE_NOTE_EV_REPLY, NULL);   /* the text is s_turn.text, filled in at muse_note_event() */
    }
}

static void subscription_error(int status)
{
    if (status == 403) fail("MUSE REPLY ACCESS DENIED (403)");
    else if (status == 401) fail("MUSE REPLY AUTH REQUIRED (401)");
    else if (status <= 0 || status == 200) fail("LOST CONNECTION TO MUSE");
    else {
        char why[48];
        snprintf(why, sizeof(why), "MUSE REPLY ERROR (HTTP %d)", status);
        fail(why);
    }
}

/* Moves the turn along; runs on the turn's task each time it asks for events. */
static void pump(void)
{
    int64_t now = esp_timer_get_time();
    if (s_turn.phase == T_ACK && received(RX_NOTE)) {
        on_ack();
    }
    if (s_turn.phase == T_IDLE || s_turn.phase == T_TALKING) return;
    static row_t pending;   /* turn's task only; too big for its stack */
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    int status = s_rx[RX_SUB].status;
    bool closed = s_rx[RX_SUB].done;
    bool early_evicted = s_early_evicted;
    bool skipped_big = s_skipped_big;
    xSemaphoreGive(s_rx_lock);
    if (status < 0 || status >= 400) {
        subscription_error(status);
        return;
    }
    if (s_turn.phase == T_REPLY) {
        for (;;) {
            xSemaphoreTake(s_rx_lock, portMAX_DELAY);
            bool have = s_pending_count > 0;
            if (have) {
                pending = s_pending[0];
                if (--s_pending_count) s_pending[0] = s_pending[1];
            }
            xSemaphoreGive(s_rx_lock);
            if (!have) break;
            on_row(&pending);
        }
        if (closed && !s_turn.replied) {
            subscription_error(status);
            return;
        }
    }
    if (s_turn.phase != T_REPLY) {
        if (s_turn.phase == T_ACK && now - s_turn.t_end > REPLY_TIMEOUT_US) {
            fail("NO REPLY FROM MUSE");
        }
        return;
    }
    if (s_turn.replied) {
        if (now - s_turn.t_reply > SETTLE_US) {
            end_turn();
            emit(MUSE_NOTE_EV_DONE, NULL);
        }
    } else if (now - s_turn.t_end > REPLY_TIMEOUT_US) {
        fail(skipped_big ? "REPLY TOO LONG" : early_evicted ? "REPLY BUFFER LIMIT - TRY AGAIN" : "NO REPLY FROM MUSE");
    }
}

/* ---- Public ---- */

static bool s_inited;

static void init_once(void)
{
    if (s_inited) return;
    s_rx_lock = xSemaphoreCreateMutex();
    static char ack[ACK_MAX];
    s_rx[RX_NOTE].body = ack;
    s_rx[RX_NOTE].cap = sizeof(ack);
    s_inited = true;
}

bool muse_note_ready(void)
{
    return config_setup_complete() && config_is_provisioned() && wifi_mgr_is_connected()
        && noise_ctrl_is_connected();
}

bool muse_note_begin(void)
{
    init_once();
    end_turn();
    s_ev_head = s_ev_count = 0;
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.stage = malloc(STAGE_BYTES);
    s_turn.chunk = malloc(CHUNK_BYTES + sizeof(NOTE_TAIL));
    if (!s_turn.stage || !s_turn.chunk) {
        fail("OUT OF MEMORY");
        return false;
    }
    if (!open_request(RX_NOTE, "/chat/stream")
        || !send_body(RX_NOTE, NOTE_HEAD, sizeof(NOTE_HEAD) - 1, false)) {
        fail("CAN'T REACH MUSE");
        return false;
    }
    wav_header(s_turn.stage, MUSE_NOTE_SAMPLE_RATE);
    s_turn.stage_len = WAV_HEADER;
    s_turn.t_begin = esp_timer_get_time();
    s_turn.phase = T_TALKING;
    return true;
}

void muse_note_audio(const int16_t *pcm, size_t frames)
{
    const uint8_t *p = (const uint8_t *)pcm;
    size_t n = frames * 2;
    while (s_turn.phase == T_TALKING && n) {
        size_t take = STAGE_BYTES - s_turn.stage_len < n ? STAGE_BYTES - s_turn.stage_len : n;
        memcpy(s_turn.stage + s_turn.stage_len, p, take);
        s_turn.stage_len += take;
        s_turn.pcm_bytes += take;
        p += take;
        n -= take;
        if (s_turn.stage_len == STAGE_BYTES && !send_stage(false)) {
            fail(send_failure());
        }
    }
}

void muse_note_end(void)
{
    if (s_turn.phase != T_TALKING) {
        return;   /* already failed while recording; that error was reported */
    }
    /* Subscribe only after recording: keep inbound reply traffic out of the
     * real-time audio upload. Queue it before the note's final body chunk. */
    if (!open_request(RX_SUB, "/chat/subscribe") || !send_body(RX_SUB, "{}", 2, true)) {
        fail(noise_ctrl_is_connected() ? "CAN'T SUBSCRIBE TO MUSE" : "LOST CONNECTION TO MUSE");
        return;
    }
    if (!send_stage(true)) {
        fail(send_failure());
        return;
    }
    free(s_turn.stage);
    free(s_turn.chunk);
    s_turn.stage = NULL;
    s_turn.chunk = NULL;
    s_turn.t_end = esp_timer_get_time();
    /* Behind: how long after the speech ended its last chunk got onto the session. */
    int64_t speech_ms = (int64_t)(s_turn.pcm_bytes / PCM_BYTES_PER_MS);
    int64_t behind_ms = (s_turn.t_end - s_turn.t_begin) / 1000 - speech_ms;
    ESP_LOGI(TAG, "note: %.1fs of speech, queued %.1fs behind real time (longest wait for the link %d ms)",
             speech_ms / 1e3, (behind_ms > 0 ? behind_ms : 0) / 1e3, (int)(s_turn.longest_wait / 1000));
    s_turn.phase = T_ACK;
}

void muse_note_cancel(void)
{
    init_once();
    end_turn();
    s_ev_head = s_ev_count = 0;
}

muse_note_ev_t muse_note_event(char *text, size_t cap)
{
    if (!s_inited) return MUSE_NOTE_EV_NONE;
    pump();
    if (!s_ev_count) {
        return MUSE_NOTE_EV_NONE;
    }
    ev_t *ev = &s_ev[s_ev_head];
    s_ev_head = (s_ev_head + 1) % EV_QUEUE;
    s_ev_count--;
    if (text && cap) {
        strlcpy(text, ev->type == MUSE_NOTE_EV_REPLY ? s_turn.text : ev->text, cap);
    }
    return ev->type;
}
