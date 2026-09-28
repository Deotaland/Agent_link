// Cloud session state machine, internal to the WiFi backend. See cloud_status.h.
//
// One task, one blocking HTTP request at a time. The MQTT client (cloud_mqtt.h) runs on a task of
// its own, which this one starts and stops.

#include "cloud_session.h"

#include <cstring>

#include "agent_link_ota.h"
#include "cloud_api.h"
#include "cloud_credential.h"
#include "cloud_mqtt.h"
#include "device_identity.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>

namespace {
constexpr const char* TAG = "agent_link.cloud";

// bind-status returns immediately (short poll), so this is a plain interval.
constexpr uint32_t kBindPollMs = 3000;

// The platform does not specify a heartbeat interval.
constexpr uint32_t kHeartbeatMs = 60000;

// Codes live 600s; request a new one shortly before expiry.
constexpr uint32_t kCodeRenewLeadS = 20;

constexpr uint32_t kNetPollMs   = 1000;
constexpr uint32_t kRetryMs     = 5000;   // after a transport failure
constexpr uint32_t kFaultRetryMs = 30000; // after the platform refuses

// Retry delay after a failed request. No response (code < 0) is usually transient, so retry soon.
// A refusal (code > 0) won't change until something happens in the console, so back off. Keyed on
// whether there was a response, not on specific codes, so unknown refusals back off too.
uint32_t BackoffFor(const cloud_api_err_t& err) {
    return err.code > 0 ? kFaultRetryMs : kRetryMs;
}

// An MQTT token is renewed a tenth of its lifetime before it expires, but never sooner than this
// after it was issued, whatever lifetime it claims.
constexpr uint32_t kTokenMinLifeS = 30;

// A token the broker refuses at this age or younger is not the problem, so the next one is asked for
// only after kFaultRetryMs. Asking at once would loop against a broker that refuses every token.
constexpr uint32_t kTokenFreshS = 60;

// Session task notification bits.
constexpr uint32_t kNotifyMqttRefused = 1u << 0;

// Agent config returned by auth (not parsed yet).
constexpr size_t kAgentJsonCap = 2048;

cloud_session_config_t s_cfg  = {};
agent_cloud_status_t s_status = {};
TaskHandle_t         s_task   = nullptr;
bool                 s_run    = false;
char*                s_agent_json = nullptr;

// MQTT credentials. Session task only, and RAM only: the platform issues them on demand, and NVS is
// not encrypted.
cloud_mqtt_token_t* s_token        = nullptr;   // allocated once, in PSRAM when there is some
bool                s_token_ok     = false;     // *s_token holds credentials the broker has not refused
int64_t             s_token_issued = 0;         // esp_timer time it arrived
int64_t             s_token_renew  = 0;         // renew it from then on; 0 = only when refused
int64_t             s_token_retry  = 0;         // do not ask for one before then

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void Publish(agent_cloud_state_t st, const cloud_api_err_t* err) {
    agent_cloud_status_t snap;

    taskENTER_CRITICAL(&s_lock);
    s_status.state = st;
    if (err) {
        s_status.api_code = err->code;
        snprintf(s_status.message, sizeof s_status.message, "%s", err->message);
    }
    snap = s_status;
    taskEXIT_CRITICAL(&s_lock);

    if (s_cfg.on_state) s_cfg.on_state(&snap, s_cfg.ctx);
}

agent_cloud_state_t CurrentState() {
    taskENTER_CRITICAL(&s_lock);
    const agent_cloud_state_t s = s_status.state;
    taskEXIT_CRITICAL(&s_lock);
    return s;
}

void PublishCode(const char* code, uint32_t expires_s) {
    taskENTER_CRITICAL(&s_lock);
    snprintf(s_status.code, sizeof s_status.code, "%s", code ? code : "");
    s_status.expires_s = expires_s;
    taskEXIT_CRITICAL(&s_lock);
    Publish(AGENT_CLOUD_CLAIMING, nullptr);
}

// Clear the spent code without publishing. Publishing CLAIMING here would briefly show an empty
// code with 0s left before OnlineLoop publishes "Signing in".
void ClearCode() {
    taskENTER_CRITICAL(&s_lock);
    s_status.code[0]   = '\0';
    s_status.expires_s = 0;
    taskEXIT_CRITICAL(&s_lock);
}

// Consecutive requests without a response, and whether a platform refusal is on screen.
// Session task only.
int  s_unanswered      = 0;
bool s_showing_refusal = false;

// Roughly 30-45s without a response before "Reconnecting" replaces a refusal on screen.
constexpr int kUnansweredBeforeShown = 3;

// Publish the result of a failed request. Refusals are always published. A missing response is
// usually a one-off, so while a refusal is on screen it is only published after
// kUnansweredBeforeShown misses in a row.
void PublishFailure(agent_cloud_state_t state, const cloud_api_err_t& err) {
    if (err.code < 0) {
        ++s_unanswered;
        if (s_showing_refusal && s_unanswered < kUnansweredBeforeShown) {
            ESP_LOGW(TAG, "no answer from the platform (%d in a row) — keeping its last answer on screen",
                     s_unanswered);
            return;
        }
        s_showing_refusal = false;
    } else {
        s_unanswered      = 0;
        s_showing_refusal = true;
    }
    Publish(state, &err);
}

// Reset after an accepted request.
void NoteAccepted() {
    s_unanswered      = 0;
    s_showing_refusal = false;
}

// Station MAC in the "AA:BB:CC:DD:EE:FF" form the platform expects. Read once, then cached.
const char* StaMac() {
    static char s_mac[18] = {0};
    if (!s_mac[0]) {
        uint8_t m[6] = {0};
        esp_read_mac(m, ESP_MAC_WIFI_STA);
        snprintf(s_mac, sizeof s_mac, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return s_mac;
}

// Delegated to the backend, which knows more than whether there is an IP.
bool NetReady() { return s_cfg.net_ready(); }

void WaitForNet() {
    if (NetReady()) return;
    // A kept-alive connection belongs to the network that just went away.
    cloud_api_drop_connection();
    ESP_LOGI(TAG, "waiting for the network (station joining, or the portal is still up)");
    Publish(AGENT_CLOUD_NO_NETWORK, nullptr);
    while (s_run && !NetReady()) vTaskDelay(pdMS_TO_TICKS(kNetPollMs));
}

// The key was revoked in the console (reset-key or unbind): clear it and re-claim.
bool HandleRevoked(const cloud_api_err_t& err) {
    if (err.code != CLOUD_API_KEY_REVOKED) return false;
    ESP_LOGW(TAG, "platform revoked our auth_key — re-claiming");
    (void)cloud_cred_clear();
    return true;
}

// Request a code and poll until it is claimed. Returns true once an auth_key is stored.
bool ClaimLoop() {
    while (s_run) {
        WaitForNet();
        if (!s_run) return false;

        cloud_claim_t   claim = {};
        cloud_api_err_t err   = {};
        if (cloud_api_claim_code(s_cfg.product_id, dev_identity_sn(), StaMac(),
                                 s_cfg.chip_type, agent_link_ota_running_version(),
                                 &claim, &err) != ESP_OK) {
            // Already bound: no new code until the device is unbound in the console.
            PublishFailure(err.code == CLOUD_API_ALREADY_BOUND ? AGENT_CLOUD_ALREADY_BOUND
                                                               : AGENT_CLOUD_FAULT, err);
            vTaskDelay(pdMS_TO_TICKS(BackoffFor(err)));
            continue;
        }

        NoteAccepted();
        uint32_t left = claim.expires_in;
        PublishCode(claim.code, left);
        ESP_LOGI(TAG, "show code %s — waiting for a user to claim it", claim.code);

        while (s_run && left > kCodeRenewLeadS) {
            vTaskDelay(pdMS_TO_TICKS(kBindPollMs));
            if (!s_run) return false;

            cloud_bind_t bind = {};
            if (cloud_api_bind_status(dev_identity_sn(), claim.code, &bind, &err) != ESP_OK) {
                // No response; the code is still valid, retry on the next tick.
                left = left > (kBindPollMs / 1000) ? left - (kBindPollMs / 1000) : 0;
                PublishCode(claim.code, left);
                continue;
            }

            if (bind.state == CLOUD_BIND_BOUND) {
                if (cloud_cred_set_auth_key(bind.auth_key) != ESP_OK) return false;
                ClearCode();
                return true;
            }
            if (bind.state == CLOUD_BIND_EXPIRED) {
                ESP_LOGI(TAG, "code expired — asking for a new one");
                break;   // back to claim-code for a new one
            }
            left = bind.remaining_ttl ? bind.remaining_ttl
                                      : (left > (kBindPollMs / 1000) ? left - (kBindPollMs / 1000) : 0);
            PublishCode(claim.code, left);
        }
    }
    return false;
}

enum class TokenResult { kOk, kLater, kKeyRevoked, kNoAgent };

// Ask for MQTT credentials. A revoked key and a missing agent are handled as auth handles them; any
// other failure leaves the device online over HTTP and asks again after the usual backoff.
TokenResult FetchToken() {
    cloud_api_err_t err = {};
    const esp_err_t r   = cloud_api_mqtt_token(cloud_cred_auth_key(), s_token, &err);
    const int64_t   now = esp_timer_get_time();
    if (r != ESP_OK) {
        s_token_ok = false;   // the request cleared it
        if (HandleRevoked(err)) return TokenResult::kKeyRevoked;
        if (err.code == CLOUD_API_NO_AGENT) {
            PublishFailure(AGENT_CLOUD_NO_AGENT, err);
            return TokenResult::kNoAgent;
        }
        s_token_retry = now + static_cast<int64_t>(BackoffFor(err)) * 1000;
        ESP_LOGW(TAG, "no MQTT token (code=%d) — asking again in %us",
                 err.code, static_cast<unsigned>(BackoffFor(err) / 1000));
        return TokenResult::kLater;
    }

    // The platform issues the client id as the device_sn; use that if the field is missing.
    if (!s_token->client_id[0]) {
        snprintf(s_token->client_id, sizeof s_token->client_id, "%s", dev_identity_sn());
    }
    s_token_ok     = true;
    s_token_issued = now;
    s_token_retry  = 0;

    // A tenth of the lifetime early; 0 when the platform gives no lifetime.
    uint32_t after = s_token->expires_in - s_token->expires_in / 10;
#if CONFIG_AGENT_LINK_MQTT_TOKEN_RENEW_S > 0
    after = CONFIG_AGENT_LINK_MQTT_TOKEN_RENEW_S;
    ESP_LOGW(TAG, "CONFIG_AGENT_LINK_MQTT_TOKEN_RENEW_S=%u: renewing early, for bench testing only",
             static_cast<unsigned>(after));
#endif
    if (after) {
        if (after < kTokenMinLifeS) after = kTokenMinLifeS;
        s_token_renew = now + static_cast<int64_t>(after) * 1000000;
        ESP_LOGI(TAG, "MQTT token for %s://%s: valid %us, renewing it in %us", s_token->protocol,
                 s_token->host, static_cast<unsigned>(s_token->expires_in),
                 static_cast<unsigned>(after));
    } else {
        s_token_renew = 0;
        ESP_LOGW(TAG, "MQTT token for %s://%s has no lifetime — renewing it only when refused",
                 s_token->protocol, s_token->host);
    }
    return TokenResult::kOk;
}

// The broker refused our credentials. Runs on the MQTT task; the session task acts on it. s_task is
// valid here because the session stops the client before it exits.
void OnMqttRefused(int /*connack_code*/, void* /*ctx*/) {
    xTaskNotify(s_task, kNotifyMqttRefused, eSetBits);
}

// Stop the client, and drop any refusal it reported that is still pending: once the stop returns
// its task is gone, so the refusal is stale and must not tear down the next client.
void StopMqtt() {
    cloud_mqtt_stop();
    ulTaskNotifyValueClear(nullptr, kNotifyMqttRefused);
}

void HandleMqttRefused() {
    StopMqtt();   // it would go on retrying with the refused credentials
    s_token_ok = false;
    const int64_t  now = esp_timer_get_time();
    const uint32_t age = static_cast<uint32_t>((now - s_token_issued) / 1000000);
    if (age < kTokenFreshS) {
        s_token_retry = now + static_cast<int64_t>(kFaultRetryMs) * 1000;
        ESP_LOGW(TAG, "broker refused a token issued %us ago — asking for another in %us",
                 static_cast<unsigned>(age), static_cast<unsigned>(kFaultRetryMs / 1000));
    } else {
        s_token_retry = 0;
        ESP_LOGW(TAG, "broker refused our token — asking for a new one");
    }
}

// Why StayOnline() returned.
enum class Leave {
    kStopped,     // cloud_session_stop()
    kNoNetwork,   // authenticate again once it is back
    kKeyGone,     // revoked or forgotten: claim again
    kNoAgent,     // back off, then authenticate again
};

// Authenticated: heartbeats and the MQTT connection, in the order auth -> mqtt-token -> MQTT. The
// token is renewed shortly before it expires or at once when the broker refuses it, and is otherwise
// kept, across network drops too. The link stays CONNECTED: MQTT carries nothing yet.
Leave StayOnline() {
    int64_t next_beat = esp_timer_get_time() + static_cast<int64_t>(kHeartbeatMs) * 1000;
    Leave   why;
    for (;;) {
        if (!s_run)                 { why = Leave::kStopped;   break; }
        if (!cloud_cred_is_bound()) { why = Leave::kKeyGone;   break; }
        if (!NetReady())            { why = Leave::kNoNetwork; break; }
        const int64_t now = esp_timer_get_time();

        const bool renew = !s_token_ok || (s_token_renew && now >= s_token_renew);
        if (renew && now >= s_token_retry) {
            const TokenResult t = FetchToken();
            if (t == TokenResult::kKeyRevoked) { why = Leave::kKeyGone; break; }
            if (t == TokenResult::kNoAgent)    { why = Leave::kNoAgent; break; }
            if (t == TokenResult::kOk && cloud_mqtt_started()) {
                ESP_LOGI(TAG, "token renewed — reconnecting MQTT with it");
                StopMqtt();
            }
        }
        if (s_token_ok && !cloud_mqtt_started() &&
            cloud_mqtt_start(s_token, &OnMqttRefused, nullptr) != ESP_OK) {
            // Unusable as issued (a protocol without TLS), or no memory: ask for another later.
            s_token_ok    = false;
            s_token_retry = now + static_cast<int64_t>(kFaultRetryMs) * 1000;
        }

        if (now >= next_beat) {
            next_beat = now + static_cast<int64_t>(kHeartbeatMs) * 1000;
            cloud_api_err_t hb = {};
            if (cloud_api_heartbeat(cloud_cred_auth_key(), &hb) != ESP_OK) {
                if (HandleRevoked(hb)) { why = Leave::kKeyGone; break; }
                // Anything else: stay online and retry on the next beat.
                ESP_LOGW(TAG, "heartbeat failed (code=%d) — staying online", hb.code);
            }
        }

        // Sleep until the broker refuses us, or for a second: the network and the deadlines above
        // are polled.
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(kNetPollMs)) == pdTRUE &&
            (bits & kNotifyMqttRefused)) {
            HandleMqttRefused();
        }
    }
    StopMqtt();
    if (why == Leave::kKeyGone) s_token_ok = false;   // issued against a key that is gone
    return why;
}

// Authenticate, then stay online. Returns when the key stops working.
void OnlineLoop() {
    while (s_run && cloud_cred_is_bound()) {
        WaitForNet();
        if (!s_run) return;

        // Show "Signing in" on the way in, but not on retries: once the platform has answered,
        // keep that answer on screen instead of flashing "Signing in" on every attempt.
        const agent_cloud_state_t shown = CurrentState();
        if (shown != AGENT_CLOUD_NO_AGENT && shown != AGENT_CLOUD_FAULT) {
            Publish(AGENT_CLOUD_BOUND, nullptr);
        }

        cloud_api_err_t err = {};
        if (cloud_api_auth(cloud_cred_auth_key(), StaMac(),
                           agent_link_ota_running_version(),
                           s_agent_json, kAgentJsonCap, &err) != ESP_OK) {
            if (HandleRevoked(err)) return;
            // NO_AGENT: claimed, but no agent attached yet. Keep retrying until one is bound.
            PublishFailure(err.code == CLOUD_API_NO_AGENT ? AGENT_CLOUD_NO_AGENT
                                                          : AGENT_CLOUD_FAULT, err);
            vTaskDelay(pdMS_TO_TICKS(BackoffFor(err)));
            continue;
        }
        NoteAccepted();

        if (s_agent_json && s_agent_json[0]) {
            ESP_LOGI(TAG, "agent config: %uB (held unparsed until the platform settles its shape)",
                     static_cast<unsigned>(strlen(s_agent_json)));
        }

        Publish(AGENT_CLOUD_ONLINE, &err);
        ESP_LOGI(TAG, "online");

        // No agent: wait as after auth's own refusal, then authenticate again.
        if (StayOnline() == Leave::kNoAgent) vTaskDelay(pdMS_TO_TICKS(kFaultRetryMs));
    }
}

void SessionTask(void*) {
    ESP_LOGI(TAG, "cloud session up: sn=%s product_id=%d", dev_identity_sn(), s_cfg.product_id);
    while (s_run) {
        if (!cloud_cred_is_bound() && !ClaimLoop()) break;
        OnlineLoop();
    }
    ESP_LOGI(TAG, "cloud session stopped");
    s_task = nullptr;
    vTaskDelete(nullptr);
}
}  // namespace

