// Production command/manifest/OTA/security routines with deterministic transports.
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <new>
#include "agent_link.h"
#include "protocol.h"
#include "ota_service.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

namespace {
bool ble = true, s_manifest_sent = false;
int s_io_count = 1;
uint16_t mtu = 23;
uint16_t agent_transport_ble_att_mtu() { return mtu; }
bool UsesBleLink() { return ble; }
std::string manifest;
std::string BuildManifestJson() { return manifest; }
std::vector<std::vector<uint8_t>> sent;
esp_err_t Send(void*, const uint8_t* data, size_t size) {
    if (ble) assert(size <= static_cast<size_t>(mtu - 3));
    sent.emplace_back(data, data + size); return ESP_OK;
}
struct Transport { esp_err_t (*send_ctrl)(void*, const uint8_t*, size_t); void* impl; } transport{Send, nullptr};
Transport* s_tx = &transport;
std::atomic<uint32_t> s_link_generation{3};
agent_state_t s_state = AGENT_STATE_READY;
// INSERT_MANIFEST
}
uint32_t agent_link_command_generation(void);
esp_err_t agent_transport_ble_command_reply(uint32_t, const uint8_t*, size_t);
// INSERT_REPLY

// Minimal NimBLE store/connection fixture; the access decision functions are production code.
struct ble_addr_t { uint8_t type; uint8_t val[6]; };
struct ble_gap_conn_desc { struct { bool encrypted = false, bonded = false; } sec_state; ble_addr_t peer_id_addr{}; } connection;
std::vector<ble_addr_t> bonds;
bool descriptor_ok = true, store_ok = true;
#define MYNEWT_VAL(x) 3
int ble_gap_conn_find(uint16_t, ble_gap_conn_desc* out) { *out = connection; return descriptor_ok ? 0 : -1; }
int ble_store_util_bonded_peers(ble_addr_t* out, int* count, int cap) {
    assert(bonds.size() <= static_cast<size_t>(cap)); *count = static_cast<int>(bonds.size());
    for (size_t i = 0; i < bonds.size(); ++i) out[i] = bonds[i];
    return store_ok ? 0 : -1;
}
// INSERT_PEER_POLICY

std::atomic<bool> s_authorized{false};
bool s_event_subscribed = false;
int connections = 0, ready_events = 0, terminations = 0;
void Connected(bool up) { if (up) ++connections; }
void Ready() { ++ready_events; }
void (*s_on_conn)(bool) = Connected;
void (*s_on_ready)() = Ready;
constexpr int BLE_GAP_EVENT_ENC_CHANGE = 1, BLE_GAP_EVENT_REPEAT_PAIRING = 2;
constexpr int BLE_GAP_REPEAT_PAIRING_IGNORE = 0, BLE_ERR_REM_USER_CONN_TERM = 19;
struct SecurityEventData { int type; struct { uint16_t conn_handle = 1; int status = 0; } enc_change; };
int ble_gap_terminate(uint16_t, int) { ++terminations; return 0; }
int SecurityEvent(SecurityEventData* event) {
    switch (event->type) {
// INSERT_SECURITY_EVENTS
    default: return -1;
    }
}
constexpr int BLE_UUID_TYPE_16 = 16, BLE_GATT_ACCESS_OP_WRITE_CHR = 1, BLE_GATT_ACCESS_OP_READ_CHR = 2;
constexpr int BLE_ATT_ERR_UNLIKELY = 1, BLE_ATT_ERR_INSUFFICIENT_AUTHEN = 5, BLE_ATT_ERR_INSUFFICIENT_RES = 17;
struct ble_uuid_t { int type; };
struct ble_uuid16_t { ble_uuid_t u; uint16_t value; };
#define BLE_UUID16(p) reinterpret_cast<const ble_uuid16_t*>(p)
struct TestCharacteristic { const ble_uuid_t* uuid; };
struct ble_gatt_access_ctxt { TestCharacteristic* chr; int op; void* om; };
constexpr uint16_t kChrCmdRequest = 0xffc1, kChrFileCmd = 0xffb1, kChrBatteryLevel = 0x2a19;
constexpr uint16_t kChrManufacturer = 0x2a29, kChrModelNumber = 0x2a24, kChrFirmwareRev = 0x2a26;
uint8_t s_batt_level = 50;
char s_di_mfr[] = "Test", s_di_model[] = "TEST-BOARD", s_di_fw[] = "2.0.0";
int control_calls = 0;
void Receive(const uint8_t*, size_t) { ++control_calls; }
void (*s_on_recv)(const uint8_t*, size_t) = Receive;
int ble_hs_mbuf_to_flat(void*, void* out, size_t, uint16_t* length) {
    *length = 1; static_cast<uint8_t*>(out)[0] = 1; return 0;
}
int os_mbuf_append(void*, const void*, uint16_t) { return 0; }
// INSERT_GATT_ACCESS

