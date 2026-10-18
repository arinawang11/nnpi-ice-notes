/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_STATUS_POLICY_H
#define NNPI_STATUS_POLICY_H
#include "nnpi_status_abi.h"
#include "nnpi_diag_abi.h"
#define NNPI_STATUS0_OFFSET 0x1b8U
#define NNPI_STATUS0_BYTES 4U
#define NNPI_STATUS_OK 0U
#define NNPI_STATUS_IDENTITY 1U
#define NNPI_STATUS_ASSOCIATION 2U
#define NNPI_STATUS_RANGE 3U
#define NNPI_STATUS_MAP_FAILED 4U
#define NNPI_STATUS_REMOVED 5U
#define NNPI_STATUS_NOT_READY 6U
/* Pure decision shared by production and portable tests. Input is the cached,
 * already collected association, not a user-selected BAR/address. Validate again
 * with the existing matcher; flags/generation here describe publication only. */
static inline uint32_t nnpi_status_window(const struct nnpi_diag_response *d, uint64_t *address)
{
    struct nnpi_diag_response check;
    const struct nnpi_bar_entry *b;
    if (!address) return NNPI_STATUS_RANGE;
    *address = 0;
    if (!d) return NNPI_STATUS_ASSOCIATION;
    if (d->config_bytes != 64 || nnpi_bar_le32(d->config) != 0x45c68086U ||
        nnpi_bar_le32(d->config + 44) != 0x45ce8086U || d->config[8] != 0)
        return NNPI_STATUS_IDENTITY;
    check = *d;
    check.flags = NNPI_DIAG_PREPARED_D0;
    check.generation = 1;
    if (!nnpi_diag_response_valid(&check, sizeof(check)) || check.reason != NNPI_BAR_OK)
        return NNPI_STATUS_ASSOCIATION;
    b = &d->bars[0];
    if (b->bar != 0 || b->type != NNPI_BAR_MEMORY || b->prefetch ||
        b->length < NNPI_STATUS0_OFFSET + NNPI_STATUS0_BYTES ||
        !nnpi_bar_range(b->translated_start, b->length) ||
        (b->translated_start & 3U) ||
        b->translated_start > (uint64_t)INT64_MAX - NNPI_STATUS0_OFFSET - (NNPI_STATUS0_BYTES - 1U))
        return NNPI_STATUS_RANGE;
    *address = b->translated_start + NNPI_STATUS0_OFFSET;
    return NNPI_STATUS_OK;
}
static inline uint32_t nnpi_status_gate(uint32_t flags, int removed,
                                       int mapped, uint64_t generation, uint32_t map_reason)
{
    if (removed) return NNPI_STATUS_REMOVED;
    if (flags != NNPI_DIAG_PREPARED_D0 || !generation) return NNPI_STATUS_NOT_READY;
    if (map_reason != NNPI_STATUS_OK) return map_reason;
    return mapped ? NNPI_STATUS_OK : NNPI_STATUS_MAP_FAILED;
}
/* Caller holds the lifecycle lock over gate AND callback. Callback is exactly
 * one fixed 32-bit read; no retries, polls, writes or cancellation ownership. */
typedef uint32_t (*nnpi_status_reader)(void *context);
static inline uint32_t nnpi_status_sample(uint32_t flags, int removed,
    int mapped, uint64_t generation, uint32_t map_reason,
    nnpi_status_reader read_once, void *context, struct nnpi_status_response *out)
{
    struct nnpi_status_response zero = {0, 0, 0, 0, 0, 0, 0, {0, 0, 0, 0}};
    uint32_t reason;
    if (!out) return NNPI_STATUS_NOT_READY;
    *out = zero;
    reason = nnpi_status_gate(flags, removed, mapped, generation, map_reason);
    if (reason != NNPI_STATUS_OK) return reason;
    if (!read_once) return NNPI_STATUS_NOT_READY;
    out->value = read_once(context);
    out->version = NNPI_STATUS_VERSION;
    out->size = (uint32_t)sizeof(*out);
    out->generation = generation;
    out->valid = 1;
    out->attributes = NNPI_STATUS_ATTRIBUTES;
    out->read_count = 1;
    return NNPI_STATUS_OK;
}
/* Clear ownership before returning the old mapping to the adapter for unmap. */
static inline void *nnpi_status_detach(void **mapping)
{
    void *old = *mapping;
    *mapping = NULL;
    return old;
}
#endif
