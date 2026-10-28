// Production worker lifecycle and TLS upgrade, driven without a device/network.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "realtime_buffer.h"
#include "realtime_ws_upgrade.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_OK 0
#define ESP_ERR_NO_MEM -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_FAIL -3
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
using esp_err_t = int;
using TaskHandle_t = void*;
struct Queue { unsigned reset = 0; };
using QueueHandle_t = Queue*;
struct Grant { uint32_t epoch = 0; bool platform = false; };
struct Tx { unsigned unused; };
constexpr size_t kTxDepth = 8;
constexpr int AGENT_STREAM_KIND_COUNT = 2;
QueueHandle_t s_grants = nullptr, s_tx = nullptr;
bool s_grants_caps = false, s_tx_caps = false;
std::atomic<TaskHandle_t> s_task{nullptr};
std::atomic<bool> s_run{false}, s_ready{false}, s_mute{false}, s_continuous{false};
std::atomic<uint32_t> s_epoch{0};
int s_input = AGENT_STREAM_KIND_COUNT;
std::mutex s_mutex;
unsigned creates = 0, created_stack_bytes = 0, queues = 0;
bool fail_task = false;
int64_t now_us = 0;
void Worker(void*) {}
int xTaskCreate(void (*entry)(void*), const char*, unsigned stack, void*, unsigned, TaskHandle_t* out) {
    assert(entry == Worker); ++creates; created_stack_bytes = stack;
    if (fail_task) return 0;
    *out = reinterpret_cast<void*>(uintptr_t(creates)); return pdPASS;
}
QueueHandle_t xQueueCreateWithCaps(size_t, size_t, unsigned) { return nullptr; }
QueueHandle_t xQueueCreate(size_t, size_t) { ++queues; return new Queue; }
void vQueueDelete(QueueHandle_t queue) { assert(queue); --queues; delete queue; }
void vQueueDeleteWithCaps(QueueHandle_t) { assert(false); }
void xQueueReset(QueueHandle_t queue) { ++queue->reset; }
int xQueueOverwrite(QueueHandle_t queue, const Grant*) { assert(queue); return pdPASS; }
void vTaskDelay(unsigned ticks) {
    now_us += ticks * 1000;
    // Model the worker acknowledging SDK stop after its queue wait.
    if (!s_run.load()) s_task.store(nullptr);
}

class Call {
public:
    // INSERT_CALL_START
};

// INSERT_START_WORKER
// INSERT_QUEUE_GRANT
// INSERT_SDK_START
// INSERT_SDK_STOP
// INSERT_CANCEL

struct WssAddress { char host[128] = "voice.example.test", path[384] = "/realtime"; int port = 443; };
struct TxScratch { char json[1800]; uint8_t read[1024]; };
constexpr int kConnectTimeoutMs = 10000;
constexpr int ESP_TLS_ERR_SSL_WANT_READ = -10, ESP_TLS_ERR_SSL_WANT_WRITE = -11;
#define CONFIG_MBEDTLS_DYNAMIC_BUFFER 1
constexpr int ESP_TLS_DYN_BUF_RX_STATIC = 1;
struct esp_tls_cfg_t {
    bool non_block = false;
    int timeout_ms = 0;
    void (*crt_bundle_attach)() = nullptr;
    int esp_tls_dyn_buf_strategy = 0;
};
void esp_crt_bundle_attach() {}
struct esp_tls_t { size_t received = 0; unsigned writes = 0, connects = 0; std::string request; };
std::string response, last_request;
unsigned live_tls = 0, fragment = 7;
bool cancel_connect = false;
esp_tls_t* esp_tls_init() { ++live_tls; return new esp_tls_t; }
void esp_tls_conn_destroy(esp_tls_t* tls) { last_request = tls->request; --live_tls; delete tls; }
int esp_tls_conn_new_async(const char*, size_t, int, const esp_tls_cfg_t* cfg, esp_tls_t* tls) {
    assert(cfg->non_block && cfg->crt_bundle_attach == esp_crt_bundle_attach);
    assert(cfg->esp_tls_dyn_buf_strategy == ESP_TLS_DYN_BUF_RX_STATIC);
    if (cancel_connect) s_epoch.fetch_add(1);
    return ++tls->connects == 1 ? 0 : 1;
}
int esp_tls_get_conn_sockfd(esp_tls_t*, int* socket) { *socket = 7; return ESP_OK; }
int setsockopt(int, int, int, const void*, size_t) { return 0; }
int esp_tls_conn_write(esp_tls_t* tls, const char* data, size_t bytes) {
    if (++tls->writes % 3 == 1) return ESP_TLS_ERR_SSL_WANT_WRITE;
    const size_t take = std::min(bytes, size_t(13));
    tls->request.append(data, take); return static_cast<int>(take);
}
int esp_tls_conn_read(esp_tls_t* tls, char* data, size_t bytes) {
    if (tls->received == response.size()) return ESP_TLS_ERR_SSL_WANT_READ;
    const size_t take = std::min({bytes, response.size() - tls->received, size_t(fragment)});
    memcpy(data, response.data() + tls->received, take); tls->received += take;
    return static_cast<int>(take);
}
bool TlsWouldBlock(int result) {
    return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE;
}
int64_t esp_timer_get_time() { return now_us; }
bool Current(uint32_t epoch) { return s_run && s_epoch == epoch; }
unsigned finished_calls = 0;
bool transport_running = true;
void Finished() { ++finished_calls; transport_running = false; }
struct Callbacks { void (*finished)() = Finished; } s_cb;
// INSERT_FINISH_GRANT
void esp_fill_random(void* data, size_t bytes) { assert(bytes == 16); memcpy(data, "the sample nonce", 16); }
int mbedtls_base64_encode(uint8_t* out, size_t cap, size_t* written, const uint8_t* input, size_t bytes) {
    const char* value = bytes == 16 ? "dGhlIHNhbXBsZSBub25jZQ==" : "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    if (bytes == 16) assert(memcmp(input, "the sample nonce", 16) == 0);
    assert(cap > strlen(value)); *written = strlen(value); memcpy(out, value, *written + 1); return 0;
}
int mbedtls_sha1(const uint8_t* input, size_t bytes, uint8_t* digest) {
    // RFC 6455 challenge, including the exact production GUID and key encoding.
    const std::string expected = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    assert(std::string(reinterpret_cast<const char*>(input), bytes) == expected);
    memset(digest, 0xa5, 20); return 0;
}
void* Resize(void* data, size_t bytes) { if (!bytes) { free(data); return nullptr; } return realloc(data, bytes); }
// INSERT_NATIVE_WSS

