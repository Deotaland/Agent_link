// agent_link core — transport-agnostic capability routing + lifecycle.
//
// One API, two transports: init selects BLE or WiFi backend (s_tx) based on cfg.transport,
// and both wire the upstream callbacks to the same OnCtrlFrame/OnConn, so upper-layer
// push_*/on_* are transport-independent.
//
// Control plane (already wired):
//   - start()   -> s_tx->start() (BLE: NimBLE advertising + GATT Service C 0xFFC0).
//   - downlink commands -> transport receives write -> OnCtrlFrame decodes ->
//     the SDK's built-ins, else the board's on_command, else a 1001 answer.
//   - uplink events -> report_battery etc. -> protocol frames -> s_tx->send_ctrl
//     (notify 0xFFC4 events / 0xFFC1 responses).
//   - connection state -> OnConn -> on_state.
// Data plane (partially wired):
//   - voice uplink push_voice/voice_end -> s_tx->stream_start/send_stream/stream_end.
//     BLE backend: GATT Notify 0xFFA1, event 0x40 VoiceChunk (see transport_ble.cpp voice uploader).
//   - generic I/O (register_io/push_reading/actuate) — self-describing manifest (event 0x18),
//     reading reports (event 0x19), actuator downlink (command 0x33), manifest fetch (command 0x34).
// Not yet carried by any backend: AGENT_STREAM_FILE (BLE L2CAP) and AGENT_STREAM_VIDEO (WiFi).
#include "agent_link.h"
#include "agent_link_ota.h"
#include "audio_downlink.h"
#include "agent_link_transport.h"
#include "ota_service.h"
#include "protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

// Definition of the type agent_link_stream.h forward-declares. Global scope on purpose: an
// anonymous-namespace copy would be a different type from the one in the public handle.
struct agent_stream_session {
    agent_stream_t kind;
    bool           open;
};

namespace {
constexpr const char* TAG = "agent_link";

agent_link_config_t s_cfg   = {};
agent_output_cb_t   s_out   = {};
bool                s_have_out = false;
agent_state_t       s_state = AGENT_STATE_DISCONNECTED;
agent_transport_t*  s_tx    = nullptr;
// One slot per agent_stream_t. The handle a caller holds is the address of its slot, which is why
// reopening a kind is harmless and why a disconnect can invalidate every stream at once.
agent_stream_session s_streams[AGENT_STREAM_KIND_COUNT] = {
    {AGENT_STREAM_VOICE, false}, {AGENT_STREAM_AUDIO, false}, {AGENT_STREAM_IMAGE, false},
    {AGENT_STREAM_VIDEO, false}, {AGENT_STREAM_FILE,  false},
};

// The SDK only notifies when the battery level changes / charging state changes / a
// low-battery edge occurs. The cache is updated only on a successful send; a failed
// send is retried on the next call.
int  s_bat_pct = -1;          // last successfully pushed battery level (-1 = unknown: force a resend after connect for initial sync)
int  s_bat_chg = -1;          // last successfully pushed charging state (-1 = unknown; otherwise 0/1)
bool s_bat_low_armed = true;  // low-battery edge armed (true = not yet triggered)
constexpr uint8_t kBatLowThreshold = 20;  // low-battery threshold (%)

// Generic I/O endpoint registry (manifest + reading reports + downlink actuators).
// Downlink actuator commands are routed by id to the matching cb.
constexpr int kMaxIo = 32;
struct IoEntry {
    const agent_link_io_desc_t* desc;
    agent_io_actuate_cb_t       cb;
    void*                       ctx;
    // Reading cache + reporting policy (see 0x35 GetReading / 0x36 SetReadingConfig).
    uint8_t  policy_mode;      // 0=passthrough(default) 1=off 2=periodic(rate) 3=on-change
    uint16_t policy_rate_hz;   // target rate when policy_mode == periodic
    int64_t  last_sent_us;     // last time a reading was actually sent (for throttling)
    int64_t  last_ts_us;       // last time push_reading was called (for 0x35 age_ms)
    uint8_t  last_val[16];     // cached last value (fixed-size types <= 16B; for 0x35 + dedup)
    uint8_t  last_len;         // cached value length (0 = none cached)
    bool     has_last;         // whether last_val / last_ts_us are valid
};

IoEntry s_io[kMaxIo] = {};
int     s_io_count = 0;
bool    s_manifest_sent = false;  // whether the manifest has been sent in this connection
uint32_t s_manifest_rev = 1;      // manifest revision; bumped by agent_link_notify_manifest_changed()

// Per-endpoint reporting policy values (0x36 SetReadingConfig). Default 0 = passthrough so a
// zero-initialized IoEntry keeps the pre-existing "forward every push_reading" behavior.
enum { IO_POLICY_PASSTHROUGH = 0, IO_POLICY_OFF = 1, IO_POLICY_PERIODIC = 2, IO_POLICY_ONCHANGE = 3 };

// Single exit point for data-plane calls made before the link is usable.
esp_err_t not_ready(const char* what) {
    if (s_tx && s_tx->is_ready && s_tx->is_ready(s_tx->impl)) {
        ESP_LOGD(TAG, "%s: transport ready but not wired yet (data plane TODO)", what);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGD(TAG, "%s: link not ready — ignored", what);
    return ESP_ERR_INVALID_STATE;
}

// ── Generic I/O: manifest serialization + reading/actuator routing ──────────────
// Look up an endpoint by id.
const IoEntry* FindIo(const char* id) {
    if (!id) return nullptr;
    for (int i = 0; i < s_io_count; ++i)
        if (s_io[i].desc && s_io[i].desc->id && strcmp(s_io[i].desc->id, id) == 0)
            return &s_io[i];
    return nullptr;
}

// Look up an endpoint index by id (-1 if not found); used where the entry must be mutated.
int FindIoIndex(const char* id) {
    if (!id) return -1;
    for (int i = 0; i < s_io_count; ++i)
        if (s_io[i].desc && s_io[i].desc->id && strcmp(s_io[i].desc->id, id) == 0)
            return i;
    return -1;
}

// Fixed byte size of a value type; BLOB is variable-length and returns -1. Used for push_reading length checks.
int IoValueSize(agent_val_t t) {
    switch (t) {
    case AGENT_VAL_BOOL: return 1;
    case AGENT_VAL_U16:  return 2;
    case AGENT_VAL_I32:  return 4;
    case AGENT_VAL_F32:  return 4;
    case AGENT_VAL_RGB:  return 4;
    case AGENT_VAL_VEC2: return 8;
    case AGENT_VAL_VEC3: return 12;
    case AGENT_VAL_BLOB: return -1;
    case AGENT_VAL_STR:  return -1;
    }
    return -1;
}

// Value type -> the type string used in the manifest.
const char* ValTypeName(agent_val_t t) {
    switch (t) {
    case AGENT_VAL_BOOL: return "bool";
    case AGENT_VAL_U16:  return "u16";
    case AGENT_VAL_I32:  return "i32";
    case AGENT_VAL_F32:  return "f32";
    case AGENT_VAL_RGB:  return "rgb";
    case AGENT_VAL_VEC2: return "vec2";
    case AGENT_VAL_VEC3: return "vec3";
    case AGENT_VAL_BLOB: return "blob";
    case AGENT_VAL_STR:  return "str";
    }
    return "blob";
}

// Escape a string for JSON (UTF-8 multibyte bytes >= 0x80 pass through unchanged, which is valid JSON).
void JsonEsc(std::string& out, const char* s) {
    if (!s) return;
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += static_cast<char>(c);
        }
    }
}

