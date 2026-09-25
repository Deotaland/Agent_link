// Cloud session state machine, internal to the WiFi backend. See cloud_status.h.
//
// One task, one blocking HTTP request at a time.

#include "cloud_session.h"

#include <cstring>

#include "agent_link_ota.h"
#include "cloud_api.h"
#include "cloud_credential.h"
#include "device_identity.h"

#include "esp_log.h"
#include "esp_mac.h"
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

// Agent config returned by auth (not parsed yet).
constexpr size_t kAgentJsonCap = 2048;

cloud_session_config_t s_cfg  = {};
agent_cloud_status_t s_status = {};
TaskHandle_t         s_task   = nullptr;
bool                 s_run    = false;
char*                s_agent_json = nullptr;

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

// Authenticate, then keep the link alive with heartbeats. Returns when the key stops working.
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

        // TODO: fetch an MQTT token and connect the runtime plane once the platform defines it.
        // Until then an authenticated device stays ONLINE on heartbeats alone.
        Publish(AGENT_CLOUD_ONLINE, &err);
        ESP_LOGI(TAG, "online");

        while (s_run) {
            vTaskDelay(pdMS_TO_TICKS(kHeartbeatMs));
            if (!s_run) return;
            if (!NetReady()) break;   // lost the network; re-auth when it comes back

            cloud_api_err_t hb = {};
            if (cloud_api_heartbeat(cloud_cred_auth_key(), &hb) == ESP_OK) continue;
            if (HandleRevoked(hb)) return;
            // Anything else: stay online and retry on the next beat.
            ESP_LOGW(TAG, "heartbeat failed (code=%d) — staying online", hb.code);
        }
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
