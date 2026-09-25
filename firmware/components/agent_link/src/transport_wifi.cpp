// agent_link WiFi transport backend — station (STA) bring-up + captive-portal provisioning (配网).
//
// What this file owns:
//    1.The ESP-IDF WiFi stack for the WiFi transport: esp_netif + driver init, STA connect,
//     auto-reconnect with backoff, and credential persistence in NVS
//    2.First-run provisioning: with no stored credentials it brings up an open SoftAP and hands
//     off to the captive portal (wifi_provision.cpp) — the user joins the device's WiFi, a page
//     opens automatically, and they enter their home SSID/password. Credentials are saved only
//     once the device confirms it can actually join (got an IP), so a wrong password is never kept
//    3.Self-healing: if stored credentials stop working (AP moved / password changed), it reopens
//     the portal after a bounded number of failed joins
//    4.The cloud session (cloud/cloud_session.h): claiming the device, authenticating it and
//     keeping it online with the platform. Its progress becomes the link state; see OnCloudState.
//
// Built only where SOC_WIFI_SUPPORTED (esp_wifi.h does not compile on the ESP32-P4). Other targets
// get the stub at the end of the file, whose start() fails.
#include "soc/soc_caps.h"

#if SOC_WIFI_SUPPORTED

#include "agent_link_transport.h"
#include "agent_link.h"          // agent_wifi_config_t, agent_platform_t, agent_link_status_t
#include "cloud_session.h"
#include "wifi_provision.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "agent_link.wifi";

// NVS store for provisioned station credentials.
constexpr const char* kNvsNs   = "al_wifi";
constexpr const char* kNvsSsid = "ssid";
constexpr const char* kNvsPass = "pass";

// Reconnect / provisioning-fallback thresholds.
constexpr int      kProvFailLimit   = 3;      // provisioning: give up an attempt after 3 disconnects
constexpr int      kBootFailLimit   = 8;      // stored creds: after 8 failed joins without ever getting IP ,then open the portal
constexpr uint32_t kReconnectBaseMs = 2000;   // base station reconnect backoff
constexpr uint32_t kReconnectMaxMs  = 30000;  // backoff cap
constexpr uint32_t kProvTeardownMs  = 4000;   // linger on the portal after success so the page can show it, then drop the AP

enum class Phase { kIdle, kProvisioning, kStaConnecting, kStaConnected };

char  s_name[25]    = "AgentLink";   // device name,SoftAP SSID prefix
char  s_ap_ssid[33] = {0};           // "<name>-XXXX" (XXXX from the SoftAP MAC)

const agent_wifi_config_t* s_cfg      = nullptr;   // preset STA creds; usually NULL (portal)
const agent_platform_t*    s_platform = nullptr;   // platform identity; NULL = station only

// Transport -> core uplink callbacks
void (*s_on_recv)(const uint8_t*, size_t) = nullptr;
void (*s_on_conn)(bool) = nullptr;   // generic hook; the core uses s_on_state on this backend
void (*s_on_stream)(agent_stream_t, const uint8_t*, size_t) = nullptr;
void (*s_on_state)(agent_state_t) = nullptr;
void (*s_on_status)(const agent_link_status_t*) = nullptr;

bool                       s_cloud_started = false;
std::atomic<agent_state_t> s_link{AGENT_STATE_DISCONNECTED};

// Last status from the session, kept so portal changes can be re-reported without waiting for it.
// Written by the session task and read elsewhere, hence the lock.
agent_cloud_status_t s_last_cloud = {};
portMUX_TYPE         s_status_lock = portMUX_INITIALIZER_UNLOCKED;

bool  s_wifi_inited = false;
bool  s_started     = false;
Phase s_phase       = Phase::kIdle;
bool  s_want_connect = false;    // gate STA_START/reconnect auto-connect (off while waiting for portal input)
bool  s_ever_got_ip  = false;    // once true, keep retrying forever instead of falling back to the portal
int   s_retry        = 0;        // consecutive failed joins in the current phase

char       s_ssid[33] = {0};     // credentials currently being tried
char       s_pass[65] = {0};
std::mutex s_cred_mtx;

esp_timer_handle_t s_reconnect_timer = nullptr;
esp_timer_handle_t s_teardown_timer  = nullptr;

