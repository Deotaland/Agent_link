#pragma once
// Use the ordinary SDK adapters, replacing their fixed-slot flash with one
// contiguous device and a partition list reloaded only at simulated reboot.
#define esp_partition_find_first fixed_find
#define esp_partition_read fixed_read
#define esp_partition_mmap fixed_mmap
#define esp_partition_erase_range fixed_erase
#define esp_partition_write fixed_write
#define esp_image_verify fixed_verify
#define esp_ota_get_partition_description fixed_description
#include "agent_system_stubs.h"
#undef esp_partition_find_first
#undef esp_partition_read
#undef esp_partition_mmap
#undef esp_partition_erase_range
#undef esp_partition_write
#undef esp_image_verify
#undef esp_ota_get_partition_description
#include "agent_partition_store.h"
#include <cassert>

#define EXT_RAM_BSS_ATTR
constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_ANY = 255;
namespace dynamic_fixture {
inline std::vector<uint8_t> flash(32 * 1024 * 1024, 255);
inline std::vector<esp_partition_t> partitions;
inline bool encrypted = false;
inline int mutations = 0, cut = -1;
inline size_t torn = 0;
inline int Read(void*, uint32_t address, void* out, size_t size) {
    if (address > flash.size() || size > flash.size() - address)
        return ESP_FAIL;
    memcpy(out, flash.data() + address, size);
    return ESP_OK;
}
inline int Erase(void*, uint32_t address, size_t size) {
    if (address > flash.size() || size > flash.size() - address || (address | size) & 4095)
        return ESP_FAIL;
    const bool fail = ++mutations == cut;
    memset(flash.data() + address, 255, fail ? std::min(size, torn) : size);
    return fail ? ESP_FAIL : ESP_OK;
}
inline int Write(void*, uint32_t address, const void* data, size_t size) {
    if (address > flash.size() || size > flash.size() - address)
        return ESP_FAIL;
    const auto* input = static_cast<const uint8_t*>(data);
    const bool fail = ++mutations == cut;
    for (size_t i = 0; i < (fail ? std::min(size, torn) : size); ++i)
        flash[address + i] &= input[i];
    return fail ? ESP_FAIL : ESP_OK;
}
inline int Md5(const void* data, size_t size, uint8_t out[16]) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ULONG length = 0, copied = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0)
        return -1;
    assert(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), 4,
                             &copied, 0) >= 0);
    std::vector<uint8_t> object(length);
    assert(BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) >= 0);
    assert(BCryptHashData(hash, (PUCHAR)data, static_cast<ULONG>(size), 0) >= 0);
    assert(BCryptFinishHash(hash, out, 16, 0) >= 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return 0;
#else
    auto* context = EVP_MD_CTX_new();
    unsigned length = 0;
    assert(context && EVP_DigestInit_ex(context, EVP_md5(), nullptr) == 1);
    assert(EVP_DigestUpdate(context, data, size) == 1 &&
           EVP_DigestFinal_ex(context, out, &length) == 1);
    EVP_MD_CTX_free(context);
    return length == 16 ? 0 : -1;
#endif
}
inline alpt_io Io() {
    return {nullptr, Read, Erase, Write, Md5};
}
inline void Reload() {
    partitions.clear();
    for (unsigned offset = 0; offset < 0xC00; offset += 32) {
        alpt_entry entry;
        memcpy(&entry, flash.data() + 0x8000 + offset, 32);
        if (entry.magic != 0x50AA)
            break;
        esp_partition_t part{entry.offset, entry.size, entry.type, entry.subtype, {}};
        memcpy(part.label, entry.label, 16);
        partitions.push_back(part);
    }
    fixture::running = nullptr;
    fixture::boot = nullptr;
    for (const auto& part : partitions)
        if (part.type == 0 && part.subtype == 0)
            fixture::running = &part;
    assert(fixture::running);
}
}  // namespace dynamic_fixture
inline const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* label) {
    for (const auto& part : dynamic_fixture::partitions)
        if (part.type == type && (subtype == 255 || part.subtype == subtype) &&
            (!label || !strcmp(label, part.label)))
            return &part;
    return nullptr;
}
inline int esp_partition_read(const esp_partition_t* p, size_t offset, void* out, size_t size) {
    if (offset > p->size || size > p->size - offset)
        return ESP_ERR_INVALID_SIZE;
    return dynamic_fixture::Read(nullptr, p->address + offset, out, size);
}
inline int esp_ota_get_partition_description(const esp_partition_t* p, esp_app_desc_t* out) {
    const int result = esp_partition_read(p, 32, out, sizeof *out);
    return result == ESP_OK && out->magic_word == ESP_APP_DESC_MAGIC_WORD ? ESP_OK
                                                                          : ESP_ERR_INVALID_VERSION;
}
inline int esp_partition_write(const esp_partition_t* p, size_t offset, const void* data,
                               size_t size) {
    if (offset > p->size || size > p->size - offset)
        return ESP_ERR_INVALID_SIZE;
    return dynamic_fixture::Write(nullptr, p->address + offset, data, size);
}
inline int esp_partition_erase_range(const esp_partition_t* p, size_t offset, size_t size) {
    if (offset > p->size || size > p->size - offset)
        return ESP_ERR_INVALID_SIZE;
    return dynamic_fixture::Erase(nullptr, p->address + offset, size);
}
inline int esp_partition_mmap(const esp_partition_t* p, size_t offset, size_t size, int,
                              const void** out, uint32_t* handle) {
    if (offset > p->size || size > p->size - offset)
        return ESP_ERR_INVALID_SIZE;
    *out = dynamic_fixture::flash.data() + p->address + offset;
    *handle = p->address;
    return ESP_OK;
}
inline int esp_image_verify(int, const esp_partition_pos_t* p, esp_image_metadata_t* image) {
    if (p->offset > dynamic_fixture::flash.size() ||
        p->size > dynamic_fixture::flash.size() - p->offset || p->size < 32)
        return ESP_FAIL;
    const auto* bytes = dynamic_fixture::flash.data() + p->offset;
    if (bytes[0] != 0xE9 || !bytes[1] || bytes[1] > 16 || bytes[12] != 9 || bytes[13])
        return ESP_FAIL;
    uint32_t cursor = 24;
    uint8_t checksum = 0xEF;
    for (unsigned segment = 0; segment < bytes[1]; ++segment) {
        uint32_t length;
        if (cursor > p->size - 8)
            return ESP_FAIL;
        memcpy(&length, bytes + cursor + 4, 4);
        cursor += 8;
        if (length > p->size - cursor)
            return ESP_FAIL;
        for (uint32_t i = 0; i < length; ++i)
            checksum ^= bytes[cursor + i];
        cursor += length;
    }
    uint32_t checksum_offset = (cursor + 16) & ~15u;
    if (!checksum_offset || checksum_offset > p->size || bytes[checksum_offset - 1] != checksum)
        return ESP_FAIL;
    cursor = checksum_offset;
    if (bytes[23]) {
        if (p->size - cursor < 32)
            return ESP_FAIL;
        mbedtls_sha256_context hash;
        mbedtls_sha256_init(&hash);
        uint8_t actual[32];
        const bool valid = mbedtls_sha256_starts(&hash, 0) == 0 &&
                           mbedtls_sha256_update(&hash, bytes, cursor) == 0 &&
                           mbedtls_sha256_finish(&hash, actual) == 0 &&
                           !memcmp(actual, bytes + cursor, 32);
        mbedtls_sha256_free(&hash);
        if (!valid)
            return ESP_FAIL;
        cursor += 32;
    }
    image->image_len = cursor;
    return ESP_OK;
}
inline void* esp_flash_default_chip = nullptr;
inline int esp_flash_read(void*, void* out, uint32_t address, size_t size) {
    return dynamic_fixture::Read(nullptr, address, out, size);
}
inline int esp_flash_write(void*, const void* data, uint32_t address, size_t size) {
    return dynamic_fixture::Write(nullptr, address, data, size);
}
inline int esp_flash_erase_region(void*, uint32_t address, size_t size) {
    return dynamic_fixture::Erase(nullptr, address, size);
}
inline bool esp_flash_encryption_enabled() {
    return dynamic_fixture::encrypted;
}
struct md5_context_t {
    std::vector<uint8_t> data;
};
inline void esp_rom_md5_init(md5_context_t* c) {
    c->data.clear();
}
inline void esp_rom_md5_update(md5_context_t* c, const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    c->data.insert(c->data.end(), p, p + size);
}
inline void esp_rom_md5_final(uint8_t out[16], md5_context_t* c) {
    assert(dynamic_fixture::Md5(c->data.data(), c->data.size(), out) == 0);
}
