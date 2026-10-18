#ifndef NNPI_RUNTIME_PROTOCOL_H
#define NNPI_RUNTIME_PROTOCOL_H

#include <stdint.h>
#include <string.h>

/* Pure, bounded, numeric-QW runtime decoder. No MMIO, ACK, retry, allocation,
 * channel creation, SSH success claim or arbitrary register-write interface.
 * Local structures are NOT an IOCTL ABI. Caller owns lifetime/serialization.
 * Qwords are host numeric values (convert little-endian bytes at transport).
 *
 * Sources (paths relative to the original vendor driver tree):
 * nnpi-drv-5.15/ipc_include/ipc_protocol.h:85-113,183-190 (masks/opcodes);
 * nnpi-card/card_driver/card/driver/host_interface/ipc_protocol.h:94-155
 * (legacy reply1/2 lengths and identical QW0); device.c:135-162 only handles
 * reply3 in nnpi-drv-5.15. Legacy replies below are diagnostic, NOT readiness.
 * card-drv-build/sphcs_cs.c:122-153 emits reply3 and both 2-bit length tables.
 * Known 1150 SSH image: NNPI1150_SSH_SUCCESS.md:30,47,81 records 4.1.0;
 * card host_interface/ipc_protocol.h:50 also defines 4.1.0 (0x1020), NOT 4.0.0
 * from the older linux_upstream header. A reply is not a live SSH validation.
 */
#define NNPI_RUNTIME_MAX_QWORDS 3u
#define NNPI_RUNTIME_CHANNEL_OPCODES 32u
#define NNPI_RUNTIME_PROTOCOL_1150 UINT16_C(0x1020)
#define NNPI_RUNTIME_QUERY_VERSION_QW0 UINT64_C(0)
#define NNPI_RUNTIME_HOST_DRIVER_MASK UINT32_C(0x00000f00)
#define NNPI_RUNTIME_HOST_DRIVER_READY UINT32_C(0x00000100)
#define NNPI_RUNTIME_KEEPALIVE_MASK UINT32_C(0x00f00000)

/* One push consumes exactly one QW on NEED_MORE or MESSAGE. On UNKNOWN the
 * offending QW is retained, failure latches, and no more input is accepted.
 * Neither result grants permission to re-read/ACK a hardware FIFO snapshot.
 * out is written only on MESSAGE. Invalid arguments do not change the parser.
 */
enum nnpi_runtime_result {
    NNPI_RUNTIME_NEED_MORE = 0,
    NNPI_RUNTIME_MESSAGE = 1,
    NNPI_RUNTIME_INVALID = 2,
    NNPI_RUNTIME_UNKNOWN = 3,
    NNPI_RUNTIME_STOPPED = 4
};

struct nnpi_runtime_message {
    uint64_t qwords[NNPI_RUNTIME_MAX_QWORDS];
    uint64_t response_sizes_raw;
    uint64_t command_sizes_raw;
    uint16_t protocol_version;
    uint16_t firmware_version;
    uint16_t channel_version;
    uint8_t opcode;
    uint8_t qword_count;
    /* Index 0 describes opcode 32, index 31 opcode 63. Zero is unsupported,
     * NOT one QW. Values 1..3 are lengths in QWs, not bytes or length-minus-1.
     */
    uint8_t response_sizes[NNPI_RUNTIME_CHANNEL_OPCODES];
    uint8_t command_sizes[NNPI_RUNTIME_CHANNEL_OPCODES];
};

struct nnpi_runtime_parser {
    uint64_t partial[NNPI_RUNTIME_MAX_QWORDS];
    uint32_t count;
    uint32_t expected;
    uint32_t stopped;
};

/* Use only at a NEW transport session/known message boundary, never as an
 * automatic error recovery action. Partial input must survive polling rounds.
 */
static inline void nnpi_runtime_init(struct nnpi_runtime_parser *parser)
{
    if (parser)
        memset(parser, 0, sizeof(*parser));
}

static inline uint32_t nnpi_runtime_message_qwords(uint32_t opcode)
{
    switch (opcode) {
    case 0u: return 1u; /* legacy QUERY_VERSION_REPLY */
    case 1u: return 2u; /* legacy QUERY_VERSION_REPLY2 */
    case 2u: return 3u; /* QUERY_VERSION_REPLY3 */
    case 4u: return 1u; /* EVENT_REPORT, retained raw, NOT success */
    case 11u: return 1u; /* SYS_INFO, retained raw, NOT success */
    case 39u: return 1u; /* ETH_CONFIG, route only after a complete frame. */
    case 40u: return 1u; /* ETH_MSG_DSCR: frame or slot ACK, independent route. */
    default: return 0u; /* BIOS and channels are outside this finite parser. */
    }
}

