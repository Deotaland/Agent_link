// JSON control plane of the WiFi channel. See cloud_json.h.

#include "cloud_json.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agent_link_io.h"
#include "cJSON.h"
#include "cloud_clock.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "protocol.h"

namespace {
constexpr const char* TAG = "agent_link.json";

// BLE command ids the named commands run as.
constexpr uint8_t kCmdDeviceInfo = 0x01;
constexpr uint8_t kCmdPower      = 0x03;
constexpr uint8_t kCmdVoiceReply = 0x05;
constexpr uint8_t kCmdActuate    = 0x33;
constexpr uint8_t kCmdManifest   = 0x34;
constexpr uint8_t kCmdRead       = 0x35;
constexpr uint8_t kCmdPolicy     = 0x36;
constexpr uint8_t kCmdMicStart   = 0x3C;
constexpr uint8_t kCmdMicStop    = 0x3D;
constexpr uint8_t kCmdOtaAbort   = 0x56;

constexpr uint8_t kEvtManifest        = 0x18;
constexpr uint8_t kEvtManifestChanged = 0x1A;

// Codes answered here rather than by the core; the numbers are the ones BLE uses.
constexpr int kErrUnknownCommand = 1001;
constexpr int kErrRejected       = 1003;
constexpr int kErrInvalid        = 1004;

constexpr size_t kManifestMax = 16 * 1024;   // 32 endpoints, the registry's limit, come to a few KB
constexpr int    kLogMax      = 200;         // characters of each response that are logged

cloud_json_hooks_t s_hooks = {};

// The command being run. The core answers it through send_ctrl before deliver() returns, and on the
// same task, so only that task touches these. Frames from other tasks are told apart by s_run_task.
struct Pending {
    uint8_t              cmd      = 0;
    uint8_t              seq      = 0;
    bool                 answered = false;
    uint8_t              status   = 0;
    uint16_t             error    = 0;
    std::vector<uint8_t> extra;
};
Pending                   s_pending;
std::atomic<TaskHandle_t> s_run_task{nullptr};
uint8_t                   s_seq = 0;

// Manifest text collected from the 0x18 chunks a 0x34 produces.
std::string s_manifest;
bool        s_manifest_whole = false;

// The last manifest, for encoding actuator arguments. Any task marks it stale when the core pushes a
// manifest outside a command, which it does when the endpoints change.
std::string       s_manifest_cache;
std::atomic<bool> s_manifest_stale{true};

// Detail for a 1004, built while a command runs.
char s_err[96];

// ── Byte helpers (little-endian, as on BLE) ──────────────────────────────────────────────────────
void PutU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void PutF32(std::vector<uint8_t>& v, float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof x);
    PutU32(v, x);
}
uint16_t GetU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
float GetF32(const uint8_t* p) {
    const uint32_t x = GetU32(p);
    float f;
    memcpy(&f, &x, sizeof f);
    return f;
}

std::string Base64(const uint8_t* p, size_t n) {
    if (n == 0) return "";
    size_t need = 0;
    (void)mbedtls_base64_encode(nullptr, 0, &need, p, n);   // reports the size, NUL included
    std::string s(need, '\0');
    size_t olen = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&s[0]), s.size(), &olen, p, n) != 0) return "";
    s.resize(olen);
    return s;
}

bool Unbase64(const char* s, std::vector<uint8_t>& out) {
    const size_t n = strlen(s);
    out.resize(n);   // decoded is always shorter than its text
    size_t olen = 0;
    if (mbedtls_base64_decode(out.data(), out.size(), &olen,
                              reinterpret_cast<const unsigned char*>(s), n) != 0) {
        return false;
    }
    out.resize(olen);
    return true;
}

// An integral JSON number in [lo, hi].
bool Integer(const cJSON* j, double lo, double hi, double* out) {
    if (!cJSON_IsNumber(j)) return false;
    const double d = j->valuedouble;
    if (d < lo || d > hi || d != std::floor(d)) return false;
    *out = d;
    return true;
}

// A float with the digits it really has: 0.1f comes out as 0.1, not 0.100000001490116.
cJSON* Float(float f) {
    char b[24];
    snprintf(b, sizeof b, "%.7g", static_cast<double>(f));
    return cJSON_CreateNumber(strtod(b, nullptr));
}

// ── Endpoint values: JSON <-> the bytes BLE carries ──────────────────────────────────────────────
// The JSON forms are the ones the platform's MCP gateway gives the tools it makes from the manifest,
// so a tool call's arguments can be passed through untouched.