// ── D1: rich outputs (LED / haptic / screen text) exposed as generic OUT endpoints ──
// The rich callbacks (on_led / on_haptic / on_show_text) had no downlink command that reached
// them. The SDK now auto-registers a synthetic OUT endpoint for each rich capability the board
// actually wired, so the Agent drives them through the same 0x33 IoActuate path as any actuator.
// Board code is unchanged — it just keeps providing the callbacks.
void SynthLedCb(const char* /*id*/, const uint8_t* args, size_t len, void* /*ctx*/) {
    if (len < 4 || !(s_have_out && s_out.on_led)) return;
    const uint32_t rgb = static_cast<uint32_t>(args[0]) | (static_cast<uint32_t>(args[1]) << 8) |
                         (static_cast<uint32_t>(args[2]) << 16) | (static_cast<uint32_t>(args[3]) << 24);
    s_out.on_led(rgb, s_out.ctx);
}
void SynthHapticCb(const char* /*id*/, const uint8_t* args, size_t len, void* /*ctx*/) {
    if (len < 2 || !(s_have_out && s_out.on_haptic)) return;
    const uint32_t ms = static_cast<uint32_t>(args[0]) | (static_cast<uint32_t>(args[1]) << 8);
    s_out.on_haptic(ms, s_out.ctx);
}
void SynthScreenCb(const char* /*id*/, const uint8_t* args, size_t len, void* /*ctx*/) {
    if (!(s_have_out && s_out.on_show_text)) return;
    char buf[256];
    const size_t n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, args, n);
    buf[n] = '\0';
    s_out.on_show_text(buf, s_out.ctx);
}

// Static descriptors (register_io stores the pointer, so these must outlive the call).
const agent_link_io_desc_t kSynthLed = {
    .id = "led0", .dir = AGENT_IO_OUT, .kind = "led", .value = AGENT_VAL_RGB,
    .desc = "status LED color, 0x00RRGGBB",
};
const agent_link_io_desc_t kSynthHaptic = {
    .id = "motor0", .dir = AGENT_IO_OUT, .kind = "haptic", .value = AGENT_VAL_U16,
    .unit = "ms", .desc = "vibration motor, duration in ms",
};
const agent_link_io_desc_t kSynthScreen = {
    .id = "screen0", .dir = AGENT_IO_OUT, .kind = "screen.text", .value = AGENT_VAL_STR,
    .desc = "text display",
};

// Register synthetic endpoints for whichever rich outputs the board wired + advertised via caps.
void RegisterSyntheticEndpoints() {
    if (!s_have_out) return;
    if ((s_cfg.caps & AGENT_CAP_LED)    && s_out.on_led       && !FindIo("led0"))
        agent_link_register_io(&kSynthLed, SynthLedCb, nullptr);
    if ((s_cfg.caps & AGENT_CAP_HAPTIC) && s_out.on_haptic    && !FindIo("motor0"))
        agent_link_register_io(&kSynthHaptic, SynthHapticCb, nullptr);
    if ((s_cfg.caps & AGENT_CAP_SCREEN) && s_out.on_show_text && !FindIo("screen0"))
        agent_link_register_io(&kSynthScreen, SynthScreenCb, nullptr);
}

// Serialize the manifest as an object envelope: {proto, rev, caps, io:[...]}.
std::string BuildManifestJson() {
    std::string s = "{\"proto\":";
    s += std::to_string(AGENT_LINK_PROTO_VERSION);
    s += ",\"rev\":";  s += std::to_string(s_manifest_rev);
    s += ",\"caps\":"; s += std::to_string(static_cast<unsigned>(s_cfg.caps));
    s += ",\"io\":[";
    bool first = true;
    for (int i = 0; i < s_io_count; ++i) {
        const agent_link_io_desc_t* d = s_io[i].desc;
        if (!d || !d->id || !d->kind) continue;
        if (!first) s += ",";
        first = false;
        s += "{\"id\":\"";     JsonEsc(s, d->id);
        s += "\",\"dir\":\"";  s += (d->dir == AGENT_IO_OUT ? "out" : "in");
        s += "\",\"kind\":\""; JsonEsc(s, d->kind);
        s += "\",\"value\":\""; s += ValTypeName(d->value); s += "\"";
        if (d->unit && d->unit[0])                 { s += ",\"unit\":\""; JsonEsc(s, d->unit); s += "\""; }
        if (d->desc && d->desc[0])                 { s += ",\"desc\":\""; JsonEsc(s, d->desc); s += "\""; }
        if (d->display_name && d->display_name[0]) { s += ",\"name\":\""; JsonEsc(s, d->display_name); s += "\""; }
        if (d->range_min != d->range_max) {
            char b[64];
            snprintf(b, sizeof b, ",\"range\":[%g,%g]",
                     static_cast<double>(d->range_min), static_cast<double>(d->range_max));
            s += b;
        }
        if (d->rate_hz) {
            char b[32];
            snprintf(b, sizeof b, ",\"rate_hz\":%u", static_cast<unsigned>(d->rate_hz));
            s += b;
        }
        if (d->event == AGENT_EVT_ON_CHANGE)       s += ",\"event\":\"change\"";
        else if (d->event == AGENT_EVT_THRESHOLD)  s += ",\"event\":\"threshold\"";
        if (d->audience == AGENT_AUD_USER)         s += ",\"audience\":\"user\"";
        if (d->enum_json && d->enum_json[0])       { s += ",\"enum\":"; s += d->enum_json; }
        if (d->default_json && d->default_json[0]) { s += ",\"default\":"; s += d->default_json; }
        if (d->dir == AGENT_IO_OUT && d->args_schema && d->args_schema[0]) {
            s += ",\"args\":"; s += d->args_schema;   // args_schema is itself JSON
        }
        s += "}";
    }
    s += "]}";
    return s;
}

// Send the manifest, fragmented into 0x18 IoManifest events over the control plane (send_ctrl).
//   Each chunk payload = [chunk_idx(1)] [last(1: 0/1)] [json fragment...]; the App concatenates
//   chunks in order until last=1, then parses the whole JSON.
// Triggered: after the BLE peer subscribes to 0xFFC4 (OnLinkReady) / after WiFi connects (OnConn) /
//   on the App's 0x34 fetch command.
void SendManifest(bool force) {
    if (s_io_count == 0) return;                    // no endpoints: no manifest
    if (!force && s_manifest_sent) return;          // already sent in this connection
    if (!s_tx || !s_tx->send_ctrl) return;

    const std::string json = BuildManifestJson();

    // Chunk budget: a BLE notify value is <= ATT_MTU-3, minus 6 (frame header) + 2 (chunk header). WiFi has no such limit.
    size_t budget;
    if (s_cfg.transport == AGENT_TRANSPORT_WIFI) {
        budget = 1024;
    } else {
        const uint16_t mtu = agent_transport_ble_att_mtu();
        budget = (mtu > 3 + 8 + 16) ? static_cast<size_t>(mtu - 3 - 8) : 150;
    }
    if (budget < 16)  budget = 16;
    if (budget > 480) budget = 480;

    const uint8_t* jb    = reinterpret_cast<const uint8_t*>(json.data());
    const size_t   total = json.size();
    size_t  off = 0;
    uint8_t idx = 0;
    bool    ok  = true;
    do {
        size_t n = total - off;
        if (n > budget) n = budget;
        std::vector<uint8_t> p;
        p.reserve(2 + n);
        p.push_back(idx);
        p.push_back((off + n >= total) ? 0x01 : 0x00);   // last flag
        p.insert(p.end(), jb + off, jb + off + n);
        auto ev = agentlink::BuildEvent(0x18, p.data(), p.size());
        if (s_tx->send_ctrl(s_tx->impl, ev.data(), ev.size()) != ESP_OK) { ok = false; break; }
        off += n;
        ++idx;
    } while (off < total);

    if (ok) {
        s_manifest_sent = true;
        ESP_LOGI(TAG, "manifest sent: %d endpoint(s), %uB in %u chunk(s)",
                 s_io_count, static_cast<unsigned>(total), static_cast<unsigned>(idx));
    } else {
        ESP_LOGW(TAG, "manifest send failed at chunk %u — App can re-fetch via command 0x34",
                 static_cast<unsigned>(idx));
    }
}

