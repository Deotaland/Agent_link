#include "friend_protocol.h"
#include <cassert>
#include <deque>
#include <cstdio>
using namespace agentlink::friends;
struct Wire { uint8_t from[6], to[6]; Packet packet; bool encrypted; };
std::deque<Wire> wire;
int drop_answer = 0, drop_confirm = 0, drop_ack = 0;
int64_t clock_ms = 0;
struct FakeRadio : Radio {
    uint8_t mac[6];
    uint64_t nonce = 0;
    uint8_t tag = 0;
    explicit FakeRadio(uint8_t id) : mac{2,0,0,0,0,id}, nonce(id * 1000) {}
    uint64_t RenewNonce() override { return ++nonce; }
    bool Pair(const uint8_t*, uint64_t peer_nonce, const uint8_t*, uint32_t* code) override {
        *code = static_cast<uint32_t>(nonce + peer_nonce);
        tag = static_cast<uint8_t>((*code % 250) + 1); return true;
    }
    bool Send(const uint8_t* to, const Packet& p, bool encrypted) override {
        Wire frame{}; memcpy(frame.from, mac, 6); memcpy(frame.to, to, 6);
        frame.packet = p; frame.encrypted = encrypted;
        if (encrypted) frame.packet.tag[0] = tag;
        wire.push_back(frame); return true;
    }
    bool Verify(const Packet& p) override { return tag && p.tag[0] == tag; }
    void Forget() override { tag = 0; }
};
FakeRadio ra(1), rb(2);
Engine a(ra), b(rb);
void Pump() {
    int bounded = 0;
    while (!wire.empty()) {
        assert(++bounded < 200);
        Wire f = wire.front(); wire.pop_front();
        if (f.packet.type == Type::Answer && drop_answer) { --drop_answer; continue; }
        if (f.packet.type == Type::Confirm && drop_confirm) { --drop_confirm; continue; }
        if (f.packet.type == Type::Ack && drop_ack) { --drop_ack; continue; }
        if (!memcmp(f.to, kBroadcast, 6) || !memcmp(f.to, ra.mac, 6)) a.Receive(f.from, f.packet, -45, f.encrypted, clock_ms);
        if (!memcmp(f.to, kBroadcast, 6) || !memcmp(f.to, rb.mac, 6)) b.Receive(f.from, f.packet, -46, f.encrypted, clock_ms);
    }
}
void Advance(int ms) {
    for (int end = static_cast<int>(clock_ms) + ms; clock_ms < end;) {
        clock_ms += 100; a.Tick(clock_ms); b.Tick(clock_ms); Pump();
    }
}
void Pair() {
    Advance(1500);
    assert(a.View().peer_count == 1 && b.View().peer_count == 1);
    a.Select(rb.mac, clock_ms); Pump(); Advance(1500);
    assert(a.View().state == AGENT_FRIEND_CONFIRM && b.View().state == AGENT_FRIEND_CONFIRM);
    assert(!strcmp(a.View().code, b.View().code));
}
int main() {
    uint8_t key[65] = {4}; a.Begin(ra.mac, key, 0); b.Begin(rb.mac, key, 0);
    Packet bad; assert(!Valid(bad));
    bad.nonce = 1; bad.sequence = 1; strcpy(bad.name, "Link-1"); assert(Valid(bad));
    bad.version = 2; assert(!Valid(bad)); bad.version = 1;
    bad.type = static_cast<Type>(99); assert(!Valid(bad));
    drop_answer = 1; Pair();
    // Broadcast or unauthenticated unicast cannot manufacture remote consent.
    Packet fake; fake.type = Type::Confirm; fake.nonce = rb.nonce; fake.target_nonce = ra.nonce;
    fake.confirmed = 1; fake.sequence = 100; strcpy(fake.name, "Link-2"); memcpy(fake.target, ra.mac, 6);
    a.Receive(rb.mac, fake, -40, false, clock_ms);
    a.Receive(rb.mac, fake, -40, true, clock_ms);
    assert(!a.View().remote_confirmed);
    a.Confirm(true, clock_ms); Pump(); Advance(500);
    assert(a.View().state == AGENT_FRIEND_CONFIRM && b.View().state == AGENT_FRIEND_CONFIRM);
    drop_confirm = 1; b.Confirm(true, clock_ms); Pump(); Advance(1500);
    assert(a.View().state == AGENT_FRIEND_CONNECTED && b.View().state == AGENT_FRIEND_CONNECTED);
    drop_ack = 1; a.Greet(clock_ms); Pump(); Advance(1800);
    assert(a.View().sent == 1 && b.View().received == 1); // retry does not duplicate hello
    b.Greet(clock_ms); Pump(); assert(b.View().sent == 1 && a.View().received == 1);
    // Capture an authenticated old-session frame, then reconnect with fresh nonces.
    a.Greet(clock_ms); Wire old = wire.back(); Pump();
    a.Disconnect(clock_ms); Pump();
    assert(a.View().state == AGENT_FRIEND_SCANNING && b.View().state == AGENT_FRIEND_SCANNING);
    Pair(); a.Confirm(true, clock_ms); b.Confirm(true, clock_ms); Pump(); Advance(800);
    assert(b.View().received == 0);
    b.Receive(old.from, old.packet, -30, true, clock_ms); assert(b.View().received == 0);
    // Loss of all peer heartbeats returns to discovery after the bounded timeout.
    clock_ms += 6000; a.Tick(clock_ms); wire.clear();
    assert(a.View().state == AGENT_FRIEND_SCANNING && a.View().event == AGENT_FRIEND_EVENT_LOST);
    b.Disconnect(clock_ms); wire.clear();
    Pair(); b.Confirm(false, clock_ms); Pump();
    assert(a.View().state == AGENT_FRIEND_SCANNING && b.View().state == AGENT_FRIEND_SCANNING);
    Pair(); Advance(31000);
    assert(a.View().state == AGENT_FRIEND_SCANNING && b.View().state == AGENT_FRIEND_SCANNING);
    // Silent devices disappear rather than remaining selectable forever.
    clock_ms += 6000; a.Tick(clock_ms); wire.clear(); assert(a.View().peer_count == 0);
    puts("ESP-NOW production protocol: discovery, mutual consent, packet loss/retries, authenticated-message gate, greeting deduplication, fresh-session replay rejection, reject/timeout and peer expiry passed.");
}