struct ble_npl_event { void (*callback)(ble_npl_event*); void* arg; };
std::vector<ble_npl_event*> host_events;
void* ble_npl_event_get_arg(ble_npl_event* event) { return event->arg; }
void ble_npl_event_deinit(ble_npl_event*) {}
void ble_npl_event_init(ble_npl_event* event, void (*fn)(ble_npl_event*), void* arg) {
    event->callback = fn; event->arg = arg;
}
void* nimble_port_get_dflt_eventq() { return nullptr; }
void ble_npl_eventq_put(void*, ble_npl_event* event) { host_events.push_back(event); }
esp_err_t ble_send_ctrl(void*, const uint8_t* data, size_t size) { return Send(nullptr, data, size); }
// INSERT_DEFERRED_REPLY
esp_err_t agent_transport_ble_command_reply(uint32_t generation, const uint8_t* frame, size_t size) {
    return QueueCommandReply(generation, frame, size);
}
void RunHostEvents() {
    while (!host_events.empty()) {
        auto* event = host_events.front(); host_events.erase(host_events.begin()); event->callback(event);
    }
}

namespace agentlink::ota {
std::atomic<agent_ota_state_t> s_state{AGENT_OTA_IDLE};
std::atomic<bool> s_abort{false};
std::atomic<uint32_t> s_received{0};
std::atomic<uint16_t> s_error{0};
struct Partition { uint32_t size; } partition{4096};
Partition* s_part = nullptr;
void* s_buf = nullptr;
void* s_worker = nullptr;
void (*s_cb)(const agent_ota_status_t*, void*) = nullptr;
void* s_cb_ctx = nullptr;
char s_model[40] = "TEST-BOARD", s_version[32]{};
uint32_t s_total = 0, s_last_progress_ms = 0;
uint8_t s_sha_expect[32]{};
constexpr size_t kBufBytes = 1024;
constexpr int MALLOC_CAP_SPIRAM = 1, pdPASS = 1;
int allocations = 0;
bool VersionAllowed(const char*) { return true; }
const char* RunningVersion() { return "2.0.0"; }
Partition* esp_ota_get_next_update_partition(void*) { return &partition; }
void* xStreamBufferCreateWithCaps(size_t, int, int) { ++allocations; return &partition; }
void* xStreamBufferCreate(size_t, int) { ++allocations; return &partition; }
void xStreamBufferReset(void*) {}
void Snapshot(agent_ota_status_t*) {}
void WorkerTask(void*) {}
int xTaskCreate(void (*)(void*), const char*, int, void*, int, void**) { return pdPASS; }
void Cleanup() {}
// INSERT_OTA_START
}