// BLE: fired after the peer subscribes to the event channel 0xFFC4 (notifications are only
// delivered from that point on) -> send the manifest.
void OnLinkReady() { SendManifest(/*force=*/false); }

// ── Device identity (0x01 RequestDeviceInfo) ───────────────────────────────────
// A 16-byte identifier that survives reflashing, so the App keeps recognising the unit across
// an OTA. Random on first boot, then kept in NVS
// which stores a UUID v4 and puts its 16 raw bytes on the wire.
constexpr const char* kNvsNamespace = "agent_link";
constexpr const char* kNvsKeyUuid   = "dev_uuid";
uint8_t s_device_uuid[16] = {};
bool    s_device_uuid_ready = false;

void EnsureDeviceUuid() {
    if (s_device_uuid_ready) return;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(s_device_uuid);
        if (nvs_get_blob(h, kNvsKeyUuid, s_device_uuid, &len) == ESP_OK && len == sizeof(s_device_uuid)) {
            s_device_uuid_ready = true;
        } else {
            esp_fill_random(s_device_uuid, sizeof(s_device_uuid));
            s_device_uuid[6] = static_cast<uint8_t>((s_device_uuid[6] & 0x0F) | 0x40);  // UUID v4
            s_device_uuid[8] = static_cast<uint8_t>((s_device_uuid[8] & 0x3F) | 0x80);  // RFC 4122 variant
            if (nvs_set_blob(h, kNvsKeyUuid, s_device_uuid, sizeof(s_device_uuid)) == ESP_OK) nvs_commit(h);
            s_device_uuid_ready = true;
            ESP_LOGI(TAG, "generated device UUID (persisted in NVS)");
        }
        nvs_close(h);
        return;
    }
    // NVS unavailable: fall back to the MAC so the field is still stable for this boot.
    esp_fill_random(s_device_uuid, sizeof(s_device_uuid));
    esp_read_mac(s_device_uuid, ESP_MAC_BT);
    s_device_uuid_ready = true;
    ESP_LOGW(TAG, "NVS unavailable — device UUID is not persistent this boot");
}

const char* FirmwareVersion() {
    return (s_cfg.firmware_rev && *s_cfg.firmware_rev) ? s_cfg.firmware_rev
                                                       : agent_link_ota_running_version();
}

const char* DeviceModel() {
    return (s_cfg.model && *s_cfg.model) ? s_cfg.model : s_cfg.device_name;
}

// 0x01 extra_data:
//   uuid(16) mac(6) battery(1) voltage_mv(2 LE) wifi_state(1)
//   ver_len(1) version  profile_len(1) profile  model_len(1) model
size_t BuildDeviceInfo(uint8_t* out, size_t cap) {
    EnsureDeviceUuid();
    const char* ver   = FirmwareVersion();
    const char* model = DeviceModel();
    const size_t ver_len   = strnlen(ver, 255);
    const size_t model_len = strnlen(model, 255);
    const size_t need = 16 + 6 + 1 + 2 + 1 + 1 + ver_len + 1 + 1 + model_len;
    if (cap < need) return 0;

    size_t k = 0;
    memcpy(out + k, s_device_uuid, 16); k += 16;
    uint8_t mac[6] = {};
    if (!agent_transport_ble_get_mac(mac)) esp_read_mac(mac, ESP_MAC_BT);  // MSB-first either way
    memcpy(out + k, mac, 6); k += 6;
    out[k++] = (s_bat_pct >= 0 && s_bat_pct <= 100) ? static_cast<uint8_t>(s_bat_pct) : 0;
    out[k++] = 0; out[k++] = 0;   // voltage_mv (LE) — not measured
    out[k++] = 0;                 // wifi_state — 0 = Idle (no WiFi file server in this SDK)
    out[k++] = static_cast<uint8_t>(ver_len);
    memcpy(out + k, ver, ver_len); k += ver_len;
    out[k++] = 0;                 // profile_len — no agent profile selected
    out[k++] = static_cast<uint8_t>(model_len);
    memcpy(out + k, model, model_len); k += model_len;
    return k;
}

