// Exercise the production EspRadio adapter with two independent ESP-NOW devices.
// Deterministic crypto fakes model shared-secret symmetry, not cryptographic
// strength. Tests verify actual PMK/peer installation and signed packet paths.
#include "friend_protocol.h"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#define ESP_IDF_VERSION_MAJOR 5
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,4)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr int WIFI_IF_STA = 0, WIFI_PS_NONE = 0, WIFI_PS_MIN_MODEM = 1;
constexpr int MBEDTLS_ECP_DP_SECP256R1 = 3, MBEDTLS_ECP_PF_UNCOMPRESSED = 0, MBEDTLS_MD_SHA256 = 6;
constexpr int ESP_NOW_SEND_SUCCESS = 0, ESP_NOW_SEND_FAIL = 1;
using wifi_ps_type_t = int;
using esp_now_send_status_t = int;
using QueueHandle_t = void*;
struct esp_now_send_info_t {};
using SendCallback = void (*)(const esp_now_send_info_t*, esp_now_send_status_t);
struct esp_now_peer_info_t {
    uint8_t peer_addr[6]{}, lmk[16]{};
    uint8_t channel = 0;
    int ifidx = 0;
    bool encrypt = false;
};
struct Device {
    uint8_t mac[6]{2,0,0,0,0,0}, pmk[16]{}, lmk[16]{};
    uint32_t random = 1;
    int pmk_calls = 0, pmk_error = ESP_OK, peer_error = ESP_OK, send_error = ESP_OK;
    bool pmk_set = false, peer_set = false, opened = false;
    SendCallback callback = nullptr;
    agentlink::friends::Packet outgoing{};
};
Device* device = nullptr;
int agent_wifi_local_begin(uint8_t channel) { assert(channel == 6); return ESP_OK; }
int agent_wifi_local_end() { return ESP_OK; }
int esp_wifi_get_ps(int* value) { *value = WIFI_PS_MIN_MODEM; return ESP_OK; }
int esp_wifi_set_ps(int) { return ESP_OK; }
int esp_wifi_get_mac(int iface, uint8_t* mac) { assert(iface == WIFI_IF_STA); memcpy(mac, device->mac, 6); return ESP_OK; }
void esp_fill_random(void* dest, size_t size) {
    auto* bytes = static_cast<uint8_t*>(dest);
    for (size_t i = 0; i < size; ++i) { device->random = device->random * 1664525u + 1013904223u; bytes[i] = device->random >> 24; }
}
int esp_now_init() { device->opened = true; return ESP_OK; }
int esp_now_register_send_cb(SendCallback cb) { device->callback = cb; return ESP_OK; }
int esp_now_unregister_recv_cb() { return ESP_OK; }
int esp_now_unregister_send_cb() { device->callback = nullptr; return ESP_OK; }
int esp_now_deinit() { device->opened = false; device->pmk_set = device->peer_set = false; return ESP_OK; }
int esp_now_set_pmk(const uint8_t* pmk) {
    assert(device->opened); ++device->pmk_calls;
    if (device->pmk_error != ESP_OK) return device->pmk_error;
    memcpy(device->pmk, pmk, 16); device->pmk_set = true; return ESP_OK;
}
int esp_now_add_peer(const esp_now_peer_info_t* peer) {
    assert(device->opened && peer->channel == 6 && peer->ifidx == WIFI_IF_STA);
    if (peer->encrypt) {
        // Enforce the driver setup boundary: never add an encrypted peer with
        // an unrelated/default PMK and try to repair it after installation.
        assert(device->pmk_set);
        if (device->peer_error != ESP_OK) return device->peer_error;
        memcpy(device->lmk, peer->lmk, 16); device->peer_set = true;
    }
    return ESP_OK;
}
int esp_now_del_peer(const uint8_t*) { device->peer_set = false; return ESP_OK; }
int esp_now_send(const uint8_t*, const uint8_t* data, size_t size) {
    assert(size == sizeof(device->outgoing));
    if (device->send_error != ESP_OK) return device->send_error;
    memcpy(&device->outgoing, data, size);
    if (device->callback) device->callback(nullptr, ESP_NOW_SEND_SUCCESS);
    return ESP_OK;
}

