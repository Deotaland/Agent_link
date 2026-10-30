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
//     keeping it online with the platform, by heartbeat and over MQTT. Its progress becomes the
//     link state; see OnCloudState.
//
// The C3 BLE build leaves the unused WiFi backend out of its internal RAM.
#include "soc/soc_caps.h"
#include "sdkconfig.h"

#if SOC_WIFI_SUPPORTED && !(CONFIG_IDF_TARGET_ESP32C3 && CONFIG_AGENT_LINK_TRANSPORT_BLE)

#include "agent_link_transport.h"
#include "agent_link.h"          // agent_wifi_config_t, agent_platform_t, agent_link_status_t
#include "agent_link_network.h"
#include "wifi_local_radio.h"
#include "cloud_json.h"
#include "cloud_mqtt.h"
#include "cloud_platform.h"
#include "cloud_realtime.h"
#include "cloud_session.h"
#include "wifi_provision.h"
#include "wifi_join.h"

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

constexpr unsigned kProvFailLimit = 3;
constexpr int64_t kProvTeardownMs = 4000;

ESP_EVENT_DEFINE_BASE(kWifiControl);
enum { kWifiTick, kWifiCredentials, kWifiSetup };
struct WifiCredentials { char ssid[33]; char pass[65]; };

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

// Every call into the core from this backend holds this. Commands arrive on the MQTT task, link
// changes on the cloud session task and status on the WiFi event task, but the core is not
// thread-safe, and boards are promised callbacks that never run at the same time. Recursive
// because a state change reports status on its way through.
std::recursive_mutex s_core_mtx;

bool                       s_cloud_started = false;
std::atomic<agent_state_t> s_link{AGENT_STATE_DISCONNECTED};
std::atomic<bool> s_realtime_wanted{false};
std::atomic<bool> s_platform_voice{false};
char s_voice_hint[112] = {};
void (*s_pcm)(const uint8_t*, size_t) = nullptr;
void (*s_audio_end)(void) = nullptr;
void (*s_audio_reset)(void) = nullptr;
void (*s_text)(const char*) = nullptr;

// Last status from the session, kept so portal changes can be re-reported without waiting for it.
// Written by the session task and read elsewhere, hence the lock.
agent_cloud_status_t s_last_cloud = {};
portMUX_TYPE         s_status_lock = portMUX_INITIALIZER_UNLOCKED;

bool s_wifi_inited = false;
bool s_started = false;
std::atomic<Phase> s_phase{Phase::kIdle};
bool s_want_connect = false;
std::atomic<bool> s_local_radio{false};
bool s_ignore_leave = false;
std::mutex s_wifi_mtx;
agentlink::wifi::JoinWindow s_join;
int64_t s_portal_close_at = 0;
unsigned s_prov_failures = 0;

enum class JoinFailure { kNone, kNotFound, kAuth, kDisconnected, kNoIp, kTimeout, kDriver };
std::atomic<JoinFailure> s_join_failure{JoinFailure::kNone};
std::atomic<unsigned> s_join_left{0};
std::atomic<unsigned> s_join_attempt{0};
std::atomic<bool> s_waiting_ip{false};

char       s_ssid[33] = {0};     // credentials currently being tried
char       s_pass[65] = {0};
std::mutex s_cred_mtx;

esp_timer_handle_t s_tick_timer = nullptr;

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
JoinFailure JoinReason(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return JoinFailure::kNotFound;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return JoinFailure::kAuth;
    default: return JoinFailure::kDisconnected;
    }
}

const char* PortalReason(JoinFailure reason) {
    switch (reason) {
    case JoinFailure::kNotFound: return "notfound";
    case JoinFailure::kAuth: return "badpass";
    case JoinFailure::kNoIp: return "noip";
    case JoinFailure::kTimeout: return "timeout";
    default: return "fail";
    }
}