// OTA -> App: the engine hands us an event id + payload; we frame it and push it out on the
// event channel. Keeping the transport out of ota_service.cpp is what makes the engine work
// unchanged over BLE today and over WiFi once that data plane exists.
void OtaEventSink(uint8_t event_id, const uint8_t* payload, size_t len) {
    if (!s_tx || !s_tx->send_ctrl) return;
    auto ev = agentlink::BuildEvent(event_id, payload, len);
    // Terminal phases (Success/Failed) must not be lost — the App would sit on a stale progress
    // bar — and by then the firmware stream has stopped competing for buffers, so retry a little.
    const bool terminal = (event_id == agentlink::ota::kEvtOtaProgress && len >= 1 &&
                           (payload[0] == 2 || payload[0] == 3));
    for (int attempt = 0; attempt < (terminal ? 3 : 1); ++attempt) {
        if (s_tx->send_ctrl(s_tx->impl, ev.data(), ev.size()) == ESP_OK) return;
        if (terminal) vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// 0x37 StartOtaUpgrade — payload: fw_size(4 LE) + sha256(32) + ver_len(1) + version
//                                 [+ model_len(1) + model]   (model appended by newer Apps)
// The firmware itself arrives on the data channel (BLE: L2CAP CoC PSM 0x0081) right after the
// ACK. Everything here is a cheap synchronous check so that ACK goes out immediately; the
// 2-4s partition erase happens in the OTA worker.
void HandleStartOta(const std::vector<uint8_t>& pl, uint8_t* status, uint16_t* error) {
    if (pl.size() < 38) {                       // 4 + 32 + 1 + at least one version char
        *status = 1; *error = agentlink::ota::kErrInvalidPayload;
        return;
    }
    uint32_t fw_size = static_cast<uint32_t>(pl[0]) | (static_cast<uint32_t>(pl[1]) << 8) |
                       (static_cast<uint32_t>(pl[2]) << 16) | (static_cast<uint32_t>(pl[3]) << 24);
    const uint8_t* sha256 = pl.data() + 4;
    const uint8_t ver_len = pl[36];
    if (ver_len == 0 || pl.size() < 37u + ver_len) {
        *status = 1; *error = agentlink::ota::kErrInvalidPayload;
        return;
    }
    char version[32] = {};
    memcpy(version, pl.data() + 37, ver_len < sizeof(version) ? ver_len : sizeof(version) - 1);

    char model[40] = {};
    const size_t after_ver = 37u + ver_len;
    if (pl.size() > after_ver) {                // optional trailing model section
        const uint8_t model_len = pl[after_ver];
        if (model_len == 0 || pl.size() != after_ver + 1u + model_len) {
            *status = 1; *error = agentlink::ota::kErrInvalidPayload;
            return;
        }
        memcpy(model, pl.data() + after_ver + 1,
               model_len < sizeof(model) ? model_len : sizeof(model) - 1);
    }

    // The firmware bytes need somewhere to arrive. Today that is the BLE L2CAP channel only:
    // the WiFi backend has no data plane yet, so an OTA there would just sit and time out.
    if (s_cfg.transport == AGENT_TRANSPORT_WIFI) {
        ESP_LOGW(TAG, "0x37 rejected: OTA needs the BLE data channel (WiFi data plane is TODO)");
        *status = 1; *error = agentlink::ota::kErrBusinessFailed;
        return;
    }
    if (!agent_transport_ble_l2cap_ready()) {
        ESP_LOGW(TAG, "0x37 rejected: App has not opened L2CAP PSM 0x0081");
        *status = 1; *error = agentlink::ota::kErrBusinessFailed;
        return;
    }

    uint16_t err = 0;
    if (!agentlink::ota::Start(fw_size, sha256, version, model[0] ? model : nullptr, &err)) {
        *status = 1; *error = err;
    }
}

// Transport -> core: connection state change.
// Everything that becomes meaningless the moment the peer is gone. Shared by the disconnect path
// and by agent_link_stop(), which otherwise left stream slots marked open against sessions the
// transport had already torn down — the next write would target a session nobody remembers.
void ResetLinkState() {
    s_manifest_sent = false;
    for (auto& st : s_streams) st.open = false;
}

void OnConn(bool connected) {
    // The protocol is plaintext with no encryption gate, so the link is usable as soon as it connects -> go READY.
    s_state = connected ? AGENT_STATE_READY : AGENT_STATE_DISCONNECTED;
    // Cleared on connect too, not just disconnect: the manifest is re-sent after the peer
    // subscribes (BLE) or connects (WiFi), and any stream slot left over from a previous peer
    // names a session this one has never heard of.
    ResetLinkState();
    if (!connected) {
        // nothing further: ResetLinkState already dropped the stale sessions
    } else {
        // Clear the battery cache on connect so the next report_battery force-resends the current value (initial sync for the App).
        s_bat_pct = -1; s_bat_chg = -1; s_bat_low_armed = true;
        // WiFi: notifications work as soon as the WS connects -> send the manifest immediately.
        // BLE: must wait for the peer to subscribe to 0xFFC4; OnLinkReady sends it (otherwise the notify is dropped).
        if (s_cfg.transport == AGENT_TRANSPORT_WIFI) SendManifest(/*force=*/false);
    }
    // Link up confirms a freshly OTA'd image (the link is the only upgrade path, so it is the
    // self-test that matters); link down aborts an upgrade that was mid-flight.
    agentlink::ota::OnLinkState(connected);
    // Downlink audio: link down disarms the session and resets the codec to raw PCM, which is
    // the documented per-connection default.
    agentlink::audio::OnLinkState(connected);
    if (s_cfg.on_state) s_cfg.on_state(s_state, s_cfg.state_ctx);
}

// Transport -> core: a control frame arrived (peer wrote the command channel 0xFFC1).
void OnCtrlFrame(const uint8_t* data, size_t len) {
    agentlink::Frame f;
    if (!agentlink::ParseFrame(data, len, f)) {
        ESP_LOGW(TAG, "bad ctrl frame (%u bytes)", static_cast<unsigned>(len));
        return;
    }
    if (f.msg_type != agentlink::kMsgCommand) return;  // the device side only handles "commands"

    ESP_LOGD(TAG, "cmd 0x%02X seq=%u payload=%uB",
             f.command_id, f.sequence, static_cast<unsigned>(f.payload.size()));

    // ── Command dispatch + response (may carry data) ────────────────────────────
    // Priority: SDK built-ins > the board's on_command > 1001 UnknownCommand. Nothing fabricates
    // a success for a command no layer implements.
    uint8_t  extra[128];
    size_t   extra_len = 0;
    uint8_t  status = 0;   // 0 = success
    uint16_t error  = 0;

    if (f.command_id == 0x01) {
        // 0x01 RequestDeviceInfo: identity + battery + firmware version + model. The App reads the
        // version and model from here to decide whether it has an OTA to offer this unit, so it is
        // Payload is ignored.
        extra_len = BuildDeviceInfo(extra, sizeof(extra));
        if (extra_len == 0) { status = 1; error = 1005; }   // response buffer too small
    } else if (f.command_id == 0x37) {
        // 0x37 StartOtaUpgrade (written on 0xFFB1 by the App; 0xFFC1 works too).
        HandleStartOta(f.payload, &status, &error);
    } else if (f.command_id == 0x56) {
        // 0x56 AbortOtaUpgrade: cooperative. The ACK only means "asked"; the App should wait for
        // the 0x38 event with phase=Failed error=2035 before calling the upgrade cancelled.
        if (!f.payload.empty()) { status = 1; error = 1004; }
        else                    agentlink::ota::Abort("App requested");
    } else if (f.command_id == 0x05) {
        // 0x05 VoiceReply: payload = session_id(4) + status(1) [+ downlink_codec(1)].
        //   status=2 -> an audio reply follows on the data channel; arm the downlink stage.
        //   status=3 -> the App finished pushing; tell the board, release the App from flow control.
        // The optional 6th byte picks the wire format of that audio (0 = raw PCM16, 1 = IMA-ADPCM)
        // and is sticky for the connection, which is what lets us decode whatever the App sends.
        if (f.payload.size() != 5 && f.payload.size() != 6) {
            status = 1; error = 1004;
        } else if (f.payload.size() == 6 && !agentlink::audio::SetCodec(f.payload[5])) {
            status = 1; error = 1004;          // downlink_codec > 1
        } else {
            switch (f.payload[4]) {
            case 2:
                agentlink::audio::Arm();
                break;
            case 3:
                agentlink::audio::Disarm();
                if (s_have_out && s_out.on_audio_end) s_out.on_audio_end(s_out.ctx);
                break;
            default:
                break;                         // 0 = success, 1 = failure: nothing audio-side
            }
        }
    } else if (f.command_id == 0x03) {
        // 0x03 GetChargingStatus: answered by the SDK from cached battery. extra = [charging_state(0/1), level(0-100)].
        // The battery cache comes from report_battery (the board polls every 5s); payload must be empty, otherwise 1004.
        if (f.payload.empty()) {
            extra[0] = (s_bat_chg == 1) ? 0x01 : 0x00;
            extra[1] = (s_bat_pct >= 0 && s_bat_pct <= 100) ? static_cast<uint8_t>(s_bat_pct) : 0;
            extra_len = 2;
        } else {
            status = 1; error = 1004;  // InvalidPayload
        }
    } else if (f.command_id == 0x33) {
        // 0x33 IoActuate: payload = [id_len(1)][id(UTF-8)][args...], routed by id to the registered actuator cb.
        const std::vector<uint8_t>& pl = f.payload;
        char io_id[64];
        if (pl.empty() || pl[0] == 0 || pl[0] >= sizeof(io_id) || 1u + pl[0] > pl.size()) {
            status = 1; error = 1004;  // InvalidPayload
        } else {
            const uint8_t id_len = pl[0];
            memcpy(io_id, pl.data() + 1, id_len);
            io_id[id_len] = '\0';
            const IoEntry* e = FindIo(io_id);
            if (e && e->desc->dir == AGENT_IO_OUT && e->cb) {
                const uint8_t* args     = pl.data() + 1 + id_len;
                const size_t   args_len = pl.size() - 1 - id_len;
                e->cb(io_id, args, args_len, e->ctx);   // implementation must return quickly; do not block the transport task
                ESP_LOGI(TAG, "actuate '%s' (%uB args)", io_id, static_cast<unsigned>(args_len));
            } else {
                status = 1; error = 1003;  // BusinessFailed: unknown or non-actuatable id
                ESP_LOGW(TAG, "actuate '%s': no matching OUT endpoint/callback", io_id);
            }
        }
    } else if (f.command_id == 0x34) {
        // 0x34 GetIoManifest: App-initiated fetch -> (re)send the manifest (0x18), then reply with the usual ACK.
        SendManifest(/*force=*/true);
    } else if (f.command_id == 0x35) {
        // 0x35 GetReading: payload = [id_len][id]; reply extra = [val_type][age_ms(2,LE)][value] from cache.
        const std::vector<uint8_t>& pl = f.payload;
        char io_id[64];
        if (pl.empty() || pl[0] == 0 || pl[0] >= sizeof(io_id) || 1u + pl[0] > pl.size()) {
            status = 1; error = 1004;  // InvalidPayload
        } else {
            memcpy(io_id, pl.data() + 1, pl[0]);
            io_id[pl[0]] = '\0';
            const int idx = FindIoIndex(io_id);
            if (idx < 0 || s_io[idx].desc->dir != AGENT_IO_IN) {
                status = 1; error = 1003;              // unknown / non-readable id
            } else if (!s_io[idx].has_last) {
                status = 1; error = 1005;              // NoData: nothing cached yet
            } else {
                const int64_t age = (esp_timer_get_time() - s_io[idx].last_ts_us) / 1000;
                const uint16_t age_ms = (age < 0) ? 0 : (age > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(age));
                size_t k = 0;
                extra[k++] = static_cast<uint8_t>(s_io[idx].desc->value);
                extra[k++] = age_ms & 0xFF;
                extra[k++] = (age_ms >> 8) & 0xFF;
                memcpy(extra + k, s_io[idx].last_val, s_io[idx].last_len);
                k += s_io[idx].last_len;
                extra_len = k;
            }
        }
    } else if (f.command_id == 0x36) {
        // 0x36 SetReadingConfig: payload = [id_len][id][mode(1)][rate_hz(2,LE)]. Handled entirely in the SDK.
        const std::vector<uint8_t>& pl = f.payload;
        char io_id[64];
        if (pl.empty() || pl[0] == 0 || pl[0] >= sizeof(io_id) ||
            static_cast<size_t>(2 + pl[0]) > pl.size()) {
            status = 1; error = 1004;
        } else {
            const uint8_t id_len = pl[0];
            memcpy(io_id, pl.data() + 1, id_len);
            io_id[id_len] = '\0';
            const uint8_t mode = pl[1 + id_len];
            uint16_t rate = 0;
            if (static_cast<size_t>(4 + id_len) <= pl.size())
                rate = static_cast<uint16_t>(pl[2 + id_len]) | (static_cast<uint16_t>(pl[3 + id_len]) << 8);
            const int idx = FindIoIndex(io_id);
            if (idx < 0 || s_io[idx].desc->dir != AGENT_IO_IN) { status = 1; error = 1003; }
            else if (mode > IO_POLICY_ONCHANGE)                { status = 1; error = 1004; }
            else {
                s_io[idx].policy_mode    = mode;
                s_io[idx].policy_rate_hz = rate;
                s_io[idx].last_sent_us   = 0;
                ESP_LOGI(TAG, "reading config '%s': mode=%u rate=%uHz", io_id, mode, static_cast<unsigned>(rate));
            }
        }
    } else if (f.command_id == 0x3C) {
        // 0x3C StartCapture: payload = [mode(1)][max_ms(2,LE)] -> on_listen(true, max_ms). Requires MIC + on_listen.
        if (!(s_cfg.caps & AGENT_CAP_MIC) || !(s_have_out && s_out.on_listen)) {
            status = 1; error = 1002;                  // NotCapable
        } else {
            uint32_t max_ms = 0;
            if (f.payload.size() >= 3)
                max_ms = static_cast<uint32_t>(f.payload[1]) | (static_cast<uint32_t>(f.payload[2]) << 8);
            s_out.on_listen(true, max_ms, s_out.ctx);
        }
    } else if (f.command_id == 0x3D) {
        // 0x3D StopCapture -> on_listen(false, 0).
        if (s_have_out && s_out.on_listen) s_out.on_listen(false, 0, s_out.ctx);
    } else if (s_have_out && s_out.on_command &&
               s_out.on_command(f.command_id, f.payload.data(), f.payload.size(),
                                extra, sizeof(extra), &extra_len, s_out.ctx)) {
        // handled by the device (extra may carry response data); status stays 0.
    } else {
        // Nobody implements this command: not the SDK, and not the board. Say so.
        //
        // The alternative — an empty success ACK for anything unrecognised — is strictly worse for
        // the App. Told "done", it proceeds as though the thing happened: it believes the setting
        // took, or starts streaming at a device that will do nothing with the data. Told 1001, it
        // knows the feature is absent and can degrade around it. A board that wants a command
        // implements it through on_command, which is consulted above and wins.
        status = 1; error = 1001;   // UnknownCommand
        ESP_LOGD(TAG, "cmd 0x%02X unimplemented -> 1001", f.command_id);
    }

    // Reply (request-response pairing); a query command carrying extra_data returns its data to the App here.
    auto resp = agentlink::BuildResponse(f.command_id, f.sequence, status, error,
                                         extra_len ? extra : nullptr, extra_len);
    if (s_tx && s_tx->send_ctrl) s_tx->send_ctrl(s_tx->impl, resp.data(), resp.size());
}

// Transport -> core: a data-plane chunk arrived (e.g. downlink TTS voice over L2CAP).
void OnStreamData(agent_stream_t type, const uint8_t* data, size_t len) {
    if (type == AGENT_STREAM_VOICE) {
        // Downlink TTS: whatever the wire format, the board only ever sees PCM16. The audio
        // stage decodes IMA-ADPCM when that is the negotiated codec and meters the App with
        // 0x20 flow control; on raw PCM it is a straight pass-through.
        agentlink::audio::Feed(data, len);
    }
    // Future downlink video/file: type == AGENT_STREAM_VIDEO -> on_video_out, etc.
}

// audio stage -> board. Kept separate from OnStreamData so the decode/flow-control stage sits
// between the wire and on_audio_out without the board noticing.
void OnDecodedPcm(const uint8_t* pcm16, size_t bytes) {
    if (s_have_out && s_out.on_audio_out) s_out.on_audio_out(pcm16, bytes, s_out.ctx);
}

// audio stage -> App (0x20 AudioFlowControl on the event channel).
void AudioEventSink(uint8_t event_id, const uint8_t* payload, size_t len) {
    if (!s_tx || !s_tx->send_ctrl) return;
    auto ev = agentlink::BuildEvent(event_id, payload, len);
    // Best-effort single send: this runs on the flow-control ticker and must not block, and the
    // payload carries absolute state so a lost frame is re-asserted within 2s anyway.
    (void)s_tx->send_ctrl(s_tx->impl, ev.data(), ev.size());
}
}  // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────────
// A capability bit is a promise to the App: advertise AGENT_CAP_SPEAKER and it will send audio.
// The promise is only kept if the matching callback is actually wired, and nothing else in the
// system notices when it is not — the App just sends into silence and the symptom shows up as
// "the hardware does not work". Say it once, loudly, at init.
//
// Only the bits that map to a single callback can be checked here. Ones the board drives itself
// (CAMERA, RECORDING, BATTERY, SENSOR) have nothing to point at, so they are left alone rather
// than half-checked.
void CheckCapabilityWiring() {
    struct Wiring { uint32_t cap; const void* cb; const char* cap_name; const char* cb_name; };
    const Wiring kWiring[] = {
        {AGENT_CAP_SPEAKER,  reinterpret_cast<const void*>(s_out.on_audio_out), "SPEAKER",  "on_audio_out"},
        {AGENT_CAP_SCREEN,   reinterpret_cast<const void*>(s_out.on_show_text), "SCREEN",   "on_show_text"},
        {AGENT_CAP_HAPTIC,   reinterpret_cast<const void*>(s_out.on_haptic),    "HAPTIC",   "on_haptic"},
        {AGENT_CAP_LED,      reinterpret_cast<const void*>(s_out.on_led),       "LED",      "on_led"},
        {AGENT_CAP_ACTUATOR, reinterpret_cast<const void*>(s_out.on_actuate),   "ACTUATOR", "on_actuate"},
        {AGENT_CAP_MIC,      reinterpret_cast<const void*>(s_out.on_listen),    "MIC",      "on_listen"},
    };
    for (const auto& w : kWiring) {
        const bool declared = (s_cfg.caps & w.cap) != 0;
        const bool wired    = s_have_out && w.cb != nullptr;
        if (declared && !wired) {
            ESP_LOGE(TAG, "caps declares %s but %s is NULL — the App will use this and nothing "
                          "will happen", w.cap_name, w.cb_name);
        } else if (!declared && wired) {
            ESP_LOGW(TAG, "%s is wired but caps does not declare %s — the App will never call it",
                     w.cb_name, w.cap_name);
        }
    }
}

esp_err_t agent_link_init(const agent_link_config_t* cfg) {
    if (!cfg || !cfg->device_name) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    if (cfg->output) { s_out = *cfg->output; s_have_out = true; }

    CheckCapabilityWiring();

    // Transport backend selection ("two transports"). Both wire their uplink callbacks to the
    // same core (OnCtrlFrame/OnConn), so the upper-layer push_*/on_* stay transport-independent.
    switch (s_cfg.transport) {
    case AGENT_TRANSPORT_WIFI:
        s_tx = agent_transport_wifi();
        agent_transport_wifi_set_config(s_cfg.wifi);
        agent_transport_wifi_set_name(s_cfg.device_name);  // SoftAP SSID prefix for captive-portal provisioning
        agent_transport_wifi_set_recv(&OnCtrlFrame);
        agent_transport_wifi_set_conn(&OnConn);
        agent_transport_wifi_set_stream_recv(&OnStreamData);
        break;
    case AGENT_TRANSPORT_BOTH:
        // Not implemented: falls back to BLE alone. Say so rather than let a caller believe it
        // asked for a hybrid link and got one.
        ESP_LOGW(TAG, "transport BOTH is not implemented — using BLE only");
        [[fallthrough]];
    case AGENT_TRANSPORT_BLE:
    default:
        s_tx = agent_transport_ble();
        agent_transport_ble_set_name(s_cfg.device_name);  // advertising name = device name
        agent_transport_ble_set_recv(&OnCtrlFrame);       // control frame received -> parse and route
        agent_transport_ble_set_conn(&OnConn);            // connection state -> on_state
        agent_transport_ble_set_stream_recv(&OnStreamData); // L2CAP downlink TTS -> on_audio_out
        agent_transport_ble_set_ready(&OnLinkReady);        // after 0xFFC4 subscribe -> send the I/O manifest
        // Standard Device Information Service (0x180A): model defaults to device_name; NULL manufacturer/firmware keep the SDK defaults.
        agent_transport_ble_set_device_info(s_cfg.manufacturer, DeviceModel(), FirmwareVersion());
        break;
    }

    // OTA: give the engine the transport-independent bits it needs (how to notify progress, and
    // which model string an incoming firmware must claim). Everything else it owns itself, so a
    // board gets App-driven upgrades without writing a line of code.
    agentlink::ota::SetEventSink(&OtaEventSink);
    agentlink::ota::SetDeviceModel(DeviceModel());

    // Downlink audio stage: decoded PCM goes to the board, 0x20 flow control goes to the App.
    agentlink::audio::SetSinks(&OnDecodedPcm, &AudioEventSink);

    // D1: expose wired rich outputs (LED / haptic / screen) as generic OUT endpoints in the manifest.
    RegisterSyntheticEndpoints();

    const char* tx_name = s_cfg.transport == AGENT_TRANSPORT_WIFI ? "wifi"
                        : s_cfg.transport == AGENT_TRANSPORT_BOTH ? "both(ble)" : "ble";
    ESP_LOGI(TAG, "init: name='%s' model='%s' fw=%s caps=0x%04x proto=v%d transport=%s io=%d",
             s_cfg.device_name, DeviceModel(), FirmwareVersion(),
             static_cast<unsigned>(s_cfg.caps), AGENT_LINK_PROTO_VERSION, tx_name, s_io_count);
    return ESP_OK;
}

esp_err_t agent_link_start(void) {
    ESP_LOGI(TAG,
             "start: caps=0x%04x out{audio=%d text=%d image=%d video=%d haptic=%d led=%d actuate=%d agentlist=%d command=%d} io=%d",
             static_cast<unsigned>(s_cfg.caps),
             s_have_out && s_out.on_audio_out  ? 1 : 0,
             s_have_out && s_out.on_show_text  ? 1 : 0,
             s_have_out && s_out.on_show_image ? 1 : 0,
             s_have_out && s_out.on_video_out  ? 1 : 0,
             s_have_out && s_out.on_haptic     ? 1 : 0,
             s_have_out && s_out.on_led        ? 1 : 0,
             s_have_out && s_out.on_actuate    ? 1 : 0,
             s_have_out && s_out.on_agent_list ? 1 : 0,
             s_have_out && s_out.on_command    ? 1 : 0,
             s_io_count);
    // Start the transport backend (BLE: NimBLE advertising + GATT Service C). Control plane
    // (commands/events) is wired; the data plane rides whatever channels the backend provides.
    if (!s_tx || !s_tx->start) return ESP_ERR_INVALID_STATE;
    return s_tx->start(s_tx->impl);
}

void agent_link_stop(void) {
    if (s_tx && s_tx->stop) s_tx->stop(s_tx->impl);
    s_state = AGENT_STATE_DISCONNECTED;
    // stop() does not go through the transport's disconnect callback, so it has to do this itself
    // or a later start() comes up believing streams from the previous session are still open.
    ResetLinkState();
}

agent_state_t agent_link_state(void) { return s_state; }

// ── Uplink: events / status (control plane, wired) ───────────────────────────────
esp_err_t agent_link_report_battery(uint8_t percent, bool charging) {
    // Event 0x14 (PowerStatus). payload = {charging_state, level}:
    //   charging_state: 0x00 = not charging, 0x01 = charging, 0x02 = low-battery alert
    //   (below kBatLowThreshold, edge-triggered once).
    // Pushed only on change: the device may call this periodically; the SDK de-duplicates internally.
    if (!s_tx || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    const int chg = charging ? 1 : 0;
    const bool changed  = (static_cast<int>(percent) != s_bat_pct) || (chg != s_bat_chg);
    const bool low_edge = (percent < kBatLowThreshold) && s_bat_low_armed;
    if (!changed && !low_edge) return ESP_OK;  // no change: do not push

    const uint8_t state = low_edge ? 0x02 : static_cast<uint8_t>(chg);
    uint8_t p[2] = { state, percent };
    auto ev = agentlink::BuildEvent(0x14, p, sizeof(p));
    esp_err_t r = s_tx->send_ctrl(s_tx->impl, ev.data(), ev.size());
    if (r != ESP_OK) return r;  // send failed (not connected / congested): cache not updated, next poll retries so it eventually lands

    // Update the cache / re-arm the alert only after a successful send.
    s_bat_pct = percent;
    s_bat_chg = chg;
    s_bat_low_armed = (percent >= kBatLowThreshold);  // re-arm once back above the threshold; disarm below (already alerted)
    agent_transport_ble_update_battery(percent);      // also update the standard Battery Service (0x2A19) and notify
    ESP_LOGD(TAG, "battery event 0x14: state=%u level=%u%%", state, percent);
    return ESP_OK;
}
esp_err_t agent_link_report_selected_agent(const char* agent_id) {
    (void)agent_id;
    return not_ready("report_selected_agent");  // TODO: 0x16 event (needs index + name_len + name format)
}
// Largest control-plane payload that fits one frame on the active transport.
//
// Every control-plane send is a single frame: there is no fragmentation on this path, so a payload
// that does not fit is not "slow", it is lost or silently cut. BLE gives us ATT_MTU - 3 for the
// notification minus the 6-byte frame header; the 480 ceiling keeps one frame inside a single mbuf
// even when a peer negotiates a large MTU.
static size_t SingleFramePayloadBudget() {
    if (s_cfg.transport == AGENT_TRANSPORT_WIFI) return 1024;
    const uint16_t mtu = agent_transport_ble_att_mtu();
    // 14 = the unnegotiated default MTU (23) - 3 - 6, i.e. what is safe before the peer exchanges.
    size_t budget = (mtu > 3 + 6) ? static_cast<size_t>(mtu - 3 - 6) : 14;
    return budget > 480 ? 480 : budget;
}

esp_err_t agent_link_push_event(agent_event_t type, const uint8_t* data, size_t len) {
    if (!s_tx || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    if (!(s_tx->is_ready && s_tx->is_ready(s_tx->impl))) return not_ready("push_event");
    // The event_id on the wire is the agent_event_t value: AGENT_EVT_BUTTON/SENSOR/WAKEWORD, or
    // AGENT_EVT_CUSTOM (0x64) for device-private packets — the board owns the payload and the App
    // matches on event_id 0x64 (best-effort, like other events; a dropped frame is not resent).
    // Refuse rather than hand the transport a frame it cannot send in one piece. Without this the
    // caller gets either an opaque failure or, worse, a frame the ATT layer quietly truncates and
    // a success return.
    const size_t budget = SingleFramePayloadBudget();
    if (len > budget) {
        ESP_LOGW(TAG, "push_event 0x%02X: %uB payload exceeds the single-frame budget (%uB) — not sent",
                 static_cast<unsigned>(type), static_cast<unsigned>(len), static_cast<unsigned>(budget));
        return ESP_ERR_INVALID_SIZE;
    }
    auto ev = agentlink::BuildEvent(static_cast<uint8_t>(type), data, len);
    return s_tx->send_ctrl(s_tx->impl, ev.data(), ev.size());
}

// Push firmware-authored text as a single AGENT_EVT_PROMPT (0x04) event; the App forwards it verbatim to the Agent as a prompt.
esp_err_t agent_link_push_prompt(const char* utf8) {
    if (!utf8 || !utf8[0]) return ESP_ERR_INVALID_ARG;
    if (!s_tx || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    if (!(s_tx->is_ready && s_tx->is_ready(s_tx->impl))) return not_ready("push_prompt");

    const size_t len = strlen(utf8);

    const size_t budget = SingleFramePayloadBudget();
    if (len > budget) {
        ESP_LOGW(TAG, "push_prompt: %uB text exceeds single-frame budget (%uB) — not sent, call again per chunk",
                 static_cast<unsigned>(len), static_cast<unsigned>(budget));
        return ESP_ERR_INVALID_SIZE;
    }

    return agent_link_push_event(AGENT_EVT_PROMPT, reinterpret_cast<const uint8_t*>(utf8), len);
}

// ── Generic I/O: sensors / actuators — one channel that does not grow a new API per sensor kind ──
esp_err_t agent_link_register_io(const agent_link_io_desc_t* desc,
                                 agent_io_actuate_cb_t cb, void* ctx) {
    if (!desc || !desc->id || !desc->kind) return ESP_ERR_INVALID_ARG;
    if (s_io_count >= kMaxIo) {
        ESP_LOGE(TAG, "register_io: registry full (max %d)", kMaxIo);
        return ESP_ERR_NO_MEM;
    }
    s_io[s_io_count] = { desc, cb, ctx };
    ESP_LOGI(TAG, "register_io[%d]: id='%s' kind='%s' dir=%s",
             s_io_count, desc->id, desc->kind, desc->dir == AGENT_IO_OUT ? "out" : "in");
    s_io_count++;
    // The manifest is serialized and sent during the connection handshake (BLE: after the 0xFFC4
    // subscribe; WiFi: after connect), see SendManifest. Downlink actuator commands (0x33) are
    // matched by id to s_io[].cb, see OnCtrlFrame.
    return ESP_OK;
}

// Report one sensor reading: look up the registry for the value type -> validate the length ->
// encode a 0x19 IoReading event -> send_ctrl.
//   payload = [id_len(1)] [id(UTF-8)] [val_type(1)] [value (per type, little-endian)...].
//   Unregistered / non-IN endpoint / length mismatch -> return an error (not sent);
//   not connected -> INVALID_STATE (safely ignored).
esp_err_t agent_link_push_reading(const char* id, const void* value, size_t len) {
    if (!id || !value || len == 0)  return ESP_ERR_INVALID_ARG;
    if (!s_tx || !s_tx->send_ctrl)  return ESP_ERR_INVALID_STATE;
    if (s_tx->is_ready && !s_tx->is_ready(s_tx->impl)) return ESP_ERR_INVALID_STATE;

    const int idx = FindIoIndex(id);
    if (idx < 0 || s_io[idx].desc->dir != AGENT_IO_IN) return ESP_ERR_NOT_FOUND;  // only registered IN endpoints may report
    IoEntry& e = s_io[idx];
    const int want = IoValueSize(e.desc->value);
    if (want >= 0 && static_cast<size_t>(want) != len) {
        ESP_LOGW(TAG, "push_reading '%s': len=%u does not match expected %dB for type",
                 id, static_cast<unsigned>(len), want);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t id_len = strlen(id);
    if (id_len == 0 || id_len > 255) return ESP_ERR_INVALID_ARG;

    // Cache the latest value (for 0x35 GetReading + on-change dedup) before applying the policy.
    const int64_t now_us = esp_timer_get_time();
    e.last_ts_us = now_us;
    bool same_as_last = false;
    if (len <= sizeof(e.last_val)) {
        same_as_last = e.has_last && e.last_len == len && memcmp(e.last_val, value, len) == 0;
        memcpy(e.last_val, value, len);
        e.last_len = static_cast<uint8_t>(len);
        e.has_last = true;
    }

    // Reporting policy set via 0x36 (default 0 = passthrough).
    switch (e.policy_mode) {
    case IO_POLICY_OFF:
        return ESP_OK;                                         // dropped by policy
    case IO_POLICY_PERIODIC: {
        const int64_t period_us = e.policy_rate_hz ? (1000000 / e.policy_rate_hz) : 0;
        if (period_us && e.last_sent_us && (now_us - e.last_sent_us) < period_us)
            return ESP_OK;                                     // throttled to rate_hz
        break;
    }
    case IO_POLICY_ONCHANGE:
        if (same_as_last) return ESP_OK;                       // unchanged (dedup for fixed-size <=16B values)
        break;
    default:
        break;                                                 // passthrough
    }

    // [id_len][id][val_type][value] must also fit one frame — a long BLOB or STR reading is the
    // realistic way to exceed it.
    const size_t payload_len = 1 + id_len + 1 + len;
    const size_t budget      = SingleFramePayloadBudget();
    if (payload_len > budget) {
        ESP_LOGW(TAG, "push_reading '%s': %uB payload exceeds the single-frame budget (%uB) — not sent",
                 id, static_cast<unsigned>(payload_len), static_cast<unsigned>(budget));
        return ESP_ERR_INVALID_SIZE;
    }

    std::vector<uint8_t> p;
    p.reserve(payload_len);
    p.push_back(static_cast<uint8_t>(id_len));
    const uint8_t* idb = reinterpret_cast<const uint8_t*>(id);
    p.insert(p.end(), idb, idb + id_len);
    p.push_back(static_cast<uint8_t>(e.desc->value));
    const uint8_t* v = static_cast<const uint8_t*>(value);
    p.insert(p.end(), v, v + len);

    auto ev = agentlink::BuildEvent(0x19, p.data(), p.size());
    const esp_err_t r = s_tx->send_ctrl(s_tx->impl, ev.data(), ev.size());
    if (r == ESP_OK) e.last_sent_us = now_us;
    return r;
}

// ── Generic I/O: notify the Agent that the manifest changed (runtime endpoint add/remove) ──
esp_err_t agent_link_notify_manifest_changed(void) {
    if (!s_tx || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    ++s_manifest_rev;
    // Event 0x1A ManifestChanged: payload = [rev(4, LE)]. Best-effort; the App re-fetches via 0x34.
    const uint8_t p[4] = { static_cast<uint8_t>(s_manifest_rev),       static_cast<uint8_t>(s_manifest_rev >> 8),
                           static_cast<uint8_t>(s_manifest_rev >> 16), static_cast<uint8_t>(s_manifest_rev >> 24) };
    if (s_tx->is_ready && s_tx->is_ready(s_tx->impl)) {
        auto ev = agentlink::BuildEvent(0x1A, p, sizeof p);
        (void)s_tx->send_ctrl(s_tx->impl, ev.data(), ev.size());
    }
    SendManifest(/*force=*/true);   // re-push the full manifest carrying the new rev
    ESP_LOGI(TAG, "manifest changed -> rev=%u", static_cast<unsigned>(s_manifest_rev));
    return ESP_OK;
}

// ── Data plane: one open/write/close for every stream kind (agent_link_stream.h) ──────────────
//
// This layer owns only session bookkeeping and the per-kind metadata blob. Everything that makes a
// kind different on the wire — which channel it rides, session ids, slicing, framing, backpressure
// — belongs to the transport backend, which is what lets the same three calls serve speech, audio,
// images, video and files.

namespace {

// Pack the open-time metadata a kind's protocol expects. Kinds not listed carry none.
size_t BuildStreamMeta(agent_stream_t kind, const agent_stream_opts_t* o,
                       uint8_t* buf, size_t cap) {
    if (!o) return 0;
    switch (kind) {
    case AGENT_STREAM_AUDIO:
    case AGENT_STREAM_FILE: {
        // A label the App shows or files the payload under.
        if (!o->name) return 0;
        const size_t n = strlen(o->name);
        if (n == 0 || n > cap) return 0;
        memcpy(buf, o->name, n);
        return n;
    }
    case AGENT_STREAM_IMAGE: {
        // [encoding(1)][width(2 LE)][height(2 LE)][total_bytes(4 LE)] — sent before any pixel so
        // the App knows how to decode it and how much to expect.
        if (cap < 9) return 0;
        buf[0] = static_cast<uint8_t>(o->encoding);
        buf[1] = static_cast<uint8_t>(o->width  & 0xFF);
        buf[2] = static_cast<uint8_t>((o->width  >> 8) & 0xFF);
        buf[3] = static_cast<uint8_t>(o->height & 0xFF);
        buf[4] = static_cast<uint8_t>((o->height >> 8) & 0xFF);
        buf[5] = static_cast<uint8_t>(o->total_bytes & 0xFF);
        buf[6] = static_cast<uint8_t>((o->total_bytes >>  8) & 0xFF);
        buf[7] = static_cast<uint8_t>((o->total_bytes >> 16) & 0xFF);
        buf[8] = static_cast<uint8_t>((o->total_bytes >> 24) & 0xFF);
        return 9;
    }
    default:
        return 0;
    }
}

const char* StreamName(agent_stream_t k) {
    switch (k) {
    case AGENT_STREAM_VOICE: return "voice";
    case AGENT_STREAM_AUDIO: return "audio";
    case AGENT_STREAM_IMAGE: return "image";
    case AGENT_STREAM_VIDEO: return "video";
    case AGENT_STREAM_FILE:  return "file";
    default:                 return "?";
    }
}

}  // namespace

esp_err_t agent_link_stream_open(agent_stream_t kind, const agent_stream_opts_t* opts,
                                 agent_stream_handle_t* out) {
    if (!out || kind < 0 || kind >= AGENT_STREAM_KIND_COUNT) return ESP_ERR_INVALID_ARG;
    *out = nullptr;
    if (!s_tx || !s_tx->stream_start || !s_tx->send_stream || !s_tx->stream_end)
        return ESP_ERR_INVALID_STATE;
    if (!(s_tx->is_ready && s_tx->is_ready(s_tx->impl))) return not_ready(StreamName(kind));

    agent_stream_session* st = &s_streams[kind];
    if (st->open) { *out = st; return ESP_OK; }   // idempotent, see the header

    uint8_t meta[32];
    const size_t meta_len = BuildStreamMeta(kind, opts, meta, sizeof(meta));

    const esp_err_t r = s_tx->stream_start(s_tx->impl, kind, meta_len ? meta : nullptr, meta_len);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "stream open(%s) rejected by transport: %s", StreamName(kind), esp_err_to_name(r));
        return r;
    }
    st->open = true;
    *out = st;
    return ESP_OK;
}

esp_err_t agent_link_stream_write(agent_stream_handle_t h, const void* data, size_t bytes) {
    if (!h || !data || bytes == 0) return ESP_ERR_INVALID_ARG;
    if (!h->open) return ESP_ERR_INVALID_STATE;
    if (!s_tx || !s_tx->send_stream) return ESP_ERR_INVALID_STATE;
    return s_tx->send_stream(s_tx->impl, h->kind, static_cast<const uint8_t*>(data), bytes);
}

esp_err_t agent_link_stream_close(agent_stream_handle_t h, bool complete) {
    if (!h) return ESP_ERR_INVALID_ARG;
    if (!h->open) return ESP_OK;
    h->open = false;
    if (!s_tx || !s_tx->stream_end) return ESP_ERR_INVALID_STATE;
    return s_tx->stream_end(s_tx->impl, h->kind, complete, nullptr, 0);
}

esp_err_t agent_link_stream_send(agent_stream_t kind, const agent_stream_opts_t* opts,
                                 const void* data, size_t bytes) {
    if (!data || bytes == 0) return ESP_ERR_INVALID_ARG;

    // total_bytes is knowable here even when the caller did not fill it in, and the App needs it
    // to show progress, so supply it rather than make every caller remember.
    agent_stream_opts_t o = opts ? *opts : agent_stream_opts_t{};
    if (o.total_bytes == 0) o.total_bytes = static_cast<uint32_t>(bytes);

    agent_stream_handle_t h = nullptr;
    esp_err_t r = agent_link_stream_open(kind, &o, &h);
    if (r != ESP_OK) return r;

    r = agent_link_stream_write(h, data, bytes);
    // Close either way: the App is waiting for an end marker, and `complete` is what tells it
    // whether the payload it got is the whole thing.
    const esp_err_t e = agent_link_stream_close(h, r == ESP_OK);
    return (r != ESP_OK) ? r : e;
}

bool agent_link_stream_is_open(agent_stream_t kind) {
    if (kind < 0 || kind >= AGENT_STREAM_KIND_COUNT) return false;
    return s_streams[kind].open;
}
