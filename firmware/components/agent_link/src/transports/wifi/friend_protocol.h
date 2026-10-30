#pragma once
#include "agent_link_friends.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace agentlink::friends {
constexpr uint8_t kBroadcast[6] = {255,255,255,255,255,255};
enum class Type : uint8_t { Hello, Offer, Answer, Confirm, Heartbeat, Greeting, Ack, Bye, Reject };
#pragma pack(push, 1)
struct Packet {
    uint32_t magic = 0x31464c41;
    uint8_t version = 1;
    Type type = Type::Hello;
    uint8_t confirmed = 0, reserved = 0;
    uint64_t nonce = 0, target_nonce = 0;
    uint32_t sequence = 0, ack = 0;
    uint8_t target[6] = {};
    char name[24] = {};
    uint8_t public_key[65] = {};
    uint8_t tag[16] = {};
};
#pragma pack(pop)
static_assert(sizeof(Packet) < 250);
inline bool Valid(const Packet& p) {
    if (p.magic != 0x31464c41 || p.version != 1 || p.reserved || !p.nonce || !p.sequence ||
        p.confirmed > 1 || static_cast<unsigned>(p.type) > static_cast<unsigned>(Type::Reject)) return false;
    const char* end = static_cast<const char*>(memchr(p.name, 0, sizeof p.name));
    if (!end || end == p.name) return false;
    for (const char* c = p.name; c != end; ++c)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
              (*c >= '0' && *c <= '9') || *c == '-')) return false;
    return true;
}
struct Radio {
    virtual ~Radio() = default;
    virtual bool Pair(const uint8_t mac[6], uint64_t nonce, const uint8_t key[65], uint32_t* code) = 0;
    virtual bool Send(const uint8_t mac[6], const Packet& packet, bool encrypted) = 0;
    virtual bool Verify(const Packet& packet) = 0;
    virtual uint64_t RenewNonce() = 0;
    virtual void Forget() = 0;
};
// Pure protocol state machine, also exercised by the native two-device tests.
class Engine {
public:
    explicit Engine(Radio& radio) : radio_(radio) {}
    void Begin(const uint8_t mac[6], const uint8_t key[65], int64_t now) {
        view_ = {}; memcpy(mac_, mac, 6); memcpy(public_, key, 65);
        snprintf(view_.local_name, sizeof view_.local_name, "Link-%02X%02X%02X", mac[3],mac[4],mac[5]);
        seq_ = 0; Reset(now, AGENT_FRIEND_EVENT_NONE);
    }
    const agent_friend_snapshot_t& View() const { return view_; }
    void Select(const uint8_t mac[6], int64_t now) {
        if (view_.state != AGENT_FRIEND_SCANNING) return;
        for (int i = 0; i < view_.peer_count; ++i) if (!memcmp(mac, view_.peers[i].mac, 6)) {
            memcpy(remote_, mac, 6); remote_nonce_ = peer_nonces_[i];
            snprintf(view_.remote_name, sizeof view_.remote_name, "%s", view_.peers[i].name);
            view_.state = AGENT_FRIEND_WAITING; deadline_ = now + 30000; next_ = 0;
            view_.event = AGENT_FRIEND_EVENT_NONE; incoming_ = false; Tick(now); return;
        }
    }
    void Confirm(bool accept, int64_t now) {
        if (view_.state != AGENT_FRIEND_CONFIRM) return;
        if (!accept) { Transmit(Type::Bye); Reset(now, AGENT_FRIEND_EVENT_REJECTED); return; }
        view_.local_confirmed = true; Transmit(Type::Confirm); CheckConnected(now);
    }
    void Disconnect(int64_t now) {
        if (view_.state != AGENT_FRIEND_SCANNING) Transmit(keyed_ ? Type::Bye : Type::Reject);
        Reset(now, AGENT_FRIEND_EVENT_NONE);
    }
    void Greet(int64_t now) {
        if (view_.state != AGENT_FRIEND_CONNECTED || greeting_pending_) return;
        greeting_ = Make(Type::Greeting); greeting_pending_ = true; retries_ = 0; greeting_at_ = now;
        radio_.Send(remote_, greeting_, true);
    }
    void Receive(const uint8_t from[6], const Packet& p, int rssi, bool unicast, int64_t now) {
        if (!Valid(p) || !memcmp(from, mac_, 6)) return;
        if (p.type == Type::Hello) {
            if (!unicast && view_.state == AGENT_FRIEND_SCANNING) Discover(from, p, rssi, now);
            return;
        }
        if (memcmp(p.target, mac_, 6) || p.target_nonce != nonce_) return;
        if (p.type == Type::Offer) {
            if (unicast) return;
            if (view_.state == AGENT_FRIEND_SCANNING) {
                memcpy(remote_, from, 6); remote_nonce_ = p.nonce;
                snprintf(view_.remote_name, sizeof view_.remote_name, "%s", p.name);
                incoming_ = true; deadline_ = now + 30000;
            } else if (memcmp(remote_, from, 6) || p.nonce != remote_nonce_) return;
            if (!keyed_ && !Derive(p)) { Reset(now, AGENT_FRIEND_EVENT_REJECTED); return; }
            if (view_.state != AGENT_FRIEND_CONNECTED) view_.state = AGENT_FRIEND_CONFIRM;
            Transmit(Type::Answer); return;
        }
        if (memcmp(remote_, from, 6) || p.nonce != remote_nonce_) return;
        if (p.type == Type::Reject && !unicast && view_.state == AGENT_FRIEND_WAITING) {
            Reset(now, AGENT_FRIEND_EVENT_REJECTED); return;
        }
        if (p.type == Type::Answer) {
            if (unicast || (view_.state != AGENT_FRIEND_WAITING && view_.state != AGENT_FRIEND_CONFIRM)) return;
            if (!keyed_ && !Derive(p)) { Reset(now, AGENT_FRIEND_EVENT_REJECTED); return; }
            view_.state = AGENT_FRIEND_CONFIRM; incoming_ = false; next_ = 0; return;
        }
        // Only encrypted unicast is meaningful after the public key exchange.
        if (!keyed_ || !unicast || !radio_.Verify(p)) return;
        const bool fresh = static_cast<int32_t>(p.sequence - remote_seq_) > 0;
        if (!fresh) {
            if (p.type == Type::Greeting && view_.state == AGENT_FRIEND_CONNECTED) {
                if (static_cast<int32_t>(p.sequence - greeting_rx_) > 0) { greeting_rx_ = p.sequence; ++view_.received; }
                Ack(p.sequence);
            }
            if (p.type == Type::Ack && view_.state == AGENT_FRIEND_CONNECTED && greeting_pending_ && p.ack == greeting_.sequence) {
                greeting_pending_ = false; ++view_.sent;
            }
            return;
        }
        remote_seq_ = p.sequence; last_rx_ = now;
        if (p.type == Type::Bye) { Reset(now, AGENT_FRIEND_EVENT_LOST); return; }
        if (p.type == Type::Confirm || p.type == Type::Heartbeat) {
            view_.remote_confirmed = p.confirmed != 0; CheckConnected(now); return;
        }
        if (view_.state != AGENT_FRIEND_CONNECTED) return;
        if (p.type == Type::Greeting) { greeting_rx_ = p.sequence; ++view_.received; Ack(p.sequence); }
        if (p.type == Type::Ack && greeting_pending_ && p.ack == greeting_.sequence) {
            greeting_pending_ = false; ++view_.sent;
        }
    }
    void Tick(int64_t now) {
        if (view_.state == AGENT_FRIEND_SCANNING) {
            for (int i = 0; i < view_.peer_count;) {
                if (now - peer_seen_[i] <= 5000) { ++i; continue; }
                for (int j = i; j + 1 < view_.peer_count; ++j) {
                    view_.peers[j] = view_.peers[j+1]; peer_seen_[j] = peer_seen_[j+1]; peer_nonces_[j] = peer_nonces_[j+1];
                }
                --view_.peer_count;
            }
            if (now >= next_) { Transmit(Type::Hello); next_ = now + 650 + (mac_[5] % 7) * 31; }
            return;
        }
        if (view_.state == AGENT_FRIEND_WAITING || view_.state == AGENT_FRIEND_CONFIRM) {
            if (now > deadline_) { Transmit(keyed_ ? Type::Bye : Type::Reject); Reset(now, AGENT_FRIEND_EVENT_TIMEOUT); return; }
            if (now >= next_) {
                if (!keyed_) Transmit(Type::Offer);
                else { if (incoming_) Transmit(Type::Answer); Transmit(Type::Confirm); }
                next_ = now + 500;
            }
        }
        if (view_.state == AGENT_FRIEND_CONNECTED) {
            if (now - last_rx_ > 5000) { Reset(now, AGENT_FRIEND_EVENT_LOST); return; }
            if (now >= next_) { Transmit(Type::Heartbeat); next_ = now + 700; }
            if (greeting_pending_ && now - greeting_at_ > 500) {
                if (++retries_ > 5) { greeting_pending_ = false; view_.event = AGENT_FRIEND_EVENT_GREETING_FAILED; }
                else { radio_.Send(remote_, greeting_, true); greeting_at_ = now; }
            }
        }
    }
private:
    Packet Make(Type type) {
        Packet p; p.type = type; p.nonce = nonce_; p.sequence = ++seq_;
        p.target_nonce = remote_nonce_; memcpy(p.target, remote_, 6);
        p.confirmed = view_.local_confirmed; snprintf(p.name, sizeof p.name, "%s", view_.local_name);
        if (type == Type::Hello || type == Type::Offer || type == Type::Answer) memcpy(p.public_key, public_, 65);
        return p;
    }
    void Transmit(Type type) {
        Packet p = Make(type);
        const bool public_frame = type == Type::Hello || type == Type::Offer || type == Type::Answer || type == Type::Reject;
        radio_.Send(public_frame ? kBroadcast : remote_, p, !public_frame);
    }
    void Ack(uint32_t sequence) { Packet p = Make(Type::Ack); p.ack = sequence; radio_.Send(remote_, p, true); }
    bool Derive(const Packet& p) {
        uint32_t code = 0;
        if (!radio_.Pair(remote_, remote_nonce_, p.public_key, &code)) return false;
        snprintf(view_.code, sizeof view_.code, "%06u", static_cast<unsigned>(code % 1000000));
        keyed_ = true; remote_seq_ = 0; return true;
    }
    void CheckConnected(int64_t now) {
        if (view_.local_confirmed && view_.remote_confirmed && view_.state != AGENT_FRIEND_CONNECTED) {
            view_.state = AGENT_FRIEND_CONNECTED; last_rx_ = now; next_ = 0;
        }
    }
    void Reset(int64_t now, agent_friend_event_t event) {
        radio_.Forget(); keyed_ = incoming_ = greeting_pending_ = false;
        nonce_ = radio_.RenewNonce();
        view_.state = AGENT_FRIEND_SCANNING; view_.event = event;
        view_.peer_count = 0; view_.local_confirmed = view_.remote_confirmed = false;
        view_.remote_name[0] = view_.code[0] = 0; view_.sent = view_.received = 0;
        remote_nonce_ = remote_seq_ = greeting_rx_ = 0; next_ = now; memset(remote_, 0, 6);
    }
    void Discover(const uint8_t from[6], const Packet& p, int rssi, int64_t now) {
        int index = 0;
        for (; index < view_.peer_count && memcmp(from, view_.peers[index].mac, 6); ++index) {}
        if (index == 6) return;
        if (index == view_.peer_count) ++view_.peer_count;
        memcpy(view_.peers[index].mac, from, 6);
        snprintf(view_.peers[index].name, sizeof view_.peers[index].name, "%s", p.name);
        view_.peers[index].rssi = rssi; peer_nonces_[index] = p.nonce; peer_seen_[index] = now;
    }
    Radio& radio_;
    agent_friend_snapshot_t view_ = {};
    uint8_t mac_[6] = {}, remote_[6] = {}, public_[65] = {};
    uint64_t nonce_ = 0, remote_nonce_ = 0, peer_nonces_[6] = {};
    uint32_t seq_ = 0, remote_seq_ = 0, greeting_rx_ = 0;
    int64_t next_ = 0, deadline_ = 0, last_rx_ = 0, peer_seen_[6] = {}, greeting_at_ = 0;
    bool keyed_ = false, incoming_ = false, greeting_pending_ = false;
    Packet greeting_;
    int retries_ = 0;
};
}  // namespace agentlink::friends