// JSON value -> wire bytes for manifest type `type` ("bool", "u16", ...).
bool EncodeValue(const char* type, const cJSON* v, std::vector<uint8_t>& out) {
    double d = 0;
    if (strcmp(type, "bool") == 0) {
        if (!cJSON_IsBool(v)) return false;
        out.push_back(cJSON_IsTrue(v) ? 1 : 0);
    } else if (strcmp(type, "i32") == 0) {
        if (!Integer(v, -2147483648.0, 2147483647.0, &d)) return false;
        PutU32(out, static_cast<uint32_t>(static_cast<int32_t>(d)));
    } else if (strcmp(type, "u16") == 0) {
        if (!Integer(v, 0, 65535, &d)) return false;
        PutU16(out, static_cast<uint16_t>(d));
    } else if (strcmp(type, "f32") == 0) {
        if (!cJSON_IsNumber(v)) return false;
        PutF32(out, static_cast<float>(v->valuedouble));
    } else if (strcmp(type, "vec2") == 0 || strcmp(type, "vec3") == 0) {
        const int n = type[3] - '0';
        if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != n) return false;
        const cJSON* e = nullptr;
        cJSON_ArrayForEach(e, v) {
            if (!cJSON_IsNumber(e)) return false;
            PutF32(out, static_cast<float>(e->valuedouble));
        }
    } else if (strcmp(type, "rgb") == 0) {
        // "#RRGGBB", the '#' optional -> u32 0x00RRGGBB
        if (!cJSON_IsString(v)) return false;
        const char* s = v->valuestring;
        if (*s == '#') ++s;
        if (strlen(s) != 6) return false;
        for (int i = 0; i < 6; ++i) {
            if (!isxdigit(static_cast<unsigned char>(s[i]))) return false;
        }
        PutU32(out, static_cast<uint32_t>(strtoul(s, nullptr, 16)));
    } else if (strcmp(type, "str") == 0) {
        if (!cJSON_IsString(v)) return false;
        out.insert(out.end(), v->valuestring, v->valuestring + strlen(v->valuestring));
    } else if (strcmp(type, "blob") == 0) {
        std::vector<uint8_t> raw;
        if (!cJSON_IsString(v) || !Unbase64(v->valuestring, raw)) return false;
        out.insert(out.end(), raw.begin(), raw.end());
    } else {
        return false;
    }
    return true;
}

// Wire bytes of agent_val_t `type` -> JSON; nullptr if they do not fit the type.
cJSON* DecodeValue(uint8_t type, const uint8_t* p, size_t n) {
    switch (type) {
    case AGENT_VAL_BOOL: return n == 1 ? cJSON_CreateBool(p[0] != 0) : nullptr;
    case AGENT_VAL_I32:  return n == 4 ? cJSON_CreateNumber(static_cast<int32_t>(GetU32(p))) : nullptr;
    case AGENT_VAL_U16:  return n == 2 ? cJSON_CreateNumber(GetU16(p)) : nullptr;
    case AGENT_VAL_F32:  return n == 4 ? Float(GetF32(p)) : nullptr;
    case AGENT_VAL_VEC2:
    case AGENT_VAL_VEC3: {
        const size_t k = (type == AGENT_VAL_VEC2) ? 2 : 3;
        if (n != 4 * k) return nullptr;
        cJSON* a = cJSON_CreateArray();
        for (size_t i = 0; i < k; ++i) cJSON_AddItemToArray(a, Float(GetF32(p + 4 * i)));
        return a;
    }
    case AGENT_VAL_RGB: {
        if (n != 4) return nullptr;
        char s[8];
        snprintf(s, sizeof s, "#%06X", static_cast<unsigned>(GetU32(p) & 0xFFFFFF));
        return cJSON_CreateString(s);
    }
    case AGENT_VAL_BLOB: return cJSON_CreateString(Base64(p, n).c_str());
    case AGENT_VAL_STR:  return cJSON_CreateString(std::string(reinterpret_cast<const char*>(p), n).c_str());
    default:             return nullptr;
    }
}

// ── Running a command through the core ───────────────────────────────────────────────────────────

