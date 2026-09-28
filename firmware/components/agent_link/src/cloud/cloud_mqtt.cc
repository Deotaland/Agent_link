// The cloud session's MQTT connection. See cloud_mqtt.h.

#include "cloud_mqtt.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

namespace {
constexpr const char* TAG = "agent_link.mqtt";

// The platform is behind Cloudflare, which closes a WebSocket after 100s without traffic, and
// esp-mqtt's default keepalive is 120s. esp-mqtt pings after half the keepalive, so 40s means a
// ping every 20s, and the broker drops a client it has not heard from in 60s (1.5x).
constexpr int kKeepaliveS = 40;

// esp-mqtt's own retry interval after a lost connection or a failed attempt.
constexpr int kReconnectMs = 10000;

constexpr int    kDownlinkQos   = 1;
constexpr size_t kLogPayloadMax = 256;   // bytes of each downlink message that are printed

esp_mqtt_client_handle_t s_client = nullptr;
bool                     s_ws     = false;   // MQTT over WebSocket, rather than straight over TLS
std::atomic<bool>        s_connected{false};

cloud_mqtt_refused_cb_t s_on_refused = nullptr;
void*                   s_ctx        = nullptr;

// Copied from the token, which the caller may reuse. The session is clean, so the topics are
// subscribed again on every connect.
char s_topics[2][sizeof(cloud_mqtt_token_t::topic_down)] = {};
char s_url[160] = {};   // for logs

const char* RefusalName(int code) {
    switch (code) {
    case MQTT_CONNECTION_REFUSE_PROTOCOL:           return "protocol version not accepted";
    case MQTT_CONNECTION_REFUSE_ID_REJECTED:        return "client id rejected";
    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE: return "server unavailable";
    case MQTT_CONNECTION_REFUSE_BAD_USERNAME:       return "bad username or password";
    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:     return "not authorized";
    default:                                        return "unknown reason";
    }
}

void Subscribe(esp_mqtt_client_handle_t client) {
    esp_mqtt_topic_t list[2];
    int n = 0;
    for (const auto& t : s_topics) {
        if (t[0]) list[n++] = {t, kDownlinkQos};
    }
    if (n == 0) {
        ESP_LOGW(TAG, "the platform gave no downlink topics — nothing to subscribe to");
        return;
    }
    // One SUBSCRIBE for all of them; the SUBACK answers in the same order.
    const int id = esp_mqtt_client_subscribe_multiple(client, list, n);
    if (id < 0) ESP_LOGW(TAG, "SUBSCRIBE could not be sent (%d)", id);
}

// One return code per topic, in the order subscribed: the QoS granted, or 0x80 when the broker's
// ACL does not let this client have the topic.
void LogSuback(esp_mqtt_event_handle_t ev) {
    int i = 0;
    for (const auto& t : s_topics) {
        if (!t[0]) continue;
        if (i >= ev->data_len) break;
        const uint8_t rc = static_cast<uint8_t>(ev->data[i++]);
        if (rc >= 0x80) ESP_LOGW(TAG, "subscribe %s: refused by the broker", t);
        else            ESP_LOGI(TAG, "subscribed %s (QoS %u)", t, rc);
    }
}

bool Printable(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

// Logged, not acted on: the message format is not settled yet.
void LogDownlink(esp_mqtt_event_handle_t ev) {
    // A message longer than esp-mqtt's buffer arrives as several events; only the first has the topic.
    if (ev->current_data_offset != 0) {
        ESP_LOGD(TAG, "  ... %dB more at offset %d of %d",
                 ev->data_len, ev->current_data_offset, ev->total_data_len);
        return;
    }
    const char*  data = ev->data ? ev->data : "";   // NULL for an empty message
    const size_t len  = ev->data_len > 0 ? static_cast<size_t>(ev->data_len) : 0;
    const size_t n    = len < kLogPayloadMax ? len : kLogPayloadMax;
    const bool   more = n < static_cast<size_t>(ev->total_data_len);
    if (Printable(data, n)) {
        ESP_LOGI(TAG, "downlink %.*s (%dB, QoS %d%s): %.*s%s", ev->topic_len, ev->topic,
                 ev->total_data_len, ev->qos, ev->retain ? ", retained" : "",
                 static_cast<int>(n), data, more ? " ..." : "");
    } else {
        ESP_LOGI(TAG, "downlink %.*s (%dB, QoS %d%s), binary:", ev->topic_len, ev->topic,
                 ev->total_data_len, ev->qos, ev->retain ? ", retained" : "");
        ESP_LOG_BUFFER_HEX(TAG, data, n < 64 ? n : 64);
    }
}

// A connection attempt failed below MQTT: in DNS, TCP or TLS, which esp-tls names, or in the
// WebSocket upgrade. The upgrade status is 0 when no HTTP reply came; anything but 101 is a proxy
// or gateway turning the handshake away (a 403 from Cloudflare, say).
void LogConnectFailure(esp_mqtt_event_handle_t ev) {
    const esp_mqtt_error_codes_t* e = ev->error_handle;
    char ws[24] = "";
    if (s_ws) {
        char scheme[] = "wss";   // esp-mqtt takes a non-const string
        esp_transport_handle_t t = esp_mqtt_client_get_transport(ev->client, scheme);
        if (t) snprintf(ws, sizeof ws, " upgrade=%d", esp_transport_ws_get_upgrade_request_status(t));
    }
    // The heap figures tell a TLS allocation failure from a network one.
    ESP_LOGW(TAG, "connect to %s failed: tls=%s (-0x%04x, cert 0x%x) errno=%d%s — "
                  "internal heap free=%uB largest=%uB",
             s_url, esp_err_to_name(e->esp_tls_last_esp_err),
             static_cast<unsigned>(-e->esp_tls_stack_err),
             static_cast<unsigned>(e->esp_tls_cert_verify_flags), e->esp_transport_sock_errno, ws,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

// Runs on the MQTT task.
void OnEvent(void* /*arg*/, esp_event_base_t /*base*/, int32_t id, void* data) {
    const auto ev = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        // The TLS handshake has just run on this task, which is as deep as its stack gets.
        ESP_LOGI(TAG, "connected to %s (task stack headroom %uB)", s_url,
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        Subscribe(ev->client);
        break;

    case MQTT_EVENT_DISCONNECTED:
        // Also posted after every failed attempt; only a connection that was up is news.
        if (s_connected.exchange(false)) {
            ESP_LOGW(TAG, "connection lost — reconnecting in %ds", kReconnectMs / 1000);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        LogSuback(ev);
        break;

    case MQTT_EVENT_DATA:
        LogDownlink(ev);
        break;

    case MQTT_EVENT_ERROR:
        if (!ev->error_handle) break;
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            const int rc = ev->error_handle->connect_return_code;
            ESP_LOGW(TAG, "broker refused the login: %s (CONNACK %d)", RefusalName(rc), rc);
            if (s_on_refused) s_on_refused(rc, s_ctx);
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT && !s_connected) {
            LogConnectFailure(ev);   // a connection that was up is reported by DISCONNECTED
        }
        break;

    default:
        break;
    }
}
}  // namespace

esp_err_t cloud_mqtt_start(const cloud_mqtt_token_t* tok, cloud_mqtt_refused_cb_t on_refused, void* ctx) {
    if (!tok || !tok->host[0]) return ESP_ERR_INVALID_ARG;
    if (s_client) return ESP_ERR_INVALID_STATE;

    // TLS only: over "ws" or "mqtt" the password, a bearer token, would cross the network in clear.
    esp_mqtt_transport_t transport;
    if (strcmp(tok->protocol, "wss") == 0) {
        transport = MQTT_TRANSPORT_OVER_WSS;
    } else if (strcmp(tok->protocol, "mqtts") == 0) {
        transport = MQTT_TRANSPORT_OVER_SSL;
    } else {
        ESP_LOGE(TAG, "the platform offers the broker over \"%s\"; only wss and mqtts are supported",
                 tok->protocol);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_ws = (transport == MQTT_TRANSPORT_OVER_WSS);

    esp_mqtt_client_config_t mc = {};
    mc.broker.address.transport = transport;
    mc.broker.address.hostname  = tok->host;
    mc.broker.address.port      = tok->port;   // 0: the transport's default
    if (s_ws && tok->path[0]) mc.broker.address.path = tok->path;
    mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;   // as cloud_api: public CAs
    mc.credentials.client_id               = tok->client_id;
    mc.credentials.username                = tok->username;
    mc.credentials.authentication.password = tok->password;
    mc.session.keepalive            = kKeepaliveS;
    mc.network.reconnect_timeout_ms = kReconnectMs;

    snprintf(s_topics[0], sizeof s_topics[0], "%s", tok->topic_down);
    snprintf(s_topics[1], sizeof s_topics[1], "%s", tok->topic_ota_down);
    snprintf(s_url, sizeof s_url, "%s://%s:%u%s", tok->protocol, tok->host,
             static_cast<unsigned>(tok->port), s_ws ? tok->path : "");
    s_on_refused = on_refused;
    s_ctx        = ctx;

    // esp-mqtt copies every string it is given.
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, &OnEvent, nullptr);
    const esp_err_t r = esp_mqtt_client_start(s_client);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "client start failed: %s", esp_err_to_name(r));
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        return r;
    }
    ESP_LOGI(TAG, "connecting to %s as %s", s_url, tok->client_id);
    return ESP_OK;
}

void cloud_mqtt_stop(void) {
    if (!s_client) return;
    // Sends DISCONNECT when connected, then waits for the MQTT task to exit. esp-mqtt refuses this
    // from its own task, which is why the session calls it and the event handler never does.
    esp_mqtt_client_destroy(s_client);
    s_client = nullptr;
    if (s_connected.exchange(false)) ESP_LOGI(TAG, "disconnected");
}

bool cloud_mqtt_started(void) { return s_client != nullptr; }
