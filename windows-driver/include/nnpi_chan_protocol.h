/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_CHAN_PROTOCOL_H
#define NNPI_CHAN_PROTOCOL_H

#include <stdint.h>

/* Pure, bounded, numeric-QW codec for the IPC channel lifecycle commands. No
 * MMIO, ACK replay, allocation or SSH success claim. Qwords are host numeric
 * values; the transport serializes little-endian bytes.
 *
 * Sources (relative to /home/arina/nnpi):
 * nnpi-host/src/nnpi_drv/include/nnpi_ipc/ipc_protocol.h:229-259
 *   (union h2c_channel_op / h2c_channel_data_ringbuf_op), :361-371 (opcodes
 *   CHANNEL_OP=22, CHANNEL_RB_OP=23, CHANNEL_HOSTRES_OP=24);
 * nnpi-host/src/nnpi_drv/include/nnpi_ipc/ipc_c2h_events.h:48-53,64,81,100,
 *   118-119 (EVENT_NON_ERR_DRV_START=4, EVENT_ERR_DRV_START=52,
 *   CREATE_CHANNEL_SUCCESS=21, CREATE_CHANNEL_FAILED=70, DESTROY_CHANNEL_FAILED
 *   =71, EVENT_CARD_FATAL_START=112);
 * nnpi-drv-5.15/cmd_chan.c:473-501 (nnp_chan_send_destroy encodes CHANNEL_OP
 *   with destroy=1) - a channel object is allocated by the host, then the card
 *   confirms creation with a CREATE_CHANNEL_SUCCESS EVENT_REPORT carrying the
 *   channel id in obj_id. The network channel uses CHANNEL_RB_OP to publish its
 *   H2C/C2H DMA chains by chain-header page-frame number, NOT a hostres handle
 *   or the PFN of the first data page.
 */

/* Non-channel H2C opcodes (0-31 range) used for the channel lifecycle. */
#define NNPI_CHAN_OP_H2C         22u /* NNP_IPC_H2C_OP_CHANNEL_OP */
#define NNPI_CHAN_RB_OP_H2C      23u /* NNP_IPC_H2C_OP_CHANNEL_RB_OP */
#define NNPI_CHAN_HOSTRES_OP_H2C 24u /* NNP_IPC_H2C_OP_CHANNEL_HOSTRES_OP */

/* Channel id field width and bounds. */
#define NNPI_CHAN_ID_BITS        10u
#define NNPI_CHAN_ID_MASK        0x3ffu
#define NNPI_CHAN_MIN_NONCTX     256u /* inference contexts use 0..255 */

/* DMA address is published as a 45-bit page-frame number (byte addr >> 12). */
#define NNPI_CHAN_PAGE_SHIFT     12u
#define NNPI_CHAN_PFN_BITS       45u
#define NNPI_CHAN_PFN_MASK       ((UINT64_C(1) << NNPI_CHAN_PFN_BITS) - 1u)

/* Channel protocol version 1.6.0 = NNP_MAKE_VERSION(1,6,0) = (1<<10)|(6<<5). */
#define NNPI_CHAN_PROTOCOL_VERSION UINT16_C(0x04c0)

/* EVENT_REPORT event_code values that confirm/reject the channel lifecycle.
 * Sources: nnpi-host/.../ipc_c2h_events.h:81-83,118-119. */
#define NNPI_CHAN_EVENT_CREATE_SUCCESS   21u /* EVENT_NON_ERR_DRV_START+17 */
#define NNPI_CHAN_EVENT_CHANNEL_DESTROYED 22u /* EVENT_NON_ERR_DRV_START+18 */
#define NNPI_CHAN_EVENT_SET_RB_SUCCESS   23u /* EVENT_NON_ERR_DRV_START+19 */
#define NNPI_CHAN_EVENT_CREATE_FAILED    70u /* EVENT_ERR_DRV_START+18 */
#define NNPI_CHAN_EVENT_DESTROY_FAILED   71u /* EVENT_ERR_DRV_START+19 */
#define NNPI_CHAN_EVENT_SET_RB_FAILED    72u /* EVENT_ERR_DRV_START+20 */
#define NNPI_CHAN_EVENT_CARD_FATAL_START 112u

enum nnpi_chan_result {
    NNPI_CHAN_OK = 0,
    NNPI_CHAN_INVALID = 1
};

/* h2c_channel_op (one QW): opcode|chan_id<<6|destroy<<16|reserved<<17|
 * privileged<<31|uid<<32. destroy=0 creates, destroy=1 tears the channel down.
 * privileged/uid mirror the Linux host credentials; Windows maps them at the
 * transport layer. */
static inline int nnpi_chan_op_encode(uint16_t chan_id, int destroy,
                                      int privileged, uint32_t uid,
                                      uint64_t *out)
{
    if (!out || chan_id > NNPI_CHAN_ID_MASK)
        return NNPI_CHAN_INVALID;
    *out = (uint64_t)NNPI_CHAN_OP_H2C |
           ((uint64_t)chan_id << 6) |
           (destroy ? UINT64_C(1) << 16 : 0u) |
           (privileged ? UINT64_C(1) << 31 : 0u) |
           ((uint64_t)uid << 32);
    return NNPI_CHAN_OK;
}

