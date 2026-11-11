#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Shared by the app, bootloader and fault-injection tests. No heap or SDK calls. */
#ifdef _MSC_VER
#pragma pack(push, 1)
#define ALPT_PACKED
#else
#define ALPT_PACKED __attribute__((packed))
#endif
typedef struct ALPT_PACKED {
    uint16_t magic;
    uint8_t type, subtype;
    uint32_t offset, size;
    char label[16];
    uint32_t flags;
} alpt_entry;
typedef struct ALPT_PACKED {
    char magic[8];
    uint32_t sequence, bytes, table_crc, header_crc, reserved, committed;
} alpt_record;
typedef struct ALPT_PACKED {
    char magic[12];
    uint32_t version, policy;
} alpt_boot_marker;
#ifdef _MSC_VER
#pragma pack(pop)
#endif
typedef struct {
    uint32_t table, guard, pool_start, pool_end, slots;
    size_t fixed_count;
    const alpt_entry* fixed;
} alpt_policy;
typedef struct {
    void* context;
    int (*read)(void*, uint32_t, void*, size_t);
    int (*erase)(void*, uint32_t, size_t);
    int (*write)(void*, uint32_t, const void*, size_t);
    int (*md5)(const void*, size_t, uint8_t[16]);
} alpt_io;
enum {
    ALPT_TABLE_BYTES = 4096,
    ALPT_BANK_BYTES = 8192,
    ALPT_MAX_ENTRIES = 95,
    ALPT_OK = 0,
    ALPT_NONE = 1,
    ALPT_INVALID = -1,
    ALPT_IO = -2
};
#define ALPT_COMMITTED 0xA17C03EDu