// Hand one command to the core and collect its answer. false if it never answered, which only a
// malformed frame can cause.
bool Run(uint8_t cmd, const std::vector<uint8_t>& payload) {
    s_pending        = Pending{};
    s_pending.cmd    = cmd;
    s_pending.seq    = ++s_seq;
    s_manifest.clear();
    s_manifest_whole = false;

    std::vector<uint8_t> f;
    f.reserve(agentlink::kHeaderSize + payload.size());
    f.push_back(agentlink::kVersion);
    f.push_back(agentlink::kMsgCommand);
    f.push_back(cmd);
    f.push_back(s_pending.seq);
    PutU16(f, static_cast<uint16_t>(payload.size()));
    f.insert(f.end(), payload.begin(), payload.end());

    s_run_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    if (s_hooks.deliver) s_hooks.deliver(f.data(), f.size());
    s_run_task.store(nullptr, std::memory_order_release);
    return s_pending.answered;
}

// The manifest, parsed; the caller frees it. It is fetched with the same 0x34 the App sends, and
// kept until the core pushes a new one.
cJSON* Manifest() {
    if (s_manifest_stale.exchange(false, std::memory_order_acq_rel) || s_manifest_cache.empty()) {
        if (Run(kCmdManifest, {}) && s_manifest_whole) s_manifest_cache = s_manifest;
        else                                            s_manifest_cache.clear();
    }
    if (s_manifest_cache.empty()) return nullptr;
    return cJSON_ParseWithLength(s_manifest_cache.data(), s_manifest_cache.size());
}

const cJSON* FindEndpoint(const cJSON* manifest, const char* id) {
    const cJSON* io = cJSON_GetObjectItemCaseSensitive(manifest, "io");
    const cJSON* e  = nullptr;
    cJSON_ArrayForEach(e, io) {
        const cJSON* jid = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0) return e;
    }
    return nullptr;
}

// ── Command arguments -> BLE payload ─────────────────────────────────────────────────────────────
using Build  = bool (*)(const cJSON* in, std::vector<uint8_t>& payload);
using Decode = void (*)(const std::vector<uint8_t>& extra, cJSON* reply);

bool BadParam(const char* why) {
    snprintf(s_err, sizeof s_err, "%s", why);
    return false;
}

// [id_len(1)][id]
bool PutIo(const cJSON* in, std::vector<uint8_t>& p, const char** id_out = nullptr) {
    const cJSON* io = cJSON_GetObjectItemCaseSensitive(in, "io");
    const size_t n  = cJSON_IsString(io) ? strlen(io->valuestring) : 0;
    if (n == 0 || n > 63) return BadParam("params.io must be a string of 1-63 bytes");
    p.push_back(static_cast<uint8_t>(n));
    p.insert(p.end(), io->valuestring, io->valuestring + n);
    if (id_out) *id_out = io->valuestring;
    return true;
}

// Endpoint arguments, encoded the way the endpoint declares them: a single "value" of its type, or
// one field per entry of its args schema, in the schema's order.
bool EncodeArgs(const cJSON* ep, const cJSON* args, std::vector<uint8_t>& p) {
    const cJSON* schema = cJSON_GetObjectItemCaseSensitive(ep, "args");
    if (cJSON_IsObject(schema)) {
        const cJSON* field = nullptr;
        cJSON_ArrayForEach(field, schema) {
            const char* type = cJSON_IsString(field) ? field->valuestring : "?";
            if (!EncodeValue(type, cJSON_GetObjectItemCaseSensitive(args, field->string), p)) {
                snprintf(s_err, sizeof s_err, "params.args.%s must be %s", field->string, type);
                return false;
            }
        }
        return true;
    }
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(ep, "value");
    const char*  t    = cJSON_IsString(type) ? type->valuestring : "?";
    if (!EncodeValue(t, cJSON_GetObjectItemCaseSensitive(args, "value"), p)) {
        snprintf(s_err, sizeof s_err, "params.args.value must be %s", t);
        return false;
    }
    return true;
}

bool BuildActuate(const cJSON* in, std::vector<uint8_t>& p) {
    const char* id = nullptr;
    if (!PutIo(in, p, &id)) return false;
    const cJSON* args = cJSON_GetObjectItemCaseSensitive(in, "args");
    if (!cJSON_IsObject(args)) return BadParam("params.args must be an object");

    cJSON*       manifest = Manifest();
    const cJSON* ep       = FindEndpoint(manifest, id);
    const cJSON* dir      = cJSON_GetObjectItemCaseSensitive(ep, "dir");
    bool ok = true;
    // An id that is not an OUT endpoint goes to the core without arguments; it answers 1003, as
    // it does over BLE.
    if (ep && cJSON_IsString(dir) && strcmp(dir->valuestring, "out") == 0) ok = EncodeArgs(ep, args, p);
    cJSON_Delete(manifest);
    return ok;
}