const char* JoinMessage(JoinFailure reason) {
    switch (reason) {
    case JoinFailure::kNotFound: return "Network not found";
    case JoinFailure::kAuth: return "Authentication failed";
    case JoinFailure::kDisconnected: return "Connection lost";
    case JoinFailure::kNoIp: return "No IP address received";
    case JoinFailure::kTimeout: return "Connection timed out";
    case JoinFailure::kDriver: return "WiFi request failed";
    default: return "Connecting";
    }
}

int64_t NowMs() { return esp_timer_get_time() / 1000; }

void UpdateJoinProgress() {
    s_join_left = s_join.RemainingSeconds(NowMs());
    s_join_attempt = s_join.Attempts();
    s_waiting_ip = s_join.WaitingIp();
}

void BuildApSsid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s-%02X%02X", s_name, mac[4], mac[5]);
}

esp_err_t ApplyStaConfig(const char* ssid, const char* pass) {
    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char*>(wc.sta.ssid), ssid, sizeof(wc.sta.ssid) - 1);
    if (pass) strncpy(reinterpret_cast<char*>(wc.sta.password), pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = wc.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;   // allow WPA3/PMF APs
    return esp_wifi_set_config(WIFI_IF_STA, &wc);
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

void ConnectStation() {
    const esp_err_t r = esp_wifi_connect();
    if (r != ESP_OK) {
        s_join_failure = JoinFailure::kDriver;
        s_join.Failed(NowMs());
        ESP_LOGW(TAG, "WiFi connect request: %s", esp_err_to_name(r));
    }
}

void DisconnectStation() { s_ignore_leave = esp_wifi_disconnect() == ESP_OK; }

// Station reached the network
void OnGotIp(const esp_netif_ip_info_t& ip) {
    char ip_str[16];
    esp_ip4addr_ntoa(&ip.ip, ip_str, sizeof ip_str);
    if (!s_want_connect || !s_join.Active()) return;
    s_join.Stop();
    s_prov_failures = 0;
    s_join_failure = JoinFailure::kNone;
    UpdateJoinProgress();

    if (s_phase == Phase::kProvisioning) {
        // First successful join from the portal: persist the credentials now
        std::lock_guard<std::mutex> lk(s_cred_mtx);
        SaveCreds(s_ssid, s_pass);
        al_wifi_prov_set_status(AL_PROV_CONNECTED, ip_str, nullptr);
        ESP_LOGI(TAG, "provisioning succeeded; credentials saved");
        s_portal_close_at = NowMs() + kProvTeardownMs;
    } else {
        ESP_LOGI(TAG, "WiFi connected");
    }
    s_phase = Phase::kStaConnected;
}

void OnStaDisconnected(uint8_t reason) {
    if (!s_want_connect) return;
    if (reason == WIFI_REASON_ASSOC_LEAVE && s_ignore_leave) { s_ignore_leave = false; return; }
    s_portal_close_at = 0;
    if (!s_join.Active()) {
        s_join.Begin(NowMs());
        s_phase = al_wifi_prov_active() ? Phase::kProvisioning : Phase::kStaConnecting;
        ESP_LOGI(TAG, "WiFi disconnected; reconnecting for up to 20s");
    }
    s_join_failure = JoinReason(reason);
    s_join.Failed(NowMs());
    if (s_phase == Phase::kProvisioning && ++s_prov_failures >= kProvFailLimit) {
        s_want_connect = false;
        s_join.Stop();
        al_wifi_prov_set_status(AL_PROV_FAILED, nullptr, PortalReason(s_join_failure));
    }
    UpdateJoinProgress();
    ESP_LOGW(TAG, "WiFi attempt %u: %s (reason=%u, %us left)", s_join.Attempts(),
             JoinMessage(s_join_failure), reason, s_join_left.load());
}

void WifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base != WIFI_EVENT) return;
    {
        std::lock_guard<std::mutex> lock(s_wifi_mtx);
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_want_connect) ConnectStation();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            if (s_want_connect && s_join.Active()) {
                s_ignore_leave = false;
                s_join.Associated();
                s_join_failure = JoinFailure::kNone;
                UpdateJoinProgress();
                ESP_LOGI(TAG, "WiFi associated; waiting for IP");
            }
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
    if (id == WIFI_EVENT_STA_CONNECTED || id == WIFI_EVENT_STA_DISCONNECTED) ReportStatus();
}

void IpEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        {
            std::lock_guard<std::mutex> lock(s_wifi_mtx);
            OnGotIp(static_cast<const ip_event_got_ip_t*>(data)->ip_info);
        }
        ReportStatus();
    }
}

void OnProvCreds(const char* ssid, const char* pass);
void ApplyProvCreds(const WifiCredentials& creds);

void TickCb(void*) { (void)esp_event_post(kWifiControl, kWifiTick, nullptr, 0, 0); }

void WifiControl(void*, esp_event_base_t, int32_t id, void* data) {
    bool report = false;
    {
        std::lock_guard<std::mutex> lock(s_wifi_mtx);
        if (id == kWifiSetup) {
            if (s_phase != Phase::kIdle) { StartProvisioning(); report = true; }
        } else if (id == kWifiCredentials) {
            if (s_phase == Phase::kIdle) return;
            ApplyProvCreds(*static_cast<const WifiCredentials*>(data));
            report = true;
        } else if (s_join.Active()) {
            const int64_t now = NowMs();
            if (s_join.Expired(now)) {
                if (s_join.WaitingIp()) s_join_failure = JoinFailure::kNoIp;
                else if (s_join_failure == JoinFailure::kNone) s_join_failure = JoinFailure::kTimeout;
                ESP_LOGW(TAG, "WiFi join stopped after 20s: %s; setup available", JoinMessage(s_join_failure));
                s_join.Stop();
                s_want_connect = false;
                if (s_phase == Phase::kProvisioning) {
                    al_wifi_prov_set_status(AL_PROV_FAILED, nullptr, PortalReason(s_join_failure));
                    DisconnectStation();
                } else {
                    StartProvisioning();
                }
                UpdateJoinProgress();
                report = true;
            } else {
                const unsigned previous = s_join_left;
                if (s_join.TakeRetry(now) && s_want_connect) {
                    ESP_LOGI(TAG, "WiFi retry %u (%us left)", s_join.Attempts(), s_join.RemainingSeconds(now));
                    ConnectStation();
                }
                UpdateJoinProgress();
                report = previous != s_join_left && s_join_left % 5 == 0;
            }
        }
        if (s_portal_close_at && NowMs() >= s_portal_close_at) {
            s_portal_close_at = 0;
            al_wifi_prov_stop();
            esp_wifi_set_mode(WIFI_MODE_STA);
            ESP_LOGI(TAG, "provisioning portal closed; SoftAP down");
            report = true;
        }
    }
    if (report) ReportStatus();
}

// One-time WiFi stack bring-up: netif + event loop + default STA/AP interfaces + driver + handlers.
esp_err_t WifiInitOnce() {
    if (s_wifi_inited) return ESP_OK;

    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

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
    esp_event_handler_instance_register(kWifiControl, ESP_EVENT_ANY_ID, &WifiControl, nullptr, nullptr);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // we manage credential persistence ourselves (kNvsNs)

    const esp_timer_create_args_t tick = { .callback = &TickCb, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "al_wifi_join", .skip_unhandled_events = true };
    r = esp_timer_create(&tick, &s_tick_timer);
    if (r != ESP_OK) return r;

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
    s_join.Begin(NowMs());
    s_join_failure = JoinFailure::kNone;
    UpdateJoinProgress();
    esp_wifi_set_mode(WIFI_MODE_STA);
    const esp_err_t r = ApplyStaConfig(ssid, pass);
    if (r != ESP_OK) {
        s_join_failure = JoinFailure::kDriver;
        ESP_LOGW(TAG, "WiFi configuration: %s", esp_err_to_name(r));
    }
    if (!s_started) { esp_wifi_start(); s_started = true; }   // STA_START -> connect
    else            { ConnectStation(); }
    ESP_LOGI(TAG, "joining WiFi (20s limit)");
}