static inline uint32_t alpt_crc(const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;
    for (i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return crc ^ 0xFFFFFFFFu;
}
static inline int alpt_ota(const alpt_entry* p) {
    return p->type == 0 && p->subtype >= 0x10 && p->subtype < 0x20;
}
static inline void alpt_label(char out[16], unsigned index) {
    memset(out, 0, 16);
    memcpy(out, "ota_", 4);
    if (index >= 10) {
        out[4] = '1';
        out[5] = (char)('0' + index - 10);
    } else
        out[4] = (char)('0' + index);
}
static inline int alpt_validate(const alpt_policy* cfg, const alpt_io* io, const uint8_t* table,
                                int seed_allowed) {
    uint32_t mask = 0, fixed_mask = 0;
    size_t i, j, count = 0;
    int digest_seen = 0;
    if (cfg->slots != 16 || cfg->fixed_count > 31 || cfg->pool_start >= cfg->pool_end)
        return 0;
    for (i = 0; i < ALPT_MAX_ENTRIES; ++i) {
        alpt_entry part;
        memcpy(&part, table + i * 32, 32);
        if (part.magic == 0xEBEB) {
            uint8_t digest[16];
            if (digest_seen || io->md5(table, i * 32, digest) ||
                memcmp(digest, table + i * 32 + 16, 16))
                return 0;
            digest_seen = 1;
            count = i;
            continue;
        }
        if (part.magic == 0xFFFF) {
            if (!digest_seen)
                return 0;
            if (fixed_mask != ((1u << cfg->fixed_count) - 1))
                return 0;
            if (mask == 0xFFFFu)
                return 1;
            if (!seed_allowed || mask != 1u)
                return 0;
            for (j = 0; j < count; ++j) {
                alpt_entry p;
                memcpy(&p, table + j * 32, 32);
                if (alpt_ota(&p))
                    return p.offset == cfg->pool_start && p.size == cfg->pool_end - cfg->pool_start;
            }
            return 0;
        }
        if (part.magic != 0x50AA || digest_seen || !memchr(part.label, 0, 16) || part.flags)
            return 0;
        if (alpt_ota(&part)) {
            unsigned slot = part.subtype - 0x10;
            char label[16];
            alpt_label(label, slot);
            if (mask & (1u << slot) || memcmp(label, part.label, 16))
                return 0;
            mask |= 1u << slot;
            if (!part.size) {
                if (part.offset != cfg->pool_end)
                    return 0;
                continue;
            }
            if (part.offset < cfg->pool_start || part.offset >= cfg->pool_end ||
                part.size > cfg->pool_end - part.offset || (part.offset & 0xFFFF) ||
                (part.size & 0xFFFF))
                return 0;
            for (j = 0; j < i; ++j) {
                alpt_entry previous;
                memcpy(&previous, table + j * 32, 32);
                if (alpt_ota(&previous) && previous.size &&
                    part.offset < previous.offset + previous.size &&
                    previous.offset < part.offset + part.size)
                    return 0;
            }
        } else {
            for (j = 0; j < cfg->fixed_count; ++j) {
                if (!memcmp(&part, &cfg->fixed[j], 32))
                    break;
            }
            if (j == cfg->fixed_count || (fixed_mask & (1u << j)))
                return 0;
            fixed_mask |= 1u << j;
        }
    }
    return 0;
}
static inline int alpt_record_header(const alpt_record* header) {
    return !memcmp(header->magic, "ALPTJ001", 8) && header->bytes == ALPT_TABLE_BYTES &&
           header->sequence && header->reserved == 0 && header->committed == ALPT_COMMITTED &&
           alpt_crc(header, 20) == header->header_crc;
}
/* scratch is caller-owned (4 KiB); the bootloader uses static BSS, not its stack. */
static inline int alpt_latest(const alpt_policy* cfg, const alpt_io* io, uint8_t* scratch,
                              int* bank, alpt_record* newest) {
    int i;
    *bank = -1;
    memset(newest, 0, sizeof(*newest));
    for (i = 0; i < 2; ++i) {
        alpt_record h;
        uint32_t address = cfg->guard + (uint32_t)i * ALPT_BANK_BYTES;
        if (io->read(io->context, address, &h, sizeof h))
            return ALPT_IO;
        if (!alpt_record_header(&h))
            continue;
        if (io->read(io->context, address + 32, scratch, ALPT_TABLE_BYTES))
            return ALPT_IO;
        if (alpt_crc(scratch, ALPT_TABLE_BYTES) != h.table_crc ||
            !alpt_validate(cfg, io, scratch, 0))
            continue;
        if (*bank < 0 || (int32_t)(h.sequence - newest->sequence) > 0) {
            *bank = i;
            *newest = h;
        }
    }
    return *bank < 0 ? ALPT_NONE : ALPT_OK;
}
static inline int alpt_restore(const alpt_policy* cfg, const alpt_io* io, uint8_t* scratch) {
    int bank;
    alpt_record newest;
    int status = alpt_latest(cfg, io, scratch, &bank, &newest);
    if (status != ALPT_OK)
        return status;
    if (io->read(io->context, cfg->table, scratch, ALPT_TABLE_BYTES))
        return ALPT_IO;
    if (alpt_crc(scratch, ALPT_TABLE_BYTES) == newest.table_crc &&
        alpt_validate(cfg, io, scratch, 0))
        return ALPT_OK;
    if (io->read(io->context, cfg->guard + (uint32_t)bank * ALPT_BANK_BYTES + 32, scratch,
                 ALPT_TABLE_BYTES))
        return ALPT_IO;
    if (alpt_crc(scratch, ALPT_TABLE_BYTES) != newest.table_crc ||
        !alpt_validate(cfg, io, scratch, 0))
        return ALPT_INVALID;
    if (io->erase(io->context, cfg->table, ALPT_TABLE_BYTES) ||
        io->write(io->context, cfg->table, scratch, ALPT_TABLE_BYTES))
        return ALPT_IO;
    if (io->read(io->context, cfg->table, scratch, ALPT_TABLE_BYTES))
        return ALPT_IO;
    return alpt_crc(scratch, ALPT_TABLE_BYTES) == newest.table_crc ? ALPT_OK : ALPT_IO;
}
static inline int alpt_commit(const alpt_policy* cfg, const alpt_io* io, const uint8_t* table,
                              uint8_t* scratch) {
    int bank;
    alpt_record previous, h;
    if (!alpt_validate(cfg, io, table, 0))
        return ALPT_INVALID;
    int status = alpt_latest(cfg, io, scratch, &bank, &previous);
    if (status < 0)
        return status;
    uint32_t address = cfg->guard + (bank == 0 ? ALPT_BANK_BYTES : 0);
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "ALPTJ001", 8);
    h.sequence = previous.sequence + 1;
    if (!h.sequence)
        h.sequence = 1;
    h.bytes = ALPT_TABLE_BYTES;
    h.table_crc = alpt_crc(table, ALPT_TABLE_BYTES);
    h.header_crc = alpt_crc(&h, 20);
    h.committed = ALPT_COMMITTED;
    if (io->erase(io->context, address, ALPT_BANK_BYTES) ||
        io->write(io->context, address + 32, table, ALPT_TABLE_BYTES) ||
        io->write(io->context, address, &h, 28) ||
        io->write(io->context, address + 28, &h.committed, 4))
        return ALPT_IO;
    alpt_record check;
    if (io->read(io->context, address, &check, 32) || !alpt_record_header(&check) ||
        io->read(io->context, address + 32, scratch, ALPT_TABLE_BYTES) ||
        memcmp(table, scratch, ALPT_TABLE_BYTES))
        return ALPT_IO;
    /* The journal is authoritative from this point. Restore can finish a torn primary write. */
    if (io->erase(io->context, cfg->table, ALPT_TABLE_BYTES) ||
        io->write(io->context, cfg->table, table, ALPT_TABLE_BYTES))
        return ALPT_IO;
    if (io->read(io->context, cfg->table, scratch, ALPT_TABLE_BYTES) ||
        memcmp(table, scratch, ALPT_TABLE_BYTES))
        return ALPT_IO;
    return ALPT_OK;
}
static inline int alpt_build(const alpt_policy* cfg, const alpt_io* io, const alpt_entry slots[16],
                             uint8_t* table) {
    alpt_entry entries[48];
    size_t i, j, n = cfg->fixed_count + 16;
    if (n > 48)
        return ALPT_INVALID;
    memcpy(entries, cfg->fixed, cfg->fixed_count * 32);
    memcpy(entries + cfg->fixed_count, slots, 16 * 32);
    for (i = 1; i < n; ++i) {
        alpt_entry value = entries[i];
        j = i;
        while (j && (entries[j - 1].offset > value.offset ||
                     (entries[j - 1].offset == value.offset && value.size == 0 &&
                      entries[j - 1].size != 0))) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = value;
    }
    memset(table, 0xFF, ALPT_TABLE_BYTES);
    memcpy(table, entries, n * 32);
    table[n * 32] = 0xEB;
    table[n * 32 + 1] = 0xEB;
    if (io->md5(table, n * 32, table + n * 32 + 16))
        return ALPT_IO;
    return alpt_validate(cfg, io, table, 0) ? ALPT_OK : ALPT_INVALID;
}
static inline void alpt_empty(const alpt_policy* cfg, unsigned i, alpt_entry* entry) {
    memset(entry, 0, 32);
    entry->magic = 0x50AA;
    entry->subtype = (uint8_t)(0x10 + i);
    entry->offset = cfg->pool_end;
    alpt_label(entry->label, i);
}
static inline int alpt_slots(const uint8_t* table, alpt_entry slots[16]) {
    size_t i;
    uint32_t mask = 0;
    memset(slots, 0, 16 * 32);
    for (i = 0; i < ALPT_MAX_ENTRIES; ++i) {
        alpt_entry p;
        memcpy(&p, table + i * 32, 32);
        if (p.magic != 0x50AA)
            break;
        if (alpt_ota(&p)) {
            slots[p.subtype - 0x10] = p;
            mask |= 1u << (p.subtype - 0x10);
        }
    }
    return mask == 0xFFFFu ? ALPT_OK : ALPT_INVALID;
}
/* free_mask comes from verified installed images and persistent write markers. */
static inline int alpt_allocate(const alpt_policy* cfg, const alpt_io* io, const uint8_t* current,
                                uint32_t bytes, uint32_t reserve, uint32_t free_mask,
                                unsigned limit, uint8_t* next, unsigned* index,
                                alpt_entry* result) {
    alpt_entry slots[16], used[16];
    unsigned i, j, n = 0, chosen = 16;
    uint32_t need, best = 0xFFFFFFFFu, start = 0, cursor = cfg->pool_start;
    if (!alpt_validate(cfg, io, current, 0) || alpt_slots(current, slots))
        return ALPT_INVALID;
    if (!bytes || bytes > cfg->pool_end - cfg->pool_start ||
        reserve > cfg->pool_end - cfg->pool_start - bytes)
        return ALPT_NONE;
    need = (bytes + reserve + 0xFFFFu) & ~0xFFFFu;
    for (i = 0; i < 16; ++i) {
        if (free_mask & (1u << i)) {
            alpt_empty(cfg, i, &slots[i]);
            if (i < limit && chosen == 16)
                chosen = i;
        } else if (slots[i].size)
            used[n++] = slots[i];
    }
    if (chosen == 16)
        return ALPT_NONE;
    for (i = 1; i < n; ++i) {
        alpt_entry p = used[i];
        j = i;
        while (j && used[j - 1].offset > p.offset) {
            used[j] = used[j - 1];
            --j;
        }
        used[j] = p;
    }
    for (i = 0; i <= n; ++i) {
        uint32_t end = i < n ? used[i].offset : cfg->pool_end;
        uint32_t gap = end - cursor;
        if (gap >= need && gap < best) {
            best = gap;
            start = cursor;
        }
        if (i < n)
            cursor = used[i].offset + used[i].size;
    }
    if (best == 0xFFFFFFFFu)
        return ALPT_NONE;
    slots[chosen].offset = start;
    slots[chosen].size = need;
    if (alpt_build(cfg, io, slots, next))
        return ALPT_INVALID;
    *index = chosen;
    *result = slots[chosen];
    return ALPT_OK;
}