bool BuildRead(const cJSON* in, std::vector<uint8_t>& p) { return PutIo(in, p); }

// [id_len][id][mode(1)][rate_hz(2)]
bool BuildPolicy(const cJSON* in, std::vector<uint8_t>& p) {
    if (!PutIo(in, p)) return false;
    static const char* const kModes[] = {"all", "off", "periodic", "change"};   // BLE 0..3
    const cJSON* mode = cJSON_GetObjectItemCaseSensitive(in, "mode");
    int m = -1;
    for (int i = 0; i < 4 && cJSON_IsString(mode); ++i) {
        if (strcmp(mode->valuestring, kModes[i]) == 0) m = i;
    }
    if (m < 0) return BadParam("params.mode must be all, off, periodic or change");
    double hz = 0;
    const cJSON* jhz = cJSON_GetObjectItemCaseSensitive(in, "hz");
    if (jhz && !Integer(jhz, 0, 65535, &hz)) return BadParam("params.hz must be an integer 0-65535");
    p.push_back(static_cast<uint8_t>(m));
    PutU16(p, static_cast<uint16_t>(hz));
    return true;
}

// [mode(1)][max_ms(2)]; the SDK ignores mode.
bool BuildMicStart(const cJSON* in, std::vector<uint8_t>& p) {
    double ms = 0;
    const cJSON* jms = cJSON_GetObjectItemCaseSensitive(in, "max_ms");
    if (jms && !Integer(jms, 0, 65535, &ms)) return BadParam("params.max_ms must be an integer 0-65535");
    p.push_back(0);
    PutU16(p, static_cast<uint16_t>(ms));
    return true;
}

// [session_id(4)][status(1)][downlink_codec(1), optional]
bool BuildVoiceReply(const cJSON* in, std::vector<uint8_t>& p) {
    double session = 0;
    const cJSON* js = cJSON_GetObjectItemCaseSensitive(in, "session");
    if (js && !Integer(js, 0, 4294967295.0, &session)) return BadParam("params.session must be an integer");
    static const char* const kStatus[] = {"ok", "fail", "audio", "done"};   // BLE 0..3
    const cJSON* status = cJSON_GetObjectItemCaseSensitive(in, "status");
    int st = -1;
    for (int i = 0; i < 4 && cJSON_IsString(status); ++i) {
        if (strcmp(status->valuestring, kStatus[i]) == 0) st = i;
    }
    if (st < 0) return BadParam("params.status must be ok, fail, audio or done");
    PutU32(p, static_cast<uint32_t>(session));
    p.push_back(static_cast<uint8_t>(st));
    const cJSON* codec = cJSON_GetObjectItemCaseSensitive(in, "codec");
    if (codec) {
        if (!cJSON_IsString(codec)) return BadParam("params.codec must be pcm or adpcm");
        if      (strcmp(codec->valuestring, "pcm") == 0)   p.push_back(0);
        else if (strcmp(codec->valuestring, "adpcm") == 0) p.push_back(1);
        else    return BadParam("params.codec must be pcm or adpcm");
    }
    return true;
}

// ── BLE response data -> reply fields ────────────────────────────────────────────────────────────

// A length-prefixed string at x[k]; advances k.
bool Take(const std::vector<uint8_t>& x, size_t& k, std::string& out) {
    if (k >= x.size()) return false;
    const size_t n = x[k++];
    if (k + n > x.size()) return false;
    out.assign(reinterpret_cast<const char*>(x.data()) + k, n);
    k += n;
    return true;
}

// uuid(16) mac(6) battery(1) voltage_mv(2) wifi_state(1) ver_len ver profile_len profile model_len model
void DecodeDeviceInfo(const std::vector<uint8_t>& x, cJSON* r) {
    if (x.size() < 16 + 6 + 1 + 2 + 1) return;
    const uint8_t* u = x.data();
    char uuid[37];
    snprintf(uuid, sizeof uuid, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
             u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    size_t k = 16 + 6;            // the Bluetooth address means nothing to the platform
    const uint8_t battery = x[k++];
    k += 2 + 1;                   // voltage_mv and wifi_state, always 0
    std::string ver, profile, model;
    if (!Take(x, k, ver) || !Take(x, k, profile) || !Take(x, k, model)) return;

    // The platform knows this device by its station MAC, the one claim-code registered.
    uint8_t m[6] = {};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    char mac[18];
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);

    cJSON_AddStringToObject(r, "uuid", uuid);
    cJSON_AddStringToObject(r, "mac", mac);
    cJSON_AddNumberToObject(r, "battery", battery);
    cJSON_AddStringToObject(r, "fw", ver.c_str());
    cJSON_AddStringToObject(r, "model", model.c_str());
}

