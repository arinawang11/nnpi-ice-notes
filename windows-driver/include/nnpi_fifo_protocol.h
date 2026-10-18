#ifndef NNPI_FIFO_PROTOCOL_H
#define NNPI_FIFO_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Pure snapshot arithmetic and a DATA-ONLY command push plan. No hardware I/O,
 * allocation, DMA, message execution, locking, or register update prediction.
 * Success is NNPI_FIFO_OK; every error leaves the complete output unchanged.
 * A successful plan is not a reservation: callers own snapshot freshness and
 * synchronization. Structures here are local helper types, not a wire/IOCTL ABI.
 */
#define NNPI_FIFO_DEPTH 16u
#define NNPI_FIFO_QWORD_BYTES 8u
#define NNPI_FIFO_COMMAND_CONTROL_OFFSET UINT32_C(0x044)
#define NNPI_FIFO_READ_MASK UINT32_C(0x0000000f)
#define NNPI_FIFO_WRITE_MASK UINT32_C(0x00001f00)
#define NNPI_FIFO_COMMAND_NO_NOTIFY_OFFSET UINT32_C(0x050)
#define NNPI_FIFO_COMMAND_NOTIFY_OFFSET UINT32_C(0x058)

enum nnpi_fifo_result {
    NNPI_FIFO_OK = 0,
    NNPI_FIFO_INVALID = 1,
    NNPI_FIFO_RANGE = 2,
    NNPI_FIFO_BAD_STATE = 3,
    NNPI_FIFO_NO_SPACE = 4
};

struct nnpi_fifo_snapshot {
    uint32_t read_pointer;
    uint32_t write_pointer;
    uint32_t used_qwords;
    uint32_t free_qwords;
};

struct nnpi_fifo_command_write {
    uint64_t value;
    uint32_t low_offset;
    uint32_t high_offset;
    uint32_t notify;
};

struct nnpi_fifo_command_plan {
    uint32_t count;
    uint32_t free_before;
    uint32_t free_after;
    struct nnpi_fifo_command_write writes[NNPI_FIFO_DEPTH];
};

/* Linux nnp_pcie.c uses write-read, NOT a modulo-32 difference. The 5-bit
 * write field is an extended position relative to the 4-bit read field.
 * Accept only 0 <= read <= 15 and read <= write <= read+16 (write <= 31).
 * Unrelated control bits are ignored, not claimed to be reserved/zero.
 * A wide input makes accidental >32-bit values rejectable before narrowing.
 */
static inline int nnpi_fifo_decode(uint64_t control,
                                   struct nnpi_fifo_snapshot *out)
{
    struct nnpi_fifo_snapshot result;
    if (!out)
        return NNPI_FIFO_INVALID;
    if (control > UINT64_C(0xffffffff))
        return NNPI_FIFO_RANGE;
    result.read_pointer = (uint32_t)control & NNPI_FIFO_READ_MASK;
    result.write_pointer = ((uint32_t)control & NNPI_FIFO_WRITE_MASK) >> 8;
    if (result.write_pointer < result.read_pointer ||
        result.write_pointer - result.read_pointer > NNPI_FIFO_DEPTH)
        return NNPI_FIFO_BAD_STATE;
    result.used_qwords = result.write_pointer - result.read_pointer;
    result.free_qwords = NNPI_FIFO_DEPTH - result.used_qwords;
    *out = result;
    return NNPI_FIFO_OK;
}

/* Bounded ring-index arithmetic only, as used for RESPONSE read traversal in
 * nnp_pcie.c. Not a command write index or a register-write instruction.
 * Full-depth advance returns the same index. Linux's response full-drain
 * requires TWO register updates; this helper deliberately does not plan them.
 */
static inline int nnpi_fifo_ring_advance(uint32_t index, uint64_t qwords,
                                        uint32_t *out)
{
    uint32_t result;
    if (!out)
        return NNPI_FIFO_INVALID;
    if (index >= NNPI_FIFO_DEPTH || qwords > NNPI_FIFO_DEPTH)
        return NNPI_FIFO_RANGE;
    result = (index + (uint32_t)qwords) % NNPI_FIFO_DEPTH;
    *out = result;
    return NNPI_FIFO_OK;
}

/* Plan a single already-encoded message (e.g. nnpi_boot_protocol.h output).
 * Bytes are LE64 qwords; no opcode or payload semantics are validated.
 * Every qword goes to the SAME push port, never FIFO_0 + index*8.
 * All but the last use 0x050/0x054; the last uses 0x058/0x05c and notify=1.
 * low/high identify register halves, not an executable split-write ordering
 * contract. value is the full 64-bit number; no Windows MMIO is implemented.
 * Empty messages are no-ops, allow NULL bytes, and never notify. A valid
 * snapshot is still required. Nonempty bytes must name byte_count readable
 * bytes. Validation and all input reads precede publishing the output plan.
 * Unused entries are zeroed. free_after is arithmetic, NOT a hardware state.
 */