int main() {
    // Every actual wire frame fits the ATT budget, including default/minimum MTUs.
    manifest = std::string(900, 'x');
    for (uint16_t size : {23, 24, 27, 28, 64, 185, 512}) {
        mtu = size; sent.clear(); s_manifest_sent = false; SendManifest(false);
        std::string received;
        for (size_t i = 0; i < sent.size(); ++i) {
            agentlink::Frame frame; assert(agentlink::ParseFrame(sent[i].data(), sent[i].size(), frame));
            assert(frame.command_id == 0x18 && frame.payload[0] == i);
            assert(frame.payload[1] == (i + 1 == sent.size()));
            received.append(reinterpret_cast<const char*>(frame.payload.data() + 2), frame.payload.size() - 2);
        }
        assert(s_manifest_sent && received == manifest);
    }
    mtu = 23; sent.clear(); s_manifest_sent = false; manifest.assign(12 * 256 + 1, 'x');
    assert(!SendManifest(false) && sent.empty() && !s_manifest_sent);
    mtu = 185; assert(SendManifest(false) && s_manifest_sent);
    // Deferred responses preserve sequence/error and reject old connections/oversized payloads.
    mtu = 185; sent.clear(); s_authorized = true;
    agent_command_request_t request{4, 37, 3, 128};
    assert(agent_link_command_reply(&request, 1003, nullptr, 0) == ESP_OK);
    assert(sent.empty()); RunHostEvents();
    agentlink::Frame reply; assert(agentlink::ParseFrame(sent[0].data(), sent[0].size(), reply));
    assert(reply.sequence == 37 && reply.payload[1] == 1 && reply.payload[2] == (1003 & 255));
    sent.clear();
    assert(agent_link_command_reply(&request, 0, nullptr, 0) == ESP_OK);
    ++s_link_generation; RunHostEvents(); assert(sent.empty());
    assert(agent_link_command_reply(&request, 0, nullptr, 0) == ESP_ERR_INVALID_STATE);
    request.link_generation = s_link_generation; request.response_capacity = 1;
    uint8_t bytes[2]{}; assert(agent_link_command_reply(&request, 0, bytes, 2) == ESP_ERR_INVALID_SIZE);
    for (int i = 0; i < 8; ++i) assert(agent_link_command_reply(&request, 0, nullptr, 0) == ESP_OK);
    assert(agent_link_command_reply(&request, 0, nullptr, 0) == ESP_ERR_NO_MEM);
    RunHostEvents(); assert(s_pending_replies == 0);

    connection.peer_id_addr = {0, {1, 2, 3, 4, 5, 6}};
    assert(KnownOrFirstPeer(1));
    bonds.push_back(connection.peer_id_addr); assert(KnownOrFirstPeer(1));
    ++connection.peer_id_addr.val[0]; assert(!KnownOrFirstPeer(1));
    store_ok = false; assert(!KnownOrFirstPeer(1)); store_ok = true;
    descriptor_ok = false; assert(!SecurePeer(1)); descriptor_ok = true;
    assert(!SecurePeer(1)); connection.sec_state.encrypted = true; assert(!SecurePeer(1));
    connection.sec_state.bonded = true; assert(SecurePeer(1));

    ble_uuid16_t uuid{{BLE_UUID_TYPE_16}, kChrCmdRequest}; TestCharacteristic characteristic{&uuid.u};
    ble_gatt_access_ctxt access{&characteristic, BLE_GATT_ACCESS_OP_WRITE_CHR, &uuid};
    s_authorized = false;
    assert(GattAccess(1, 0, &access, nullptr) == BLE_ATT_ERR_INSUFFICIENT_AUTHEN && control_calls == 0);
    SecurityEventData event{BLE_GAP_EVENT_ENC_CHANGE}; s_event_subscribed = true;
    connection.sec_state.encrypted = false;
    SecurityEvent(&event); assert(terminations == 1 && connections == 0 && ready_events == 0);
    connection.sec_state.encrypted = true;
    SecurityEvent(&event); SecurityEvent(&event);
    assert(connections == 1 && ready_events == 1 && s_authorized);
    assert(GattAccess(1, 0, &access, nullptr) == 0 && control_calls == 1);
    uuid.value = kChrFileCmd; connection.sec_state.bonded = false;
    assert(GattAccess(1, 0, &access, nullptr) == BLE_ATT_ERR_INSUFFICIENT_AUTHEN && control_calls == 1);
    event.type = BLE_GAP_EVENT_REPEAT_PAIRING;
    assert(SecurityEvent(&event) == BLE_GAP_REPEAT_PAIRING_IGNORE && bonds.size() == 1);

    uint8_t digest[32]{}; uint16_t error = 0;
    for (const char* model : {static_cast<const char*>(nullptr), "", "OTHER-BOARD"}) {
        assert(!agentlink::ota::Start(100, digest, "3.0.0", model, &error));
        assert(error == agentlink::ota::kErrModelMismatch && agentlink::ota::allocations == 0);
    }
    assert(agentlink::ota::Start(100, digest, "3.0.0", "TEST-BOARD", &error));
    assert(error == 0 && agentlink::ota::allocations == 1);
    puts("Control safety: BLE trust/encryption, OTA model admission, MTU fragments and deferred error/stale replies passed.");
}