// [charging(1)][level(1)]
void DecodePower(const std::vector<uint8_t>& x, cJSON* r) {
    if (x.size() < 2) return;
    cJSON_AddNumberToObject(r, "level", x[1]);
    cJSON_AddBoolToObject(r, "charging", x[0] == 1);
}

// The manifest travels whole in the reply rather than as the 0x18 chunks BLE needs.
void DecodeManifest(const std::vector<uint8_t>& /*extra*/, cJSON* r) {
    if (!s_manifest_whole) return;   // no endpoints registered: the core sent none
    cJSON* m = cJSON_ParseWithLength(s_manifest.data(), s_manifest.size());
    if (!m) {
        ESP_LOGW(TAG, "io_manifest: the core's manifest is not valid JSON");
        return;
    }
    cJSON_AddItemToObject(r, "manifest", m);
    s_manifest_cache = s_manifest;
    s_manifest_stale.store(false, std::memory_order_release);
}

// [val_type(1)][age_ms(2)][value(N)]
void DecodeRead(const std::vector<uint8_t>& x, cJSON* r) {
    if (x.size() < 3) return;
    cJSON* v = DecodeValue(x[0], x.data() + 3, x.size() - 3);
    if (v) cJSON_AddItemToObject(r, "value", v);
    cJSON_AddNumberToObject(r, "age_ms", GetU16(x.data() + 1));
}

void DecodeRaw(const std::vector<uint8_t>& x, cJSON* r) {
    if (!x.empty()) cJSON_AddStringToObject(r, "raw", Base64(x.data(), x.size()).c_str());
}

// Actions that run a BLE command. Builders read the request's params; decoders fill the response's.
struct Named {
    const char* action;
    uint8_t     cmd;
    Build       build;    // nullptr: no params
    Decode      decode;   // nullptr: nothing in the response's params
};
const Named kNamed[] = {
    {"device_info", kCmdDeviceInfo, nullptr,         DecodeDeviceInfo},
    {"power_get",   kCmdPower,      nullptr,         DecodePower},
    {"io_manifest", kCmdManifest,   nullptr,         DecodeManifest},
    {"io_actuate",  kCmdActuate,    BuildActuate,    nullptr},
    {"io_read",     kCmdRead,       BuildRead,       DecodeRead},
    {"io_policy",   kCmdPolicy,     BuildPolicy,     nullptr},
    {"mic_start",   kCmdMicStart,   BuildMicStart,   nullptr},
    {"mic_stop",    kCmdMicStop,    nullptr,         nullptr},
    {"voice_reply", kCmdVoiceReply, BuildVoiceReply, nullptr},
    {"ota_abort",   kCmdOtaAbort,   nullptr,         nullptr},
};

// ── The envelope ─────────────────────────────────────────────────────────────────────────────────
// {"msg_id","type","action","timestamp","params","reply_to","code","message"}, the platform's, both
// ways. msg_id is "msg_<13-digit ms>_<6 hex>" and params is always an object. A request and an
// event carry null reply_to, code and message; a response names the request in reply_to and
// always carries a code and a message.

struct Request {
    std::string msg_id;             // what the response's reply_to names
    std::string action = "error";   // what the response answers: "<action>_status"
};

const char* CodeMsg(int code) {
    switch (code) {
    case 1001: return "unknown command";   // from the core: a raw command id nothing handles
    case 1002: return "not capable";
    case 1003: return "rejected";
    case 1004: return "invalid params";
    case 1005: return "no data";
    default:   return (code >= 2030 && code <= 2037) ? "ota error" : "failed";
    }
}

std::string NewMsgId(int64_t now_ms) {
    char b[40];
    snprintf(b, sizeof b, "msg_%lld_%06x", static_cast<long long>(now_ms),
             static_cast<unsigned>(esp_random() & 0xFFFFFF));
    return b;
}