static inline int nnpi_runtime_push(struct nnpi_runtime_parser *parser,
                                    uint64_t qword,
                                    struct nnpi_runtime_message *out)
{
    struct nnpi_runtime_message result;
    uint32_t i, opcode;
    if (!parser || !out)
        return NNPI_RUNTIME_INVALID;
    if (parser->stopped)
        return NNPI_RUNTIME_STOPPED;
    /* Validate bounds before any array access, even for corrupted caller state. */
    if (parser->count >= NNPI_RUNTIME_MAX_QWORDS ||
        (parser->count == 0u && parser->expected != 0u) ||
        (parser->count != 0u &&
         (parser->expected != nnpi_runtime_message_qwords(
              (uint32_t)(parser->partial[0] & UINT64_C(0x3f))) ||
          parser->expected <= parser->count)))
        return NNPI_RUNTIME_INVALID;
    if (parser->count == 0u) {
        parser->expected = nnpi_runtime_message_qwords(
            (uint32_t)(qword & UINT64_C(0x3f)));
        if (parser->expected == 0u) {
            parser->partial[0] = qword;
            parser->count = 1u;
            parser->stopped = 1u;
            return NNPI_RUNTIME_UNKNOWN;
        }
    }
    parser->partial[parser->count++] = qword;
    if (parser->count < parser->expected)
        return NNPI_RUNTIME_NEED_MORE;

    memset(&result, 0, sizeof(result));
    opcode = (uint32_t)(parser->partial[0] & UINT64_C(0x3f));
    result.opcode = (uint8_t)opcode;
    result.qword_count = (uint8_t)parser->expected;
    for (i = 0; i < parser->expected; ++i)
        result.qwords[i] = parser->partial[i];
    if (opcode <= 2u) {
        result.protocol_version = (uint16_t)((parser->partial[0] >> 6) & UINT64_C(0xffff));
        result.firmware_version = (uint16_t)((parser->partial[0] >> 22) & UINT64_C(0xffff));
        result.channel_version = (uint16_t)((parser->partial[0] >> 38) & UINT64_C(0xffff));
        if (opcode >= 1u)
            result.response_sizes_raw = parser->partial[1];
        if (opcode == 2u)
            result.command_sizes_raw = parser->partial[2];
        for (i = 0; i < NNPI_RUNTIME_CHANNEL_OPCODES; ++i) {
            result.response_sizes[i] = (uint8_t)((result.response_sizes_raw >> (2u * i)) & UINT64_C(3));
            result.command_sizes[i] = (uint8_t)((result.command_sizes_raw >> (2u * i)) & UINT64_C(3));
        }
    }
    *out = result;
    memset(parser, 0, sizeof(*parser));
    return NNPI_RUNTIME_MESSAGE;
}

/* Only protocol compatibility evidence, NOT DriverReady/CardReady/SSH success.
 * Linux device.c:104-116 compares major/minor and requires nonzero response
 * table. Patch is deliberately ignored. Reserved bits are retained, not guessed.
 * Only use with a message returned by this decoder, not untrusted structures.
 */
static inline int nnpi_runtime_compatible_1150(
    const struct nnpi_runtime_message *message)
{
    return message && message->opcode == 2u && message->qword_count == 3u &&
           (message->protocol_version & UINT16_C(0x7fe0)) ==
               NNPI_RUNTIME_PROTOCOL_1150 && message->response_sizes_raw != 0;
}

/* QUERY_VERSION is exactly one zero QW: Linux device.c:1363-1369.
 * Use the existing command FIFO planner/transport; this does not send it.
 */
static inline uint64_t nnpi_runtime_query_version(void)
{
    return NNPI_RUNTIME_QUERY_VERSION_QW0;
}

/* Data-only, fixed-purpose doorbell transformations, never writes. A snapshot
 * must be fresh and serialized at the transport; preserve all unrelated bits.
 * nnp_boot_defs.h:47,49,67: driver bits11:8, READY=1, keepalive bits23:20.
 * nnp_pcie.c:1138-1149: counter 0->1, 1..13->next, 14->1. Zero is initial state,
 * never emitted; 15 is rejected. Caller owns card-state gating/timer/commit.
 * Output arguments must not overlap each other or input parser/message objects.
 */
static inline int nnpi_runtime_host_ready_value(uint64_t snapshot, uint32_t *out)
{
    if (!out || snapshot > UINT64_C(0xffffffff))
        return NNPI_RUNTIME_INVALID;
    *out = ((uint32_t)snapshot & ~NNPI_RUNTIME_HOST_DRIVER_MASK) |
           NNPI_RUNTIME_HOST_DRIVER_READY;
    return NNPI_RUNTIME_MESSAGE;
}

static inline int nnpi_runtime_keepalive_value(uint64_t snapshot,
                                              uint32_t previous,
                                              uint32_t *next, uint32_t *out)
{
    uint32_t counter, value;
    if (!next || !out || next == out || snapshot > UINT64_C(0xffffffff) ||
        previous > 14u)
        return NNPI_RUNTIME_INVALID;
    counter = previous < 14u ? previous + 1u : 1u;
    value = ((uint32_t)snapshot & ~NNPI_RUNTIME_KEEPALIVE_MASK) | (counter << 20);
    *next = counter;
    *out = value;
    return NNPI_RUNTIME_MESSAGE;
}

/* Transport order is intentionally NOT implemented here:
 * nnp_pcie.c:153-178 snapshot -> read ALL QWs (slot increments before read),
 * :140-142 low then high, :186-200 ACK full drain (16 QWs needs +1 then final),
 * :202-203 parse retained copy. Incomplete read: no full-drain ACK. A completed
 * or partial ACK sequence must never be blindly replayed on parser failure.
 */
#endif /* NNPI_RUNTIME_PROTOCOL_H */
