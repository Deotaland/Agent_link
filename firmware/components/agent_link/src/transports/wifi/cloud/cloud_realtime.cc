#include "cloud_realtime.h"
#include "realtime_audio.h"
#include "realtime_audio_json.h"
#include "realtime_buffer.h"
#include "realtime_recovery.h"
#include "realtime_ws_frame.h"
#include "realtime_ws_stream.h"
#include "realtime_ws_upgrade.h"
#include "audio_downlink.h"
#include "cloud_platform.h"
#include "cloud_limits.h"
#include "cloud_memory.h"
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
#include "realtime_codec_memory.h"
#endif

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <new>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#if CONFIG_AGENT_LINK_REALTIME_OPUS
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/version.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#else
#include "mbedtls/sha1.h"
#endif
#include "lwip/sockets.h"
#include "lwip/tcp.h"

namespace {
using agentlink::realtime::AudioFormat;
using agentlink::realtime::ReadAudioFormat;
using namespace agentlink::cloud_limits;
constexpr const char* TAG = "agent_link.realtime";
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 3000;
constexpr size_t kMicBytes = 640;
#if CONFIG_IDF_TARGET_ESP32C3
// Negotiation accepts exactly 20 ms, decoded at 16 kHz mono PCM16.
constexpr size_t kDecodePcmBytes = kMicBytes;
#else
constexpr size_t kDecodePcmBytes = 3840;
#endif
constexpr unsigned kOpusBatchFrames = kEncodedDepth < 3 ? kEncodedDepth : 3;
constexpr size_t kUplinkBatchBytes = kMicBytes * kUplinkBatchPackets;
constexpr char kAudioPrefix[] = "{\"type\":\"input_audio.append\",\"audio\":\"";
constexpr size_t kAudioJsonBytes = sizeof(kAudioPrefix) - 1 + (kUplinkBatchBytes + 2) / 3 * 4 + 3;
constexpr int64_t kReadyTimeoutUs = 10000000;
#if CONFIG_IDF_TARGET_ESP32C3
using VoiceTls = esp_tls_t*;
#else
using VoiceTls = esp_transport_handle_t;
#endif

void* ResizeMessage(void* data, size_t bytes) {
    if (!bytes) { free(data); return nullptr; }
    void* next = heap_caps_realloc(data, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return next ? next : realloc(data, bytes);
}

struct Grant {
    char url[512];
    char token[256];
    uint32_t epoch;
    bool direct;
    agentlink::realtime::AudioFormat audio;
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
    uint8_t wire[kAudioJsonBytes + 8];
    uint8_t pcm[kUplinkBatchBytes];
    char json[kAudioJsonBytes];
    uint8_t read[kReadBytes];
};

struct Rx {
    unsigned opcode;
    size_t bytes;
    uint8_t* data;
    int64_t received_at;
    uint32_t reset;
};

struct EncodedTx {
    TxKind kind;
    uint16_t bytes;
    uint32_t turn;
    uint8_t data[1275];
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
std::atomic<uint32_t> s_tx_write_calls{0};
std::atomic<uint32_t> s_tx_bytes{0}, s_rx_bytes{0}, s_decoded_samples{0};
std::atomic<uint32_t> s_tx_send_ms{0}, s_tx_encode_max_us{0};
std::atomic<uint32_t> s_downlink_bytes{0}, s_media_stack{0}, s_encoder_stack{0};
std::atomic<uint32_t> s_encoded_pending{0};
std::atomic<uint32_t> s_tx_send_max_ms{0};
std::atomic<uint32_t> s_worker_phase{0};
std::atomic<uint32_t> s_worker_phase_since_ms{0};
uint32_t s_diag_last_ms = 0;
uint32_t s_diag_mic_frames = 0;
uint32_t s_diag_dropped_frames = 0;
uint32_t s_diag_prev_sent_frames = 0;
uint32_t s_diag_prev_sent_messages = 0;
uint32_t s_diag_prev_tx_bytes = 0, s_diag_prev_rx_bytes = 0, s_diag_prev_samples = 0;

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
    const uint32_t tx_bytes = s_tx_bytes.load(), rx_bytes = s_rx_bytes.load();
    const uint32_t samples = s_decoded_samples.load();
    const uint32_t interval = now - s_diag_last_ms;
    const uint32_t messages = sent_messages - s_diag_prev_sent_messages;
    const uint32_t send_ms = s_tx_send_ms.exchange(0);
    const uint32_t writes = s_tx_write_calls.exchange(0);
    const uint32_t phase = s_worker_phase.load();
    const uint32_t phase_age = now - s_worker_phase_since_ms.load();
    constexpr const char* phases[] = {"loop", "send", "poll", "read", "handle"};
    const char* phase_name = phase < sizeof phases / sizeof phases[0] ? phases[phase] : "unknown";
    ESP_LOGI(TAG,
             "voice %ums: mic=%u tx=%u drop=%u queue=%ums ws=%u writes=%u tx=%uB/s rx=%uB/s "
             "decoded=%ums send=%u/%ums enc=%uus pending=%uB worker=%s/%ums stack=%u/%u/%uB",
             static_cast<unsigned>(now - s_diag_last_ms), static_cast<unsigned>(s_diag_mic_frames),
             static_cast<unsigned>(sent_frames - s_diag_prev_sent_frames),
             static_cast<unsigned>(s_diag_dropped_frames),
             static_cast<unsigned>((uxQueueMessagesWaiting(s_tx) + s_encoded_pending.load()) * 20),
             static_cast<unsigned>(messages),
             static_cast<unsigned>(writes),
             static_cast<unsigned>((tx_bytes - s_diag_prev_tx_bytes) * 1000 / interval),
             static_cast<unsigned>((rx_bytes - s_diag_prev_rx_bytes) * 1000 / interval),
             static_cast<unsigned>((samples - s_diag_prev_samples) / 16),
             static_cast<unsigned>(writes ? send_ms / writes : 0),
             static_cast<unsigned>(s_tx_send_max_ms.exchange(0)),
             static_cast<unsigned>(s_tx_encode_max_us.exchange(0)),
             static_cast<unsigned>(s_downlink_bytes.load()),
             phase_name,
             static_cast<unsigned>(phase_age),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(s_media_stack.load()), static_cast<unsigned>(s_encoder_stack.load()));
#if CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGI(TAG, "voice memory: internal free=%u largest=%u minimum=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
    s_diag_last_ms = now;
    s_diag_mic_frames = s_diag_dropped_frames = 0;
    s_diag_prev_sent_frames = sent_frames;
    s_diag_prev_sent_messages = sent_messages;
    s_diag_prev_tx_bytes = tx_bytes;
    s_diag_prev_rx_bytes = rx_bytes;
    s_diag_prev_samples = samples;
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
    agentlink::cloud::NetworkMemoryLock memory;
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
    AudioFormat audio;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    audio.output_opus = true;
#endif
    esp_err_t result = DirectHttp(HTTP_METHOD_POST, "", response, 8192, &status);
    ESP_LOGI(TAG, "voice grant request: HTTP %d (%s)", status, esp_err_to_name(result));
    if (result == ESP_OK && status == 409 && Current(grant->epoch)) {
        result = DirectHttp(HTTP_METHOD_GET, "", response, 8192, &status);
        cJSON* existing = result == ESP_OK && status == 200 ? cJSON_Parse(response) : nullptr;
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(existing, "data");
        const bool known = ReadAudioFormat(cJSON_IsObject(data) ? data : existing, &audio, false);
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
                SafeToken(token->valuestring) && (!has_format || ReadAudioFormat(body, &audio, false));
        if (valid) {
            snprintf(grant->url, sizeof grant->url, "%s", url->valuestring);
            snprintf(grant->token, sizeof grant->token, "%s", token->valuestring);
            grant->audio = audio;
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

void LogTransportFailure(VoiceTls transport, const char* operation,
                         int result, int saved_errno) {
    int tls_code = 0, tls_flags = 0;
#if CONFIG_IDF_TARGET_ESP32C3
    esp_tls_error_handle_t error = nullptr;
    (void)esp_tls_get_error_handle(transport, &error);
    int socket_error = 0, socket = -1;
    socklen_t size = sizeof socket_error;
    if (esp_tls_get_conn_sockfd(transport, &socket) == ESP_OK && socket >= 0)
        (void)getsockopt(socket, SOL_SOCKET, SO_ERROR, &socket_error, &size);
#else
    const auto error = esp_transport_get_error_handle(transport);
    const int socket_error = esp_transport_get_errno(transport);
#endif
    const esp_err_t tls_error = error
        ? esp_tls_get_and_clear_last_error(error, &tls_code, &tls_flags) : ESP_OK;
    wifi_ap_record_t ap = {};
    const bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    ESP_LOGW(TAG, "%s: result=%d errno=%d socket_error=%d tls=%s code=%d flags=0x%x "
                  "wifi=%s rssi=%d internal=%u largest=%u",
             operation, result, saved_errno, socket_error, esp_err_to_name(tls_error),
             tls_code, tls_flags, associated ? "associated" : "disconnected", associated ? ap.rssi : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

struct WsSender {
    VoiceTls tls;
    uint8_t* wire;
    size_t capacity;
    int64_t last_slow_log = 0;
#if CONFIG_IDF_TARGET_ESP32C3
    agentlink::realtime::PendingWrite pending;
    unsigned audio_frames = 0, audio_messages = 0;
    size_t audio_bytes = 0;
    int64_t queued_at = 0;
#endif
};

int PollWritable(VoiceTls tls) {
#if CONFIG_IDF_TARGET_ESP32C3
    int socket = -1;
    if (esp_tls_get_conn_sockfd(tls, &socket) != ESP_OK || socket < 0) return -1;
    fd_set writes; FD_ZERO(&writes); FD_SET(socket, &writes);
    timeval timeout{};
    return select(socket + 1, nullptr, &writes, nullptr, &timeout);
#else
    return esp_transport_poll_write(tls, 0);
#endif
}

#if CONFIG_IDF_TARGET_ESP32C3
bool TlsWouldBlock(int result) {
    return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE;
}

bool PumpWire(WsSender& sender) {
    if (!sender.pending.Busy()) return true;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (sender.pending.Expired(now_ms)) {
        ESP_LOGW(TAG, "voice TLS write stalled for 3000 ms");
        return false;
    }
    // Retry exactly the retained suffix, including after WANT_READ/WANT_WRITE.
    // This socket stays nonblocking after the verified async TLS handshake.
    errno = 0;
    const int sent = esp_tls_conn_write(sender.tls, sender.wire + sender.pending.Offset(),
                                        sender.pending.Remaining());
    const bool waiting = TlsWouldBlock(sent);
    if (!sender.pending.Advance(sent, waiting, esp_timer_get_time() / 1000)) {
        LogTransportFailure(sender.tls, "write", sent, errno);
        return false;
    }
    if (!sender.pending.Busy()) {
        memset(sender.wire, 0, sender.pending.Size());
        if (sender.audio_messages) {
            const uint32_t elapsed = (esp_timer_get_time() - sender.queued_at) / 1000;
            s_tx_sent_frames.fetch_add(sender.audio_frames);
            s_tx_sent_messages.fetch_add(sender.audio_messages);
            s_tx_bytes.fetch_add(sender.audio_bytes);
            s_tx_write_calls.fetch_add(1);
            s_tx_send_ms.fetch_add(elapsed);
            if (elapsed > s_tx_send_max_ms.load()) s_tx_send_max_ms.store(elapsed);
            sender.audio_frames = sender.audio_messages = 0;
            sender.audio_bytes = 0;
        }
    }
    return true;
}
#endif

bool WriteWire(WsSender& sender, size_t expected) {
    if (!expected || expected > sender.capacity) return false;
#if CONFIG_IDF_TARGET_ESP32C3
    sender.queued_at = esp_timer_get_time();
    return sender.pending.Begin(expected, sender.queued_at / 1000);
#else
    errno = 0;
    const int64_t started = esp_timer_get_time();
    const int sent = esp_transport_write(sender.tls, reinterpret_cast<const char*>(sender.wire),
                                         static_cast<int>(expected), kSendTimeoutMs);
    const int saved_errno = errno;
    const uint32_t write_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
    const bool ok = sent == static_cast<int>(expected);
    // Clear the reusable masked buffer, including session.connect credentials.
    if (expected) memset(sender.wire, 0, expected);
    if (ok) {
        const int64_t now = esp_timer_get_time();
        if (write_ms >= 200 && now - sender.last_slow_log >= 1000000) {
            sender.last_slow_log = now;
            ESP_LOGW(TAG, "slow WSS write: wire=%u write=%ums (header+payload, one TLS write)",
                     static_cast<unsigned>(expected), static_cast<unsigned>(write_ms));
        }
        return true;
    }
    // Even a negative result can follow a partial frame header/payload write.
    // Close this connection; never replay the frame on the same stream.
    ESP_LOGW(TAG, "WSS frame send failed: wire=%u returned=%d write=%ums timeout=%dms",
             static_cast<unsigned>(expected), sent, static_cast<unsigned>(write_ms), kSendTimeoutMs);
    LogTransportFailure(sender.tls, "send", sent, saved_errno);
    return false;
#endif
}

size_t AppendFrame(WsSender& sender, size_t offset, const char* json, size_t bytes) {
#if CONFIG_IDF_TARGET_ESP32C3
    if (sender.pending.Busy()) return 0;
#endif
    if (offset > sender.capacity) return 0;
    uint8_t mask[4];
    esp_fill_random(mask, sizeof mask);
    return agentlink::realtime::EncodeTextFrame(json, bytes, mask,
                                               sender.wire + offset, sender.capacity - offset);
}

bool Send(WsSender& sender, const char* json, size_t bytes) {
    const size_t size = AppendFrame(sender, 0, json, bytes);
    return size && WriteWire(sender, size);
}

bool Send(WsSender& ws, const char* json) {
    return agentlink::realtime::SendControl(json, [&ws](char* frame, size_t bytes) {
        return Send(ws, frame, bytes);
    });
}

bool IsInterruption(const uint8_t* data, size_t bytes) {
    constexpr char event_name[] = "response.interrupted";
    if (std::search(data, data + bytes, event_name, event_name + sizeof event_name - 1) == data + bytes)
        return false;
    cJSON* json = cJSON_ParseWithLength(reinterpret_cast<const char*>(data), bytes);
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(json, "type");
    const bool interrupted = cJSON_IsString(type) && strcmp(type->valuestring, event_name) == 0;
    cJSON_Delete(json);
    return interrupted;
}

void ResetInput() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_input = AGENT_STREAM_KIND_COUNT;
    if (s_tx) xQueueReset(s_tx);
}

class Call {
public:
    Call(uint32_t epoch, AudioFormat audio, bool continuous)
        : epoch_(epoch), continuous_(continuous), audio_(audio), input_opus_(audio.input_opus) {}
    ~Call() {
        Stop();
        if (encoded_queue_) {
            if (encoded_queue_caps_) vQueueDeleteWithCaps(encoded_queue_);
            else vQueueDelete(encoded_queue_);
        }
        s_encoded_pending.store(0);
        if (rx_queue_) {
            Rx packet;
            while (xQueueReceive(rx_queue_, &packet, 0) == pdTRUE) free(packet.data);
            if (rx_caps_) vQueueDeleteWithCaps(rx_queue_);
            else vQueueDelete(rx_queue_);
        }
        s_downlink_bytes.store(0);
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (decoder_) esp_opus_dec_close(decoder_);
        if (encoder_) esp_opus_enc_close(encoder_);
#endif
        free(decode_pcm_);
        free(silence_);
    }

    bool Prepare() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        // The shared worker stack is already resident. TLS handshake scratch
        // has been released before these per-call codec states are allocated.
        ESP_LOGI(TAG, "before Opus allocation: internal free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        if (SharedCodec() && !codec_memory_.Reserve()) {
            ESP_LOGE(TAG, "could not reserve the %u-byte Opus workspace",
                     static_cast<unsigned>(agentlink::realtime::CodecWorkspace::kBytes));
            return false;
        }
        if ((!SharedCodec() && !OpenDecoder()) || (audio_.input_opus && !StartEncoder())) return false;
        ReportMemory("codec prepared");
#endif
        return true;
    }

    bool Start() {
#if CONFIG_IDF_TARGET_ESP32C3
        ESP_LOGI(TAG, "resident voice worker: network + codec, internal free=%u largest=%u stack_free=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        return true;
#else
        rx_queue_ = xQueueCreateWithCaps(kDownlinkDepth, sizeof(Rx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        rx_caps_ = rx_queue_ != nullptr;
        if (!rx_queue_) rx_queue_ = xQueueCreate(kDownlinkDepth, sizeof(Rx));
        if (!rx_queue_) return false;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        constexpr unsigned stack_bytes = 24576;
#else
        constexpr unsigned stack_bytes = 8192;
#endif
        media_started_ = xTaskCreate(&MediaEntry, "agentlink_audio", stack_bytes, this, 5, nullptr) == pdPASS;
        if (!media_started_)
            ESP_LOGE(TAG, "could not start downlink audio worker (%uB stack, internal free=%u largest=%u)",
                     stack_bytes,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return media_started_;
#endif
    }

#if CONFIG_IDF_TARGET_ESP32C3
    void BindNetwork(WsSender* sender, TxScratch* scratch) { sender_ = sender; scratch_ = scratch; }
    bool Pump() {
        if (reset_ != s_reset.load()) ResetAudio();
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (!PumpEncoder()) running_.store(false);
#endif
        s_media_stack.store(uxTaskGetStackHighWaterMark(nullptr));
        return Healthy();
    }
#endif

    bool Healthy() const { return running_.load(); }
    bool TransportFailed() const { return transport_failed_; }
    void Stop() {
        running_.store(false);
        while (media_started_ && !media_done_.load()) vTaskDelay(1);
        while (encoder_started_ && !encoder_done_.load()) vTaskDelay(1);
    }

    bool SharedCodec() const {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        // The Passport application already suppresses mic audio during speaker
        // playback. Keep its workers resident, but never keep both codec states
        // resident for a continuous half-duplex call.
        return continuous_ && audio_.input_opus && audio_.output_opus;
#else
        return false;
#endif
    }

    void ReportMemory(const char* phase) {
        (void)phase;
#if CONFIG_IDF_TARGET_ESP32C3
        ESP_LOGI(TAG, "voice memory %s: free=%u largest=%u minimum=%u stack_free=%u",
                 phase,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (SharedCodec())
            ESP_LOGI(TAG, "Opus workspace: reserved=%u peak=%u allocations=%u",
                     static_cast<unsigned>(agentlink::realtime::CodecWorkspace::kBytes),
                     static_cast<unsigned>(codec_memory_.Peak()), static_cast<unsigned>(codec_memory_.Live()));
#endif
#endif
    }

    void ClearCodecUplink() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        // Only queued packets are discarded at a reply boundary. A partially
        // written TLS/WebSocket frame must finish with its original bytes.
        std::lock_guard<std::mutex> lock(s_mutex);
        xQueueReset(s_tx);
        if (encoded_queue_) xQueueReset(encoded_queue_);
        s_encoded_pending.store(0);
        shared_frames_.Reset();
        batch_gate_.Reset();
#endif
    }

    bool BeginReply() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        if (!SharedCodec()) return true;
        replying_ = true;
        ClearCodecUplink();
        if (encoder_) { esp_opus_enc_close(encoder_); encoder_ = nullptr; }
        ResetDecoder();
        const bool opened = OpenDecoder();
        ReportMemory("reply decoder only");
        return opened;
#else
        return true;
#endif
    }

    bool EndReply() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
        if (!SharedCodec()) return true;
        replying_ = false;
        ResetDecoder();
        ClearCodecUplink();
        const bool opened = StartEncoder();
        ReportMemory("listening encoder only");
        return opened;
#else
        return true;
#endif
    }

    bool TxPending() {
        if (input_opus_.load()) {
            const unsigned queued = encoded_queue_ ? uxQueueMessagesWaiting(encoded_queue_) : 0;
            const bool due = batch_gate_.Ready(queued, esp_timer_get_time() / 1000);
            if (!queued) return false;
            if (!continuous_) return true;
            if (xQueuePeek(encoded_queue_, &encoded_packet_, 0) != pdTRUE) return false;
            return encoded_packet_.kind != TxKind::kPcm || due;
        }
        return uxQueueMessagesWaiting(s_tx);
    }
    bool WriteBlocked() const { return write_stall_.Blocked(); }

    // A busy queue pauses reads, while the network loop continues to send microphone audio.
    bool Deliver(unsigned opcode, const uint8_t* data, size_t bytes, int64_t received_at, uint32_t reset) {
#if CONFIG_IDF_TARGET_ESP32C3
        received_at_ = received_at;
        if (reset != s_reset.load()) return true;
        const bool handled = opcode == WS_TRANSPORT_OPCODES_BINARY
                           ? Audio(const_cast<uint8_t*>(data), bytes) : Text(data, bytes);
        if (!handled) running_.store(false);
        return true;
#else
        if (s_downlink_bytes.load() + bytes > kDownlinkBytes || !uxQueueSpacesAvailable(rx_queue_)) return false;
        auto* copy = static_cast<uint8_t*>(heap_caps_malloc(bytes ? bytes : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!copy) copy = static_cast<uint8_t*>(malloc(bytes ? bytes : 1));
        if (!copy) { running_.store(false); return false; }
        if (bytes) memcpy(copy, data, bytes);
        Rx packet{opcode, bytes, copy, received_at, reset};
        s_downlink_bytes.fetch_add(bytes);
        if (xQueueSend(rx_queue_, &packet, 0) == pdTRUE) return true;
        s_downlink_bytes.fetch_sub(bytes);
        free(copy);
        return false;
#endif
    }

    bool OpenDecoder() {
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (audio_.output_opus && !decoder_) {
            if (!decode_pcm_) {
                decode_pcm_ = static_cast<int16_t*>(heap_caps_malloc(kDecodePcmBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!decode_pcm_) decode_pcm_ = static_cast<int16_t*>(malloc(kDecodePcmBytes));
                if (!decode_pcm_) return false;
            }
            esp_opus_dec_cfg_t config = {};
            config.sample_rate = 16000;
            config.channel = 1;
            config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
            config.self_delimited = false;
            if (esp_opus_dec_open(&config, sizeof config, &decoder_) != ESP_AUDIO_ERR_OK) {
#if CONFIG_IDF_TARGET_ESP32C3
                if (codec_memory_.Reserved() && !decoder_ && !encoder_) codec_memory_.Rollback();
#endif
                ESP_LOGE(TAG, "Opus decoder could not be opened (internal free=%u largest=%u)",
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
                return false;
            }
        }
#else
        if (audio_.output_opus) return false;
#endif
        return true;
    }

    void ResetDecoder() {
        converter_.Reset();
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        if (decoder_) {
            esp_opus_dec_close(decoder_);
            decoder_ = nullptr;
#if CONFIG_IDF_TARGET_ESP32C3
            // Recreate immediately at a reply boundary, before receiving more
            // messages can consume/fragment the state block just released.
            if ((!SharedCodec() || replying_) && Healthy() && Current(epoch_) && !OpenDecoder()) running_.store(false);
#endif
        }
#endif
    }

    void ResetAudio() {
        if (SharedCodec()) {
            if (!EndReply()) running_.store(false);
        } else ResetDecoder();
        play_due_ = 0;
        reset_ = s_reset.load();
        if (s_cb.audio_reset) s_cb.audio_reset();
    }

    void Pcm(const uint8_t* data, size_t bytes) {
        if (!s_ready || s_mute) return;
        while (bytes && Healthy() && Current(epoch_) && !s_mute && reset_ == s_reset) {
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
        auto room = [&]() {
            size_t queued = 0;
            if (agentlink::audio::BufferLevelBytes(&queued))
                return agentlink::realtime::PlaybackRoom(queued, samples * 2, agentlink::audio::BufferMs());
            return play_due_ + duration - esp_timer_get_time() <= buffer;
        };
        if (samples * 2 > uint64_t(agentlink::audio::BufferMs()) * 32) {
            ESP_LOGE(TAG, "decoded frame exceeds playback capacity");
            running_.store(false); return;
        }
        while (!room()) {
            if (!Healthy() || !Current(epoch_) || s_mute || reset_ != s_reset) return;
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            if (!PumpEncoder()) { running_.store(false); return; }
#endif
#if CONFIG_IDF_TARGET_ESP32C3
            if (sender_ && (!PumpWire(*sender_) || !FlushTx(*sender_, scratch_))) {
                transport_failed_ = true; running_.store(false); return;
            }
#endif
            vTaskDelay(1);
        }
        if (!Healthy() || !Current(epoch_) || s_mute || reset_ != s_reset) return;
        const int64_t before_sink = esp_timer_get_time();
        if (before_sink - entered > max_pacing_) max_pacing_ = before_sink - entered;
        if (s_cb.pcm) s_cb.pcm(reinterpret_cast<const uint8_t*>(pcm), samples * 2);
        const int64_t now = esp_timer_get_time();
        if (now - before_sink > max_sink_) max_sink_ = now - before_sink;
        if (play_due_ < now) play_due_ = now;
        play_due_ += duration;
        samples_ += samples;
        s_decoded_samples.fetch_add(samples);
    }

    bool Audio(uint8_t* data, size_t bytes) {
        if (!s_ready || s_mute || !bytes) return true;
        const int64_t now = received_at_;
        if (last_audio_ && now - last_audio_ > max_gap_) max_gap_ = now - last_audio_;
        last_audio_ = now;
        encoded_bytes_ += bytes;
        ++packets_;
        if (!audio_.output_opus) {
            if (bytes % 2) return false;
            Pcm(data, bytes);
            return true;
        }
#if CONFIG_AGENT_LINK_REALTIME_OPUS
        const int64_t decode_started = esp_timer_get_time();
        if (SharedCodec() && !replying_ && !BeginReply()) return false;
        if (!OpenDecoder()) return false;
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = data;
        raw.len = static_cast<uint32_t>(bytes);
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = reinterpret_cast<uint8_t*>(decode_pcm_);
        frame.len = kDecodePcmBytes;
        esp_audio_dec_info_t info = {};
        const esp_audio_err_t result = esp_opus_dec_decode(decoder_, &raw, &frame, &info);
        const int64_t decoded_in = esp_timer_get_time() - decode_started;
        if (decoded_in > max_decode_) max_decode_ = decoded_in;
        if (result != ESP_AUDIO_ERR_OK || raw.consumed != bytes || frame.decoded_size % 2 ||
            frame.decoded_size > frame.len || info.sample_rate != 16000 || info.channel != 1) {
            ESP_LOGW(TAG, "invalid Opus packet (%d, %u bytes)", result, static_cast<unsigned>(bytes));
            return false;
        }
        Output(decode_pcm_, frame.decoded_size / 2);
        return true;
#else
        return false;
#endif
    }

    bool Text(const uint8_t* data, size_t bytes) {
#if CONFIG_IDF_TARGET_ESP32C3
        // The C3 delivers synchronously from its writable message buffer. Keep
        // Base64 there instead of making a second payload-sized cJSON string.
        uint8_t* borrowed_audio = nullptr;
        size_t audio_bytes = 0;
        if (agentlink::realtime::BorrowAudioDelta(const_cast<uint8_t*>(data), bytes, &borrowed_audio, &audio_bytes)) {
            if (s_mute || !s_ready) return true;
            size_t decoded = 0;
            return mbedtls_base64_decode(borrowed_audio, audio_bytes, &decoded, borrowed_audio, audio_bytes) == 0 &&
                   Audio(borrowed_audio, decoded);
        }
#endif
        cJSON* json = cJSON_ParseWithLength(reinterpret_cast<const char*>(data), bytes);
        if (!json) return false;
        const cJSON* type = cJSON_GetObjectItemCaseSensitive(json, "type");
        const char* event = cJSON_IsString(type) ? type->valuestring : "";
        bool keep = true;
        if (strcmp(event, "session.ready") == 0) {
            AudioFormat format = audio_;
            if (agentlink::realtime::AudioSettings(json)) keep = ReadAudioFormat(json, &format, false);
            if (s_ready && (format.input_opus != audio_.input_opus || format.output_opus != audio_.output_opus))
                keep = false;
#if !CONFIG_AGENT_LINK_REALTIME_OPUS
            if (format.input_opus || format.output_opus) keep = false;
#endif
            if (!keep) { cJSON_Delete(json); return false; }
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            // The first ready event may refine either direction. Release the
            // prepared state before switching allocation/half-duplex policy.
            if (format.input_opus != audio_.input_opus || format.output_opus != audio_.output_opus) {
                if (decoder_) { esp_opus_dec_close(decoder_); decoder_ = nullptr; }
                if (encoder_) { esp_opus_enc_close(encoder_); encoder_ = nullptr; }
                replying_ = false;
                ClearCodecUplink();
            }
#endif
            audio_ = format;
            input_opus_.store(audio_.input_opus);
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            // A first ready event may refine a PCM-input grant to Opus. Free
            // the decoder from preparation before entering encoder-only mode.
            if (SharedCodec() && !replying_ && decoder_) {
                esp_opus_dec_close(decoder_);
                decoder_ = nullptr;
            }
#endif
            if (!SharedCodec() && !OpenDecoder()) { cJSON_Delete(json); return false; }
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            // Codec allocation must succeed before enabling capture/UI readiness.
            if (audio_.input_opus && !StartEncoder()) { cJSON_Delete(json); return false; }
#endif
            s_mute.store(false);
            if (!s_ready.exchange(true) && s_cb.state)
                s_cb.state(true, continuous_ ? "Microphone on" : "Hold to talk, release to send");
            ESP_LOGI(TAG, "voice session ready (%s 16 kHz up, %s 24 kHz down, %s)",
                     audio_.input_opus ? "Opus" : "PCM16", audio_.output_opus ? "Opus" : "PCM16",
                     continuous_ ? "server endpointing" : "client PTT");
#if CONFIG_AGENT_LINK_REALTIME_OPUS
            if (continuous_ && (!audio_.input_opus || !audio_.output_opus))
                ESP_LOGW(TAG, "server negotiated PCM16 despite the Opus request");
#endif
        } else if (strcmp(event, "response.audio.delta") == 0) {
            const cJSON* audio = cJSON_GetObjectItemCaseSensitive(json, "audio");
            if (cJSON_IsString(audio) && !s_mute && s_ready) {
                const size_t len = strlen(audio->valuestring);
                // mbedTLS validates first, then decodes forwards (4 source
                // bytes -> 3 destination bytes). Its own string is writable;
                // decoding in place avoids a third copy of every audio delta.
                auto* decoded = reinterpret_cast<uint8_t*>(audio->valuestring);
                size_t got = 0;
                if (mbedtls_base64_decode(decoded, len, &got, decoded, len) != 0) {
                    keep = false;
                } else {
                    keep = Audio(decoded, got);
                }
            }
        } else if (strcmp(event, "response.audio.done") == 0) {
            ESP_LOGI(TAG, "reply audio end #%u (started=%u, interrupted=%u)",
                     ++reply_ends_, replies_, interruptions_);
            ESP_LOGI(TAG, "reply audio: %s, %u packets, %u bytes, %u ms PCM, max gap %u ms",
                     audio_.output_opus ? "Opus" : "PCM16", packets_, static_cast<unsigned>(encoded_bytes_),
                     static_cast<unsigned>(samples_ / 16), static_cast<unsigned>(max_gap_ / 1000));
            ESP_LOGI(TAG, "reply processing: decode max %u us, sink max %u us, pacing max %u ms",
                     static_cast<unsigned>(max_decode_), static_cast<unsigned>(max_sink_),
                     static_cast<unsigned>(max_pacing_ / 1000));
            if (SharedCodec()) keep = EndReply();
            else ResetDecoder();
            if (s_cb.audio_end) s_cb.audio_end();
            if (s_cb.state && s_ready)
                s_cb.state(true, continuous_ ? "Microphone on" : "Hold to talk, release to send");
        } else if (strcmp(event, "response.started") == 0) {
            ESP_LOGI(TAG, "reply started #%u", ++replies_);
            if (SharedCodec()) keep = BeginReply();
            else ResetDecoder();
            samples_ = encoded_bytes_ = packets_ = 0;
            last_audio_ = max_gap_ = 0;
            max_decode_ = max_sink_ = max_pacing_ = 0;
            if (s_cb.state && s_ready && !s_mute) s_cb.state(true, "Receiving agent reply");
        } else if (strcmp(event, "response.interrupted") == 0) {
            ESP_LOGI(TAG, "reply interrupted #%u (started=%u, ended=%u)", ++interruptions_, replies_, reply_ends_);
            if (reset_ != s_reset.load()) ResetAudio();
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

    bool FlushTx(WsSender& ws, TxScratch* scratch) {
        if (!s_ready) return true;
#if CONFIG_IDF_TARGET_ESP32C3
        if (ws.pending.Busy()) return true;
#endif
#if CONFIG_AGENT_LINK_REALTIME_OPUS && !CONFIG_IDF_TARGET_ESP32C3
        if (input_opus_.load() && !StartEncoder()) return false;
#endif
        if (!TxPending()) return true;
        // Do not wait three seconds in poll_write while reply audio is waiting
        // to be read on this same worker. Keep the packets queued until writable.
        errno = 0;
        const int writable = PollWritable(ws.tls);
        const int saved_errno = errno;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        write_stall_.Observe(writable > 0, now_ms);
        if (writable < 0 || write_stall_.Expired(now_ms)) {
            LogTransportFailure(ws.tls, "uplink wait writable", writable, saved_errno);
            transport_failed_ = true;
            return false;
        }
        if (!writable) return true;
        if (input_opus_.load()) {
#if CONFIG_AGENT_LINK_REALTIME_OPUS
            if (xQueuePeek(encoded_queue_, &encoded_packet_, 0) != pdTRUE) return true;
            if (encoded_packet_.kind == TxKind::kPcm) {
                size_t wire_bytes = 0, json_bytes = 0;
                unsigned frames = 0;
                const unsigned limit = continuous_ ? kOpusBatchFrames : 1;
                while (frames < limit && Current(epoch_) &&
                       xQueuePeek(encoded_queue_, &encoded_packet_, 0) == pdTRUE &&
                       encoded_packet_.kind == TxKind::kPcm) {
                    const size_t json_size = AudioJson(scratch, encoded_packet_.data, encoded_packet_.bytes);
                    if (!json_size) return false;
                    const size_t added = AppendFrame(ws, wire_bytes, scratch->json, json_size);
                    if (!added) {
                        if (!frames) return false;
                        break;  // Leave the next complete packet queued.
                    }
                    if (xQueueReceive(encoded_queue_, &encoded_packet_, 0) != pdTRUE) return false;
                    s_encoded_pending.fetch_sub(1);
                    wire_bytes += added;
                    json_bytes += json_size;
                    ++frames;
                }
                batch_gate_.Reset();
                return !Current(epoch_) || WriteAudio(ws, wire_bytes, json_bytes, frames, frames);
            }
            if (xQueueReceive(encoded_queue_, &encoded_packet_, 0) != pdTRUE) return true;
            s_encoded_pending.fetch_sub(1);
            if (!Send(ws, encoded_packet_.kind == TxKind::kStart
                         ? "{\"type\":\"input_audio.activity\",\"kind\":\"start\"}"
                         : "{\"type\":\"input_audio.activity\",\"kind\":\"end\"}")) {
                transport_failed_ = true;
                return false;
            }
            if (encoded_packet_.kind == TxKind::kEnd) {
                if (encoded_packet_.turn == s_reset.load()) s_mute.store(false);
                if (s_cb.state) s_cb.state(true, "Waiting for the agent");
            }
            return true;
#else
            return false;
#endif
        }
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
                if (!SendAudio(ws, scratch, scratch->pcm, pcm_bytes, pcm_packets)) return false;
            } else {
                if (!Send(ws, packet.kind == TxKind::kStart
                        ? "{\"type\":\"input_audio.activity\",\"kind\":\"start\"}"
                        : "{\"type\":\"input_audio.activity\",\"kind\":\"end\"}")) {
                    transport_failed_ = true;
                    return false;
                }
                if (packet.kind == TxKind::kEnd) {
                    if (packet.turn == s_reset.load()) s_mute.store(false);
                    if (s_cb.state) s_cb.state(true, "Waiting for the agent");
                }
            }
        }
        return true;
    }

private:
    bool transport_failed_ = false;  // network worker only
#if CONFIG_IDF_TARGET_ESP32C3
    WsSender* sender_ = nullptr;
    TxScratch* scratch_ = nullptr;
#endif
    agentlink::realtime::OpusBatchGate batch_gate_{kOpusBatchFrames};
    agentlink::realtime::WriteStall write_stall_;
    static void MediaEntry(void* context) { static_cast<Call*>(context)->MediaLoop(); }

    void MediaLoop() {
        Rx packet;
        while (Healthy() && Current(epoch_)) {
            if (reset_ != s_reset) ResetAudio();
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            if (!PumpEncoder()) { running_.store(false); break; }
#endif
            TickType_t wait = pdMS_TO_TICKS(10);
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
            if (s_ready && input_opus_.load() && uxQueueMessagesWaiting(s_tx) &&
                encoded_queue_ && uxQueueSpacesAvailable(encoded_queue_) >= 2) wait = 0;
#endif
            if (xQueueReceive(rx_queue_, &packet, wait) != pdTRUE) continue;
            received_at_ = packet.received_at;
            bool handled = true;
            if (packet.reset == s_reset.load()) {
                handled = packet.opcode == WS_TRANSPORT_OPCODES_BINARY
                        ? Audio(packet.data, packet.bytes) : Text(packet.data, packet.bytes);
            }
            free(packet.data);
            s_downlink_bytes.fetch_sub(packet.bytes);
            s_media_stack.store(uxTaskGetStackHighWaterMark(nullptr));
            if (!handled) running_.store(false);
        }
        media_done_.store(true);
        vTaskDelete(nullptr);
    }

    size_t AudioJson(TxScratch* scratch, const uint8_t* data, size_t bytes) {
        memcpy(scratch->json, kAudioPrefix, sizeof kAudioPrefix - 1);
        size_t encoded = 0;
        if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(scratch->json + sizeof kAudioPrefix - 1),
                sizeof scratch->json - sizeof kAudioPrefix - 1, &encoded, data, bytes) != 0) return 0;
        const size_t n = sizeof kAudioPrefix - 1 + encoded;
        scratch->json[n] = '"';
        scratch->json[n + 1] = '}';
        return n + 2;
    }

    bool SendAudio(WsSender& ws, TxScratch* scratch, const uint8_t* data,
                   size_t bytes, unsigned pcm_packets) {
        const size_t json_size = AudioJson(scratch, data, bytes);
        if (!json_size) return false;
        const size_t wire_size = AppendFrame(ws, 0, scratch->json, json_size);
        return wire_size && WriteAudio(ws, wire_size, json_size, pcm_packets, 1);
    }

    bool WriteAudio(WsSender& ws, size_t wire_bytes, size_t json_bytes,
                    unsigned pcm_packets, unsigned messages) {
#if CONFIG_IDF_TARGET_ESP32C3
        if (!WriteWire(ws, wire_bytes)) { transport_failed_ = true; return false; }
        ws.audio_frames = pcm_packets; ws.audio_messages = messages; ws.audio_bytes = json_bytes;
        return true;
#else
        SetWorkerPhase(1);
        const int64_t started = esp_timer_get_time();
        const bool sent = WriteWire(ws, wire_bytes);
        const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
        s_tx_send_ms.fetch_add(elapsed_ms);
        s_tx_write_calls.fetch_add(1);
        if (elapsed_ms > s_tx_send_max_ms.load()) s_tx_send_max_ms.store(elapsed_ms);
        SetWorkerPhase(0);
        if (!sent) {
            transport_failed_ = true;
            ESP_LOGW(TAG, "audio WSS send failed after %u ms (queue=%ums)",
                     static_cast<unsigned>(elapsed_ms),
                     static_cast<unsigned>((uxQueueMessagesWaiting(s_tx) + s_encoded_pending.load()) * 20));
            return false;
        }
        s_tx_sent_frames.fetch_add(pcm_packets);
        s_tx_sent_messages.fetch_add(messages);
        s_tx_bytes.fetch_add(json_bytes);
        return true;
#endif
    }

#if CONFIG_AGENT_LINK_REALTIME_OPUS
    bool StartEncoder() {
#if CONFIG_IDF_TARGET_ESP32C3
        if (encoder_) return !SharedCodec() || CacheSilence();
#else
        if (encoder_started_) return true;
#endif
        if (!encoded_queue_) {
            encoded_queue_ = xQueueCreateWithCaps(kEncodedDepth, sizeof(EncodedTx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            encoded_queue_caps_ = encoded_queue_ != nullptr;
            if (!encoded_queue_) encoded_queue_ = xQueueCreate(kEncodedDepth, sizeof(EncodedTx));
        }
        if (!encoded_queue_) return false;
#if CONFIG_IDF_TARGET_ESP32C3
        if (SharedCodec() && replying_) return silence_ != nullptr;
        const bool opened = OpenEncoder() && (!SharedCodec() || CacheSilence());
        ESP_LOGI(TAG, "shared Opus worker: codec=%s queue=%u; internal free=%u largest=%u",
                 opened ? "ready" : "failed", static_cast<unsigned>(kEncodedDepth),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return opened;
#else
        encoder_caps_ = true;
        encoder_started_ = xTaskCreateWithCaps(&EncoderEntry, "agentlink_encode", 40960, this, 5,
                                               nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        if (!encoder_started_) {
            encoder_caps_ = false;
            encoder_started_ = xTaskCreate(&EncoderEntry, "agentlink_encode", 40960, this, 5, nullptr) == pdPASS;
        }
        if (!encoder_started_) ESP_LOGE(TAG, "could not start microphone Opus worker");
        else ESP_LOGI(TAG, "Opus encoder stack=%s; internal free=%u largest=%u",
                      encoder_caps_ ? "PSRAM" : "internal",
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        return encoder_started_;
#endif
    }

    static void EncoderEntry(void* context) { static_cast<Call*>(context)->EncoderLoop(); }

    bool QueueEncoded(const EncodedTx& packet) {
        while (Healthy() && Current(epoch_)) {
            s_encoded_pending.fetch_add(1);
#if CONFIG_IDF_TARGET_ESP32C3
            // PumpEncoder reserves enough slots for the frame and end marker.
            if (xQueueSend(encoded_queue_, &packet, 0) == pdTRUE) return true;
            s_encoded_pending.fetch_sub(1);
            return false;
#else
            if (xQueueSend(encoded_queue_, &packet, pdMS_TO_TICKS(10)) == pdTRUE) return true;
            s_encoded_pending.fetch_sub(1);
#endif
        }
        return false;
    }

    bool Encode(const uint8_t* pcm, EncodedTx* packet) {
        if (SharedCodec() && replying_) {
            // Preserve endpointing cadence while the application is half-duplex.
            // Reusing a valid encoded silence packet needs no live encoder state.
            if (!silence_ || !silence_bytes_) return false;
            memcpy(packet->data, silence_, silence_bytes_);
            packet->kind = TxKind::kPcm;
            packet->bytes = silence_bytes_;
            return QueueEncoded(*packet);
        }
        esp_audio_enc_in_frame_t input{const_cast<uint8_t*>(pcm), kMicBytes};
        esp_audio_enc_out_frame_t output{};
        output.buffer = packet->data;
        output.len = sizeof packet->data;
        const int64_t started = esp_timer_get_time();
        const esp_audio_err_t result = esp_opus_enc_process(encoder_, &input, &output);
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - started);
        if (elapsed > s_tx_encode_max_us.load()) s_tx_encode_max_us.store(elapsed);
        if (result != ESP_AUDIO_ERR_OK || !output.encoded_bytes || output.encoded_bytes > output.len) {
            ESP_LOGW(TAG, "microphone Opus encoding failed (%d)", result);
            return false;
        }
        packet->kind = TxKind::kPcm;
        packet->bytes = static_cast<uint16_t>(output.encoded_bytes);
        return QueueEncoded(*packet);
    }

    bool OpenEncoder() {
#if CONFIG_IDF_TARGET_ESP32C3
        if (SharedCodec() && !codec_memory_.Reserve()) return false;
#endif
        esp_opus_enc_config_t config = ESP_OPUS_ENC_CONFIG_DEFAULT();
        config.sample_rate = 16000;
        config.channel = 1;
        config.bitrate = 24000;
        config.enable_vbr = true;
        config.complexity = 0;
        int input_size = 0, output_size = 0;
        const bool valid = esp_opus_enc_open(&config, sizeof config, &encoder_) == ESP_AUDIO_ERR_OK &&
                     esp_opus_enc_get_frame_size(encoder_, &input_size, &output_size) == ESP_AUDIO_ERR_OK &&
                     input_size == kMicBytes && output_size <= sizeof(EncodedTx::data);
        if (!valid) ESP_LOGE(TAG, "Opus encoder initialization failed (input=%d output=%d)", input_size, output_size);
#if CONFIG_IDF_TARGET_ESP32C3
        if (!valid && codec_memory_.Reserved() && !encoder_ && !decoder_) codec_memory_.Rollback();
#endif
        return valid;
    }

    bool CacheSilence() {
#if CONFIG_IDF_TARGET_ESP32C3
        if (silence_) return true;
        uint8_t pcm[kMicBytes]{};
        esp_audio_enc_in_frame_t input{pcm, sizeof pcm};
        esp_audio_enc_out_frame_t output{};
        output.buffer = encoded_packet_.data;
        output.len = sizeof encoded_packet_.data;
        if (esp_opus_enc_process(encoder_, &input, &output) != ESP_AUDIO_ERR_OK ||
            !output.encoded_bytes || output.encoded_bytes > output.len) return false;
        silence_ = static_cast<uint8_t*>(malloc(output.encoded_bytes));
        if (!silence_) return false;
        silence_bytes_ = static_cast<uint16_t>(output.encoded_bytes);
        memcpy(silence_, output.buffer, silence_bytes_);
#endif
        return true;
    }

#if CONFIG_IDF_TARGET_ESP32C3
    bool PumpEncoder() {
        if (!s_ready || !input_opus_.load()) return true;
        // An end marker can flush a padded frame plus a control packet. Never
        // wait for network space here: decoding must continue during TX stalls.
        if (!encoded_queue_) return false;
        Tx input;
        if (xQueuePeek(s_tx, &input, 0) != pdTRUE) return true;
        const unsigned needed = input.kind == TxKind::kEnd ? 2 : 1;
        if (uxQueueSpacesAvailable(encoded_queue_) < needed) return true;
        if (xQueueReceive(s_tx, &input, 0) != pdTRUE) return true;
        EncodedTx output{};
        const bool valid = EncodeInput(input, shared_frames_, output);
        s_encoder_stack.store(uxTaskGetStackHighWaterMark(nullptr));
        return valid;
    }
    agentlink::realtime::PcmFrames<kMicBytes> shared_frames_;
#endif

    bool EncodeInput(const Tx& input, agentlink::realtime::PcmFrames<kMicBytes>& frames, EncodedTx& output) {
        output.turn = input.turn;
        if (input.kind == TxKind::kPcm)
            return frames.Append(input.data, input.bytes, [&](const uint8_t* pcm) { return Encode(pcm, &output); });
        bool valid = true;
        if (input.kind == TxKind::kEnd)
            valid = frames.Flush([&](const uint8_t* pcm) { return Encode(pcm, &output); });
        frames.Reset();
        output.kind = input.kind;
        output.bytes = 0;
        return valid && QueueEncoded(output);
    }

    void EncoderLoop() {
        bool valid = OpenEncoder();
        agentlink::realtime::PcmFrames<kMicBytes> frames;
        Tx input;
        EncodedTx output{};
        while (valid && Healthy() && Current(epoch_)) {
            if (xQueueReceive(s_tx, &input, pdMS_TO_TICKS(10)) != pdTRUE) continue;
            valid = EncodeInput(input, frames, output);
            s_encoder_stack.store(uxTaskGetStackHighWaterMark(nullptr));
        }
        if (!valid && Healthy() && Current(epoch_)) {
            ESP_LOGW(TAG, "microphone Opus worker stopped after a codec error");
            running_.store(false);
        }
        const bool caps = encoder_caps_;
        encoder_done_.store(true);
        if (caps) vTaskDeleteWithCaps(nullptr);
        else vTaskDelete(nullptr);
    }
#endif

    uint32_t epoch_;
    bool continuous_ = false;
    bool replying_ = false;
    uint8_t* silence_ = nullptr;
    uint16_t silence_bytes_ = 0;
    unsigned replies_ = 0, reply_ends_ = 0, interruptions_ = 0;
    uint32_t reset_ = s_reset.load();
    int64_t play_due_ = 0;
    AudioFormat audio_;
    std::atomic<bool> input_opus_{false};
    QueueHandle_t encoded_queue_ = nullptr;
    bool encoded_queue_caps_ = false;
    EncodedTx encoded_packet_{};
    bool encoder_started_ = false, encoder_caps_ = false;
    std::atomic<bool> encoder_done_{false};
    QueueHandle_t rx_queue_ = nullptr;
    bool rx_caps_ = false, media_started_ = false;
    std::atomic<bool> running_{true}, media_done_{false};
    int16_t* decode_pcm_ = nullptr;
    int64_t received_at_ = 0;
    size_t samples_ = 0, encoded_bytes_ = 0;
    unsigned packets_ = 0;
    int64_t last_audio_ = 0, max_gap_ = 0;
    int64_t max_decode_ = 0, max_sink_ = 0, max_pacing_ = 0;
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    void* decoder_ = nullptr;
    void* encoder_ = nullptr;
#endif
    agentlink::realtime::Pcm24To16 converter_;
#if CONFIG_AGENT_LINK_REALTIME_OPUS && CONFIG_IDF_TARGET_ESP32C3
    // Destroyed after the destructor body has closed both vendor handles.
    agentlink::realtime::CodecWorkspace codec_memory_;
#endif
};

#if CONFIG_IDF_TARGET_ESP32C3
class NativeWss {
public:
    NativeWss() : tls(esp_tls_init()) {}
    ~NativeWss() { if (tls) esp_tls_conn_destroy(tls); }
    bool Connect(const WssAddress& addr, uint32_t epoch, TxScratch& scratch,
                 agentlink::realtime::MessageBuffer& storage, size_t* trailing, int* status) {
        *trailing = 0; *status = 0;
        if (!tls) return false;
        esp_tls_cfg_t config{};
        config.non_block = true;
        config.timeout_ms = kConnectTimeoutMs;
        config.crt_bundle_attach = esp_crt_bundle_attach;
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
        // Reserve the peer's full record after the handshake, before Opus.
        // Dynamic TX remains small; RX is reused for the life of this call.
        config.esp_tls_dyn_buf_strategy = ESP_TLS_DYN_BUF_RX_STATIC;
#endif
        const int64_t deadline = esp_timer_get_time() + kConnectTimeoutMs * 1000LL;
        int result = 0;
        while (Current(epoch) && esp_timer_get_time() < deadline) {
            result = esp_tls_conn_new_async(addr.host, strlen(addr.host), addr.port, &config, tls);
            if (result != 0) break;
            vTaskDelay(1);
        }
        if (result != 1 || !Current(epoch)) return false;
        int socket = -1;
        const int enabled = 1;
        if (esp_tls_get_conn_sockfd(tls, &socket) != ESP_OK || socket < 0) return false;
        (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);

        uint8_t nonce[16], digest[20];
        char key[25]{}, challenge[80]{}, accept[29]{};
        size_t encoded = 0;
        esp_fill_random(nonce, sizeof nonce);
        if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(key), sizeof key, &encoded,
                                  nonce, sizeof nonce) != 0) return false;
        const int challenge_size = snprintf(challenge, sizeof challenge, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        if (challenge_size <= 0 || challenge_size >= static_cast<int>(sizeof challenge)) return false;
#if MBEDTLS_VERSION_MAJOR >= 4
        size_t hashed = 0;
        const bool hash_ok = psa_hash_compute(PSA_ALG_SHA_1,
            reinterpret_cast<const uint8_t*>(challenge), challenge_size, digest, sizeof digest, &hashed) == PSA_SUCCESS && hashed == sizeof digest;
#else
        const bool hash_ok = mbedtls_sha1(reinterpret_cast<const uint8_t*>(challenge), challenge_size, digest) == 0;
#endif
        if (!hash_ok ||
            mbedtls_base64_encode(reinterpret_cast<uint8_t*>(accept), sizeof accept, &encoded,
                                  digest, sizeof digest) != 0) return false;
        const int request_size = snprintf(scratch.json, sizeof scratch.json,
            "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Protocol: tidb-link.realtime.v1\r\n\r\n", addr.path, addr.host, addr.port, key);
        if (request_size <= 0 || request_size >= static_cast<int>(sizeof scratch.json)) return false;
        size_t sent = 0, received = 0;
        auto* header = reinterpret_cast<char*>(storage.Data());
        while (Current(epoch) && esp_timer_get_time() < deadline) {
            if (sent < static_cast<size_t>(request_size)) {
                result = esp_tls_conn_write(tls, scratch.json + sent, request_size - sent);
                if (result > 0) sent += result;
                else if (!TlsWouldBlock(result)) return false;
            } else {
                if (received >= storage.Capacity() - 1) return false;
                result = esp_tls_conn_read(tls, header + received, storage.Capacity() - received - 1);
                if (result > 0) {
                    received += result; header[received] = '\0';
                    const char* delimiter = strstr(header, "\r\n\r\n");
                    if (delimiter) {
                        const size_t header_size = delimiter + 4 - header;
                        const char saved = header[header_size];
                        header[header_size] = '\0';
                        const bool valid = agentlink::realtime::ValidUpgrade(header, accept, status);
                        header[header_size] = saved;
                        if (!valid || received - header_size > sizeof scratch.read) return false;
                        *trailing = received - header_size;
                        memcpy(scratch.read, header + header_size, *trailing);
                        return true;
                    }
                } else if (!TlsWouldBlock(result)) return false;
            }
            vTaskDelay(1);
        }
        return false;
    }
    esp_tls_t* tls;
};

// One resident C3 worker owns TLS, framing and codecs. I2S capture/playback stay
// on the board's independent workers. No 40 KB task is allocated during a call.
bool ConnectOnce(const Grant& grant) {
    WssAddress addr{};
    if (!ParseWss(grant.url, &addr)) return false;
    agentlink::cloud::NetworkMemoryLock memory;
    if (!Current(grant.epoch)) return false;
    VoiceWifiPower power;
    agentlink::realtime::MessageBuffer message(kMessageInitialBytes, kMessageBytes, ResizeMessage);
    std::unique_ptr<TxScratch> scratch(new (std::nothrow) TxScratch{});
    // Keep metadata off the shared stack: the codec needs most of its depth.
    std::unique_ptr<Call> owner(new (std::nothrow) Call(grant.epoch, grant.audio, grant.platform));
    if (!message.Data() || !scratch || !owner) return false;
    NativeWss connection;
    size_t trailing = 0;
    int status = 0;
    if (s_cb.state) s_cb.state(false, "Connecting voice");
    if (!connection.Connect(addr, grant.epoch, *scratch, message, &trailing, &status)) {
        LogTransportFailure(connection.tls, "connect/upgrade", -1, errno);
        ESP_LOGW(TAG, "WSS connection failed (upgrade HTTP %d)", status);
        return status <= 0 || status >= 500;
    }
    // Keep a full message alongside the fixed TLS record. Growing/shrinking
    // this buffer during playback otherwise introduces another large realloc.
    if (!message.Reserve(kMessageBytes)) {
        ESP_LOGW(TAG, "could not reserve %u-byte voice message", static_cast<unsigned>(kMessageBytes));
        return false;
    }
    ESP_LOGI(TAG, "WSS connected on resident stack: internal free=%u largest=%u stack_free=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    ESP_LOGI(TAG, "voice receive buffers: TLS RX=fixed, message=%uB retained", static_cast<unsigned>(message.Capacity()));
    Call& call = *owner;
    if (!Current(grant.epoch) || !call.Prepare() || !call.Start()) return false;
    WsSender sender{connection.tls, scratch->wire, sizeof scratch->wire};
    call.BindNetwork(&sender, scratch.get());
    char hello[sizeof Grant::token * 2 + 128];
    size_t n = snprintf(hello, sizeof hello, "{\"type\":\"session.connect\",\"connect_token\":\"");
    for (const char* p = grant.token; *p && n + 4 < sizeof hello; ++p) {
        if (*p == '"' || *p == '\\') hello[n++] = '\\';
        hello[n++] = *p;
    }
    n += snprintf(hello + n, sizeof hello - n, "\",\"endpointing\":{\"mode\":\"%s\"}}",
                  grant.platform ? "server" : "client");
    const bool queued = Send(sender, hello, n);
    memset(hello, 0, sizeof hello);
    if (!queued) return true;

    agentlink::realtime::FrameReader reader(kMessageBytes);
    agentlink::realtime::Message assembler(message.Data(), message.Capacity());
    uint8_t control[125]{}, pong[125]{};
    size_t control_size = 0, pong_size = 0;
    bool pong_pending = false, retryable = false;
    const int64_t connected_at = esp_timer_get_time();
    int64_t last_rx = connected_at, last_ping = connected_at, last_yield = connected_at;
    auto receive = [&](const uint8_t* data, size_t bytes) {
        return reader.Consume(data, bytes, [&](unsigned opcode, bool first, bool end, bool fin,
                                              const uint8_t* payload, size_t count) {
            if (opcode >= 8) {
                if (first) control_size = 0;
                if (count > sizeof control - control_size) return false;
                memcpy(control + control_size, payload, count); control_size += count;
                if (!end) return true;
                if (opcode == 8) {
                    const unsigned code = control_size >= 2 ? (unsigned(control[0]) << 8) | control[1] : 0;
                    ESP_LOGW(TAG, "voice peer closed (code=%u); automatic retry disabled", code);
                    return false;
                }
                if (opcode == 9) {
                    memcpy(pong, control, control_size); pong_size = control_size; pong_pending = true;
                }
                return true;
            }
            const size_t buffered = assembler.Active() ? assembler.Size() : 0;
            if (!message.Reserve(buffered + count)) return false;
            assembler.Rebind(message.Data(), message.Capacity());
            const auto result = assembler.Append(opcode, first, end, fin, payload, count);
            if (result == agentlink::realtime::Message::kError) return false;
            if (result == agentlink::realtime::Message::kComplete) {
                if (assembler.Opcode() == 1 && IsInterruption(message.Data(), assembler.Size())) s_reset.fetch_add(1);
                call.Deliver(assembler.Opcode(), message.Data(), assembler.Size(), last_rx, s_reset.load());
                // Retain the reservation through every reply; no realloc here.
            }
            return call.Healthy() && Current(grant.epoch);
        });
    };
    // Preserve bytes received with the HTTP upgrade (including an early ping).
    bool valid = !trailing || receive(scratch->read, trailing);
    while (valid && Current(grant.epoch) && call.Healthy()) {
        if (!call.Pump() || !PumpWire(sender)) { retryable = call.Healthy() || call.TransportFailed(); break; }
        if (!sender.pending.Busy() && pong_pending) {
            uint8_t mask[4]; esp_fill_random(mask, sizeof mask);
            const size_t size = agentlink::realtime::EncodeFrame(10, reinterpret_cast<char*>(pong), pong_size,
                                                                mask, sender.wire, sender.capacity);
            if (!size || !WriteWire(sender, size)) { retryable = true; break; }
            pong_pending = false;
        }
        if (!call.FlushTx(sender, scratch.get())) { retryable = call.TransportFailed(); break; }
        const int64_t now = esp_timer_get_time();
        if ((!s_ready && now - connected_at > kReadyTimeoutUs) || now - last_rx > 45000000) {
            ESP_LOGW(TAG, "voice session timed out"); retryable = true; break;
        }
        if (!sender.pending.Busy() && now - last_ping > 15000000) {
            uint8_t mask[4]; esp_fill_random(mask, sizeof mask);
            const size_t size = agentlink::realtime::EncodeFrame(9, nullptr, 0, mask, sender.wire, sender.capacity);
            if (!size || !WriteWire(sender, size)) { retryable = true; break; }
            last_ping = now;
        }
        SetWorkerPhase(3);
        errno = 0;
        const int got = esp_tls_conn_read(connection.tls, scratch->read, sizeof scratch->read);
        SetWorkerPhase(0);
        if (got > 0) {
            s_rx_bytes.fetch_add(got); last_rx = esp_timer_get_time();
            valid = receive(scratch->read, got);
            if (!valid) retryable = call.TransportFailed();
        } else if (!TlsWouldBlock(got)) {
            LogTransportFailure(connection.tls, "read", got, errno); retryable = true; break;
        }
        // Yield even under a burst of replies so I2S and the idle task run.
        if (got <= 0 || esp_timer_get_time() - last_yield >= 10000) {
            vTaskDelay(1); last_yield = esp_timer_get_time();
        }
    }
    call.Stop();
    // Do not append a close/control message behind an incomplete TLS write.
    if (!retryable && !sender.pending.Busy()) {
        if (Send(sender, "{\"type\":\"session.end\"}")) (void)PumpWire(sender);
    }
    memset(scratch->wire, 0, sizeof scratch->wire);
    return retryable;
}
#else
// True only for transport failures eligible for a new platform grant.
bool ConnectOnce(const Grant& grant) {
    bool retryable = false;
    WssAddress addr = {};
    if (!ParseWss(grant.url, &addr)) {
        ESP_LOGW(TAG, "rejected an invalid WSS URL");
        return false;
    }

    // Keep background HTTPS out until the codec states, task and TLS are freed.
    agentlink::cloud::NetworkMemoryLock memory;
    if (!Current(grant.epoch)) return false;
    VoiceWifiPower power;
    esp_transport_handle_t tls = esp_transport_ssl_init();
    if (!tls) {
        ESP_LOGE(TAG, "could not allocate TLS transport");
        return false;
    }
    esp_transport_ssl_crt_bundle_attach(tls, esp_crt_bundle_attach);
    esp_transport_handle_t ws = esp_transport_ws_init(tls);
    if (!ws) {
        ESP_LOGE(TAG, "could not allocate WebSocket transport");
        esp_transport_destroy(tls);
        return false;
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
        return false;
    }

    agentlink::realtime::MessageBuffer message(kMessageInitialBytes, kMessageBytes, ResizeMessage);
    if (!message.Data()) {
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return false;
    }
    auto* scratch = static_cast<TxScratch*>(heap_caps_malloc(sizeof(TxScratch),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch) scratch = static_cast<TxScratch*>(malloc(sizeof(TxScratch)));
    if (!scratch) {
        ESP_LOGE(TAG, "could not allocate voice uplink buffer");
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return false;
    }
    if (s_cb.state) s_cb.state(false, "Connecting voice");
    Call call(grant.epoch, grant.audio, grant.platform);
    // Reserve large codec states early, but leave the 40 KB C3 task stack free
    // for certificate parsing and RSA verification during the TLS handshake.
    if (!call.Prepare()) {
        free(scratch);
        esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return false;
    }
    WsSender sender{tls, scratch->wire, sizeof scratch->wire};
    errno = 0;
    const int connected = esp_transport_connect(ws, addr.host, addr.port, kConnectTimeoutMs);
    if (connected != 0) {
        const int saved_errno = errno;
        const int status = esp_transport_ws_get_upgrade_request_status(ws);
        LogTransportFailure(tls, "connect", connected, saved_errno);
        ESP_LOGW(TAG, "WSS connection failed (upgrade HTTP %d)", status);
        retryable = status <= 0 || status >= 500;
    } else {
        ESP_LOGI(TAG, "WSS connected before audio task: internal free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        const int socket = esp_transport_get_socket(tls);
        const int enabled = 1;
        if (socket < 0 || setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled) != 0)
            ESP_LOGW(TAG, "could not enable TCP_NODELAY for voice");
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
        } else if (!call.Start()) {
            ESP_LOGE(TAG, "voice audio startup failed after TLS handshake");
        } else if (!Send(sender, hello, n)) {
            ESP_LOGW(TAG, "session.connect could not be sent");
            retryable = true;
        } else {
            ESP_LOGI(TAG, "session.connect sent over WSS");
            agentlink::realtime::Message assembler(message.Data(), message.Capacity());
            uint8_t* chunk = scratch->read;
            int frame_read = 0;
            bool pending = false;
            int64_t pending_at = 0;
            uint32_t pending_reset = 0;
            const int64_t connected_at = esp_timer_get_time();
            int64_t last_rx = connected_at, last_ping = connected_at;
            SetWorkerPhase(0);
            while (Current(grant.epoch) && call.Healthy()) {
                if (!call.FlushTx(sender, scratch)) {
                    retryable = call.TransportFailed();
                    break;
                }
                if (pending) {
                    if (!call.Deliver(assembler.Opcode(), message.Data(), assembler.Size(), pending_at, pending_reset)) {
                        vTaskDelay(1);
                        continue;
                    }
                    pending = false;
                    message.Trim();
                    assembler.Rebind(message.Data(), message.Capacity());
                }
                const int64_t now = esp_timer_get_time();
                if ((!s_ready && now - connected_at > kReadyTimeoutUs) || now - last_rx > 45000000) {
                    ESP_LOGW(TAG, "voice session timed out");
                    retryable = true;
                    break;
                }
                if (now - last_ping > 15000000 && !call.WriteBlocked()) {
                    errno = 0;
                    const int ping = esp_transport_ws_send_raw(ws, static_cast<ws_transport_opcodes_t>(
                            WS_TRANSPORT_OPCODES_PING | WS_TRANSPORT_OPCODES_FIN), nullptr, 0, kSendTimeoutMs);
                    if (ping < 0) {
                        LogTransportFailure(tls, "ping", ping, errno);
                        retryable = true;
                        break;
                    }
                    last_ping = now;
                }
                SetWorkerPhase(2);
                errno = 0;
                const int polled = esp_transport_poll_read(ws, call.TxPending() && !call.WriteBlocked() ? 0 : 10);
                SetWorkerPhase(0);
                if (polled < 0) {
                    LogTransportFailure(tls, "poll read", polled, errno);
                    retryable = true;
                    break;
                }
                if (!polled) continue;
                SetWorkerPhase(3);
                errno = 0;
                const int got = esp_transport_read(ws, reinterpret_cast<char*>(chunk), sizeof scratch->read, 1000);
                SetWorkerPhase(0);
                if (got < 0) {
                    LogTransportFailure(tls, "read", got, errno);
                    retryable = true;
                    break;
                }
                if (!Current(grant.epoch)) break;
                s_rx_bytes.fetch_add(got);
                const auto opcode = esp_transport_ws_get_read_opcode(ws);
                const int frame_size = esp_transport_ws_get_read_payload_len(ws);
                const bool first = frame_read == 0;
                frame_read += got;
                const bool end = frame_read == frame_size;
                if (frame_size < 0 || frame_read > frame_size || (got == 0 && !end)) break;
                last_rx = esp_timer_get_time();
                if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
                    // Log only the numeric close code, never a server-supplied reason.
                    const unsigned code = got >= 2 ? (static_cast<unsigned>(chunk[0]) << 8) | chunk[1] : 0;
                    ESP_LOGW(TAG, "voice peer closed (code=%u); automatic retry disabled", code);
                    break;
                }
                if (opcode == WS_TRANSPORT_OPCODES_PING || opcode == WS_TRANSPORT_OPCODES_PONG) {
                    if (!first || !end || frame_size > 125) break;
                    if (opcode == WS_TRANSPORT_OPCODES_PING) {
                        errno = 0;
                        const int pong = esp_transport_ws_send_raw(ws,
                            static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN),
                            reinterpret_cast<char*>(chunk), got, kSendTimeoutMs);
                        if (pong < 0 || pong != got) {
                            LogTransportFailure(tls, "pong", pong, errno);
                            retryable = true;
                            break;
                        }
                    }
                } else {
                    const size_t buffered = assembler.Active() ? assembler.Size() : 0;
                    if (!message.Reserve(buffered + static_cast<size_t>(got))) {
                        ESP_LOGW(TAG, "voice message allocation/limit failed (%u bytes, largest=%u)",
                                 static_cast<unsigned>(buffered + got),
                                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
                        break;
                    }
                    assembler.Rebind(message.Data(), message.Capacity());
                    const auto result = assembler.Append(opcode, first, end,
                            esp_transport_ws_get_fin_flag(ws), chunk, static_cast<size_t>(got));
                    if (result == agentlink::realtime::Message::kError) {
                        ESP_LOGW(TAG, "invalid or oversized voice frame");
                        break;
                    }
                    if (result == agentlink::realtime::Message::kComplete) {
                        // Cancel queued speech before an interruption waits behind paced audio.
                        if (assembler.Opcode() == WS_TRANSPORT_OPCODES_TEXT && IsInterruption(message.Data(), assembler.Size()))
                            s_reset.fetch_add(1);
                        pending = true;
                        pending_at = esp_timer_get_time();
                        pending_reset = s_reset.load();
                    }
                }
                if (end) frame_read = 0;
            }
            // An explicit server rejection or invalid media must not be retried
            // merely because the network worker observed a simultaneous failure.
            if (!call.Healthy()) retryable = false;
        }
        call.Stop();
        memset(hello, 0, sizeof hello);
        // A failed write may have left a partial frame. Do not append session.end.
        if (!retryable) Send(sender, "{\"type\":\"session.end\"}");
        esp_transport_close(ws);
    }
    free(scratch);
    esp_transport_destroy(ws);
    esp_transport_destroy(tls);
    return retryable;
}
#endif

void Worker(void*) {
    Grant grant = {};
    while (s_run.load(std::memory_order_acquire)) {
        if (xQueueReceive(s_grants, &grant, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        const char* hint = "Voice ended. Tap Connect to retry";
        agentlink::realtime::RecoverTransport([&](bool may_retry) {
            bool granted = !grant.direct && !grant.platform;
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
            if (grant.direct && Current(grant.epoch)) granted = FetchDirectGrant(&grant, &hint);
#endif
            if (grant.platform && Current(grant.epoch)) {
                int status = 0;
                granted = cloud_platform_grant(grant.agent_id, grant.session_id, false,
                                               grant.url, sizeof grant.url, grant.token, sizeof grant.token,
                                               &grant.audio, &status) == ESP_OK;
                if (!granted && status == 409 && Current(grant.epoch)) {
                    granted = cloud_platform_grant(grant.agent_id, grant.session_id, true,
                                                   grant.url, sizeof grant.url, grant.token, sizeof grant.token,
                                                   &grant.audio, &status) == ESP_OK;
                }
                if (!granted) hint = status == 409 ? "Voice session busy. End the previous call"
                                     : "Voice access failed. Tap Connect to retry";
            }
            bool retryable = false;
            if (granted && Current(grant.epoch)) retryable = ConnectOnce(grant);
            s_ready.store(false);
            ResetInput();
            if (s_cb.audio_reset) s_cb.audio_reset();
            // Notify applications before cleanup/backoff so the mic stops while offline.
            const bool retry = grant.platform && retryable && may_retry && Current(grant.epoch);
            if (retryable) hint = "Voice network failed. Tap Connect to retry";
            if (s_cb.state) s_cb.state(false, retry ? "Voice interrupted. Reconnecting..." : hint);
            if (grant.platform && (granted || (grant.url[0] && grant.token[0])))
                cloud_platform_end_grant(grant.agent_id, grant.session_id);
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
            if (grant.direct && granted) EndDirectSession();
#endif
            return retry && Current(grant.epoch);
        }, [&](unsigned retries, unsigned delay_ms) {
            ESP_LOGW(TAG, "voice transport recovery %u/2 in %u ms (fresh grant, old audio discarded)",
                     retries, delay_ms);
            const int64_t until = esp_timer_get_time() + delay_ms * 1000LL;
            while (Current(grant.epoch) && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(50));
            if (!Current(grant.epoch)) return false;
            // Tokens are one-use. Obtain a fresh grant on the next iteration.
            memset(grant.url, 0, sizeof grant.url);
            memset(grant.token, 0, sizeof grant.token);
            return true;
        });
        if (s_cb.finished) s_cb.finished();
        grant = {};
    }
    s_task.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}
}  // namespace

namespace {
bool StartWorker() {
    if (s_task.load(std::memory_order_acquire)) return true;
    TaskHandle_t task = nullptr;
#if CONFIG_IDF_TARGET_ESP32C3
    constexpr unsigned stack_bytes = 40960;
#else
    constexpr unsigned stack_bytes = 12288;
#endif
    if (xTaskCreate(&Worker, "agentlink_rt", stack_bytes, nullptr, 5, &task) != pdPASS) return false;
    s_task.store(task, std::memory_order_release);
#if CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGI(TAG, "resident voice task ready: stack=%uB, internal free=%u largest=%u",
             stack_bytes,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
    return true;
}

// Called with s_mutex held; both grant sources share the same worker and queue.
esp_err_t QueueGrant(Grant* grant) {
    grant->epoch = s_epoch.fetch_add(1) + 1;
    s_ready.store(false);
    s_continuous.store(grant->platform);
    if (xQueueOverwrite(s_grants, grant) != pdPASS) return ESP_FAIL;
    if (!StartWorker()) {
        xQueueReset(s_grants);
        return ESP_ERR_NO_MEM;
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
#if CONFIG_IDF_TARGET_ESP32C3
    // Reserve the only large stack at SDK startup, before HTTPS fragments RAM.
    // It waits on the grant queue between calls and is deleted only at SDK stop.
    if (!StartWorker()) {
        s_run.store(false);
        if (s_tx_caps) vQueueDeleteWithCaps(s_tx); else vQueueDelete(s_tx);
        if (s_grants_caps) vQueueDeleteWithCaps(s_grants); else vQueueDelete(s_grants);
        s_tx = s_grants = nullptr;
        s_tx_caps = s_grants_caps = false;
        return ESP_ERR_NO_MEM;
    }
#endif
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
    grant = {};
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
        s_tx_bytes.store(0);
        s_rx_bytes.store(0);
        s_decoded_samples.store(0);
        s_tx_send_ms.store(0);
        s_tx_write_calls.store(0);
        s_tx_encode_max_us.store(0);
        s_encoder_stack.store(0);
        s_diag_last_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        s_diag_mic_frames = s_diag_dropped_frames = 0;
        s_diag_prev_sent_frames = s_diag_prev_sent_messages = 0;
        s_diag_prev_tx_bytes = s_diag_prev_rx_bytes = s_diag_prev_samples = 0;
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
