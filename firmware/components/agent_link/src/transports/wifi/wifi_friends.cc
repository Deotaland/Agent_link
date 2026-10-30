#include "agent_link_friends.h"
#include "friend_protocol.h"
#include "wifi_local_radio.h"
#include <atomic>
#include <cstddef>
#include <mutex>
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#if ESP_IDF_VERSION_MAJOR >= 6
#include "psa/crypto.h"
#else
#include "mbedtls/ecdh.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#endif

namespace {
using namespace agentlink::friends;
constexpr char TAG[] = "agent_link.friends";
constexpr uint8_t kChannel = 6;

// WiFi callbacks only update counters; the worker prints bounded diagnostics.
// No key material, pairing code or peer identity is included in these logs.
struct Diagnostics {
    std::atomic<uint32_t> queued{0}, enqueue_failed{0}, sent{0}, send_failed{0};
    std::atomic<uint32_t> received_unicast{0}, received_broadcast{0}, invalid{0}, dropped{0}, auth_failed{0};
    void Reset() {
        queued = enqueue_failed = sent = send_failed = 0;
        received_unicast = received_broadcast = invalid = dropped = auth_failed = 0;
    }
} diagnostics;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void Sent(const esp_now_send_info_t*, esp_now_send_status_t status) {
#else
void Sent(const uint8_t*, esp_now_send_status_t status) {
#endif
    if (status == ESP_NOW_SEND_SUCCESS) ++diagnostics.sent;
    else ++diagnostics.send_failed;
}
enum class Command { Start, Stop, Select, Accept, Reject, Greet, Disconnect, Rx };
struct Event { Command command; uint8_t mac[6] = {}; int rssi = 0; bool unicast = false; Packet packet; };
std::mutex snapshot_lock, api_lock;
agent_friend_snapshot_t snapshot = {};
QueueHandle_t events = nullptr;
std::atomic<bool> requested{false};

class EspRadio : public Radio {
public:
    EspRadio() {
#if ESP_IDF_VERSION_MAJOR < 6
        mbedtls_ecp_group_init(&group_); mbedtls_mpi_init(&private_); mbedtls_ecp_point_init(&public_);
#endif
    }
    esp_err_t Open(uint8_t key[65]) {
        esp_err_t result = agent_wifi_local_begin(kChannel);
        if (result != ESP_OK) return result;
        leased_ = true;
        esp_wifi_get_ps(&previous_ps_); esp_wifi_set_ps(WIFI_PS_NONE);
        result = esp_wifi_get_mac(WIFI_IF_STA, mac_);
        if (result != ESP_OK) return result;
        size_t size = 0;
#if ESP_IDF_VERSION_MAJOR >= 6
        if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256); psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
        psa_status_t status = psa_generate_key(&attributes, &private_);
        psa_reset_key_attributes(&attributes);
        if (status != PSA_SUCCESS || psa_export_public_key(private_, key, 65, &size) != PSA_SUCCESS || size != 65) return ESP_FAIL;
#else
        if (mbedtls_ecp_group_load(&group_, MBEDTLS_ECP_DP_SECP256R1) ||
            mbedtls_ecp_gen_keypair(&group_, &private_, &public_, Random, nullptr)) return ESP_FAIL;
        if (mbedtls_ecp_point_write_binary(&group_, &public_, MBEDTLS_ECP_PF_UNCOMPRESSED, &size, key, 65) || size != 65) return ESP_FAIL;
#endif
        result = esp_now_init();
        if (result != ESP_OK) return result;
        now_ = true;
        diagnostics.Reset();
        result = esp_now_register_send_cb(Sent);
        if (result != ESP_OK) return result;
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, kBroadcast, 6); peer.channel = kChannel; peer.ifidx = WIFI_IF_STA;
        return esp_now_add_peer(&peer);
    }
    esp_err_t Close() {
        Forget();
        if (now_) { esp_now_unregister_recv_cb(); esp_now_unregister_send_cb(); esp_now_deinit(); now_ = false; }
#if ESP_IDF_VERSION_MAJOR >= 6
        if (private_) psa_destroy_key(private_);
        private_ = 0;
#else
        mbedtls_mpi_free(&private_); mbedtls_mpi_init(&private_);
        mbedtls_ecp_point_free(&public_); mbedtls_ecp_point_init(&public_);
        mbedtls_ecp_group_free(&group_); mbedtls_ecp_group_init(&group_);
#endif
        if (!leased_) return ESP_OK;
        esp_wifi_set_ps(previous_ps_); leased_ = false;
        return agent_wifi_local_end();
    }
    const uint8_t* Mac() const { return mac_; }
    uint64_t RenewNonce() override {
        do { esp_fill_random(&nonce_, sizeof nonce_); } while (!nonce_);
        return nonce_;
    }
    bool Pair(const uint8_t mac[6], uint64_t nonce, const uint8_t key[65], uint32_t* code) override {
        uint8_t material[60] = {}, digest[32] = {};
        int result;
#if ESP_IDF_VERSION_MAJOR >= 6
        size_t size = 0;
        result = psa_raw_key_agreement(PSA_ALG_ECDH, private_, key, 65, material, 32, &size);
        if (size != 32) result = -1;
#else
        mbedtls_ecp_point other; mbedtls_ecp_point_init(&other);
        mbedtls_mpi secret; mbedtls_mpi_init(&secret);
        result = mbedtls_ecp_point_read_binary(&group_, &other, key, 65);
        if (!result) result = mbedtls_ecp_check_pubkey(&group_, &other);
        if (!result) result = mbedtls_ecdh_compute_shared(&group_, &secret, &other, &private_, Random, nullptr);
        if (!result) result = mbedtls_mpi_write_binary(&secret, material, 32);
        mbedtls_mpi_free(&secret); mbedtls_ecp_point_free(&other);
#endif
        if (result) return false;
        const bool first = memcmp(mac_, mac, 6) < 0;
        memcpy(material + 32, first ? mac_ : mac, 6);
        memcpy(material + 38, first ? mac : mac_, 6);
        memcpy(material + 44, first ? &nonce_ : &nonce, 8);
        memcpy(material + 52, first ? &nonce : &nonce_, 8);
#if ESP_IDF_VERSION_MAJOR >= 6
        size_t hashed = 0;
        result = psa_hash_compute(PSA_ALG_SHA_256, material, sizeof material, digest, sizeof digest, &hashed);
        if (hashed != 32) result = -1;
#else
        result = mbedtls_sha256(material, sizeof material, digest, 0);
#endif
        memset(material, 0, sizeof material);
        if (result) return false;
        Forget(); memcpy(session_key_, digest, 32); memset(digest, 0, sizeof digest);
        // Both devices must use the same PMK. Derive it from the shared session
        // secret with a separate label, before installing the encrypted peer.
        // A per-device random PMK lets discovery work but breaks confirmations.
        const uint8_t pmk_label[] = {'P','M','K','1'};
        if (!Mac(pmk_label, sizeof pmk_label, digest)) { Forget(); return false; }
        result = esp_now_set_pmk(digest);  // ESP-NOW consumes the first 16 bytes.
        memset(digest, 0, sizeof digest);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "pair PMK setup failed: %s", esp_err_to_name(result));
            Forget(); return false;
        }
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac, 6); memcpy(peer.lmk, session_key_, 16);
        peer.channel = kChannel; peer.ifidx = WIFI_IF_STA; peer.encrypt = true;
        result = esp_now_add_peer(&peer);
        memset(peer.lmk, 0, sizeof peer.lmk);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "encrypted peer setup failed: %s", esp_err_to_name(result));
            Forget(); return false;
        }
        memcpy(peer_, mac, 6); paired_ = true;
        // A separate HMAC authenticates application messages even if a forged
        // unencrypted unicast is delivered by the radio. The SAS is never a key.
        const uint8_t label[] = {'S','A','S','1'};
        if (!Mac(label, sizeof label, digest)) {
            Forget(); return false;
        }
        *code = static_cast<uint32_t>(digest[0]) << 24 | static_cast<uint32_t>(digest[1]) << 16 |
                static_cast<uint32_t>(digest[2]) << 8 | digest[3];
        memset(digest, 0, sizeof digest);
        ESP_LOGI(TAG, "pair keys installed: session-derived PMK, channel=%u", kChannel);
        return true;
    }
    bool Send(const uint8_t mac[6], const Packet& packet, bool encrypted) override {
        Packet outgoing = packet;
        if (encrypted) {
            if (!paired_ || memcmp(peer_, mac, 6)) { ++diagnostics.enqueue_failed; return false; }
            uint8_t digest[32];
            if (!Tag(outgoing, digest)) { ++diagnostics.enqueue_failed; return false; }
            memcpy(outgoing.tag, digest, sizeof outgoing.tag); memset(digest, 0, sizeof digest);
        }
        const esp_err_t result = esp_now_send(mac, reinterpret_cast<const uint8_t*>(&outgoing), sizeof outgoing);
        if (result == ESP_OK) ++diagnostics.queued;
        else if (++diagnostics.enqueue_failed <= 3)
            ESP_LOGW(TAG, "send enqueue failed (type=%u): %s", static_cast<unsigned>(packet.type), esp_err_to_name(result));
        return result == ESP_OK;
    }
    bool Verify(const Packet& packet) override {
        uint8_t digest[32];
        if (!paired_ || !Tag(packet, digest)) { ++diagnostics.auth_failed; return false; }
        uint8_t difference = 0;
        for (size_t i = 0; i < sizeof packet.tag; ++i) difference |= digest[i] ^ packet.tag[i];
        memset(digest, 0, sizeof digest);
        if (difference) ++diagnostics.auth_failed;
        return difference == 0;
    }
    void Forget() override {
        if (paired_) { esp_now_del_peer(peer_); paired_ = false; }
        memset(peer_, 0, sizeof peer_); memset(session_key_, 0, sizeof session_key_);
    }