// The user submitted credentials on the portal
void OnProvCreds(const char* ssid, const char* pass) {
    WifiCredentials creds = {};
    snprintf(creds.ssid, sizeof creds.ssid, "%s", ssid);
    snprintf(creds.pass, sizeof creds.pass, "%s", pass ? pass : "");
    if (esp_event_post(kWifiControl, kWifiCredentials, &creds, sizeof creds, 0) != ESP_OK) {
        al_wifi_prov_set_status(AL_PROV_FAILED, nullptr, "fail");
        ESP_LOGW(TAG, "WiFi setup request queue is full; retry from the page");
    }
}

void ApplyProvCreds(const WifiCredentials& creds) {
    if (!al_wifi_prov_active()) return;
    s_portal_close_at = 0;
    s_want_connect = false;
    DisconnectStation();
    {
        std::lock_guard<std::mutex> lk(s_cred_mtx);
        memcpy(s_ssid, creds.ssid, sizeof s_ssid);
        memcpy(s_pass, creds.pass, sizeof s_pass);
    }
    s_phase = Phase::kProvisioning;
    s_prov_failures = 0;
    s_join.Begin(NowMs());
    s_join_failure = JoinFailure::kNone;
    s_want_connect = true;
    al_wifi_prov_set_status(AL_PROV_CONNECTING, nullptr, nullptr);
    const esp_err_t r = ApplyStaConfig(creds.ssid, creds.pass);
    if (r == ESP_OK) ConnectStation();
    else {
        s_want_connect = false;
        s_join.Stop();
        s_join_failure = JoinFailure::kDriver;
        al_wifi_prov_set_status(AL_PROV_FAILED, nullptr, "fail");
        ESP_LOGW(TAG, "WiFi configuration: %s", esp_err_to_name(r));
    }
    UpdateJoinProgress();
}

