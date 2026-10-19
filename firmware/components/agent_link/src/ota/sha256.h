#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_idf_version.h"

#if ESP_IDF_VERSION_MAJOR >= 6
#include "psa/crypto.h"
#else
#include "mbedtls/sha256.h"
#endif

namespace agentlink::ota {

class Sha256 {
public:
    Sha256() {
#if ESP_IDF_VERSION_MAJOR < 6
        mbedtls_sha256_init(&context_);
#endif
    }
    ~Sha256() { Reset(); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool Start() {
        Reset();
#if ESP_IDF_VERSION_MAJOR >= 6
        return psa_crypto_init() == PSA_SUCCESS &&
               psa_hash_setup(&context_, PSA_ALG_SHA_256) == PSA_SUCCESS;
#else
        return mbedtls_sha256_starts(&context_, 0) == 0;
#endif
    }
    bool Update(const uint8_t* data, size_t size) {
#if ESP_IDF_VERSION_MAJOR >= 6
        return psa_hash_update(&context_, data, size) == PSA_SUCCESS;
#else
        return mbedtls_sha256_update(&context_, data, size) == 0;
#endif
    }
    bool Finish(uint8_t digest[32]) {
#if ESP_IDF_VERSION_MAJOR >= 6
        size_t size = 0;
        return psa_hash_finish(&context_, digest, 32, &size) == PSA_SUCCESS && size == 32;
#else
        return mbedtls_sha256_finish(&context_, digest) == 0;
#endif
    }
    void Reset() {
#if ESP_IDF_VERSION_MAJOR >= 6
        (void)psa_hash_abort(&context_);
#else
        mbedtls_sha256_free(&context_);
        mbedtls_sha256_init(&context_);
#endif
    }

private:
#if ESP_IDF_VERSION_MAJOR >= 6
    psa_hash_operation_t context_ = PSA_HASH_OPERATION_INIT;
#else
    mbedtls_sha256_context context_;
#endif
};

}  // namespace agentlink::ota
