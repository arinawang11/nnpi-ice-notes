#ifndef NNPI_RESPONSE_FIFO_H
#define NNPI_RESPONSE_FIFO_H

#include "nnpi_fifo_protocol.h"

/* Data-only full-drain plan for one RESPONSE_PCI_CONTROL snapshot.
 * No MMIO, message parser, allocation, synchronization or hardware prediction.
 * Local helper structures, NOT a wire/IOCTL ABI. See response-fifo-notes.md.
 */
#define NNPI_RESPONSE_CONTROL_OFFSET UINT32_C(0x060)
#define NNPI_RESPONSE_FIFO_BASE_OFFSET UINT32_C(0x100)
#define NNPI_RESPONSE_MAX_ACKS 2u

struct nnpi_response_fifo_read {
    uint32_t slot;
    uint32_t low_offset;
    uint32_t high_offset;
};

struct nnpi_response_fifo_ack {
    uint32_t offset;
    uint32_t value;
};

struct nnpi_response_fifo_plan {
    uint32_t read_pointer;
    uint32_t write_pointer;
    uint32_t read_count;
    uint32_t final_read_pointer;
    uint32_t ack_count;
    struct nnpi_response_fifo_read reads[NNPI_FIFO_DEPTH];
    struct nnpi_response_fifo_ack acks[NNPI_RESPONSE_MAX_ACKS];
};

/* Source: nnpi-drv-5.15/nnp_pcie.c:nnp_process_commands and nnp_elbi.h.
 * Response and command pointer masks/depth match, so reuse validated arithmetic.
 * Reject >32-bit control, write<read and occupancy>16 before publishing output.
 * Every failure leaves all output bytes unchanged; unused entries are zero.
 *
 * Execution contract for a future transport (NOT implemented here):
 * - serialize ownership and obtain a fresh snapshot;
 * - read ALL read_count qwords into retained host storage in array order;
 * - ONLY then perform every 32-bit ack in array order at offset 0x060.
 * A failed/incomplete read must not authorize any ack from this full-drain plan.
 * A stale plan or a partially completed ack sequence must not be blindly replayed.
 * low/high describe register halves; source uses lo_hi_readq (low then high).
 * Platform MMIO accessors/barriers/error recovery remain transport obligations.
 * ack values preserve EVERY non-read-pointer bit from the supplied snapshot;
 * they are not predicted fresh register contents or a replacement snapshot.
 */
static inline int nnpi_response_fifo_plan_drain(
    uint64_t control, struct nnpi_response_fifo_plan *out)
{
    struct nnpi_fifo_snapshot snapshot;
    struct nnpi_response_fifo_plan result;
    uint32_t pointer, i, preserved;
    int rc;
    if (!out)
        return NNPI_FIFO_INVALID;
    rc = nnpi_fifo_decode(control, &snapshot);
    if (rc != NNPI_FIFO_OK)
        return rc;

    memset(&result, 0, sizeof(result));
    result.read_pointer = snapshot.read_pointer;
    result.write_pointer = snapshot.write_pointer;
    result.read_count = snapshot.used_qwords;
    pointer = snapshot.read_pointer;
    for (i = 0; i < result.read_count; ++i) {
        pointer = (pointer + 1u) % NNPI_FIFO_DEPTH;
        result.reads[i].slot = pointer;
        result.reads[i].low_offset = NNPI_RESPONSE_FIFO_BASE_OFFSET +
                                     pointer * NNPI_FIFO_QWORD_BYTES;
        result.reads[i].high_offset = result.reads[i].low_offset + 4u;
    }
    result.final_read_pointer = pointer;
    preserved = (uint32_t)control & ~NNPI_FIFO_READ_MASK;
    if (result.read_count == NNPI_FIFO_DEPTH) {
        /* Same-value update is forbidden by HW: first advance one slot. */
        result.acks[0].offset = NNPI_RESPONSE_CONTROL_OFFSET;
        result.acks[0].value = preserved | ((pointer + 1u) % NNPI_FIFO_DEPTH);
        result.ack_count = 1;
    }
    if (result.read_count != 0) {
        result.acks[result.ack_count].offset = NNPI_RESPONSE_CONTROL_OFFSET;
        result.acks[result.ack_count].value = preserved | pointer;
        ++result.ack_count;
    }
    *out = result;
    return NNPI_FIFO_OK;
}

#endif /* NNPI_RESPONSE_FIFO_H */