/* h2c_channel_data_ringbuf_op (one QW): opcode|chan_id<<6|h2c<<16|rb_id<<17|
 * destroy<<18|host_pfn<<19. host_pfn is the chain header's DMA page-frame
 * number (logical address >> 12), not a data-page PFN. The network's 16 data
 * pages are described by that chain, never directly published here.
 * destroy=1 releases it. rb_id is 0 for the single network channel. */
static inline int nnpi_chan_rb_op_encode(uint16_t chan_id, int h2c,
                                         uint8_t rb_id, int destroy,
                                         uint64_t host_pfn, uint64_t *out)
{
    if (!out || chan_id > NNPI_CHAN_ID_MASK || rb_id > 1u ||
        (host_pfn & ~NNPI_CHAN_PFN_MASK))
        return NNPI_CHAN_INVALID;
    *out = (uint64_t)NNPI_CHAN_RB_OP_H2C |
           ((uint64_t)chan_id << 6) |
           (h2c ? UINT64_C(1) << 16 : 0u) |
           ((uint64_t)rb_id << 17) |
           (destroy ? UINT64_C(1) << 18 : 0u) |
           (host_pfn << 19);
    return NNPI_CHAN_OK;
}

/* c2h_event_report wire layout (nnpi-drv-5.15/ipc_include/ipc_protocol.h:97-105):
 * opcode(6)|event_code(7)<<6|chan_id(8)<<13|obj_id(16)<<21|obj_id_2(16)<<37|
 * event_val(8)<<53|ctx_valid(1)<<61|obj_valid(1)<<62|obj_valid_2(1)<<63.
 * The legacy chan_id member at bits 13-20 is the 8-bit context_id, NOT the
 * non-context channel's protocol_id. obj_id holds the channel protocol_id on
 * CREATE_CHANNEL_SUCCESS. Card SET_RB_SUCCESS/SET_RB_FAILED (23/72) use opcode
 * 4, obj_id=channel (obj_valid=1), obj_id_2=rb_id (obj_valid_2=1), ctx_valid=0.
 * event_val is 0 on success and a nonzero error on failure. No h2c direction,
 * sequence or destroy bit is echoed; two replies with the same tuple cannot
 * identify different operations. */
struct nnpi_chan_event {
    uint8_t opcode;
    uint8_t event_code;
    uint8_t chan_id;
    uint16_t obj_id;
    uint16_t obj_id_2;
    uint8_t event_val;
    uint8_t ctx_valid;
    uint8_t obj_valid;
    uint8_t obj_valid_2;
};

static inline int nnpi_chan_event_decode(uint64_t value,
                                         struct nnpi_chan_event *out)
{
    if (!out)
        return NNPI_CHAN_INVALID;
    out->opcode = (uint8_t)(value & 0x3fu);
    out->event_code = (uint8_t)((value >> 6) & 0x7fu);
    out->chan_id = (uint8_t)((value >> 13) & 0xffu);
    out->obj_id = (uint16_t)((value >> 21) & 0xffffu);
    out->obj_id_2 = (uint16_t)((value >> 37) & 0xffffu);
    out->event_val = (uint8_t)((value >> 53) & 0xffu);
    out->ctx_valid = (uint8_t)((value >> 61) & 1u);
    out->obj_valid = (uint8_t)((value >> 62) & 1u);
    out->obj_valid_2 = (uint8_t)((value >> 63) & 1u);
    return NNPI_CHAN_OK;
}

/* EVENT_REPORT classification for the channel lifecycle. event_code sits at
 * bits 6-12 of the reply QW (7 bits). */
static inline uint8_t nnpi_chan_event_code(uint64_t value)
{
    return (uint8_t)((value >> 6) & 0x7fu);
}

static inline int nnpi_chan_event_create_success(uint8_t event_code)
{
    return event_code == NNPI_CHAN_EVENT_CREATE_SUCCESS;
}

static inline int nnpi_chan_event_channel_destroyed(uint8_t event_code)
{
    return event_code == NNPI_CHAN_EVENT_CHANNEL_DESTROYED;
}

static inline int nnpi_chan_event_set_rb_success(uint8_t event_code)
{
    return event_code == NNPI_CHAN_EVENT_SET_RB_SUCCESS;
}

static inline int nnpi_chan_event_create_failed(uint8_t event_code)
{
    return event_code == NNPI_CHAN_EVENT_CREATE_FAILED;
}

static inline int nnpi_chan_event_destroy_failed(uint8_t event_code)
{
    return event_code == NNPI_CHAN_EVENT_DESTROY_FAILED;
}

static inline int nnpi_chan_event_set_rb_failed(uint8_t event_code)
{
    return event_code == NNPI_CHAN_EVENT_SET_RB_FAILED;
}

static inline int nnpi_chan_event_card_fatal(uint8_t event_code)
{
    return event_code >= NNPI_CHAN_EVENT_CARD_FATAL_START;
}

#endif /* NNPI_CHAN_PROTOCOL_H */