// Bring up the SoftAP + captive portal and wait for the user to enter their WiFi
void StartProvisioning() {
    s_want_connect = false;
    s_join.Stop();
    s_portal_close_at = 0;
    s_prov_failures = 0;
    UpdateJoinProgress();
    s_phase = Phase::kProvisioning;

    if (s_started) DisconnectStation();
    esp_wifi_set_mode(WIFI_MODE_APSTA);   // AP for the portal, STA idle so the page can scan + then join
    ApplyApConfig();
    if (!s_started) { esp_wifi_start(); s_started = true; }
    al_wifi_prov_start(s_ap_ssid, &OnProvCreds);
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

    if (s_phase != Phase::kStaConnected) {
        const unsigned left = s_join_left;
        const char* reason = JoinMessage(s_join_failure);
        if (al_wifi_prov_active()) {
            say(AGENT_LINK_PHASE_SETUP, "Set up WiFi", "");
            if (left) {
                snprintf(out->hint, sizeof out->hint, "%s - %us left", s_waiting_ip ? "Waiting for IP" : reason, left);
            } else {
                snprintf(out->hint, sizeof out->hint, "%s%sJoin %s, then open 192.168.4.1",
                         s_join_failure == JoinFailure::kNone ? "" : reason,
                         s_join_failure == JoinFailure::kNone ? "" : ". ", s_ap_ssid);
            }
        } else {
            say(AGENT_LINK_PHASE_CONNECTING, "Joining WiFi", "");
            snprintf(out->hint, sizeof out->hint, "%s. Attempt %u; setup in %us",
                     s_waiting_ip ? "Waiting for IP" : reason, s_join_attempt.load(), left);
        }
        return;
    }
    if (al_wifi_prov_active()) {
        say(AGENT_LINK_PHASE_CONNECTING, "WiFi connected", "Closing setup and connecting to the platform");
        return;
    }

    switch (c.state) {
    case AGENT_CLOUD_IDLE:
    case AGENT_CLOUD_NO_NETWORK:
        say(AGENT_LINK_PHASE_CONNECTING, "Connecting", "Connecting to the platform");
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
        say(cloud_realtime_ready() ? AGENT_LINK_PHASE_READY : AGENT_LINK_PHASE_CONNECTED,
            cloud_realtime_ready() ? "Voice ready" : "Online", "Connected to the platform");
        taskENTER_CRITICAL(&s_status_lock);
        if (s_voice_hint[0]) snprintf(out->hint, sizeof out->hint, "%s", s_voice_hint);
        taskEXIT_CRITICAL(&s_status_lock);
        break;

    case AGENT_CLOUD_ALREADY_BOUND:
        say(AGENT_LINK_PHASE_BLOCKED, "Already bound",
            "This device is claimed by an account. Unbind it in the console to activate it again.");
        break;

    case AGENT_CLOUD_NO_AGENT:
        say(AGENT_LINK_PHASE_BLOCKED, "No agent",
            "Bind an agent to this device in the platform console.");
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
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    s_on_status(&st);
}

void UpdateLink() {
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    taskENTER_CRITICAL(&s_status_lock);
    const bool online = !s_local_radio.load() && s_last_cloud.state == AGENT_CLOUD_ONLINE;
    taskEXIT_CRITICAL(&s_status_lock);
    const agent_state_t want = !online ? AGENT_STATE_DISCONNECTED
        : cloud_realtime_ready() ? AGENT_STATE_READY : AGENT_STATE_CONNECTED;
    if (s_link.exchange(want) != want && s_on_state) s_on_state(want);
    ReportStatus();
}

// MQTT stays connected between calls; READY requires session.ready from the voice server.
void OnCloudState(const agent_cloud_status_t* st, void* /*ctx*/) {
    if (!st) return;
    taskENTER_CRITICAL(&s_status_lock);
    s_last_cloud = *st;
    taskEXIT_CRITICAL(&s_status_lock);
    if (st->state != AGENT_CLOUD_ONLINE) {
        s_realtime_wanted.store(false);
        s_platform_voice.store(false);
        cloud_realtime_cancel();
    }
    UpdateLink();
}

void OnRealtimeState(bool /*ready*/, const char* hint) {
    taskENTER_CRITICAL(&s_status_lock);
    snprintf(s_voice_hint, sizeof s_voice_hint, "%s", hint ? hint : "");
    taskEXIT_CRITICAL(&s_status_lock);
    UpdateLink();
}

void OnRealtimePcm(const uint8_t* pcm, size_t bytes) {
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    if (s_link == AGENT_STATE_READY && s_pcm) s_pcm(pcm, bytes);
}
void OnRealtimeEnd() {
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    if (s_audio_end) s_audio_end();
}
void OnRealtimeReset() {
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    if (s_audio_reset) s_audio_reset();
}
void OnRealtimeFinished() {
    s_realtime_wanted.store(false);
    s_platform_voice.store(false);
}
void OnRealtimeText(const char* text) {
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    if (s_link == AGENT_STATE_READY && s_text) s_text(text);
}

// Whether the session may start requests. Besides an IP, it waits for the captive portal to close:
// the portal stays up for kProvTeardownMs after provisioning so the phone can show the result, and
// meanwhile its SoftAP and HTTP server compete for the internal RAM a TLS handshake may need.
bool NetReady() {
    if (s_phase != Phase::kStaConnected) return false;
    if (al_wifi_prov_active()) return false;
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip = {};
    return esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

// The JSON layer hands each command to the core the way BLE's GATT write does.
void DeliverToCore(const uint8_t* frame, size_t len) {
    if (s_on_recv) s_on_recv(frame, len);
}

// A command from the platform, on the MQTT task.
void OnDownlink(const char* data, size_t len, void* /*ctx*/) {
    if (s_local_radio.load()) return;
    std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
    cloud_json_handle_command(data, len);
}

esp_err_t OnRealtimeGrant(const char* websocket_url, const char* connect_token) {
    if (cloud_realtime_direct_enabled()) return ESP_ERR_NOT_SUPPORTED;
    if (s_platform_voice) return ESP_ERR_INVALID_STATE;
    if (!s_realtime_wanted || s_link == AGENT_STATE_DISCONNECTED) return ESP_ERR_INVALID_STATE;
    return cloud_realtime_offer(websocket_url, connect_token);
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
    cloud_platform_set_base_url(cc.base_url);
    cc.product_id = s_platform->product_id;
    cc.chip_type  = s_platform->chip_type;
    cc.net_ready   = &NetReady;
    cc.on_state    = &OnCloudState;
    cc.on_downlink = &OnDownlink;

    const cloud_json_hooks_t hooks = {&DeliverToCore, &cloud_mqtt_publish_up, &OnRealtimeGrant};
    cloud_json_set_hooks(&hooks);

    const cloud_realtime_callbacks_t realtime = {
        &OnRealtimeState, &OnRealtimePcm, &OnRealtimeText, &OnRealtimeEnd, &OnRealtimeReset,
        &OnRealtimeFinished,
    };
    cloud_realtime_set_callbacks(&realtime);
    if (cloud_realtime_start() != ESP_OK) {
        ESP_LOGE(TAG, "realtime worker failed to start");
    }
    const esp_err_t r = cloud_session_start(&cc);
    if (r != ESP_OK) {
        cloud_realtime_stop();
        ESP_LOGE(TAG, "cloud session failed to start: %s", esp_err_to_name(r));
        return;
    }
    s_cloud_started = true;
}

// agent_transport_t interface
esp_err_t wifi_start(void* /*impl*/) {
    {
        std::lock_guard<std::mutex> lock(s_wifi_mtx);
        esp_err_t r = WifiInitOnce();
        if (r != ESP_OK) return r;
        (void)s_on_conn; (void)s_on_stream;

        char ssid[33] = {0}, pass[65] = {0};
        if (s_cfg && s_cfg->ssid && s_cfg->ssid[0]) {
            StartSta(s_cfg->ssid, s_cfg->password ? s_cfg->password : "");
        } else if (LoadCreds(ssid, sizeof ssid, pass, sizeof pass)) {
            ESP_LOGI(TAG, "using stored WiFi credentials");
            StartSta(ssid, pass);
        } else {
            ESP_LOGI(TAG, "no WiFi credentials — starting captive-portal provisioning");
            StartProvisioning();
        }
        r = esp_timer_start_periodic(s_tick_timer, 250000);
        if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;
    }

    StartCloudSession();
    ReportStatus();
    return ESP_OK;
}

void wifi_stop(void* /*impl*/) {
    {
        std::lock_guard<std::mutex> lock(s_wifi_mtx);
        s_want_connect = false;
        s_join.Stop();
        s_portal_close_at = 0;
        s_phase = Phase::kIdle;
        UpdateJoinProgress();
        if (s_tick_timer) esp_timer_stop(s_tick_timer);
        al_wifi_prov_stop();
        if (s_started) { esp_wifi_disconnect(); esp_wifi_stop(); s_started = false; }
    }
    s_realtime_wanted.store(false);
    s_platform_voice.store(false);
    cloud_session_stop();
    cloud_realtime_stop();
    s_cloud_started = false;
    if (s_link.exchange(AGENT_STATE_DISCONNECTED) != AGENT_STATE_DISCONNECTED && s_on_state) {
        std::lock_guard<std::recursive_mutex> lk(s_core_mtx);
        s_on_state(AGENT_STATE_DISCONNECTED);
    }
}

// What MQTT does not carry yet is refused. Each caller logs once, since callers retry.
bool SayOnce(bool& said, const char* what, const char* why) {
    if (!said) {
        said = true;
        ESP_LOGW(TAG, "%s dropped: %s", what, why);
    }
    return false;
}

// Responses to the command being run, and the manifest a 0x34 produces, go back through the JSON
// layer to `up`. Events are not published yet.
esp_err_t wifi_send_ctrl(void* /*impl*/, const uint8_t* frame, size_t len) {
    const esp_err_t r = cloud_json_on_frame(frame, len);
    if (r == ESP_ERR_NOT_SUPPORTED) {
        static bool said = false;
        SayOnce(said, "event", "events are not published over WiFi yet");
    }
    return r;
}
esp_err_t wifi_stream_start(void* /*impl*/, agent_stream_t type, const uint8_t* /*meta*/, size_t /*meta_len*/) {
    return cloud_realtime_stream_start(type);
}
esp_err_t wifi_send_stream(void* /*impl*/, agent_stream_t type, const uint8_t* data, size_t len) {
    return cloud_realtime_stream_write(type, data, len);
}
esp_err_t wifi_stream_end(void* /*impl*/, agent_stream_t type, bool complete,
                          const uint8_t* /*meta*/, size_t /*meta_len*/) {
    return cloud_realtime_stream_end(type, complete);
}

// Whether voice can be streamed, rather than just MQTT being online.
bool wifi_is_ready(void* /*impl*/) {
    return s_link.load(std::memory_order_acquire) == AGENT_STATE_READY && cloud_realtime_ready();
}

agent_transport_t s_wifi = {
    wifi_start, wifi_stop, wifi_send_ctrl,
    wifi_stream_start, wifi_send_stream, wifi_stream_end,
    wifi_is_ready, nullptr, nullptr,
};
}  // namespace

extern "C" agent_transport_t* agent_transport_wifi(void) { return &s_wifi; }

esp_err_t agent_wifi_local_end() {
    {
        std::lock_guard<std::mutex> lock(s_wifi_mtx);
        if (!s_local_radio.exchange(false)) return ESP_OK;
        esp_wifi_stop(); s_started = false;
    }
    // The cloud task waits in NetReady(), so no stop/start task race or loss of
    // binding/session state is introduced by a brief visit to local mode.
    return wifi_start(nullptr);
}

esp_err_t agent_wifi_local_begin(uint8_t channel) {
    if (channel < 1 || channel > 11) return ESP_ERR_INVALID_ARG;
    esp_err_t result;
    {
        std::lock_guard<std::mutex> lock(s_wifi_mtx);
        if (!s_wifi_inited || s_local_radio.load()) return ESP_ERR_INVALID_STATE;
        s_local_radio.store(true);
        s_realtime_wanted.store(false); s_platform_voice.store(false);
        s_want_connect = false; s_join.Stop(); s_phase = Phase::kIdle;
        s_portal_close_at = 0; UpdateJoinProgress();
        if (s_tick_timer) esp_timer_stop(s_tick_timer);
        al_wifi_prov_stop();
        if (s_started) { esp_wifi_disconnect(); esp_wifi_stop(); s_started = false; }
        result = esp_wifi_set_mode(WIFI_MODE_STA);
        if (result == ESP_OK) result = esp_wifi_start();
        if (result == ESP_OK) { s_started = true; result = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE); }
    }
    cloud_realtime_cancel();
    UpdateLink();
    if (result != ESP_OK) (void)agent_wifi_local_end();
    return result;
}

