#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "agent_partition_store.h"
#include "dynamic_layout_config.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/evp.h>
#endif

namespace {
int Md5(const void* data, size_t size, uint8_t out[16]) {
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
struct Flash {
    std::vector<uint8_t> primary = std::vector<uint8_t>(4096, 255),
                         journal = std::vector<uint8_t>(16384, 255);
    int writes = 0, fail = -1;
    size_t torn = 0;
    uint8_t* Region(uint32_t address, size_t size) {
        if (address >= alpt_config.table && address + size <= alpt_config.table + 4096)
            return primary.data() + address - alpt_config.table;
        assert(address >= alpt_config.guard && address + size <= alpt_config.guard + 16384);
        return journal.data() + address - alpt_config.guard;
    }
    static int Read(void* context, uint32_t address, void* out, size_t size) {
        auto& self = *static_cast<Flash*>(context);
        memcpy(out, self.Region(address, size), size);
        return 0;
    }
    static int Erase(void* context, uint32_t address, size_t size) {
        auto& self = *static_cast<Flash*>(context);
        auto* out = self.Region(address, size);
        const bool cut = ++self.writes == self.fail;
        std::fill(out, out + (cut ? std::min(size, self.torn) : size), 255);
        return cut ? -1 : 0;
    }
    static int Write(void* context, uint32_t address, const void* input, size_t size) {
        auto& self = *static_cast<Flash*>(context);
        auto* out = self.Region(address, size);
        const auto* bytes = static_cast<const uint8_t*>(input);
        const bool cut = ++self.writes == self.fail;
        for (size_t i = 0; i < (cut ? std::min(size, self.torn) : size); ++i)
            out[i] &= bytes[i];
        return cut ? -1 : 0;
    }
    alpt_io Io() {
        return {this, Read, Erase, Write, Md5};
    }
};
}  // namespace
int main() {
    static_assert(sizeof(alpt_entry) == 32 && sizeof(alpt_record) == 32);
    Flash base;
    auto io = base.Io();
    uint8_t empty[4096], next[4096], scratch[4096];
    alpt_entry slots[16];
    for (unsigned i = 0; i < 16; ++i)
        alpt_empty(&alpt_config, i, &slots[i]);
    assert(alpt_build(&alpt_config, &io, slots, empty) == ALPT_OK);
    memcpy(base.primary.data(), empty, 4096);
    assert(alpt_commit(&alpt_config, &io, empty, scratch) == ALPT_OK);
    unsigned index;
    alpt_entry allocated;
    assert(alpt_allocate(&alpt_config, &io, empty, 0x100000, 0xE000, 0xFFFF, 16, next, &index,
                         &allocated) == 0);
    assert(index == 0 && allocated.offset == alpt_config.pool_start && allocated.size == 0x110000);
    // Recover after every mutating phase, including partial commit words and torn sectors.
    unsigned scenarios = 0;
    for (int operation = 1; operation <= 6; ++operation) {
        for (size_t bytes : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(7), size_t(15),
                             size_t(31), size_t(511), size_t(4095), size_t(4096), size_t(8192)}) {
            Flash failed = base;
            failed.writes = 0;
            failed.fail = operation;
            failed.torn = bytes;
            auto failed_io = failed.Io();
            (void)alpt_commit(&alpt_config, &failed_io, next, scratch);
            failed.fail = -1;
            assert(alpt_restore(&alpt_config, &failed_io, scratch) == ALPT_OK);
            assert(failed.primary == base.primary || !memcmp(failed.primary.data(), next, 4096));
            assert(alpt_validate(&alpt_config, &failed_io, failed.primary.data(), 0));
            ++scenarios;
        }
    }
    // The very first publication has no older journal. Before its commit word,
    // the bootstrap table remains authoritative; afterward recovery uses the new one.
    Flash bootstrap;
    std::vector<alpt_entry> seed(alpt_fixed, alpt_fixed + alpt_config.fixed_count);
    alpt_entry pool;
    alpt_empty(&alpt_config, 0, &pool);
    pool.offset = alpt_config.pool_start;
    pool.size = alpt_config.pool_end - alpt_config.pool_start;
    if (alpt_config.pool_label) snprintf(pool.label, sizeof pool.label, "%s", alpt_config.pool_label);
    seed.push_back(pool);
    memcpy(bootstrap.primary.data(), seed.data(), seed.size() * 32);
    bootstrap.primary[seed.size() * 32] = 0xEB;
    bootstrap.primary[seed.size() * 32 + 1] = 0xEB;
    assert(Md5(bootstrap.primary.data(), seed.size() * 32,
               bootstrap.primary.data() + seed.size() * 32 + 16) == 0);
    auto seed_io = bootstrap.Io();
    assert(alpt_validate(&alpt_config, &seed_io, bootstrap.primary.data(), 1));
    for (int operation = 1; operation <= 6; ++operation)
        for (size_t bytes :
             {size_t(0), size_t(1), size_t(3), size_t(31), size_t(4095), size_t(8192)}) {
            Flash failed = bootstrap;
            failed.fail = operation;
            failed.torn = bytes;
            auto failed_io = failed.Io();
            (void)alpt_commit(&alpt_config, &failed_io, empty, scratch);
            failed.fail = -1;
            assert(alpt_restore(&alpt_config, &failed_io, scratch) >= 0);
            assert(failed.primary == bootstrap.primary ||
                   !memcmp(failed.primary.data(), empty, 4096));
            assert(alpt_validate(&alpt_config, &failed_io, failed.primary.data(), 1));
            ++scenarios;
        }
    assert(alpt_commit(&alpt_config, &io, next, scratch) == ALPT_OK);
    auto current = base.primary;
    uint32_t free_mask = 0xFFFE;
    for (int app = 1; app < 5; ++app) {
        assert(alpt_allocate(&alpt_config, &io, current.data(), 0x100000, 0xE000, free_mask, 16,
                             next, &index, &allocated) == 0);
        assert(index == static_cast<unsigned>(app));
        current.assign(next, next + 4096);
        free_mask &= ~(1u << index);
    }
    assert(alpt_slots(current.data(), slots) == 0);
    unsigned occupied = 0;
    for (const auto& p : slots)
        occupied += p.size != 0 && !(alpt_config.reserved_mask & (1u << (p.subtype - 0x10)));
    assert(occupied == 5);
    // Legacy native images must not be placed above ota_1.
    assert(alpt_allocate(&alpt_config, &io, current.data(), 1024, 0xE000, free_mask, 2, next,
                         &index, &allocated) == ALPT_NONE);
    const auto released = slots[1];
    alpt_empty(&alpt_config, 1, &slots[1]);
    assert(alpt_build(&alpt_config, &io, slots, next) == 0);
    current.assign(next, next + 4096);
    assert(alpt_allocate(&alpt_config, &io, current.data(), 0x80000, 0xE000, free_mask | 2, 16,
                         next, &index, &allocated) == 0);
    assert(index == 1 && allocated.offset == released.offset);  // smallest sufficient hole
    assert(alpt_allocate(&alpt_config, &io, current.data(), 0xA00000, 0xE000, free_mask, 16, next,
                         &index, &allocated) == ALPT_NONE);
    // A forged valid digest cannot authorize changes to persistent data coordinates.
    auto invalid = base.primary;
    for (size_t i = 0; i < 95; ++i) {
        auto* p = reinterpret_cast<alpt_entry*>(invalid.data() + i * 32);
        if (p->magic != 0x50AA)
            break;
        if (!strcmp(p->label, "agent_nvs")) {
            p->offset += 0x1000;
            break;
        }
    }
    for (size_t i = 0; i < 95; ++i)
        if (invalid[i * 32] == 0xEB && invalid[i * 32 + 1] == 0xEB) {
            assert(Md5(invalid.data(), i * 32, invalid.data() + i * 32 + 16) == 0);
            break;
        }
    assert(alpt_commit(&alpt_config, &io, invalid.data(), scratch) == ALPT_INVALID);
    // Aggregate free space does not make fragmented gaps executable as one image.
    for (unsigned i = 0; i < 16; ++i)
        alpt_empty(&alpt_config, i, &slots[i]);
    for (unsigned i = 0; i < 3; ++i) {
        slots[i].offset = alpt_config.pool_start + i * 0x300000;
        slots[i].size = 0x100000;
    }
    assert(alpt_build(&alpt_config, &io, slots, next) == 0);
    current.assign(next, next + 4096);
    assert(alpt_allocate(&alpt_config, &io, current.data(), 0x300000, 0xE000, 0xFFF8, 16, next,
                         &index, &allocated) == ALPT_NONE);
    current.assign(empty, empty + 4096);
    free_mask = 0xFFFF;
    for (unsigned i = 0; i < 16; ++i) {
        if (alpt_config.reserved_mask & (1u << i))
            continue;
        assert(alpt_allocate(&alpt_config, &io, current.data(), 512, 0xE000, free_mask, 16, next,
                             &index, &allocated) == 0);
        assert(index == i);
        free_mask &= ~(1u << index);
        current.assign(next, next + 4096);
    }
    assert(alpt_allocate(&alpt_config, &io, current.data(), 512, 0xE000, 0, 16, next, &index,
                         &allocated) == ALPT_NONE);
    if (alpt_config.reserved_mask) {
        assert(alpt_slots(current.data(), slots) == ALPT_OK);
        const auto a = slots[14], b = slots[15];
        assert(a.size == 0x300000 && b.size == 0x300000);
        assert(alpt_allocate(&alpt_config, &io, current.data(), 512, 0xE000,
                             alpt_config.reserved_mask, 16, next, &index, &allocated) == ALPT_NONE);
        assert(a.offset < alpt_config.pool_start && b.offset >= 0x1000000);
    }
    printf(
        "Dynamic layout: %u torn-write recovery scenarios; five apps, best-fit reuse, "
        "legacy subtype bounds, capacity and persistent-region protection passed.\n",
        scenarios);
}