private:
    bool Tag(const Packet& packet, uint8_t digest[32]) {
        return Mac(reinterpret_cast<const uint8_t*>(&packet), offsetof(Packet, tag), digest);
    }
    bool Mac(const uint8_t* input, size_t length, uint8_t digest[32]) {
#if ESP_IDF_VERSION_MAJOR >= 6
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC); psa_set_key_bits(&attributes, 256);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
        psa_key_id_t key = 0;
        psa_status_t result = psa_import_key(&attributes, session_key_, sizeof session_key_, &key);
        psa_reset_key_attributes(&attributes);
        size_t size = 0;
        if (result == PSA_SUCCESS) result = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), input, length, digest, 32, &size);
        if (key) psa_destroy_key(key);
        return result == PSA_SUCCESS && size == 32;
#else
        return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), session_key_, 32, input, length, digest) == 0;
#endif
    }
#if ESP_IDF_VERSION_MAJOR >= 6
    psa_key_id_t private_ = 0;
#else
    static int Random(void*, unsigned char* out, size_t size) { esp_fill_random(out, size); return 0; }
    mbedtls_ecp_group group_;
    mbedtls_mpi private_;
    mbedtls_ecp_point public_;
#endif
    uint8_t mac_[6] = {}, peer_[6] = {}, session_key_[32] = {};
    uint64_t nonce_ = 0;
    wifi_ps_type_t previous_ps_ = WIFI_PS_MIN_MODEM;
    bool leased_ = false, now_ = false, paired_ = false;
};

