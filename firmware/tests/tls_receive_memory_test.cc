// Compile the local IDF's actual dynamic RX functions with a bounded allocator.
// TLS decryption/networking are outside this fixture. This checks reservation,
// buffer reuse, counter preservation and teardown, not device heap capacity.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#define MBEDTLS_PRIVATE(field) field
#define ESP_LOGV(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define __containerof(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))
// C3's configured TLS overhead. The linked target is checked separately.
#define MBEDTLS_SSL_HEADER_LEN 13
#define MBEDTLS_MAX_IV_LENGTH 16
#define MBEDTLS_SSL_MAC_ADD 48
#define MBEDTLS_SSL_PADDING_ADD 256
#define MBEDTLS_SSL_MAX_CID_EXPANSION 0
#define MBEDTLS_SSL_IN_BUFFER_LEN (16384 + 333)
#define MBEDTLS_SSL_OUT_BUFFER_LEN (1024 + 333)
#define ESP_ERR_NO_MEM -1
#define ESP_OK 0
#define MBEDTLS_ERR_SSL_ALLOC_FAILED -0x7f00
#define MBEDTLS_ERR_SSL_TIMEOUT -0x6800
#define MBEDTLS_ERR_SSL_WANT_READ -0x6900
#define MBEDTLS_ERR_SSL_CONN_EOF -0x7280
using esp_err_t = int;
struct mbedtls_ssl_context {
    unsigned char *in_buf = nullptr, *in_ctr = nullptr, *in_hdr = nullptr, *in_len = nullptr;
    unsigned char *in_iv = nullptr, *in_msg = nullptr;
    size_t in_msglen = 0, in_left = 0, in_hslen = 0;
    int in_msgtype = 0;
};
// INSERT_IDF_BUFFER_TYPES
// INSERT_IDF_STATIC_GUARD

size_t largest = 65536, allocations = 0, attempts = 0, last_attempt = 0;
unsigned record_bytes = 16408;
std::unordered_map<void*, size_t> live;
esp_mbedtls_ssl_buf* mbedtls_calloc(size_t count, size_t bytes) {
    ++attempts;
    last_attempt = bytes;
    assert(count == 1);
    if (bytes > largest) return nullptr;
    void* memory = calloc(count, bytes); assert(memory);
    live[memory] = bytes; ++allocations; return static_cast<esp_mbedtls_ssl_buf*>(memory);
}
void mbedtls_free(void* memory) { assert(live.erase(memory) == 1); free(memory); }
size_t mbedtls_ssl_in_hdr_len(mbedtls_ssl_context*) { return 5; }
int mbedtls_ssl_fetch_input(mbedtls_ssl_context* ssl, size_t bytes) {
    assert(bytes == 5);
    ssl->in_hdr[0] = 23; ssl->in_hdr[1] = 3; ssl->in_hdr[2] = 3;
    ssl->in_hdr[3] = uint8_t(record_bytes >> 8); ssl->in_hdr[4] = uint8_t(record_bytes);
    ssl->in_left = 5;
    return 0;
}
// INSERT_IDF_RX_FUNCTIONS

void Idle(mbedtls_ssl_context* ssl) {
    auto* buffer = static_cast<esp_mbedtls_ssl_buf*>(mbedtls_calloc(1, SSL_BUF_HEAD_OFFSET_SIZE + 16));
    esp_mbedtls_init_ssl_buf(buffer, 16);
    ssl->in_msg = reinterpret_cast<unsigned char*>(MBEDTLS_SSL_HEADER_LEN + 16);
    init_rx_buffer(ssl, buffer->buf);
    // The SDK's 16-byte idle representation packs counter + IV together;
    // in_iv is restored to its normal offset when a full record is allocated.
    memset(buffer->buf, 0x12, 8); memset(buffer->buf + 8, 0x34, 8);
    esp_mbedtls_set_buf_state(ssl->in_buf, ESP_MBEDTLS_SSL_BUF_NO_CACHED);
}
int main() {
    mbedtls_ssl_context dynamic;
    Idle(&dynamic);
    largest = 8704;
    assert(esp_mbedtls_add_rx_buffer(&dynamic) == MBEDTLS_ERR_SSL_ALLOC_FAILED);
    assert(last_attempt == 16749);
    assert(live.empty());

    largest = 65536;
    mbedtls_ssl_context retained;
    Idle(&retained);
    assert(esp_mbedtls_dynamic_set_rx_buf_static(&retained) == ESP_OK);
    assert(live.size() == 1 && live.begin()->second == 17058);
    unsigned char* original = retained.in_buf;
    const size_t allocated = allocations, tried = attempts;
    largest = 8704;
    for (unsigned packet = 0; packet < 10000; ++packet) {
        assert(esp_mbedtls_add_rx_buffer(&retained) == 0);
        // A complete 16 KB plaintext fits without replacing the record buffer.
        memset(retained.in_msg, 0x5a, 16384);
        retained.in_msglen = 0;
        assert(esp_mbedtls_free_rx_buffer(&retained) == 0);
        assert(retained.in_buf == original && allocations == allocated && attempts == tried);
        assert(retained.in_ctr[0] == 0x12 && retained.in_iv[0] == 0x34);
    }
    esp_mbedtls_reset_free_rx_buffer(&retained);
    assert(live.empty());

    largest = 65536;
    mbedtls_ssl_context rejected;
    Idle(&rejected); largest = 8704;
    assert(esp_mbedtls_dynamic_set_rx_buf_static(&rejected) == ESP_ERR_NO_MEM);
    assert(!rejected.in_buf && live.empty());
    puts("PASS: actual IDF RX allocator reproduces 16749-byte failure; static RX reuses one 17058-byte block over 10000 full-record cycles with an 8704-byte allocation limit; teardown/admission failure clean");
}