void StartProvisioning();
void ReportStatus();

// NVS credential store
esp_err_t EnsureNvs() {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        r = nvs_flash_init();
    }
    return r;
}

bool LoadCreds(char* ssid, size_t ssid_sz, char* pass, size_t pass_sz) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = ssid_sz, pl = pass_sz;
    const bool have_ssid = (nvs_get_str(h, kNvsSsid, ssid, &sl) == ESP_OK) && ssid[0];
    if (nvs_get_str(h, kNvsPass, pass, &pl) != ESP_OK) pass[0] = '\0';   // password may be absent (open network)
    nvs_close(h);
    return have_ssid;
}

void SaveCreds(const char* ssid, const char* pass) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGW(TAG, "nvs open failed — creds not saved"); return; }
    nvs_set_str(h, kNvsSsid, ssid);
    nvs_set_str(h, kNvsPass, pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

//  Helpers
// Returns a short machine code (not prose) so the portal page can localize it — see connecting.html.
const char* ReasonStr(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:              return "notfound";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:   return "badpass";
    default:                                   return "fail";
    }
}

void BuildApSsid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s-%02X%02X", s_name, mac[4], mac[5]);
}

void ApplyStaConfig(const char* ssid, const char* pass) {
    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char*>(wc.sta.ssid), ssid, sizeof(wc.sta.ssid) - 1);
    if (pass) strncpy(reinterpret_cast<char*>(wc.sta.password), pass, sizeof(wc.sta.password) - 1);
    wc.sta.pmf_cfg.capable = true;   // allow WPA3/PMF APs
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

void ApplyApConfig() {
    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char*>(wc.ap.ssid), s_ap_ssid, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len       = strlen(s_ap_ssid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode       = WIFI_AUTH_OPEN;   // open network
    wc.ap.beacon_interval = 100;
    esp_wifi_set_config(WIFI_IF_AP, &wc);
}

void ScheduleReconnect(uint32_t ms) {
    if (!s_reconnect_timer) return;
    esp_timer_stop(s_reconnect_timer);                 // harmless if not running
    esp_timer_start_once(s_reconnect_timer, static_cast<uint64_t>(ms) * 1000);
}

void ReconnectCb(void*) {
    if (s_want_connect) esp_wifi_connect();
}

// Fired ~kProvTeardownMs after a successful provisioning join: close the portal and drop the AP.
void TeardownProvCb(void*) {
    al_wifi_prov_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "provisioning portal closed; SoftAP down");
}

// Station reached the network
void OnGotIp(const esp_netif_ip_info_t& ip) {
    char ip_str[16];
    esp_ip4addr_ntoa(&ip.ip, ip_str, sizeof ip_str);
    s_ever_got_ip = true;
    s_retry = 0;

    if (s_phase == Phase::kProvisioning) {
        // First successful join from the portal: persist the credentials now
        std::lock_guard<std::mutex> lk(s_cred_mtx);
        SaveCreds(s_ssid, s_pass);
        al_wifi_prov_set_status(AL_PROV_CONNECTED, ip_str, nullptr);
        ESP_LOGI(TAG, "provisioning succeeded: joined '%s', ip=%s (credentials saved)", s_ssid, ip_str);
        if (s_teardown_timer) esp_timer_start_once(s_teardown_timer, static_cast<uint64_t>(kProvTeardownMs) * 1000);
    } else {
        ESP_LOGI(TAG, "WiFi connected: ip=%s", ip_str);
    }
    s_phase = Phase::kStaConnected;
}

void OnStaDisconnected(uint8_t reason) {
    if (s_phase == Phase::kProvisioning) {
        // A join attempt during provisioning failed,retry a couple times, then tell the page.
        if (++s_retry >= kProvFailLimit) {
            s_want_connect = false;                    // stop hammering; wait for a fresh submit from the page
            al_wifi_prov_set_status(AL_PROV_FAILED, nullptr, ReasonStr(reason));
            ESP_LOGW(TAG, "provisioning join failed (reason=%d) after %d tries — awaiting retry", reason, s_retry);
        } else {
            ESP_LOGD(TAG, "provisioning join retry %d (reason=%d)", s_retry, reason);
            ScheduleReconnect(1500);
        }
        return;
    }

    // Normal station path
    s_phase = Phase::kStaConnecting;
    if (!s_ever_got_ip && ++s_retry >= kBootFailLimit) {
        ESP_LOGW(TAG, "cannot join '%s' after %d tries — starting provisioning portal", s_ssid, s_retry);
        StartProvisioning();
        return;
    }
    uint32_t backoff = kReconnectBaseMs << (s_retry < 4 ? s_retry : 4);
    if (backoff > kReconnectMaxMs) backoff = kReconnectMaxMs;
    ESP_LOGD(TAG, "station disconnected (reason=%d) — reconnect in %ums", reason, static_cast<unsigned>(backoff));
    ScheduleReconnect(backoff);
}

void WifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_want_connect) esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGD(TAG, "station associated — awaiting IP");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        OnStaDisconnected(static_cast<const wifi_event_sta_disconnected_t*>(data)->reason);
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGD(TAG, "portal: a client joined the SoftAP");
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGD(TAG, "portal: a client left the SoftAP");
        break;
    default:
        break;
    }
}

void IpEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        OnGotIp(static_cast<const ip_event_got_ip_t*>(data)->ip_info);
    }
}

// One-time WiFi stack bring-up: netif + event loop + default STA/AP interfaces + driver + handlers.
esp_err_t WifiInitOnce() {
    if (s_wifi_inited) return ESP_OK;

    esp_err_t r = EnsureNvs();
    if (r != ESP_OK) { ESP_LOGE(TAG, "nvs init: %s", esp_err_to_name(r)); return r; }

    r = esp_netif_init();
    if (r != ESP_OK) { ESP_LOGE(TAG, "netif init: %s", esp_err_to_name(r)); return r; }

    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(r)); return r; }

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    r = esp_wifi_init(&ic);
    if (r != ESP_OK) { ESP_LOGE(TAG, "wifi init: %s", esp_err_to_name(r)); return r; }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &IpEvent, nullptr, nullptr);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // we manage credential persistence ourselves (kNvsNs)

    const esp_timer_create_args_t rc = { .callback = &ReconnectCb,   .arg = nullptr, .dispatch_method = ESP_TIMER_TASK, .name = "al_wifi_reconn", .skip_unhandled_events = true };
    esp_timer_create(&rc, &s_reconnect_timer);
    const esp_timer_create_args_t tc = { .callback = &TeardownProvCb, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK, .name = "al_wifi_teardown", .skip_unhandled_events = true };
    esp_timer_create(&tc, &s_teardown_timer);

    BuildApSsid();
    s_wifi_inited = true;
    return ESP_OK;
}

