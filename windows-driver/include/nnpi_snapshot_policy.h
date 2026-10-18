/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_SNAPSHOT_POLICY_H
#define NNPI_SNAPSHOT_POLICY_H
#include "nnpi_snapshot_abi.h"
#include "nnpi_status_policy.h"
#define NNPI_SNAPSHOT_WINDOW_OFFSET 0x038U
#define NNPI_SNAPSHOT_WINDOW_BYTES 0x2cU
#define NNPI_SNAPSHOT_COMMAND_OFFSET 0x044U
#define NNPI_SNAPSHOT_RESPONSE_OFFSET 0x060U
#define NNPI_SNAPSHOT_DOORBELL_OFFSET 0x038U
#define NNPI_SNAPSHOT_BAD_BUFFERS 7U
/* Same identity/association policy as status, independently bounded window. */
static inline uint32_t nnpi_snapshot_window(const struct nnpi_diag_response *d, uint64_t *address)
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
        b->length < NNPI_SNAPSHOT_WINDOW_OFFSET + NNPI_SNAPSHOT_WINDOW_BYTES ||
        !nnpi_bar_range(b->translated_start, b->length) ||
        (b->translated_start & 3U) ||
        b->translated_start > (uint64_t)INT64_MAX - NNPI_SNAPSHOT_WINDOW_OFFSET - (NNPI_SNAPSHOT_WINDOW_BYTES - 1U))
        return NNPI_STATUS_RANGE;
    *address = b->translated_start + NNPI_SNAPSHOT_WINDOW_OFFSET;
    return NNPI_STATUS_OK;
}

/* Offset is selected only here, never supplied by an IOCTL caller. Caller holds
 * the lifecycle lock from gate through all three reads. Sequential, NOT atomic;
 * the doorbell read can notify the peer. No reads of intervening registers. */
typedef uint32_t (*nnpi_snapshot_reader)(void *context, uint32_t fixed_offset);
static inline uint32_t nnpi_snapshot_sample(const struct nnpi_snapshot_request *request,
    size_t input_bytes, size_t output_bytes, uint32_t flags, int removed,
    int mapped, uint64_t generation, uint32_t map_reason,
    nnpi_snapshot_reader read_once, void *context, struct nnpi_snapshot_response *out)
{
    struct nnpi_fifo_snapshot f;
    uint32_t reason;
    if (!out) return NNPI_SNAPSHOT_BAD_BUFFERS;
    memset(out, 0, sizeof(*out));
    if (!nnpi_snapshot_buffers_valid(request, input_bytes, output_bytes))
        return NNPI_SNAPSHOT_BAD_BUFFERS;
    reason = nnpi_status_gate(flags, removed, mapped, generation, map_reason);
    if (reason != NNPI_STATUS_OK) return reason;
    if (!read_once) return NNPI_STATUS_NOT_READY;
    out->command_control = read_once(context, NNPI_SNAPSHOT_COMMAND_OFFSET);
    out->response_control = read_once(context, NNPI_SNAPSHOT_RESPONSE_OFFSET);
    out->card_doorbell = read_once(context, NNPI_SNAPSHOT_DOORBELL_OFFSET);
    out->version = NNPI_SNAPSHOT_VERSION;
    out->size = (uint32_t)sizeof(*out);
    out->generation = generation;
    out->attributes = NNPI_SNAPSHOT_ATTRIBUTES;
    out->read_count = 3;
    out->raw_valid = 7;
    out->boot_state = out->card_doorbell & 0xffU;
    out->error_code = (out->card_doorbell >> 12) & 0xfU;
    if (nnpi_fifo_decode(out->command_control, &f) == NNPI_FIFO_OK) {
        out->fifo_valid |= NNPI_SNAPSHOT_COMMAND_VALID;
        out->command_used = f.used_qwords;
        out->command_free = f.free_qwords;
    }
    if (nnpi_fifo_decode(out->response_control, &f) == NNPI_FIFO_OK) {
        out->fifo_valid |= NNPI_SNAPSHOT_RESPONSE_VALID;
        out->response_used = f.used_qwords;
        out->response_free = f.free_qwords;
    }
    return NNPI_STATUS_OK; /* Bad FIFO retains raw data and partial validity. */
}
#endif
