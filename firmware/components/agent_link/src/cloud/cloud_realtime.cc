#include "cloud_realtime.h"
#include "realtime_audio.h"
#include "audio_downlink.h"
#include "cloud_platform.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_wifi.h"
#if CONFIG_AGENT_LINK_REALTIME_OPUS
#include "esp_opus_dec.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace {
constexpr const char* TAG = "agent_link.realtime";
constexpr int kConnectTimeoutMs = 10000;
constexpr size_t kMicBytes = 640;
constexpr size_t kTxDepth = 102;
constexpr size_t kUplinkBatchPackets = 4;
constexpr size_t kUplinkBatchBytes = kMicBytes * kUplinkBatchPackets;
constexpr char kAudioPrefix[] = "{\"type\":\"input_audio.append\",\"audio\":\"";
constexpr size_t kAudioJsonBytes = sizeof(kAudioPrefix) - 1 + (kUplinkBatchBytes + 2) / 3 * 4 + 3;
constexpr size_t kMessageBytes = 128 * 1024;
constexpr int64_t kReadyTimeoutUs = 10000000;

struct Grant {
    char url[512];
    char token[256];
    uint32_t epoch;
    bool direct;
    bool opus;
    bool platform;
    int agent_id;
    char session_id[80];
};

enum class TxKind { kStart, kPcm, kEnd };
struct Tx {
    TxKind kind;
    uint16_t bytes;
    uint32_t turn;
    uint8_t data[kMicBytes];
};

struct TxScratch {
    uint8_t pcm[kUplinkBatchBytes];
    char json[kAudioJsonBytes];
};

struct WssAddress {
    char host[128];
    char path[384];
    int  port = 443;
};

QueueHandle_t              s_grants = nullptr;
bool                       s_grants_caps = false;
std::atomic<TaskHandle_t>  s_task{nullptr};
std::atomic<bool>          s_run{false};
std::atomic<bool>          s_ready{false};
std::atomic<bool>          s_mute{false};
std::atomic<bool>          s_continuous{false};
std::atomic<uint32_t>      s_epoch{0};
std::atomic<uint32_t>      s_reset{0};
std::mutex                s_mutex;
QueueHandle_t             s_tx = nullptr;
bool                      s_tx_caps = false;
agent_stream_t            s_input = AGENT_STREAM_KIND_COUNT;
cloud_realtime_callbacks_t s_cb = {};

// Diagnostics for the continuous uplink. Only counters and timings are retained.
std::atomic<uint32_t> s_tx_sent_frames{0};
std::atomic<uint32_t> s_tx_sent_messages{0};
std::atomic<uint32_t> s_rx_frames{0};
std::atomic<uint32_t> s_tx_send_max_ms{0};
std::atomic<uint32_t> s_worker_phase{0};
std::atomic<uint32_t> s_worker_phase_since_ms{0};
uint32_t s_diag_last_ms = 0;
uint32_t s_diag_mic_frames = 0;
uint32_t s_diag_dropped_frames = 0;
uint32_t s_diag_prev_sent_frames = 0;
uint32_t s_diag_prev_sent_messages = 0;
uint32_t s_diag_prev_rx_frames = 0;

void SetWorkerPhase(uint32_t phase) {
    s_worker_phase_since_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    s_worker_phase.store(phase);
}

// Called with s_mutex held from the microphone task.
void ReportUplink() {
    if (!s_continuous || !s_diag_last_ms) return;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now - s_diag_last_ms < 5000) return;
    const uint32_t sent_frames = s_tx_sent_frames.load();
    const uint32_t sent_messages = s_tx_sent_messages.load();
    const uint32_t rx = s_rx_frames.load();
    const uint32_t phase = s_worker_phase.load();
    const uint32_t phase_age = now - s_worker_phase_since_ms.load();
    constexpr const char* phases[] = {"loop", "send", "poll", "read", "handle"};
    const char* phase_name = phase < sizeof phases / sizeof phases[0] ? phases[phase] : "unknown";
    ESP_LOGI(TAG,
             "uplink %ums: mic=%u tx=%u drop=%u ws=%u queue=%u/%u rx=%u max_send=%ums worker=%s/%ums stack=%uB",
             static_cast<unsigned>(now - s_diag_last_ms), static_cast<unsigned>(s_diag_mic_frames),
             static_cast<unsigned>(sent_frames - s_diag_prev_sent_frames),
             static_cast<unsigned>(s_diag_dropped_frames),
             static_cast<unsigned>(sent_messages - s_diag_prev_sent_messages),
             static_cast<unsigned>(uxQueueMessagesWaiting(s_tx)), static_cast<unsigned>(kTxDepth),
             static_cast<unsigned>(rx - s_diag_prev_rx_frames),
             static_cast<unsigned>(s_tx_send_max_ms.exchange(0)), phase_name,
             static_cast<unsigned>(phase_age),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    s_diag_last_ms = now;
    s_diag_mic_frames = s_diag_dropped_frames = 0;
    s_diag_prev_sent_frames = sent_frames;
    s_diag_prev_sent_messages = sent_messages;
    s_diag_prev_rx_frames = rx;
}

