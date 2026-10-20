/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_HOSTWIN_POLICY_H
#define NNPI_HOSTWIN_POLICY_H
#include "nnpi_hostwin_abi.h"
#include "nnpi_diag_abi.h"
#include "nnpi_bar_match.h"

/* Pure decision shared by production and portable tests. Locates the BAR2
 * (inbound memory) association inside the cached diagnostic and bounds the
 * host-window sub-range. Returns the host physical address to map, never a
 * caller-supplied BAR/offset. */
static inline uint32_t nnpi_hostwin_window(const struct nnpi_diag_response *d, uint64_t *address)
{
    struct nnpi_diag_response check;
    const struct nnpi_bar_entry *b = NULL;
    uint32_t i;
    if (!address) return NNPI_HOSTWIN_RANGE;
    *address = 0;
    if (!d) return NNPI_HOSTWIN_ASSOCIATION;
    if (d->config_bytes != 64 || nnpi_bar_le32(d->config) != 0x45c68086U ||
        nnpi_bar_le32(d->config + 44) != 0x45ce8086U || d->config[8] != 0)
        return NNPI_HOSTWIN_ASSOCIATION;
    check = *d;
    check.flags = NNPI_DIAG_PREPARED_D0;
    check.generation = 1;
    if (!nnpi_diag_response_valid(&check, sizeof(check)) || check.reason != NNPI_BAR_OK)
        return NNPI_HOSTWIN_ASSOCIATION;
    /* BAR2 is the 64-bit prefetchable inbound-memory window (diag output shows
     * bar==2, width==64, type==memory, prefetch==1). Find it by BAR index, never
     * by array position, since 64-bit BARs skip a config slot. */
    for (i = 0; i < d->bar_count; ++i) {
        if (d->bars[i].bar == 2) { b = &d->bars[i]; break; }
    }
    if (!b || b->type != NNPI_BAR_MEMORY || b->width != 64 || !b->prefetch ||
        b->length < (uint64_t)NNPI_HOSTWIN_OFFSET + NNPI_HOSTWIN_SIZE ||
        !nnpi_bar_range(b->translated_start, b->length) ||
        (b->translated_start & 3U) ||
        b->translated_start > (uint64_t)INT64_MAX - NNPI_HOSTWIN_OFFSET - (NNPI_HOSTWIN_SIZE - 1U))
        return NNPI_HOSTWIN_RANGE;
    *address = b->translated_start + NNPI_HOSTWIN_OFFSET;
    return NNPI_HOSTWIN_OK;
}

static inline uint32_t nnpi_hostwin_gate(uint32_t flags, int removed,
    int mapped, uint64_t generation, uint32_t map_reason)
{
    if (removed) return NNPI_HOSTWIN_REMOVED;
    if (flags != NNPI_DIAG_PREPARED_D0 || !generation) return NNPI_HOSTWIN_NOT_READY;
    if (map_reason != NNPI_HOSTWIN_OK) return map_reason;
    return mapped ? NNPI_HOSTWIN_OK : NNPI_HOSTWIN_MAP_FAILED;
}

#endif