static inline int nnpi_fifo_plan_command(uint64_t control,
                                        const uint8_t *bytes, size_t byte_count,
                                        struct nnpi_fifo_command_plan *out)
{
    struct nnpi_fifo_snapshot snapshot;
    struct nnpi_fifo_command_plan result;
    size_t count, i;
    unsigned int j;
    int rc;
    if (!out || (!bytes && byte_count != 0))
        return NNPI_FIFO_INVALID;
    if (byte_count % NNPI_FIFO_QWORD_BYTES != 0 ||
        byte_count > NNPI_FIFO_DEPTH * NNPI_FIFO_QWORD_BYTES)
        return NNPI_FIFO_RANGE;
    rc = nnpi_fifo_decode(control, &snapshot);
    if (rc != NNPI_FIFO_OK)
        return rc;
    count = byte_count / NNPI_FIFO_QWORD_BYTES;
    if (count > snapshot.free_qwords)
        return NNPI_FIFO_NO_SPACE;
    memset(&result, 0, sizeof(result));
    result.count = (uint32_t)count;
    result.free_before = snapshot.free_qwords;
    result.free_after = snapshot.free_qwords - (uint32_t)count;
    for (i = 0; i < count; ++i) {
        uint64_t value = 0;
        for (j = 0; j < NNPI_FIFO_QWORD_BYTES; ++j)
            value |= (uint64_t)bytes[i * NNPI_FIFO_QWORD_BYTES + j] << (j * 8u);
        result.writes[i].value = value;
        result.writes[i].notify = (i + 1u == count) ? 1u : 0u;
        result.writes[i].low_offset = result.writes[i].notify ?
            NNPI_FIFO_COMMAND_NOTIFY_OFFSET : NNPI_FIFO_COMMAND_NO_NOTIFY_OFFSET;
        result.writes[i].high_offset = result.writes[i].low_offset + 4u;
    }
    *out = result;
    return NNPI_FIFO_OK;
}

/* Expand a validated plan into an ordered (offset, value) half-write sequence
 * for the transport. low/high are halves of the full QW, in low-then-high
 * order. relative_offset is relative to the command push-port base (BAR0+0x050);
 * a non-notify port maps to 0/4 and the notify port to 8/12, so the driver can
 * write each half with a single WRITE_REGISTER_ULONG and no per-QW decision.
 * This is the transport-agnostic form of the driver's fixed 3-QW boot emit; it
 * works for 1/2/3-QW plans and validates the plan's port fields defensively.
 * max_halves bounds the output; count is set only on success. */
#define NNPI_FIFO_HALF_MAX 32u

struct nnpi_fifo_half_write {
    uint32_t relative_offset;
    uint32_t value;
};

static inline int nnpi_fifo_plan_halves(const struct nnpi_fifo_command_plan *plan,
                                        struct nnpi_fifo_half_write *out,
                                        uint32_t max_halves, uint32_t *count)
{
    uint32_t i;
    if (!plan || !out || !count || plan->count > NNPI_FIFO_DEPTH ||
        max_halves > NNPI_FIFO_HALF_MAX)
        return NNPI_FIFO_INVALID;
    if (plan->count * 2u > max_halves)
        return NNPI_FIFO_RANGE;
    for (i = 0; i < plan->count; ++i) {
        uint64_t value = plan->writes[i].value;
        uint32_t low = plan->writes[i].low_offset;
        uint32_t high = plan->writes[i].high_offset;
        if ((low != NNPI_FIFO_COMMAND_NO_NOTIFY_OFFSET &&
             low != NNPI_FIFO_COMMAND_NOTIFY_OFFSET) ||
            high != low + 4u)
            return NNPI_FIFO_BAD_STATE;
        out[i * 2u].relative_offset = low - NNPI_FIFO_COMMAND_NO_NOTIFY_OFFSET;
        out[i * 2u].value = (uint32_t)value;
        out[i * 2u + 1u].relative_offset = high - NNPI_FIFO_COMMAND_NO_NOTIFY_OFFSET;
        out[i * 2u + 1u].value = (uint32_t)(value >> 32);
    }
    *count = plan->count * 2u;
    return NNPI_FIFO_OK;
}

#endif /* NNPI_FIFO_PROTOCOL_H */