bool ParseWss(const char* url, WssAddress* out) {
    if (!url || strncmp(url, "wss://", 6) != 0 || strchr(url, '#')) return false;
    const char* authority = url + 6;
    const char* slash = strchr(authority, '/');
    if (!slash) return false;
    const char* colon = static_cast<const char*>(memchr(authority, ':', slash - authority));
    const char* host_end = colon ? colon : slash;
    const size_t host_len = static_cast<size_t>(host_end - authority);
    const size_t path_len = strlen(slash);
    if (!host_len || host_len >= sizeof out->host || path_len >= sizeof out->path) return false;
    for (const char* p = authority; p < host_end; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!std::isalnum(c) && c != '-' && c != '.') return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(slash); *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f) return false;
    }
    memcpy(out->host, authority, host_len);
    out->host[host_len] = '\0';
    memcpy(out->path, slash, path_len + 1);
    if (colon) {
        int port = 0;
        if (colon + 1 == slash) return false;
        for (const char* p = colon + 1; p < slash; ++p) {
            if (*p < '0' || *p > '9') return false;
            port = port * 10 + (*p - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
        out->port = port;
    }
    return true;
}

bool SafeToken(const char* token) {
    if (!token || !*token || strlen(token) >= sizeof Grant::token) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(token); *p; ++p) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

bool Current(uint32_t epoch) { return s_run && s_epoch == epoch; }

bool ReadAudioFormat(const cJSON* body, bool* opus) {
    const cJSON* connection = cJSON_GetObjectItemCaseSensitive(body, "realtimeConnection");
    if (cJSON_IsObject(connection)) body = connection;
    const cJSON* audio = cJSON_GetObjectItemCaseSensitive(body, "audio");
    const cJSON* output = cJSON_GetObjectItemCaseSensitive(audio, "output");
    const cJSON* encoding = cJSON_GetObjectItemCaseSensitive(output, "encoding");
    const cJSON* rate = cJSON_GetObjectItemCaseSensitive(output, "sampleRate");
    const cJSON* channels = cJSON_GetObjectItemCaseSensitive(output, "channels");
    if (!cJSON_IsString(encoding) || !cJSON_IsNumber(rate) || rate->valueint != 24000 ||
        !cJSON_IsNumber(channels) || channels->valueint != 1) return false;
    if (strcmp(encoding->valuestring, "pcm16") == 0) { *opus = false; return true; }
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    if (strcmp(encoding->valuestring, "opus") == 0) {
        const cJSON* duration = cJSON_GetObjectItemCaseSensitive(output, "packetDurationMs");
        if (duration && (!cJSON_IsNumber(duration) || duration->valueint != 20)) return false;
        *opus = true;
        return true;
    }
#endif
    return false;
}

class VoiceWifiPower {
public:
    VoiceWifiPower() {
        changed_ = esp_wifi_get_ps(&previous_) == ESP_OK && previous_ != WIFI_PS_NONE &&
                   esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
        if (changed_) ESP_LOGI(TAG, "WiFi power saving suspended for voice");
    }
    ~VoiceWifiPower() { if (changed_) (void)esp_wifi_set_ps(previous_); }
private:
    wifi_ps_type_t previous_ = WIFI_PS_MIN_MODEM;
    bool changed_ = false;
};

#if CONFIG_AGENT_LINK_REALTIME_DIRECT
bool DirectConfigValid() {
    const char* base = CONFIG_AGENT_LINK_REALTIME_BASE_URL;
    const char* session = CONFIG_AGENT_LINK_REALTIME_SESSION_ID;
    if (strncmp(base, "http://", 7) != 0 && strncmp(base, "https://", 8) != 0) return false;
    if (!*session || !SafeToken(CONFIG_AGENT_LINK_REALTIME_USER_API_KEY)) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(session); *p; ++p) {
        if (!std::isalnum(*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

// Separate from the device gateway client; its credential is not a User API Key.
esp_err_t DirectHttp(esp_http_client_method_t method, const char* suffix,
                     char* response, size_t capacity, int* status) {
    char url[512];
    const char* base = CONFIG_AGENT_LINK_REALTIME_BASE_URL;
    size_t base_len = strlen(base);
    while (base_len && base[base_len - 1] == '/') --base_len;
    const int length = snprintf(url, sizeof url, "%.*s/api/v1/user/sessions/%s/realtime%s",
            static_cast<int>(base_len), base, CONFIG_AGENT_LINK_REALTIME_SESSION_ID, suffix);
    if (length <= 0 || length >= static_cast<int>(sizeof url)) return ESP_ERR_INVALID_SIZE;

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = method;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    char authorization[sizeof Grant::token + 16];
    snprintf(authorization, sizeof authorization, "Bearer %s", CONFIG_AGENT_LINK_REALTIME_USER_API_KEY);
    esp_err_t result = esp_http_client_set_header(client, "Authorization", authorization);
    memset(authorization, 0, sizeof authorization);
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Content-Type", "application/json");
    const bool post = method == HTTP_METHOD_POST;
    const char* body = "{}";
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    if (!*suffix) body = "{\"audio\":{\"input\":{\"encoding\":\"pcm16\",\"sampleRate\":16000,\"channels\":1},"
                         "\"output\":{\"encoding\":\"opus\",\"sampleRate\":24000,\"channels\":1,\"packetDurationMs\":20}}}";
#endif
    const int body_bytes = static_cast<int>(strlen(body));
    if (result == ESP_OK) result = esp_http_client_open(client, post ? body_bytes : 0);
    if (result == ESP_OK && post && esp_http_client_write(client, body, body_bytes) != body_bytes) result = ESP_FAIL;
    if (result == ESP_OK && esp_http_client_fetch_headers(client) < 0) result = ESP_FAIL;
    if (result == ESP_OK) {
        *status = esp_http_client_get_status_code(client);
        const int bytes = esp_http_client_read_response(client, response, static_cast<int>(capacity - 1));
        if (bytes < 0 || !esp_http_client_is_complete_data_received(client)) result = ESP_FAIL;
        else response[bytes] = '\0';
    }
    esp_http_client_cleanup(client);
    return result;
}

bool FetchDirectGrant(Grant* grant, const char** hint) {
    if (s_cb.state) s_cb.state(false, "Requesting voice access");
    auto* response = static_cast<char*>(heap_caps_calloc(1, 8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!response) response = static_cast<char*>(calloc(1, 8192));
    if (!response) { *hint = "No memory for voice access"; return false; }
    int status = 0;
    bool opus = false;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    opus = true;
#endif
    esp_err_t result = DirectHttp(HTTP_METHOD_POST, "", response, 8192, &status);
    ESP_LOGI(TAG, "voice grant request: HTTP %d (%s)", status, esp_err_to_name(result));
    if (result == ESP_OK && status == 409 && Current(grant->epoch)) {
        result = DirectHttp(HTTP_METHOD_GET, "", response, 8192, &status);
        cJSON* existing = result == ESP_OK && status == 200 ? cJSON_Parse(response) : nullptr;
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(existing, "data");
        const bool known = ReadAudioFormat(cJSON_IsObject(data) ? data : existing, &opus);
        cJSON_Delete(existing);
        if (!known) {
            memset(response, 0, 8192);
            free(response);
            *hint = "Cannot resume this audio format. End the previous call";
            return false;
        }
        result = DirectHttp(HTTP_METHOD_POST, "/connect-tokens", response, 8192, &status);
        ESP_LOGI(TAG, "voice token request: HTTP %d (%s)", status, esp_err_to_name(result));
    }
    bool valid = false;
    if (result == ESP_OK && status == 201) {
        cJSON* json = cJSON_Parse(response);
        const cJSON* body = json;
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (cJSON_IsObject(data)) body = data;
        const cJSON* url = cJSON_GetObjectItemCaseSensitive(body, "websocketUrl");
        const cJSON* token = cJSON_GetObjectItemCaseSensitive(body, "connectToken");
        const cJSON* connection = cJSON_GetObjectItemCaseSensitive(body, "realtimeConnection");
        const bool has_format = cJSON_HasObjectItem(connection, "audio");
        WssAddress parsed = {};
        valid = cJSON_IsString(url) && cJSON_IsString(token) &&
                strlen(url->valuestring) < sizeof grant->url && ParseWss(url->valuestring, &parsed) &&
                SafeToken(token->valuestring) && (!has_format || ReadAudioFormat(body, &opus));
        if (valid) {
            snprintf(grant->url, sizeof grant->url, "%s", url->valuestring);
            snprintf(grant->token, sizeof grant->token, "%s", token->valuestring);
            grant->opus = opus;
            ESP_LOGI(TAG, "voice grant accepted; connecting to WSS");
        }
        cJSON_Delete(json);
    }
    if (!valid) {
        *hint = result != ESP_OK ? "Voice API network error. Tap Connect to retry"
              : status == 401 ? "Voice API rejected the User API Key (401)"
              : status == 403 ? "Voice API access denied (403)"
              : status == 404 ? "Voice API session not found (404)"
              : status == 409 ? "Voice session is busy (409)"
              : status == 201 ? "Voice API returned an invalid WSS grant"
                              : "Voice API rejected the request. Check the log";
    }
    memset(response, 0, 8192);
    free(response);
    return valid;
}

void EndDirectSession() {
    char response[512] = {};
    int status = 0;
    const esp_err_t result = DirectHttp(HTTP_METHOD_DELETE, "", response, sizeof response, &status);
    ESP_LOGI(TAG, "voice session cleanup: HTTP %d (%s)", status, esp_err_to_name(result));
    memset(response, 0, sizeof response);
}
#endif

bool Send(esp_transport_handle_t ws, char* json, size_t bytes) {
    const auto opcode = static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN);
    return esp_transport_ws_send_raw(ws, opcode, json,
                                    static_cast<int>(bytes), 1000) == static_cast<int>(bytes);
}

bool Send(esp_transport_handle_t ws, const char* json) {
    return agentlink::realtime::SendControl(json, [ws](char* frame, size_t bytes) {
        return Send(ws, frame, bytes);
    });
}

void ResetInput() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_input = AGENT_STREAM_KIND_COUNT;
    if (s_tx) xQueueReset(s_tx);
}

class Call {
public:
    Call(uint32_t epoch, bool opus, bool continuous)
        : epoch_(epoch), continuous_(continuous), opus_(opus) {}
    ~Call() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (decoder_) esp_opus_dec_close(decoder_);
#endif
    }

    bool OpenDecoder() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (opus_ && !decoder_) {
            esp_opus_dec_cfg_t config = {};
            config.sample_rate = 16000;
            config.channel = 1;
            config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
            config.self_delimited = false;
            if (esp_opus_dec_open(&config, sizeof config, &decoder_) != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Opus decoder could not be opened");
                return false;
            }
        }
#else
        if (opus_) return false;
#endif
        return true;
    }

    void ResetDecoder() {
        converter_.Reset();
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (decoder_) { esp_opus_dec_close(decoder_); decoder_ = nullptr; }
#endif
    }

    void ResetAudio() {
        ResetDecoder();
        play_due_ = 0;
        reset_ = s_reset.load();
        if (s_cb.audio_reset) s_cb.audio_reset();
    }

    void Pcm(const uint8_t* data, size_t bytes) {
        if (!s_ready || s_mute) return;
        while (bytes && Current(epoch_) && !s_mute && reset_ == s_reset) {
            const size_t take = bytes > 2048 ? 2048 : bytes;
            int16_t pcm[684];
            const size_t samples = converter_.Convert(data, take, pcm);
            data += take;
            bytes -= take;
            if (!samples) continue;
            Output(pcm, samples);
        }
    }

    void Output(const int16_t* pcm, size_t samples) {
        const int64_t entered = esp_timer_get_time();
        const int64_t duration = static_cast<int64_t>(samples) * 1000000 / 16000;
        const int64_t buffer = static_cast<int64_t>(agentlink::audio::BufferMs()) * 750;
        while (play_due_ + duration - esp_timer_get_time() > buffer) {
            if (!Current(epoch_) || s_mute || reset_ != s_reset) return;
            vTaskDelay(1);
        }
        if (!Current(epoch_) || s_mute || reset_ != s_reset) return;
        const int64_t before_sink = esp_timer_get_time();
        if (before_sink - entered > max_pacing_) max_pacing_ = before_sink - entered;
        if (s_cb.pcm) s_cb.pcm(reinterpret_cast<const uint8_t*>(pcm), samples * 2);
        const int64_t now = esp_timer_get_time();
        if (now - before_sink > max_sink_) max_sink_ = now - before_sink;
        if (play_due_ < now) play_due_ = now;
        play_due_ += duration;
        samples_ += samples;
    }

    bool Audio(uint8_t* data, size_t bytes) {
        if (!s_ready || s_mute || !bytes) return true;
        const int64_t now = esp_timer_get_time();
        if (last_audio_ && now - last_audio_ > max_gap_) max_gap_ = now - last_audio_;
        last_audio_ = now;
        encoded_bytes_ += bytes;
        ++packets_;
        if (!opus_) {
            if (bytes % 2) return false;
            Pcm(data, bytes);
            return true;
        }
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        const int64_t decode_started = esp_timer_get_time();
        if (!OpenDecoder()) return false;
        int16_t pcm[1920];
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = data;
        raw.len = static_cast<uint32_t>(bytes);
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = reinterpret_cast<uint8_t*>(pcm);
        frame.len = sizeof pcm;
        esp_audio_dec_info_t info = {};
        const esp_audio_err_t result = esp_opus_dec_decode(decoder_, &raw, &frame, &info);
        const int64_t decoded_in = esp_timer_get_time() - decode_started;
        if (decoded_in > max_decode_) max_decode_ = decoded_in;
        if (result != ESP_AUDIO_ERR_OK || raw.consumed != bytes || frame.decoded_size % 2 ||
            frame.decoded_size > sizeof pcm || info.sample_rate != 16000 || info.channel != 1) {
            ESP_LOGW(TAG, "invalid Opus packet (%d, %u bytes)", result, static_cast<unsigned>(bytes));
            return false;
        }
        Output(pcm, frame.decoded_size / 2);
        return true;
#else
        return false;
#endif
    }

    bool Text(const uint8_t* data, size_t bytes) {
        cJSON* json = cJSON_ParseWithLength(reinterpret_cast<const char*>(data), bytes);
        if (!json) return false;
        const cJSON* type = cJSON_GetObjectItemCaseSensitive(json, "type");
        const char* event = cJSON_IsString(type) ? type->valuestring : "";
        bool keep = true;
        if (strcmp(event, "session.ready") == 0) {
            if (cJSON_HasObjectItem(json, "audio")) keep = ReadAudioFormat(json, &opus_);
            if (!keep || !OpenDecoder()) { cJSON_Delete(json); return false; }
            s_mute.store(false);
            if (!s_ready.exchange(true) && s_cb.state)
                s_cb.state(true, continuous_ ? "Microphone on" : "Hold to talk, release to send");
            ESP_LOGI(TAG, "voice session ready (PCM16 16 kHz up, %s 24 kHz down, %s)",
                     opus_ ? "Opus" : "PCM16", continuous_ ? "server endpointing" : "client PTT");
        } else if (strcmp(event, "response.audio.delta") == 0) {
            const cJSON* audio = cJSON_GetObjectItemCaseSensitive(json, "audio");
            if (cJSON_IsString(audio) && !s_mute && s_ready) {
                const size_t len = strlen(audio->valuestring);
                const size_t capacity = len / 4 * 3 + 3;
                auto* decoded = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!decoded) decoded = static_cast<uint8_t*>(malloc(capacity));
                size_t got = 0;
                if (!decoded || mbedtls_base64_decode(decoded, capacity, &got,
                        reinterpret_cast<const uint8_t*>(audio->valuestring), len) != 0) {
                    keep = false;
                } else {
                    keep = Audio(decoded, got);
                }
                free(decoded);
            }
        } else if (strcmp(event, "response.audio.done") == 0) {
            ESP_LOGI(TAG, "reply audio: %s, %u packets, %u bytes, %u ms PCM, max gap %u ms",
                     opus_ ? "Opus" : "PCM16", packets_, static_cast<unsigned>(encoded_bytes_),
                     static_cast<unsigned>(samples_ / 16), static_cast<unsigned>(max_gap_ / 1000));
            ESP_LOGI(TAG, "reply processing: decode max %u us, sink max %u us, pacing max %u ms",
                     static_cast<unsigned>(max_decode_), static_cast<unsigned>(max_sink_),
                     static_cast<unsigned>(max_pacing_ / 1000));
            ResetDecoder();
            if (s_cb.audio_end) s_cb.audio_end();
            if (s_cb.state && s_ready)
                s_cb.state(true, continuous_ ? "Microphone on" : "Hold to talk, release to send");
        } else if (strcmp(event, "response.started") == 0) {
            ResetDecoder();
            samples_ = encoded_bytes_ = packets_ = 0;
            last_audio_ = max_gap_ = 0;
            max_decode_ = max_sink_ = max_pacing_ = 0;
            if (s_cb.state && s_ready && !s_mute) s_cb.state(true, "Receiving agent reply");
        } else if (strcmp(event, "response.interrupted") == 0) {
            ResetAudio();
        } else if (strcmp(event, "transcript.partial") == 0 || strcmp(event, "transcript.final") == 0) {
            const cJSON* text = cJSON_GetObjectItemCaseSensitive(json, "text");
            if (s_cb.text && cJSON_IsString(text)) s_cb.text(text->valuestring);
        } else if (strcmp(event, "error") == 0 || strcmp(event, "session.error") == 0 ||
                   strcmp(event, "session.ended") == 0) {
            // Do not print server payloads: they can contain connection credentials.
            ESP_LOGW(TAG, "server ended or rejected the voice session");
            keep = false;
        }
        cJSON_Delete(json);
        return keep;
    }

    bool FlushTx(esp_transport_handle_t ws, TxScratch* scratch) {
        if (reset_ != s_reset) ResetAudio();
        Tx packet;
        const unsigned sends_per_pass = continuous_ ? 1 : 8;
        for (unsigned i = 0; i < sends_per_pass && Current(epoch_) &&
                             xQueueReceive(s_tx, &packet, 0) == pdTRUE; ++i) {
            if (packet.kind == TxKind::kPcm) {
                size_t pcm_bytes = packet.bytes;
                unsigned pcm_packets = 1;
                memcpy(scratch->pcm, packet.data, pcm_bytes);
                if (continuous_) {
                    Tx next;
                    while (pcm_packets < kUplinkBatchPackets && Current(epoch_) &&
                           xQueuePeek(s_tx, &next, 0) == pdTRUE && next.kind == TxKind::kPcm) {
                        if (xQueueReceive(s_tx, &next, 0) != pdTRUE) break;
                        memcpy(scratch->pcm + pcm_bytes, next.data, next.bytes);
                        pcm_bytes += next.bytes;
                        ++pcm_packets;
                    }
                }
                memcpy(scratch->json, kAudioPrefix, sizeof kAudioPrefix - 1);
                size_t encoded = 0;
                if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(scratch->json + sizeof kAudioPrefix - 1),
                        sizeof scratch->json - sizeof kAudioPrefix + 1, &encoded,
                        scratch->pcm, pcm_bytes) != 0) return false;
                const size_t n = sizeof kAudioPrefix - 1 + encoded;
                scratch->json[n] = '"';
                scratch->json[n + 1] = '}';
                SetWorkerPhase(1);
                const int64_t started = esp_timer_get_time();
                const bool sent = Send(ws, scratch->json, n + 2);
                const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
                const uint32_t old_max = s_tx_send_max_ms.load();
                if (elapsed_ms > old_max) s_tx_send_max_ms.store(elapsed_ms);
                SetWorkerPhase(0);
                if (!sent) {
                    ESP_LOGW(TAG, "audio WSS send failed after %u ms", static_cast<unsigned>(elapsed_ms));
                    return false;
                }
                s_tx_sent_frames.fetch_add(pcm_packets);
                s_tx_sent_messages.fetch_add(1);
            } else {
                if (!Send(ws, packet.kind == TxKind::kStart
                        ? "{\"type\":\"input_audio.activity\",\"kind\":\"start\"}"
                        : "{\"type\":\"input_audio.activity\",\"kind\":\"end\"}")) return false;
                if (packet.kind == TxKind::kEnd) {
                    if (packet.turn == s_reset.load()) s_mute.store(false);
                    if (s_cb.state) s_cb.state(true, "Waiting for the agent");
                }
            }
        }
        return true;
    }