extern "C" esp_err_t agent_link_wifi_setup(void) {
    if (!s_wifi_inited || s_phase == Phase::kIdle) return ESP_ERR_INVALID_STATE;
    return esp_event_post(kWifiControl, kWifiSetup, nullptr, 0, pdMS_TO_TICKS(100));
}
extern "C" esp_err_t agent_link_wifi_info(agent_link_wifi_info_t* info) {
    if (!info) return ESP_ERR_INVALID_ARG;
    *info = {};
    std::lock_guard<std::mutex> lock(s_wifi_mtx);
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;
    info->setup_active = al_wifi_prov_active();
    snprintf(info->setup_ssid, sizeof info->setup_ssid, "%s", s_ap_ssid);
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        info->connected = true; info->rssi = ap.rssi;
        memcpy(info->ssid, ap.ssid, sizeof ap.ssid); info->ssid[32] = 0;
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip) == ESP_OK)
            snprintf(info->ip, sizeof info->ip, IPSTR, IP2STR(&ip.ip));
    }
    return ESP_OK;
}

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
extern "C" void agent_transport_wifi_set_audio(void (*pcm)(const uint8_t*, size_t), void (*end)(void),
                                               void (*reset)(void), void (*text)(const char*)) {
    s_pcm = pcm; s_audio_end = end; s_audio_reset = reset; s_text = text;
}

