#pragma once
// Host-only adapter: actual SDK SHA-256 calls use the operating system crypto
// implementation for downloaded binary tests. No cryptography is reimplemented.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
struct mbedtls_sha256_context {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<uint8_t> object;
};
inline void mbedtls_sha256_init(mbedtls_sha256_context* c) {
    c->algorithm = nullptr; c->hash = nullptr; c->object.clear();
}
inline void mbedtls_sha256_free(mbedtls_sha256_context* c) {
    if (c->hash) BCryptDestroyHash(c->hash);
    if (c->algorithm) BCryptCloseAlgorithmProvider(c->algorithm, 0);
    mbedtls_sha256_init(c);
}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c, int) {
    if (BCryptOpenAlgorithmProvider(&c->algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return -1;
    ULONG size = 0, copied = 0;
    if (BCryptGetProperty(c->algorithm, BCRYPT_OBJECT_LENGTH,
                         reinterpret_cast<PUCHAR>(&size), sizeof size, &copied, 0) < 0)
        return -1;
    c->object.resize(size);
    return BCryptCreateHash(c->algorithm, &c->hash, c->object.data(), size, nullptr, 0, 0) < 0 ? -1 : 0;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* c, const uint8_t* data, size_t size) {
    return BCryptHashData(c->hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0 ? -1 : 0;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c, uint8_t out[32]) {
    return BCryptFinishHash(c->hash, out, 32, 0) < 0 ? -1 : 0;
}
#else
#include <openssl/evp.h>
struct mbedtls_sha256_context { EVP_MD_CTX* handle = nullptr; };
inline void mbedtls_sha256_init(mbedtls_sha256_context* c) { c->handle = nullptr; }
inline void mbedtls_sha256_free(mbedtls_sha256_context* c) {
    EVP_MD_CTX_free(c->handle); c->handle = nullptr;
}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c, int) {
    c->handle = EVP_MD_CTX_new();
    return c->handle && EVP_DigestInit_ex(c->handle, EVP_sha256(), nullptr) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* c, const uint8_t* data, size_t size) {
    return EVP_DigestUpdate(c->handle, data, size) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c, uint8_t out[32]) {
    unsigned size = 0;
    return EVP_DigestFinal_ex(c->handle, out, &size) == 1 && size == 32 ? 0 : -1;
}
#endif