// Join a network as a station (boot path with known credentials).
void StartSta(const char* ssid, const char* pass) {
    {
        std::lock_guard<std::mutex> lk(s_cred_mtx);
        strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = '\0';
        strncpy(s_pass, pass ? pass : "", sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = '\0';
    }
    s_phase = Phase::kStaConnecting;
    s_want_connect = true;
    s_retry = 0;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ApplyStaConfig(ssid, pass);
    if (!s_started) { esp_wifi_start(); s_started = true; }   // STA_START -> connect
    else            { esp_wifi_connect(); }
    ESP_LOGI(TAG, "joining WiFi '%s'…", ssid);
}

// The user submitted credentials on the portal
void OnProvCreds(const char* ssid, const char* pass) {
    {
        std::lock_guard<std::mutex> lk(s_cred_mtx);
        strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = '\0';
        strncpy(s_pass, pass ? pass : "", sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = '\0';
    }
    s_retry = 0;
    s_want_connect = true;
    al_wifi_prov_set_status(AL_PROV_CONNECTING, nullptr, nullptr);
    ApplyStaConfig(ssid, pass);
    esp_wifi_disconnect();   // drop any half-open attempt, then connect with the new creds
    esp_wifi_connect();
}

// Bring up the SoftAP + captive portal and wait for the user to enter their WiFi
void StartProvisioning() {
    s_want_connect = false;
    s_retry = 0;
    s_phase = Phase::kProvisioning;
    if (s_teardown_timer) esp_timer_stop(s_teardown_timer);

    if (s_started) esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_APSTA);   // AP for the portal, STA idle so the page can scan + then join
    ApplyApConfig();
    if (!s_started) { esp_wifi_start(); s_started = true; }
    al_wifi_prov_start(s_ap_ssid, &OnProvCreds);
    ReportStatus();   // show "join the hotspot" right away
}

// Translate the cloud session state into agent_link_status_t. WiFi-specific states are mapped only
// here, so boards see the same phases on either transport.
//
// The text is in English because the bundled Montserrat fonts have no CJK glyphs (the platform's
// own messages are Chinese). It is written here so every board shows the same instructions.
void Compose(const agent_cloud_status_t& c, agent_link_status_t* out) {
    memset(out, 0, sizeof *out);
    out->transport = AGENT_TRANSPORT_WIFI;
    out->detail    = c.api_code;
    auto say = [out](agent_link_phase_t phase, const char* title, const char* hint) {
        out->phase = phase;
        snprintf(out->title, sizeof out->title, "%s", title);
        snprintf(out->hint,  sizeof out->hint,  "%s", hint);
    };

    if (!s_platform) {
        say(AGENT_LINK_PHASE_BLOCKED, "Not configured",
            "This firmware has no platform set (Board::Platform), so it cannot reach the platform");
        return;
    }

    switch (c.state) {
    case AGENT_CLOUD_IDLE:
    case AGENT_CLOUD_NO_NETWORK:
        if (al_wifi_prov_active()) {
            say(AGENT_LINK_PHASE_SETUP, "Set up WiFi", "");
            snprintf(out->hint, sizeof out->hint,
                     "Join the %s hotspot from your phone, then pick your network", s_ap_ssid);
        } else {
            say(AGENT_LINK_PHASE_CONNECTING, "Joining WiFi", "Connecting to the saved network");
        }
        break;

    case AGENT_CLOUD_CLAIMING:
        say(AGENT_LINK_PHASE_PAIRING, "Activation code", "");
        snprintf(out->code, sizeof out->code, "%s", c.code);
        out->expires_s = c.expires_s;
        snprintf(out->hint, sizeof out->hint, "Enter it in the Agent Link console - %us left",
                 static_cast<unsigned>(c.expires_s));
        break;

    case AGENT_CLOUD_BOUND:
        say(AGENT_LINK_PHASE_CONNECTING, "Signing in", "Authenticating with the platform");
        break;

    case AGENT_CLOUD_ONLINE:
        // Not READY: there is no data plane over WiFi yet. See OnCloudState.
        say(AGENT_LINK_PHASE_CONNECTED, "Online", "Connected to the platform");
        break;

    case AGENT_CLOUD_ALREADY_BOUND:
        say(AGENT_LINK_PHASE_BLOCKED, "Already bound",
            "This device is claimed by an account. Unbind it in the console to activate it again.");
        break;

    case AGENT_CLOUD_NO_AGENT:
        say(AGENT_LINK_PHASE_BLOCKED, "No agent",
            "Bound, but no agent is attached. Pick one for this device in the console.");
        break;

    case AGENT_CLOUD_FAULT:
    default:
        if (c.api_code < 0) {
            // No response (socket or TLS failure); the session is retrying.
            say(AGENT_LINK_PHASE_CONNECTING, "Reconnecting", "Cannot reach the platform - retrying");
        } else if (c.api_code == 401) {
            say(AGENT_LINK_PHASE_BLOCKED, "Platform error",
                "The platform gateway is refusing device requests");
        } else if (c.api_code == 1 || c.api_code == 400) {
            say(AGENT_LINK_PHASE_BLOCKED, "Platform error",
                "The firmware sent a request the platform rejected");
        } else {
            say(AGENT_LINK_PHASE_BLOCKED, "Platform error",
                "The platform refused this device - retrying");
        }
        break;
    }
}

void ReportStatus() {
    if (!s_on_status) return;
    agent_cloud_status_t c;
    taskENTER_CRITICAL(&s_status_lock);
    c = s_last_cloud;
    taskEXIT_CRITICAL(&s_status_lock);

    agent_link_status_t st;
    Compose(c, &st);
    s_on_status(&st);
}

// Cloud session -> link state: ONLINE maps to CONNECTED, anything else to DISCONNECTED. Never
// READY: that would tell boards that streams work, and there is no WiFi data plane until MQTT.
void OnCloudState(const agent_cloud_status_t* st, void* /*ctx*/) {
    if (!st) return;
    taskENTER_CRITICAL(&s_status_lock);
    s_last_cloud = *st;
    taskEXIT_CRITICAL(&s_status_lock);
    ReportStatus();

    const agent_state_t want = (st->state == AGENT_CLOUD_ONLINE) ? AGENT_STATE_CONNECTED
                                                                 : AGENT_STATE_DISCONNECTED;
    if (s_link.exchange(want) != want) {
        ESP_LOGI(TAG, "link -> %s", want == AGENT_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED");
        if (s_on_state) s_on_state(want);
    }
}

// Whether the session may start requests. Besides an IP, it waits for the captive portal to close:
// the portal stays up for kProvTeardownMs after provisioning so the phone can show the result, and
// meanwhile its SoftAP and HTTP server compete for the internal RAM a TLS handshake may need.
bool NetReady() {
    if (al_wifi_prov_active()) return false;
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip = {};
    return esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

// Started once from wifi_start(); the session waits for NetReady() itself.
void StartCloudSession() {
    if (s_cloud_started) return;
    if (!s_platform || !s_platform->base_url || !s_platform->base_url[0]) {
        // Not fatal: the station still comes up, but there is nothing to connect to.
        s_platform = nullptr;   // treat a half-filled platform as none
        ESP_LOGW(TAG, "no platform configured (Board::Platform / agent_link_config_t::platform) — "
                      "station only; this device will never reach the Agent platform");
        ReportStatus();
        return;
    }
    cloud_session_config_t cc = {};
    cc.base_url   = s_platform->base_url;
    cc.product_id = s_platform->product_id;
    cc.chip_type  = s_platform->chip_type;
    cc.net_ready  = &NetReady;
    cc.on_state   = &OnCloudState;

    const esp_err_t r = cloud_session_start(&cc);
    if (r != ESP_OK) { ESP_LOGE(TAG, "cloud session failed to start: %s", esp_err_to_name(r)); return; }
    s_cloud_started = true;
}

// agent_transport_t interface
esp_err_t wifi_start(void* /*impl*/) {
    esp_err_t r = WifiInitOnce();
    if (r != ESP_OK) return r;
    (void)s_on_recv; (void)s_on_conn; (void)s_on_stream;  // runtime plane (MQTT) not implemented yet

    char ssid[33] = {0}, pass[65] = {0};
    if (s_cfg && s_cfg->ssid && s_cfg->ssid[0]) {
        StartSta(s_cfg->ssid, s_cfg->password ? s_cfg->password : "");
    } else if (LoadCreds(ssid, sizeof ssid, pass, sizeof pass)) {
        ESP_LOGI(TAG, "using stored WiFi credentials for '%s'", ssid);
        StartSta(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no WiFi credentials — starting captive-portal provisioning");
        StartProvisioning();
    }

    StartCloudSession();
    return ESP_OK;
}

void wifi_stop(void* /*impl*/) {
    cloud_session_stop();
    s_cloud_started = false;
    if (s_link.exchange(AGENT_STATE_DISCONNECTED) != AGENT_STATE_DISCONNECTED && s_on_state) {
        s_on_state(AGENT_STATE_DISCONNECTED);
    }
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    if (s_teardown_timer)  esp_timer_stop(s_teardown_timer);
    al_wifi_prov_stop();
    s_want_connect = false;
    if (s_started) { esp_wifi_disconnect(); esp_wifi_stop(); s_started = false; }
    s_phase = Phase::kIdle;
}

// Runtime plane: not implemented. Each function logs once, since callers retry.
bool SayOnce(bool& said, const char* what) {
    if (!said) {
        said = true;
        ESP_LOGW(TAG, "%s dropped: the WiFi runtime plane (MQTT) is not implemented yet", what);
    }
    return false;
}

esp_err_t wifi_send_ctrl(void* /*impl*/, const uint8_t* /*frame*/, size_t /*len*/) {
    static bool said = false;
    SayOnce(said, "control frame");
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t wifi_stream_start(void* /*impl*/, agent_stream_t /*type*/, const uint8_t* /*meta*/, size_t /*meta_len*/) {
    static bool said = false;
    SayOnce(said, "stream open");
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t wifi_send_stream(void* /*impl*/, agent_stream_t /*type*/, const uint8_t* /*data*/, size_t /*len*/) {
    static bool said = false;
    SayOnce(said, "stream chunk");
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t wifi_stream_end(void* /*impl*/, agent_stream_t /*type*/, bool /*complete*/,
                          const uint8_t* /*meta*/, size_t /*meta_len*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

// Whether a data plane exists (READY), not just the link (CONNECTED). Always false until MQTT.
bool wifi_is_ready(void* /*impl*/) {
    return s_link.load(std::memory_order_acquire) == AGENT_STATE_READY;
}

agent_transport_t s_wifi = {
    wifi_start, wifi_stop, wifi_send_ctrl,
    wifi_stream_start, wifi_send_stream, wifi_stream_end,
    wifi_is_ready, nullptr,
};
}  // namespace

extern "C" agent_transport_t* agent_transport_wifi(void) { return &s_wifi; }

extern "C" void agent_transport_wifi_set_config(const agent_wifi_config_t* cfg) { s_cfg = cfg; }

extern "C" void agent_transport_wifi_set_name(const char* name) {
    if (name && *name) {
        strncpy(s_name, name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }
}

extern "C" void agent_transport_wifi_set_recv(void (*cb)(const uint8_t*, size_t)) { s_on_recv = cb; }
extern "C" void agent_transport_wifi_set_conn(void (*cb)(bool)) { s_on_conn = cb; }
extern "C" void agent_transport_wifi_set_stream_recv(void (*cb)(agent_stream_t, const uint8_t*, size_t)) {
    s_on_stream = cb;
}

extern "C" void agent_transport_wifi_set_state(void (*cb)(agent_state_t)) { s_on_state = cb; }

extern "C" void agent_transport_wifi_set_platform(const agent_platform_t* p) { s_platform = p; }

extern "C" void agent_transport_wifi_set_status(void (*cb)(const agent_link_status_t*)) { s_on_status = cb; }

extern "C" esp_err_t agent_transport_wifi_forget(void) { return cloud_session_forget(); }


#else  // !SOC_WIFI_SUPPORTED

// Stub for targets without a WiFi radio. agent_link.cpp references this backend unconditionally;
// selecting it here fails at start().
#include "agent_link_transport.h"
#include "esp_log.h"

namespace {
constexpr const char* TAG = "agent_link.wifi";

esp_err_t nowifi_start(void*) {
    ESP_LOGE(TAG, "the WiFi transport was selected, but this chip has no WiFi radio — "
                  "pick the BLE transport, or a board on a target that has one");
    return ESP_ERR_NOT_SUPPORTED;
}
void      nowifi_stop(void*) {}
esp_err_t nowifi_ctrl(void*, const uint8_t*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t nowifi_sopen(void*, agent_stream_t, const uint8_t*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t nowifi_swrite(void*, agent_stream_t, const uint8_t*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t nowifi_sclose(void*, agent_stream_t, bool, const uint8_t*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
bool      nowifi_ready(void*) { return false; }

agent_transport_t s_wifi = {
    nowifi_start, nowifi_stop, nowifi_ctrl,
    nowifi_sopen, nowifi_swrite, nowifi_sclose,
    nowifi_ready, nullptr,
};
}  // namespace

extern "C" agent_transport_t* agent_transport_wifi(void) { return &s_wifi; }

extern "C" void agent_transport_wifi_set_config(const agent_wifi_config_t*) {}
extern "C" void agent_transport_wifi_set_name(const char*) {}
extern "C" void agent_transport_wifi_set_recv(void (*)(const uint8_t*, size_t)) {}
extern "C" void agent_transport_wifi_set_conn(void (*)(bool)) {}
extern "C" void agent_transport_wifi_set_stream_recv(void (*)(agent_stream_t, const uint8_t*, size_t)) {}
extern "C" void agent_transport_wifi_set_state(void (*)(agent_state_t)) {}
extern "C" void agent_transport_wifi_set_platform(const agent_platform_t*) {}
extern "C" void agent_transport_wifi_set_status(void (*)(const agent_link_status_t*)) {}
extern "C" esp_err_t agent_transport_wifi_forget(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif  // SOC_WIFI_SUPPORTED