extern "C" esp_err_t agent_transport_wifi_request_realtime(void) {
    if (s_link == AGENT_STATE_READY) return ESP_OK;
    const bool direct = cloud_realtime_direct_enabled();
    if (s_link.load(std::memory_order_acquire) != AGENT_STATE_CONNECTED || (!direct && !cloud_mqtt_started())) {
        return ESP_ERR_INVALID_STATE;
    }
    s_realtime_wanted.store(true);
    OnRealtimeState(false, direct ? "Requesting voice access" : "Waiting for voice gateway");
    const esp_err_t r = direct ? cloud_realtime_request_direct() : cloud_json_request_realtime();
    if (r != ESP_OK) {
        s_realtime_wanted.store(false);
        OnRealtimeState(false, "Voice request failed. Tap Connect to retry");
    }
    return r;
}

extern "C" esp_err_t agent_transport_wifi_request_realtime_session(int agent_id, const char* session_id) {
    // This path obtains a grant over authenticated HTTP; only the legacy
    // cloud_json_request_realtime() path above needs an MQTT connection.
    if (!cloud_platform_enabled() ||
        s_link.load(std::memory_order_acquire) != AGENT_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    if (s_realtime_wanted.exchange(true)) return ESP_ERR_INVALID_STATE;
    s_platform_voice.store(true);
    OnRealtimeState(false, "Requesting voice access");
    const esp_err_t r = cloud_realtime_request_platform(agent_id, session_id);
    if (r != ESP_OK) {
        s_realtime_wanted.store(false);
        s_platform_voice.store(false);
        OnRealtimeState(false, "Voice request failed. Tap Connect to retry");
    }
    return r;
}

extern "C" bool agent_transport_wifi_realtime_running(void) {
    return s_realtime_wanted.load(std::memory_order_acquire);
}

extern "C" bool agent_transport_wifi_realtime_recovering(void) {
    return s_realtime_wanted.load(std::memory_order_acquire) && cloud_realtime_recovering();
}

extern "C" esp_err_t agent_transport_wifi_end_realtime(void) {
    s_realtime_wanted.store(false);
    s_platform_voice.store(false);
    cloud_realtime_cancel();
    OnRealtimeReset();
    OnRealtimeState(false, "Connected to the platform");
    return ESP_OK;
}


#else  // !SOC_WIFI_SUPPORTED

// Stub for targets or builds without this backend.
#include "agent_link_transport.h"
#include "esp_log.h"

namespace {
constexpr const char* TAG = "agent_link.wifi";

esp_err_t nowifi_start(void*) {
    ESP_LOGE(TAG, "WiFi transport is unavailable in this build");
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
    nowifi_ready, nullptr, nullptr,
};
}  // namespace

extern "C" agent_transport_t* agent_transport_wifi(void) { return &s_wifi; }
extern "C" esp_err_t agent_transport_wifi_request_realtime_session(int, const char*) {
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" void agent_transport_wifi_set_config(const agent_wifi_config_t*) {}
extern "C" void agent_transport_wifi_set_name(const char*) {}
extern "C" void agent_transport_wifi_set_recv(void (*)(const uint8_t*, size_t)) {}
extern "C" void agent_transport_wifi_set_conn(void (*)(bool)) {}
extern "C" void agent_transport_wifi_set_stream_recv(void (*)(agent_stream_t, const uint8_t*, size_t)) {}
extern "C" void agent_transport_wifi_set_state(void (*)(agent_state_t)) {}
extern "C" void agent_transport_wifi_set_platform(const agent_platform_t*) {}
extern "C" void agent_transport_wifi_set_status(void (*)(const agent_link_status_t*)) {}
extern "C" esp_err_t agent_transport_wifi_forget(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void agent_transport_wifi_set_audio(void (*)(const uint8_t*, size_t), void (*)(void),
                                               void (*)(void), void (*)(const char*)) {}
extern "C" esp_err_t agent_transport_wifi_request_realtime(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_transport_wifi_end_realtime(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool agent_transport_wifi_realtime_running(void) { return false; }
extern "C" bool agent_transport_wifi_realtime_recovering(void) { return false; }

#endif
