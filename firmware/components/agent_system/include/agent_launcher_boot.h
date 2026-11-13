#pragma once
#include "agent_partition_store.h"

/* Fixed two-sector state, shared by the immutable entry, bootloader and Launcher.
 * A committed previous sector survives every interrupted erase/write of its peer.
 * This protocol never changes third-party applications' OTA confirmation state. */
typedef struct {
    char magic[8];
    uint32_t sequence, policy;
    uint8_t active, pending, attempted, valid;
    uint32_t versions[2], result, crc, committed;
} alboot_record;

typedef struct {
    char magic[8];
    uint32_t policy, version_code, product_id, reserved;
} alboot_capability;

enum { ALBOOT_NO_SLOT = 255, ALBOOT_SUCCESS = 1, ALBOOT_ROLLBACK = 2 };
#define ALBOOT_COMMITTED 0xAB57A7EDu

static inline int alboot_valid(const alboot_record* r, uint32_t policy) {
    return !memcmp(r->magic, "ALBOOT01", 8) && r->sequence && r->policy == policy &&
           r->active < 2 && (r->pending < 2 || r->pending == ALBOOT_NO_SLOT) &&
           r->pending != r->active && r->attempted <= 1 && !(r->valid & ~3u) &&
           (r->valid & (1u << r->active)) && r->committed == ALBOOT_COMMITTED &&
           r->crc == alpt_crc(r, offsetof(alboot_record, crc));
}
static inline int alboot_load(const alpt_io* io, uint32_t address, uint32_t policy,
                              alboot_record* out, int* bank) {
    unsigned i;
    memset(out, 0, sizeof(*out));
    out->pending = ALBOOT_NO_SLOT;
    out->valid = 1; /* The full USB package seeds Launcher A. */
    out->policy = policy;
    *bank = -1;
    for (i = 0; i < 2; ++i) {
        alboot_record r;
        if (io->read(io->context, address + i * 4096, &r, sizeof r))
            return ALPT_IO;
        if (alboot_valid(&r, policy) && (*bank < 0 || (int32_t)(r.sequence - out->sequence) > 0)) {
            *out = r;
            *bank = (int)i;
        }
    }
    return *bank < 0 ? ALPT_NONE : ALPT_OK;
}
static inline int alboot_save(const alpt_io* io, uint32_t address, uint32_t policy,
                              alboot_record* value) {
    alboot_record previous, next = *value, check;
    int bank;
    if (alboot_load(io, address, policy, &previous, &bank) < 0)
        return ALPT_IO;
    memcpy(next.magic, "ALBOOT01", 8);
    next.policy = policy;
    next.sequence = previous.sequence + 1;
    if (!next.sequence)
        next.sequence = 1;
    next.crc = alpt_crc(&next, offsetof(alboot_record, crc));
    next.committed = ALBOOT_COMMITTED;
    if (!alboot_valid(&next, policy))
        return ALPT_INVALID;
    address += bank == 0 ? 4096 : 0;
    if (io->erase(io->context, address, 4096) ||
        io->write(io->context, address, &next, offsetof(alboot_record, committed)) ||
        io->write(io->context, address + offsetof(alboot_record, committed), &next.committed, 4) ||
        io->read(io->context, address, &check, sizeof check) || memcmp(&check, &next, sizeof check))
        return ALPT_IO;
    *value = next;
    return ALPT_OK;
}
/* Called before entering a pending Launcher, including failures before app_main.
 * Returns 1 to reject an unconfirmed second boot, 0 to allow the first attempt. */
static inline int alboot_attempt(alboot_record* r, unsigned selected) {
    if (r->pending != selected)
        return 0;
    if (!r->attempted) {
        r->attempted = 1;
        return 0;
    }
    r->valid &= (uint8_t)~(1u << r->pending);
    r->pending = ALBOOT_NO_SLOT;
    r->attempted = 0;
    r->result = ALBOOT_ROLLBACK;
    return 1;
}
static inline int alboot_confirm(alboot_record* r, unsigned running, uint32_t version) {
    if (running > 1)
        return ALPT_INVALID;
    if (r->pending != ALBOOT_NO_SLOT &&
        (r->pending != running || r->versions[running] != version || !r->attempted))
        return ALPT_INVALID;
    r->active = (uint8_t)running;
    r->valid |= (uint8_t)(1u << running);
    r->versions[running] = version;
    if (r->pending != ALBOOT_NO_SLOT)
        r->result = ALBOOT_SUCCESS;
    r->pending = ALBOOT_NO_SLOT;
    r->attempted = 0;
    return ALPT_OK;
}