// A response to `rq`, with an empty params for the caller to fill (*params).
cJSON* NewResponse(const Request& rq, int code, const char* message, cJSON** params = nullptr) {
    static bool warned = false;
    if (!cloud_clock_valid() && !warned) {
        warned = true;
        ESP_LOGW(TAG, "the clock is not set yet — response timestamps are wrong");
    }
    const int64_t now = cloud_clock_now_ms();
    cJSON* r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "msg_id", NewMsgId(now).c_str());
    cJSON_AddStringToObject(r, "type", "response");
    cJSON_AddStringToObject(r, "action", (rq.action + "_status").c_str());
    cJSON_AddNumberToObject(r, "timestamp", static_cast<double>(now));
    cJSON* p = cJSON_AddObjectToObject(r, "params");
    cJSON_AddStringToObject(r, "reply_to", rq.msg_id.c_str());
    cJSON_AddNumberToObject(r, "code", code);
    cJSON_AddStringToObject(r, "message", code == 0 ? "success" : (message ? message : CodeMsg(code)));
    if (params) *params = p;
    return r;
}

// Publish a response and log the outcome. Takes the response.
void Send(const Request& rq, cJSON* response) {
    char* text = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (!text) {
        ESP_LOGE(TAG, "%s: no memory for the response", rq.action.c_str());
        return;
    }
    const int       n = static_cast<int>(strlen(text));
    const esp_err_t r = s_hooks.publish ? s_hooks.publish(text, static_cast<size_t>(n)) : ESP_ERR_INVALID_STATE;
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "%s -> up %.*s%s", rq.action.c_str(), n < kLogMax ? n : kLogMax, text, n > kLogMax ? " ..." : "");
    } else {
        ESP_LOGW(TAG, "%s: response not sent (%s)", rq.action.c_str(), esp_err_to_name(r));
    }
    cJSON_free(text);
}

void Fail(const Request& rq, int code, const char* message) { Send(rq, NewResponse(rq, code, message)); }

// Run a command through the core and respond with its answer.
void Answer(const Request& rq, uint8_t cmd, const std::vector<uint8_t>& payload, Decode decode) {
    if (!Run(cmd, payload)) {
        Fail(rq, kErrInvalid, "the device did not answer");
        return;
    }
    const int code = (s_pending.status == 0) ? 0 : (s_pending.error ? s_pending.error : kErrRejected);
    cJSON* params   = nullptr;
    cJSON* response = NewResponse(rq, code, nullptr, &params);
    if (code == 0 && decode) decode(s_pending.extra, params);
    Send(rq, response);
}

// params {"cmd": BLE command id 0-255, "raw": BLE payload in base64}: the way to reach a command a
// board implements in its on_command. The response's params carry the answer's data as "raw".
void RunRaw(const Request& rq, const cJSON* params) {
    double c = 0;
    if (!Integer(cJSON_GetObjectItemCaseSensitive(params, "cmd"), 0, 255, &c)) {
        Fail(rq, kErrInvalid, "params.cmd must be an integer 0-255");
        return;
    }
    std::vector<uint8_t> payload;
    const cJSON* raw = cJSON_GetObjectItemCaseSensitive(params, "raw");
    if (raw && (!cJSON_IsString(raw) || !Unbase64(raw->valuestring, payload))) {
        Fail(rq, kErrInvalid, "params.raw must be base64");
        return;
    }
    Answer(rq, static_cast<uint8_t>(c), payload, DecodeRaw);
}

void Dispatch(const Request& rq, const cJSON* params) {
    const char* action = rq.action.c_str();
    if (strcmp(action, "ping") == 0) {
        Send(rq, NewResponse(rq, 0, nullptr));
        return;
    }
    if (strcmp(action, "ota_start") == 0) {
        // Over WiFi the device will download the image itself, which is not built yet.
        Fail(rq, kErrRejected, "OTA over WiFi is not implemented yet");
        return;
    }
    if (strcmp(action, "raw") == 0) {
        RunRaw(rq, params);
        return;
    }
    const Named* n = nullptr;
    for (const auto& e : kNamed) {
        if (strcmp(e.action, action) == 0) { n = &e; break; }
    }
    if (!n) {
        Fail(rq, kErrUnknownCommand, "unknown action");
        return;
    }
    std::vector<uint8_t> payload;
    s_err[0] = '\0';
    if (n->build && !n->build(params, payload)) {
        Fail(rq, kErrInvalid, s_err[0] ? s_err : nullptr);
        return;
    }
    Answer(rq, n->cmd, payload, n->decode);
}
}  // namespace