int main() {
    assert(cloud_realtime_start() == ESP_OK);
    assert(creates == 1 && created_stack_bytes == 40960 && s_task && queues == 2);
    for (unsigned i = 0; i < 20; ++i) {
        Grant grant;
        assert(QueueGrant(&grant) == ESP_OK);
        assert(grant.epoch == s_epoch && creates == 1);
        Call call;
        assert(call.Start() && creates == 1 && queues == 2);
        cloud_realtime_cancel();
        assert(s_task && s_run && creates == 1 && queues == 2);
    }
    Grant old_grant;
    assert(QueueGrant(&old_grant) == ESP_OK);
    cloud_realtime_cancel();
    Grant new_grant;
    assert(QueueGrant(&new_grant) == ESP_OK);
    transport_running = true;
    FinishGrant(old_grant.epoch);
    assert(transport_running && finished_calls == 0);
    FinishGrant(new_grant.epoch);
    assert(!transport_running && finished_calls == 1);
    s_cb.finished = nullptr;
    FinishGrant(new_grant.epoch); // Optional callback.
    s_cb.finished = Finished;
    cloud_realtime_stop();
    transport_running = true;
    FinishGrant(new_grant.epoch);
    assert(transport_running && finished_calls == 1);
    assert(!s_task && !s_run && queues == 0);
    fail_task = true;
    assert(cloud_realtime_start() == ESP_ERR_NO_MEM);
    assert(!s_task && !s_run && !s_grants && !s_tx && queues == 0);
    fail_task = false;
    assert(cloud_realtime_start() == ESP_OK && creates == 3);

    const std::string upgrade = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Protocol: tidb-link.realtime.v1\r\n\r\n";
    for (const unsigned part : {1u, 7u, 1024u}) {
        response = upgrade + std::string("\x89\x02hi", 4); fragment = part;
        TxScratch scratch{};
        agentlink::realtime::MessageBuffer storage(2048, 16384, Resize);
        size_t tail = 0; int status = 0; now_us = 0;
        {
            NativeWss connection;
            assert(connection.Connect(WssAddress{}, s_epoch, scratch, storage, &tail, &status));
            assert(status == 101 && tail <= 4);
            if (part == 1024) assert(tail == 4 && memcmp(scratch.read, "\x89\x02hi", 4) == 0);
        }
        assert(live_tls == 0 && last_request.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos);
    }
    for (const std::string& rejected : {std::string("HTTP/1.1 403 Forbidden\r\n\r\n"),
                                        std::string(2200, 'x'), std::string()}) {
        response = rejected; fragment = 1024; now_us = 0;
        TxScratch scratch{};
        agentlink::realtime::MessageBuffer storage(2048, 16384, Resize);
        size_t tail = 0; int status = 0;
        { NativeWss connection; assert(!connection.Connect(WssAddress{}, s_epoch, scratch, storage, &tail, &status)); }
        assert(live_tls == 0 && now_us <= 10000000);
    }
    cancel_connect = true; now_us = 0;
    {
        NativeWss connection; TxScratch scratch{}; size_t tail = 0; int status = 0;
        agentlink::realtime::MessageBuffer storage(2048, 16384, Resize);
        assert(!connection.Connect(WssAddress{}, s_epoch, scratch, storage, &tail, &status));
    }
    assert(live_tls == 0);
    cloud_realtime_stop();
    assert(queues == 0);
    std::puts("PASS: resident C3 worker survives 20 calls/cancellations; startup failure cleanup; verified upgrade, partial I/O, early ping, timeout and cancellation");
}