private:
    uint32_t epoch_;
    bool continuous_ = false;
    uint32_t reset_ = s_reset.load();
    int64_t play_due_ = 0;
    bool opus_ = false;
    size_t samples_ = 0, encoded_bytes_ = 0;
    unsigned packets_ = 0;
    int64_t last_audio_ = 0, max_gap_ = 0;
    int64_t max_decode_ = 0, max_sink_ = 0, max_pacing_ = 0;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    void* decoder_ = nullptr;
#endif
    agentlink::realtime::Pcm24To16 converter_;
};

void ConnectOnce(const Grant& grant) {
    WssAddress addr = {};
    if (!ParseWss(grant.url, &addr)) {
        ESP_LOGW(TAG, "rejected an invalid WSS URL");
        return;
    }

    VoiceWifiPower power;
    esp_transport_handle_t tls = esp_transport_ssl_init();
    if (!tls) {
        ESP_LOGE(TAG, "could not allocate TLS transport");
        return;
    }
    esp_transport_ssl_crt_bundle_attach(tls, esp_crt_bundle_attach);
    esp_transport_handle_t ws = esp_transport_ws_init(tls);
    if (!ws) {
        ESP_LOGE(TAG, "could not allocate WebSocket transport");
        esp_transport_destroy(tls);
        return;
    }
    const esp_transport_ws_config_t config = {
        .ws_path = addr.path,
        .sub_protocol = "tidb-link.realtime.v1",
        .propagate_control_frames = true,
    };
    if (esp_transport_ws_set_config(ws, &config) != ESP_OK) {
        ESP_LOGE(TAG, "could not set the realtime subprotocol");
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return;
    }

    auto* message = static_cast<uint8_t*>(heap_caps_malloc(kMessageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!message) message = static_cast<uint8_t*>(malloc(kMessageBytes));
    if (!message) {
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return;
    }
    auto* scratch = static_cast<TxScratch*>(heap_caps_malloc(sizeof(TxScratch),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch) scratch = static_cast<TxScratch*>(malloc(sizeof(TxScratch)));
    if (!scratch) {
        ESP_LOGE(TAG, "could not allocate voice uplink buffer");
        free(message);
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return;
    }
    if (s_cb.state) s_cb.state(false, "Connecting voice");
    if (esp_transport_connect(ws, addr.host, addr.port, kConnectTimeoutMs) != 0) {
        ESP_LOGW(TAG, "WSS connection failed (upgrade HTTP %d)",
                 esp_transport_ws_get_upgrade_request_status(ws));
    } else {
        char hello[sizeof Grant::token * 2 + 128];
        size_t n = static_cast<size_t>(snprintf(hello, sizeof hello,
                                               "{\"type\":\"session.connect\",\"connect_token\":\""));
        for (const char* p = grant.token; *p && n + 4 < sizeof hello; ++p) {
            if (*p == '"' || *p == '\\') hello[n++] = '\\';
            hello[n++] = *p;
        }
        n += static_cast<size_t>(snprintf(hello + n, sizeof hello - n,
                "\",\"endpointing\":{\"mode\":\"%s\"}}", grant.platform ? "server" : "client"));
        if (!Current(grant.epoch)) {
            ESP_LOGI(TAG, "voice connection cancelled before session.connect");
        } else if (!Send(ws, hello, n)) {
            ESP_LOGW(TAG, "session.connect could not be sent");
        } else {
            ESP_LOGI(TAG, "session.connect sent over WSS");
            Call call(grant.epoch, grant.opus, grant.platform);
            agentlink::realtime::Message assembler(message, kMessageBytes);
            uint8_t chunk[2048];
            int frame_read = 0;
            const int64_t connected_at = esp_timer_get_time();
            int64_t last_rx = connected_at, last_ping = connected_at;
            SetWorkerPhase(0);
            while (Current(grant.epoch)) {
                if (!call.FlushTx(ws, scratch)) break;
                const int64_t now = esp_timer_get_time();
                if ((!s_ready && now - connected_at > kReadyTimeoutUs) || now - last_rx > 45000000) {
                    ESP_LOGW(TAG, "voice session timed out");
                    break;
                }
                if (now - last_ping > 15000000) {
                    if (esp_transport_ws_send_raw(ws, static_cast<ws_transport_opcodes_t>(
                            WS_TRANSPORT_OPCODES_PING | WS_TRANSPORT_OPCODES_FIN), nullptr, 0, 1000) < 0) break;
                    last_ping = now;
                }
                if (!frame_read) {
                    SetWorkerPhase(2);
                    const int polled = esp_transport_poll_read(ws, 10);
                    SetWorkerPhase(0);
                    if (polled < 0) break;
                    if (!polled) { vTaskDelay(1); continue; }
                }
                SetWorkerPhase(3);
                const int got = esp_transport_read(ws, reinterpret_cast<char*>(chunk), sizeof chunk, 1000);
                SetWorkerPhase(0);
                if (got < 0 || !Current(grant.epoch)) break;
                const auto opcode = esp_transport_ws_get_read_opcode(ws);
                const int frame_size = esp_transport_ws_get_read_payload_len(ws);
                const bool first = frame_read == 0;
                frame_read += got;
                const bool end = frame_read == frame_size;
                if (frame_size < 0 || frame_read > frame_size || (got == 0 && !end)) break;
                last_rx = esp_timer_get_time();
                if (opcode == WS_TRANSPORT_OPCODES_CLOSE) break;
                if (opcode == WS_TRANSPORT_OPCODES_PING || opcode == WS_TRANSPORT_OPCODES_PONG) {
                    if (!first || !end || frame_size > 125) break;
                    if (opcode == WS_TRANSPORT_OPCODES_PING && esp_transport_ws_send_raw(ws,
                            static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN),
                            reinterpret_cast<char*>(chunk), got, 1000) < 0) break;
                } else {
                    const auto result = assembler.Append(opcode, first, end,
                            esp_transport_ws_get_fin_flag(ws), chunk, static_cast<size_t>(got));
                    if (result == agentlink::realtime::Message::kError) {
                        ESP_LOGW(TAG, "invalid or oversized voice frame");
                        break;
                    }
                    if (result == agentlink::realtime::Message::kComplete) {
                        s_rx_frames.fetch_add(1);
                        SetWorkerPhase(4);
                        bool handled = true;
                        if (assembler.Opcode() == WS_TRANSPORT_OPCODES_BINARY) {
                            handled = call.Audio(message, assembler.Size());
                        } else handled = call.Text(message, assembler.Size());
                        SetWorkerPhase(0);
                        if (!handled) break;
                    }
                }
                if (end) frame_read = 0;
            }
        }
        memset(hello, 0, sizeof hello);
        Send(ws, "{\"type\":\"session.end\"}");
        esp_transport_close(ws);
    }
    free(scratch);
    free(message);
    esp_transport_destroy(ws);
    esp_transport_destroy(tls);
}

void Worker(void*) {
    Grant grant = {};
    while (s_run.load(std::memory_order_acquire)) {
        if (xQueueReceive(s_grants, &grant, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        const char* hint = "Voice ended. Tap Connect to retry";
        bool granted = !grant.direct && !grant.platform;
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
        if (grant.direct && Current(grant.epoch)) granted = FetchDirectGrant(&grant, &hint);
#endif
        if (grant.platform && Current(grant.epoch)) {
            int status = 0;
            granted = cloud_platform_grant(grant.agent_id, grant.session_id, false,
                                           grant.url, sizeof grant.url, grant.token, sizeof grant.token,
                                           &grant.opus, &status) == ESP_OK;
            if (!granted && status == 409 && Current(grant.epoch)) {
                granted = cloud_platform_grant(grant.agent_id, grant.session_id, true,
                                               grant.url, sizeof grant.url, grant.token, sizeof grant.token,
                                               &grant.opus, &status) == ESP_OK;
            }
            if (!granted) hint = status == 409 ? "Voice session busy. End the previous call"
                                 : "Voice access failed. Tap Connect to retry";
        }
        if (granted && Current(grant.epoch)) ConnectOnce(grant);
        if (grant.platform && granted) cloud_platform_end_grant(grant.agent_id, grant.session_id);
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
        if (grant.direct && granted) EndDirectSession();
#endif
        s_ready.store(false);
        ResetInput();
        if (s_cb.audio_reset) s_cb.audio_reset();
        if (s_cb.state) s_cb.state(false, hint);
        if (s_cb.finished) s_cb.finished();
        memset(&grant, 0, sizeof grant);
    }
    s_task.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}
}  // namespace

namespace {
// Called with s_mutex held; both grant sources share the same worker and queue.
esp_err_t QueueGrant(Grant* grant) {
    grant->epoch = s_epoch.fetch_add(1) + 1;
    s_ready.store(false);
    s_continuous.store(grant->platform);
    if (xQueueOverwrite(s_grants, grant) != pdPASS) return ESP_FAIL;
    if (!s_task.load(std::memory_order_acquire)) {
        TaskHandle_t task = nullptr;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        constexpr unsigned stack_bytes = 24576;
#else
        constexpr unsigned stack_bytes = 12288;
#endif
        if (xTaskCreate(&Worker, "agentlink_rt", stack_bytes, nullptr, 5, &task) != pdPASS) {
            xQueueReset(s_grants);
            return ESP_ERR_NO_MEM;
        }
        s_task.store(task, std::memory_order_release);
    }
    return ESP_OK;
}
}  // namespace

esp_err_t cloud_realtime_start(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_grants) return ESP_OK;
    s_grants = xQueueCreateWithCaps(1, sizeof(Grant), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_grants_caps = s_grants != nullptr;
    if (!s_grants) s_grants = xQueueCreate(1, sizeof(Grant));
    if (!s_grants) return ESP_ERR_NO_MEM;
    s_tx = xQueueCreateWithCaps(kTxDepth, sizeof(Tx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx_caps = s_tx != nullptr;
    if (!s_tx) s_tx = xQueueCreate(kTxDepth, sizeof(Tx));
    if (!s_tx) {
        if (s_grants_caps) vQueueDeleteWithCaps(s_grants);
        else vQueueDelete(s_grants);
        s_grants = nullptr;
        s_grants_caps = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voice queues: tx=%s grant=%s, internal free=%u largest=%u",
             s_tx_caps ? "PSRAM" : "internal", s_grants_caps ? "PSRAM" : "internal",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    s_run.store(true, std::memory_order_release);
    return ESP_OK;
}

void cloud_realtime_stop(void) {
    if (!s_grants) return;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_run.store(false, std::memory_order_release);
        s_epoch.fetch_add(1);
        s_ready.store(false);
    }
    while (s_task.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(50));
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_grants_caps) vQueueDeleteWithCaps(s_grants);
    else vQueueDelete(s_grants);
    s_grants = nullptr;
    s_grants_caps = false;
    // WithCaps buffers must be released with the matching API.
    if (s_tx_caps) vQueueDeleteWithCaps(s_tx);
    else vQueueDelete(s_tx);
    s_tx = nullptr;
    s_input = AGENT_STREAM_KIND_COUNT;
    s_continuous.store(false);
}

esp_err_t cloud_realtime_offer(const char* websocket_url, const char* connect_token) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_grants || !s_run) return ESP_ERR_INVALID_STATE;
    if (!websocket_url || strlen(websocket_url) >= sizeof Grant::url || !SafeToken(connect_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    WssAddress parsed = {};
    if (!ParseWss(websocket_url, &parsed)) return ESP_ERR_INVALID_ARG;
    Grant grant = {};
    snprintf(grant.url, sizeof grant.url, "%s", websocket_url);
    snprintf(grant.token, sizeof grant.token, "%s", connect_token);
    const esp_err_t result = QueueGrant(&grant);
    memset(&grant, 0, sizeof grant);
    return result;
}

bool cloud_realtime_direct_enabled(void) {
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
    return true;
#else
    return false;
#endif
}

esp_err_t cloud_realtime_request_direct(void) {
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
    if (!DirectConfigValid()) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_grants || !s_run) return ESP_ERR_INVALID_STATE;
    Grant grant = {};
    grant.direct = true;
    return QueueGrant(&grant);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t cloud_realtime_request_platform(int agent_id, const char* session_id) {
    if (!cloud_platform_enabled() || agent_id <= 0 || !session_id || !*session_id ||
        strlen(session_id) >= sizeof Grant::session_id) return ESP_ERR_INVALID_ARG;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(session_id); *p; ++p) {
        if (!std::isalnum(*p) && *p != '_' && *p != '-') return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_grants || !s_run) return ESP_ERR_INVALID_STATE;
    Grant grant = {};
    grant.direct = false;
    grant.platform = true;
    grant.agent_id = agent_id;
    snprintf(grant.session_id, sizeof grant.session_id, "%s", session_id);
    return QueueGrant(&grant);
}

void cloud_realtime_set_callbacks(const cloud_realtime_callbacks_t* callbacks) {
    s_cb = callbacks ? *callbacks : cloud_realtime_callbacks_t{};
}

bool cloud_realtime_ready(void) { return s_ready.load(); }

void cloud_realtime_cancel(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_epoch.fetch_add(1);
    s_ready.store(false);
    s_mute.store(true);
    s_input = AGENT_STREAM_KIND_COUNT;
    if (s_grants) xQueueReset(s_grants);
    if (s_tx) xQueueReset(s_tx);
}

esp_err_t cloud_realtime_stream_start(agent_stream_t type) {
    if (type != AGENT_STREAM_VOICE && type != AGENT_STREAM_AUDIO) return ESP_ERR_NOT_SUPPORTED;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_run || !s_ready || s_input != AGENT_STREAM_KIND_COUNT) return ESP_ERR_INVALID_STATE;
    if (s_continuous) {
        s_tx_sent_frames.store(0);
        s_tx_sent_messages.store(0);
        s_tx_send_max_ms.store(0);
        s_rx_frames.store(0);
        s_diag_last_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        s_diag_mic_frames = s_diag_dropped_frames = 0;
        s_diag_prev_sent_frames = s_diag_prev_sent_messages = s_diag_prev_rx_frames = 0;
        s_input = type;
        return ESP_OK;
    }
    if (uxQueueSpacesAvailable(s_tx) < 3) return ESP_ERR_NO_MEM;
    const uint32_t turn = s_reset.fetch_add(1) + 1;
    const Tx start = {TxKind::kStart, 0, turn, {}};
    if (xQueueSend(s_tx, &start, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    s_input = type;
    s_mute.store(true);
    return ESP_OK;
}

esp_err_t cloud_realtime_stream_write(agent_stream_t type, const uint8_t* data, size_t bytes) {
    if (!data || !bytes || bytes % 2) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_run || !s_ready || s_input != type) return ESP_ERR_INVALID_STATE;
    const size_t packets = (bytes + kMicBytes - 1) / kMicBytes;
    if (s_continuous) s_diag_mic_frames += static_cast<uint32_t>(packets);
    if (packets + 2 > uxQueueSpacesAvailable(s_tx)) {
        if (s_continuous) {
            s_diag_dropped_frames += static_cast<uint32_t>(packets);
            ReportUplink();
        }
        return ESP_ERR_NO_MEM;
    }
    Tx packet = {TxKind::kPcm, 0, 0, {}};
    while (bytes) {
        packet.bytes = bytes > kMicBytes ? kMicBytes : bytes;
        memcpy(packet.data, data, packet.bytes);
        if (xQueueSend(s_tx, &packet, 0) != pdTRUE) {
            if (s_continuous) {
                ++s_diag_dropped_frames;
                ReportUplink();
            }
            return ESP_ERR_NO_MEM;
        }
        data += packet.bytes;
        bytes -= packet.bytes;
    }
    if (s_continuous) ReportUplink();
    return ESP_OK;
}

esp_err_t cloud_realtime_stream_end(agent_stream_t type, bool complete) {
    if (!complete) { cloud_realtime_cancel(); return ESP_OK; }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_run || !s_ready || s_input != type) return ESP_ERR_INVALID_STATE;
    if (s_continuous) {
        s_input = AGENT_STREAM_KIND_COUNT;
        return ESP_OK;
    }
    const Tx end = {TxKind::kEnd, 0, s_reset.load(), {}};
    if (xQueueSend(s_tx, &end, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    s_input = AGENT_STREAM_KIND_COUNT;
    return ESP_OK;
}
