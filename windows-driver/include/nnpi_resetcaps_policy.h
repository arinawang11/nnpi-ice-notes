/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RESETCAPS_POLICY_H
#define NNPI_RESETCAPS_POLICY_H
#include <string.h>
#include "nnpi_resetcaps_abi.h"
#include "nnpi_info_abi.h"
/* Successful QUERY_INTERFACE transfers one reference; failure transfers none.
 * Cleanup is independent of capability/version/size validation. The adapter's
 * zero-initialized, full WDK object makes the common header safe to inspect. */
static inline int nnpi_resetcaps_needs_dereference(uint32_t status, int has_dereference)
{
    return !(status & 0x80000000U) && has_dereference;
}
static inline int nnpi_resetcaps_contract(uint32_t status, uint32_t requested_size,
    uint32_t requested_version, uint32_t returned_size, uint32_t returned_version,
    int has_dereference)
{
    return nnpi_resetcaps_needs_dereference(status, has_dereference) &&
        requested_size && returned_size >= requested_size && returned_version == requested_version;
}
static inline void nnpi_resetcaps_init(struct nnpi_resetcaps_response *r,
    uint64_t generation, uint32_t size, uint32_t version, uint32_t status)
{
    memset(r, 0, sizeof(*r));
    r->version = NNPI_RESETCAPS_VERSION;
    r->size = (uint32_t)sizeof(*r);
    r->generation = generation;
    r->requested_size = size;
    r->requested_version = version;
    r->query_ntstatus = status;
}
/* Adapter passes only scalars. No retained interface or callable reset handler. */
static inline void nnpi_resetcaps_record(struct nnpi_resetcaps_response *r,
    uint32_t returned_size, uint32_t returned_version, int has_dereference,
    uint32_t raw_types, int handler_present)
{
    if (r->query_ntstatus & 0x80000000U) return;
    r->returned_size = returned_size;
    r->returned_version = returned_version;
    if (!nnpi_resetcaps_contract(r->query_ntstatus, r->requested_size,
            r->requested_version, returned_size, returned_version, has_dereference)) return;
    r->contract_valid = 1;
    r->supported_reset_types = raw_types;
    r->handler_present = handler_present ? 1U : 0U;
    r->supported = (r->handler_present && (raw_types & NNPI_RESETCAPS_KNOWN_TYPES)) ? 1U : 0U;
}
#define NNPI_RESET_GATE_OK 0U
#define NNPI_RESET_GATE_NOT_READY 1U
#define NNPI_RESET_GATE_REMOVED 2U
/* Caller owns the same lifecycle lock as INFO and dmacheck. */
static inline uint32_t nnpi_resetcaps_query(const struct nnpi_resetcaps_response *cache,
    uint32_t flags, int removed, uint64_t generation, struct nnpi_resetcaps_response *out)
{
    memset(out, 0, sizeof(*out));
    if (removed) return NNPI_RESET_GATE_REMOVED;
    if (flags != (NNPI_INFO_FLAG_PREPARED | NNPI_INFO_FLAG_D0) || !generation ||
        cache->generation != generation || !nnpi_resetcaps_response_valid(cache, sizeof(*cache)))
        return NNPI_RESET_GATE_NOT_READY;
    *out = *cache; /* Missing interface is valid diagnostic data, not an IOCTL failure. */
    return NNPI_RESET_GATE_OK;
}
#endif