struct mbedtls_ecp_group {};
struct mbedtls_mpi { uint64_t value = 0; };
struct mbedtls_ecp_point { uint64_t value = 0; };
void mbedtls_ecp_group_init(mbedtls_ecp_group*) {}
void mbedtls_ecp_group_free(mbedtls_ecp_group*) {}
void mbedtls_mpi_init(mbedtls_mpi* value) { value->value = 0; }
void mbedtls_mpi_free(mbedtls_mpi* value) { value->value = 0; }
void mbedtls_ecp_point_init(mbedtls_ecp_point* value) { value->value = 0; }
void mbedtls_ecp_point_free(mbedtls_ecp_point* value) { value->value = 0; }
int mbedtls_ecp_group_load(mbedtls_ecp_group*, int curve) { assert(curve == MBEDTLS_ECP_DP_SECP256R1); return 0; }
using Random = int (*)(void*, unsigned char*, size_t);
int mbedtls_ecp_gen_keypair(mbedtls_ecp_group*, mbedtls_mpi* priv, mbedtls_ecp_point* pub, Random random, void* context) {
    random(context, reinterpret_cast<uint8_t*>(&priv->value), sizeof priv->value); pub->value = priv->value; return 0;
}
int mbedtls_ecp_point_write_binary(mbedtls_ecp_group*, mbedtls_ecp_point* pub, int, size_t* used, uint8_t* key, size_t size) {
    assert(size == 65); memset(key, 0, size); key[0] = 4; memcpy(key + 1, &pub->value, 8); *used = size; return 0;
}
int mbedtls_ecp_point_read_binary(mbedtls_ecp_group*, mbedtls_ecp_point* pub, const uint8_t* key, size_t size) {
    assert(size == 65 && key[0] == 4); memcpy(&pub->value, key + 1, 8); return 0;
}
int mbedtls_ecp_check_pubkey(mbedtls_ecp_group*, mbedtls_ecp_point*) { return 0; }
int mbedtls_ecdh_compute_shared(mbedtls_ecp_group*, mbedtls_mpi* shared, mbedtls_ecp_point* pub, mbedtls_mpi* priv, Random, void*) {
    shared->value = pub->value ^ priv->value; return 0;
}
int mbedtls_mpi_write_binary(mbedtls_mpi* value, uint8_t* out, size_t size) {
    memset(out, 0, size); memcpy(out + size - 8, &value->value, 8); return 0;
}
void Hash(const uint8_t* data, size_t size, uint8_t digest[32]) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    for (int i = 0; i < 32; ++i) { hash = (hash ^ (hash >> 13)) * 16777619u; digest[i] = hash >> 24; }
}
int mbedtls_sha256(const uint8_t* data, size_t size, uint8_t digest[32], int) { Hash(data, size, digest); return 0; }
const int* mbedtls_md_info_from_type(int kind) { static int value = kind; return &value; }
int mbedtls_md_hmac(const int*, const uint8_t* key, size_t key_size, const uint8_t* data, size_t size, uint8_t digest[32]) {
    std::vector<uint8_t> input(key, key + key_size); input.insert(input.end(), data, data + size);
    Hash(input.data(), input.size(), digest); return 0;
}

// INSERT_PRODUCTION_RADIO
} // production anonymous namespace

int main() {
    Device a{}, b{}; a.mac[5] = 1; b.mac[5] = 2; b.random = 100;
    EspRadio ra, rb;
    uint8_t key_a[65], key_b[65];
    device = &a; assert(ra.Open(key_a) == ESP_OK); const auto nonce_a = ra.RenewNonce();
    device = &b; assert(rb.Open(key_b) == ESP_OK); const auto nonce_b = rb.RenewNonce();
    // Regression: independent random PMKs at Open() cannot satisfy both sides.
    assert(a.pmk_calls == 0 && b.pmk_calls == 0 && a.callback && b.callback);
    uint32_t code_a = 0, code_b = 0;
    device = &a; assert(ra.Pair(b.mac, nonce_b, key_b, &code_a));
    device = &b; assert(rb.Pair(a.mac, nonce_a, key_a, &code_b));
    assert(a.pmk_calls == 1 && b.pmk_calls == 1 && a.peer_set && b.peer_set);
    assert(!memcmp(a.pmk, b.pmk, 16) && !memcmp(a.lmk, b.lmk, 16) && code_a == code_b);
    assert(memcmp(a.pmk, a.lmk, 16)); // PMK uses a separate derivation domain.
    uint8_t old_pmk[16]; memcpy(old_pmk, a.pmk, 16);

    using namespace agentlink::friends;
    Packet confirm; confirm.type = Type::Confirm; confirm.confirmed = 1;
    device = &a; assert(ra.Send(b.mac, confirm, true));
    Packet old = a.outgoing;
    device = &b; assert(rb.Verify(old));
    old.confirmed = 0; assert(!rb.Verify(old)); // signed confirmation cannot be altered.
    old = a.outgoing;
    device = &a; a.send_error = ESP_FAIL; assert(!ra.Send(b.mac, confirm, true)); a.send_error = ESP_OK;
    a.callback(nullptr, ESP_NOW_SEND_FAIL); assert(diagnostics.send_failed.load() == 1);

    // Failed PMK/peer setup must leave no usable stale peer or signing key.
    device = &b; b.pmk_error = ESP_FAIL;
    assert(!rb.Pair(a.mac, nonce_a, key_a, &code_b) && !b.peer_set);
    assert(!rb.Send(a.mac, confirm, true) && !rb.Verify(old));
    b.pmk_error = ESP_OK; b.peer_error = ESP_FAIL;
    assert(!rb.Pair(a.mac, nonce_a, key_a, &code_b) && !b.peer_set);
    b.peer_error = ESP_OK;
    const auto new_nonce_b = rb.RenewNonce();
    assert(rb.Pair(a.mac, nonce_a, key_a, &code_b));
    device = &a; assert(ra.Pair(b.mac, new_nonce_b, key_b, &code_a));
    assert(!memcmp(a.pmk, b.pmk, 16) && !memcmp(a.lmk, b.lmk, 16) && code_a == code_b);
    assert(memcmp(old_pmk, a.pmk, 16)); // new session, new shared PMK.
    device = &b; assert(!rb.Verify(old));
    device = &a; assert(ra.Close() == ESP_OK && !a.callback && !a.opened);
    device = &b; assert(rb.Close() == ESP_OK && !b.callback && !b.opened);
    puts("Production friends radio: symmetric session PMK/LMK, install order, failure cleanup, packet authentication, fresh-session keys and send diagnostics passed (deterministic crypto fakes; no RF test).");
}