void Publish(const agent_friend_snapshot_t& view) {
    std::lock_guard<std::mutex> guard(snapshot_lock);
    if (snapshot.state != view.state || snapshot.local_confirmed != view.local_confirmed ||
        snapshot.remote_confirmed != view.remote_confirmed)
        ESP_LOGI(TAG, "state %d -> %d, local_confirmed=%d remote_confirmed=%d event=%d",
            snapshot.state, view.state, view.local_confirmed, view.remote_confirmed, view.event);
    snapshot = view;
}
void Receive(const esp_now_recv_info_t* info, const uint8_t* data, int size) {
    if (!requested.load() || !events) return;
    if (!info || !data || size != sizeof(Packet) || !info->src_addr || !info->des_addr) {
        ++diagnostics.invalid; return;
    }
    Event event{}; event.command = Command::Rx;
    memcpy(event.mac, info->src_addr, 6); memcpy(&event.packet, data, size);
    if (!Valid(event.packet)) { ++diagnostics.invalid; return; }
    event.unicast = memcmp(info->des_addr, kBroadcast, 6) != 0;
    event.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    if (event.unicast) ++diagnostics.received_unicast;
    else ++diagnostics.received_broadcast;
    if (xQueueSend(events, &event, 0) != pdTRUE) ++diagnostics.dropped;
}
void Worker(void*) {
    EspRadio radio;
    Engine engine(radio);
    bool running = false;
    int64_t diagnostic_at = 0;
    while (true) {
        Event event{};
        const bool received = xQueueReceive(events, &event, pdMS_TO_TICKS(40)) == pdTRUE;
        const int64_t now = esp_timer_get_time() / 1000;
        if (received && event.command == Command::Start && requested.load() && !running) {
            uint8_t key[65];
            esp_err_t result = radio.Open(key);
            if (result == ESP_OK) result = esp_now_register_recv_cb(Receive);
            if (result == ESP_OK) {
                engine.Begin(radio.Mac(), key, now); running = true; diagnostic_at = now + 5000;
            } else {
                (void)radio.Close(); requested.store(false);
                agent_friend_snapshot_t failed{}; failed.state = AGENT_FRIEND_ERROR; failed.error = result; Publish(failed);
            }
        } else if (received && event.command == Command::Stop) {
            if (running) engine.Disconnect(now);
            running = false;
            const esp_err_t result = radio.Close();
            agent_friend_snapshot_t stopped{}; stopped.state = result == ESP_OK ? AGENT_FRIEND_OFF : AGENT_FRIEND_ERROR;
            stopped.error = result; Publish(stopped);
        } else if (received && running && requested.load()) {
            switch (event.command) {
            case Command::Select: engine.Select(event.mac, now); break;
            case Command::Accept: engine.Confirm(true, now); break;
            case Command::Reject: engine.Confirm(false, now); break;
            case Command::Greet: engine.Greet(now); break;
            case Command::Disconnect: engine.Disconnect(now); break;
            case Command::Rx: engine.Receive(event.mac, event.packet, event.rssi, event.unicast, now); break;
            default: break;
            }
        }
        if (running && requested.load()) {
            engine.Tick(now); Publish(engine.View());
            if (now >= diagnostic_at) {
                diagnostic_at = now + 5000;
                const auto& view = engine.View();
                uint8_t channel = 0; wifi_second_chan_t secondary;
                const esp_err_t channel_result = esp_wifi_get_channel(&channel, &secondary);
                ESP_LOGI(TAG, "radio state=%d local=%d remote=%d channel=%u channel_error=%s tx_queued=%lu enqueue_fail=%lu mac_ok=%lu mac_fail=%lu",
                    view.state, view.local_confirmed, view.remote_confirmed, channel, esp_err_to_name(channel_result),
                    static_cast<unsigned long>(diagnostics.queued.load()), static_cast<unsigned long>(diagnostics.enqueue_failed.load()),
                    static_cast<unsigned long>(diagnostics.sent.load()), static_cast<unsigned long>(diagnostics.send_failed.load()));
                ESP_LOGI(TAG, "radio rx_unicast=%lu rx_broadcast=%lu invalid=%lu queue_drop=%lu auth_fail=%lu",
                    static_cast<unsigned long>(diagnostics.received_unicast.load()), static_cast<unsigned long>(diagnostics.received_broadcast.load()),
                    static_cast<unsigned long>(diagnostics.invalid.load()), static_cast<unsigned long>(diagnostics.dropped.load()),
                    static_cast<unsigned long>(diagnostics.auth_failed.load()));
            }
        }
    }
}
esp_err_t SendCommand(Command command, const uint8_t* mac = nullptr) {
    if (!events) return ESP_ERR_INVALID_STATE;
    Event event{}; event.command = command;
    if (mac) memcpy(event.mac, mac, 6);
    return xQueueSendToFront(events, &event, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
}  // namespace

extern "C" void agent_link_friends_snapshot(agent_friend_snapshot_t* out) {
    if (!out) return;
    std::lock_guard<std::mutex> guard(snapshot_lock); *out = snapshot;
}
extern "C" esp_err_t agent_link_friends_start() {
    std::lock_guard<std::mutex> guard(api_lock);
    agent_friend_snapshot_t current{}; agent_link_friends_snapshot(&current);
    if (requested.load() || (current.state != AGENT_FRIEND_OFF && current.state != AGENT_FRIEND_ERROR)) return ESP_ERR_INVALID_STATE;
    if (!events) {
        events = xQueueCreate(24, sizeof(Event));
        if (!events) return ESP_ERR_NO_MEM;
        if (xTaskCreate(Worker, "al_friends", 8192, nullptr, 4, nullptr) != pdPASS) {
            vQueueDelete(events); events = nullptr; return ESP_ERR_NO_MEM;
        }
    }
    current = {}; current.state = AGENT_FRIEND_STARTING; Publish(current); requested.store(true);
    const esp_err_t result = SendCommand(Command::Start);
    if (result != ESP_OK) { requested.store(false); current.state = AGENT_FRIEND_ERROR; current.error = result; Publish(current); }
    return result;
}
extern "C" esp_err_t agent_link_friends_stop() {
    std::lock_guard<std::mutex> guard(api_lock);
    if (!events) return ESP_OK;
    requested.store(false);
    agent_friend_snapshot_t current{}; agent_link_friends_snapshot(&current); current.state = AGENT_FRIEND_STOPPING; Publish(current);
    return SendCommand(Command::Stop);
}
extern "C" esp_err_t agent_link_friends_select(const uint8_t mac[6]) {
    if (!mac || (mac[0] & 1)) return ESP_ERR_INVALID_ARG;
    return requested.load() ? SendCommand(Command::Select, mac) : ESP_ERR_INVALID_STATE;
}
extern "C" esp_err_t agent_link_friends_confirm(bool accept) { return requested.load() ? SendCommand(accept ? Command::Accept : Command::Reject) : ESP_ERR_INVALID_STATE; }
extern "C" esp_err_t agent_link_friends_greet() { return requested.load() ? SendCommand(Command::Greet) : ESP_ERR_INVALID_STATE; }
extern "C" esp_err_t agent_link_friends_disconnect() { return requested.load() ? SendCommand(Command::Disconnect) : ESP_ERR_INVALID_STATE; }
