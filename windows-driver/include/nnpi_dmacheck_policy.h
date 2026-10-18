/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_DMACHECK_POLICY_H
#define NNPI_DMACHECK_POLICY_H
#include <string.h>
#include "nnpi_dmacheck_abi.h"
#include "nnpi_boot_protocol.h"
#include "nnpi_info_abi.h"
/* All callers own the lifecycle lock. This code performs no platform DMA API. */
static inline void nnpi_dmacheck_init(struct nnpi_dmacheck_response *r, uint64_t generation)
{
    memset(r, 0, sizeof(*r));
    r->version = NNPI_DMACHECK_VERSION;
    r->size = (uint32_t)sizeof(*r);
    r->generation = generation;
    r->attributes = NNPI_DMACHECK_ATTRIBUTES;
    r->profile = NNPI_DMACHECK_PROFILE_SG64;
    r->requested_bytes = NNPI_DMACHECK_BYTES;
    r->alignment_mask = NNPI_DMACHECK_ALIGNMENT;
}
static inline uint32_t nnpi_dmacheck_layout(uint64_t logical, uint64_t bytes, int has_va)
{
    if (!has_va) return NNPI_DMA_VIRTUAL_ADDRESS;
    if (bytes != NNPI_DMACHECK_BYTES) return NNPI_DMA_LENGTH;
    switch (nnpi_boot_dma_range(logical, bytes)) {
    case NNPI_BOOT_ALIGNMENT: return NNPI_DMA_ALIGNMENT;
    case NNPI_BOOT_OK: return NNPI_DMA_READY;
    default: return NNPI_DMA_RANGE;
    }
}
/* Byte callbacks allow fault injection into the EXACT production test sequence.
 * Production callbacks must use volatile loads/stores, not an ordinary memcpy. */
typedef void (*nnpi_dma_write)(void *context, size_t offset, uint8_t value);
typedef uint8_t (*nnpi_dma_read)(void *context, size_t offset);
static inline uint8_t nnpi_dmacheck_pattern(size_t offset)
{
    return (uint8_t)((offset * 37U) ^ (offset >> 8) ^ 0xa5U);
}
static inline int nnpi_dmacheck_zero(void *context, size_t bytes,
    nnpi_dma_write write_byte, nnpi_dma_read read_byte)
{
    size_t i;
    int ok = 1;
    for (i = 0; i < bytes; ++i) write_byte(context, i, 0);
    for (i = 0; i < bytes; ++i) if (read_byte(context, i) != 0) ok = 0;
    return ok;
}
static inline uint32_t nnpi_dmacheck_cpu(void *context, nnpi_dma_write write_byte,
    nnpi_dma_read read_byte, uint32_t *checks)
{
    size_t i;
    int pattern_ok = 1, zero_ok;
    if (!nnpi_dmacheck_zero(context, NNPI_DMACHECK_BYTES, write_byte, read_byte))
        return NNPI_DMA_INITIAL_ZERO;
    *checks |= NNPI_DMACHECK_INITIAL_ZERO;
    for (i = 0; i < NNPI_DMACHECK_BYTES; ++i)
        write_byte(context, i, nnpi_dmacheck_pattern(i));
    for (i = 0; i < NNPI_DMACHECK_BYTES; ++i)
        if (read_byte(context, i) != nnpi_dmacheck_pattern(i)) pattern_ok = 0;
    /* Always scrub after the pattern, including comparison failures. */
    zero_ok = nnpi_dmacheck_zero(context, NNPI_DMACHECK_BYTES, write_byte, read_byte);
    if (!pattern_ok) return NNPI_DMA_PATTERN;
    *checks |= NNPI_DMACHECK_PATTERN;
    if (!zero_ok) return NNPI_DMA_FINAL_ZERO;
    *checks |= NNPI_DMACHECK_FINAL_ZERO;
    return NNPI_DMA_READY;
}
/* No query callback: dispatch can ONLY copy cached metadata, never touch the page. */
#define NNPI_DMA_GATE_OK 0U
#define NNPI_DMA_GATE_NOT_READY 1U
#define NNPI_DMA_GATE_REMOVED 2U
static inline uint32_t nnpi_dmacheck_query(const struct nnpi_dmacheck_response *cache,
    uint32_t flags, int removed, uint64_t generation, struct nnpi_dmacheck_response *out)
{
    memset(out, 0, sizeof(*out));
    if (removed) return NNPI_DMA_GATE_REMOVED;
    if (flags != (NNPI_INFO_FLAG_PREPARED | NNPI_INFO_FLAG_D0) || !generation ||
        cache->generation != generation || !nnpi_dmacheck_response_valid(cache, sizeof(*cache)))
        return NNPI_DMA_GATE_NOT_READY;
    *out = *cache; /* A valid failure is queryable, but is never a pass. */
    return NNPI_DMA_GATE_OK;
}
/* Release is legal ONLY while never published. Production has no publishing API.
 * Clearing metadata precedes CPU scrubbing/deletion under the same lock. */
static inline void nnpi_dmacheck_invalidate(struct nnpi_dmacheck_response *cache)
{
    memset(cache, 0, sizeof(*cache));
}
/* Platform adapter owns private addresses/handles; none enter the result. */
typedef uint32_t (*nnpi_dma_allocate)(void *context, uint64_t *logical, uint64_t *bytes, int *has_va);
typedef void (*nnpi_dma_release)(void *context);
static inline void nnpi_dmacheck_release(struct nnpi_dmacheck_response *cache,
    void *context, nnpi_dma_release release_page)
{
    nnpi_dmacheck_invalidate(cache);
    release_page(context); /* Adapter must tolerate no live allocation. */
}
static inline void nnpi_dmacheck_prepare(struct nnpi_dmacheck_response *cache,
    uint64_t generation, uint32_t enabler_status, void *context,
    nnpi_dma_allocate allocate_page, nnpi_dma_release release_page,
    nnpi_dma_write write_byte, nnpi_dma_read read_byte)
{
    uint64_t logical = 0, bytes = 0;
    uint32_t status;
    int has_va = 0;
    nnpi_dmacheck_release(cache, context, release_page);
    nnpi_dmacheck_init(cache, generation);
    cache->stage = NNPI_DMA_ENABLER;
    cache->ntstatus = enabler_status;
    if (enabler_status & 0x80000000U) return;
    cache->checks = NNPI_DMACHECK_ENABLER;
    cache->stage = NNPI_DMA_ALLOCATE;
    status = allocate_page(context, &logical, &bytes, &has_va);
    cache->ntstatus = status;
    if (status & 0x80000000U) {
        release_page(context); /* Also cleans any partial allocation. */
        return;
    }
    cache->actual_bytes = bytes;
    cache->checks |= NNPI_DMACHECK_ALLOCATED;
    cache->stage = nnpi_dmacheck_layout(logical, bytes, has_va);
    if (cache->stage == NNPI_DMA_READY) {
        cache->checks |= NNPI_DMACHECK_LAYOUT;
        cache->stage = nnpi_dmacheck_cpu(context, write_byte, read_byte, &cache->checks);
    }
    if (cache->stage != NNPI_DMA_READY) {
        cache->ntstatus = 0xc000009cU; /* STATUS_DEVICE_DATA_ERROR */
        release_page(context); /* Scrub and destroy, retaining only failure metadata. */
        return;
    }
    cache->ntstatus = 0;
    cache->passed = 1;
    cache->retained = 1;
}
#endif