void cloud_json_set_hooks(const cloud_json_hooks_t* hooks) {
    s_hooks = hooks ? *hooks : cloud_json_hooks_t{};
}

void cloud_json_handle_command(const char* json, size_t len) {
    cJSON*       in  = cJSON_ParseWithLength(json, len);
    const cJSON* jid = cJSON_GetObjectItemCaseSensitive(in, "msg_id");
    if (!cJSON_IsObject(in) || !cJSON_IsString(jid) || !jid->valuestring[0]) {
        ESP_LOGW(TAG, "not an envelope with a msg_id — ignored");   // nothing to reply to
        cJSON_Delete(in);
        return;
    }
    Request rq;
    rq.msg_id = jid->valuestring;

    const cJSON* type    = cJSON_GetObjectItemCaseSensitive(in, "type");
    const char*  kind    = cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(kind, "response") == 0 || strcmp(kind, "event") == 0) {
        // Nothing the device sent asks for a response, and no platform event has a handler yet.
        // Neither is answered, by definition.
        ESP_LOGW(TAG, "%s %s ignored: only requests are handled", kind, rq.msg_id.c_str());
        cJSON_Delete(in);
        return;
    }
    const cJSON* action = cJSON_GetObjectItemCaseSensitive(in, "action");
    if (cJSON_IsString(action) && action->valuestring[0]) rq.action = action->valuestring;
    const cJSON* ts     = cJSON_GetObjectItemCaseSensitive(in, "timestamp");
    const cJSON* params = cJSON_GetObjectItemCaseSensitive(in, "params");
    double       t      = 0;

    if (strcmp(kind, "request") != 0) {
        Fail(rq, kErrInvalid, "type must be request, response or event");
    } else if (!cJSON_IsString(action) || !action->valuestring[0]) {
        Fail(rq, kErrInvalid, "action is missing");
    } else if (!Integer(ts, 1e12, 1e13 - 1, &t)) {
        Fail(rq, kErrInvalid, "timestamp must be a 13-digit Unix time in ms");
    } else if (!cJSON_IsObject(params)) {
        Fail(rq, kErrInvalid, "params must be an object");
    } else if (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(in, "reply_to")) ||
               !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(in, "code")) ||
               !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(in, "message"))) {
        // Present and null: a missing one is as much a malformed request as a set one.
        Fail(rq, kErrInvalid, "a request carries reply_to, code and message, all null");
    } else {
        Dispatch(rq, params);
    }
    cJSON_Delete(in);
}

esp_err_t cloud_json_on_frame(const uint8_t* frame, size_t len) {
    agentlink::Frame f;
    if (!agentlink::ParseFrame(frame, len, f)) return ESP_ERR_INVALID_ARG;
    const bool ours = s_run_task.load(std::memory_order_acquire) == xTaskGetCurrentTaskHandle();

    if (f.msg_type == agentlink::kMsgResponse) {
        // payload = acked_cmd(1) status(1) error(2) extra(N)
        if (!ours || f.command_id != s_pending.cmd || f.sequence != s_pending.seq || f.payload.size() < 4) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        s_pending.answered = true;
        s_pending.status   = f.payload[1];
        s_pending.error    = GetU16(f.payload.data() + 2);
        s_pending.extra.assign(f.payload.begin() + 4, f.payload.end());
        return ESP_OK;
    }
    if (f.msg_type == agentlink::kMsgEvent && f.command_id == kEvtManifest && ours &&
        s_pending.cmd == kCmdManifest) {
        // chunk = idx(1) last(1) json(N), sent one after another while the 0x34 runs.
        if (f.payload.size() >= 2 && s_manifest.size() + f.payload.size() <= kManifestMax) {
            s_manifest.append(reinterpret_cast<const char*>(f.payload.data()) + 2, f.payload.size() - 2);
            if (f.payload[1] == 0x01) s_manifest_whole = true;
        }
        return ESP_OK;
    }
    if (f.msg_type == agentlink::kMsgEvent &&
        (f.command_id == kEvtManifest || f.command_id == kEvtManifestChanged)) {
        s_manifest_stale.store(true, std::memory_order_release);   // the endpoints changed
    }
    return ESP_ERR_NOT_SUPPORTED;
}
