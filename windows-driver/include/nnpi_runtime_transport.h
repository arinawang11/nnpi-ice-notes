/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RUNTIME_TRANSPORT_H
#define NNPI_RUNTIME_TRANSPORT_H

#include "nnpi_runtime_protocol.h"

/* Internal transport policy, not an IOCTL ABI. A command mapping starts at
 * BAR0+0x050; the response window at 0x060 is never a query destination.
 * The callback performs exactly one write32. No reads, allocation or retry.
 * The owner serializes the state for its entire transport session. */
#define NNPI_RUNTIME_COMMAND_BASE UINT32_C(0x050)
#define NNPI_RUNTIME_QUERY_LOW UINT32_C(0x058)
#define NNPI_RUNTIME_QUERY_HIGH UINT32_C(0x05c)

enum nnpi_runtime_query_result {
    NNPI_RUNTIME_QUERY_OK = 0,
    NNPI_RUNTIME_QUERY_INVALID = 1,
    NNPI_RUNTIME_QUERY_USED = 2,
    NNPI_RUNTIME_QUERY_WRITE_FAILED = 3
};

struct nnpi_runtime_query_state {
    uint32_t used;
    uint32_t attempted_halves;
    uint32_t completed_halves;
    uint32_t last_relative_offset;
    uint32_t last_value;
    uint32_t result;
};

typedef int (*nnpi_runtime_write32_fn)(void *context,
    uint32_t relative_offset, uint32_t value);

static inline int nnpi_runtime_query_send(struct nnpi_runtime_query_state *state,
    uint32_t mapping_base, int mapping_ready, void *context,
    nnpi_runtime_write32_fn write32)
{
    uint64_t query = nnpi_runtime_query_version();
    uint32_t half;
    if (!state) return NNPI_RUNTIME_QUERY_INVALID;
    /* Refuse replay without changing the retained partial-write evidence. */
    if (state->used || state->attempted_halves || state->completed_halves)
        return NNPI_RUNTIME_QUERY_USED;
    if (mapping_base != NNPI_RUNTIME_COMMAND_BASE || !mapping_ready ||
        !context || !write32) {
        state->result = NNPI_RUNTIME_QUERY_INVALID;
        return NNPI_RUNTIME_QUERY_INVALID;
    }
    state->used = 1; /* Claim before even the first possibly partial write. */
    state->result = NNPI_RUNTIME_QUERY_WRITE_FAILED;
    for (half = 0; half < 2; ++half) {
        state->last_relative_offset = (half ? NNPI_RUNTIME_QUERY_HIGH :
            NNPI_RUNTIME_QUERY_LOW) - NNPI_RUNTIME_COMMAND_BASE;
        state->last_value = (uint32_t)(half ? query >> 32 : query);
        ++state->attempted_halves;
        if (!write32(context, state->last_relative_offset, state->last_value))
            return NNPI_RUNTIME_QUERY_WRITE_FAILED;
        ++state->completed_halves;
    }
    state->result = NNPI_RUNTIME_QUERY_OK;
    return NNPI_RUNTIME_QUERY_OK;
}

#endif /* NNPI_RUNTIME_TRANSPORT_H */