esp_err_t cloud_session_start(const cloud_session_config_t* cfg) {
    // net_ready is required; see cloud_session_config_t.
    if (!cfg || !cfg->base_url || cfg->product_id <= 0 || !cfg->net_ready) return ESP_ERR_INVALID_ARG;
    if (s_task) return ESP_ERR_INVALID_STATE;

    s_cfg = *cfg;

    // Minting is safe: the backend starts the session after esp_wifi_start(), so RF is running.
    esp_err_t r = dev_identity_load(/*allow_mint=*/true);
    if (r != ESP_OK) return r;
    r = cloud_cred_load();
    if (r != ESP_OK) return r;
    r = cloud_api_init(cfg->base_url);
    if (r != ESP_OK) return r;

    if (!s_agent_json) {
        s_agent_json = static_cast<char*>(calloc(1, kAgentJsonCap));
        if (!s_agent_json) return ESP_ERR_NO_MEM;
    }
    if (!s_token) {
        // Only this task touches it, so PSRAM will do, and internal RAM is the scarcer.
        s_token = static_cast<cloud_mqtt_token_t*>(heap_caps_calloc(1, sizeof *s_token, MALLOC_CAP_SPIRAM));
        if (!s_token) s_token = static_cast<cloud_mqtt_token_t*>(calloc(1, sizeof *s_token));
        if (!s_token) return ESP_ERR_NO_MEM;
    }
    s_token_ok = false;

    memset(&s_status, 0, sizeof s_status);
    s_status.state = AGENT_CLOUD_IDLE;
    s_run = true;

    // TLS handshakes run on this stack and need several KB.
    if (xTaskCreate(SessionTask, "al_cloud", 8192, nullptr, 4, &s_task) != pdPASS) {
        s_run = false;
        ESP_LOGE(TAG, "session task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cloud_session_stop(void) {
    s_run = false;   // the task notices at its next delay and deletes itself
}

void cloud_session_get_status(agent_cloud_status_t* out) {
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t cloud_session_forget(void) { return cloud_cred_clear(); }
