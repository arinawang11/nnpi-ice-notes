/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_ETH_PROTOCOL_H
#define NNPI_ETH_PROTOCOL_H

#include <stdint.h>

/* Pure, bounded, numeric-QW Ethernet-over-PCIe protocol codec. No MMIO, ACK
 * replay, allocation, channel creation or SSH success claim. Qwords below are
 * host numeric values; the transport serializes little-endian bytes.
 *
 * Sources (relative to /home/arina/nnpi):
 * diag1150-net/nnpi-tap.cpp - standalone TAP client, the authoritative wire
 *   layout via its static_assert/self-test (H2C 57/58, C2H 39/40, page slots,
 *   ACK recycling, card-fatal events);
 * nnpi-host/src/nnpi_drv/include/nnpi_ipc/ipc_chan_protocol.h:323-465
 *   (union h2c/c2h ChanEthernetConfig / ChanEthernetMsgDscr bit fields);
 * nnpi-card-update/.../sph_cs/sphcs_chan_net.c - card-side handler: ETH_CONFIG
 *   replies with card_ip echo (0 on failure), ETH_MSG_DSCR ACKs every consumed
 *   page, pages are a random-access pool NOT a FIFO ring.
 */

/* Channel opcodes. opcode occupies bits 0-5 of every message QW. */
#define NNPI_ETH_H2C_CONFIG   57u /* NNP_IPC_H2C_OP_CHAN_ETH_CONFIG */
#define NNPI_ETH_H2C_MSG_DSCR 58u /* NNP_IPC_H2C_OP_CHAN_ETH_MSG_DSCR */
#define NNPI_ETH_C2H_CONFIG   39u /* NNP_IPC_C2H_OP_CHAN_ETH_CONFIG */
#define NNPI_ETH_C2H_MSG_DSCR 40u /* NNP_IPC_C2H_OP_CHAN_ETH_MSG_DSCR */
#define NNPI_ETH_C2H_EVENT     4u /* NNP_IPC_C2H_OP_EVENT_REPORT */

/* Geometry and field bounds. */
#define NNPI_ETH_PAGE_BYTES     4096u
#define NNPI_ETH_SLOTS          16u
#define NNPI_ETH_MIN_FRAME      14u /* Ethernet header, no FCS */
#define NNPI_ETH_CHANNEL_BITS   10u
#define NNPI_ETH_CHANNEL_MASK   0x3ffu
#define NNPI_ETH_SIZE_BITS      12u
#define NNPI_ETH_SIZE_MASK      0xfffu
#define NNPI_ETH_SLOT_BITS      8u
#define NNPI_ETH_SLOT_MASK      0xffu
#define NNPI_ETH_EVENT_FATAL_START 112u /* EVENT_CARD_FATAL_START */

enum nnpi_eth_result {
    NNPI_ETH_OK = 0,
    NNPI_ETH_INVALID = 1
};

/* h2c_ChanEthernetConfig is 16 bytes (2 QW): QW0 = opcode|chan<<6|reserved<<16
 * |card_ip<<32; QW1 = reserved2|mac[0]<<16|mac[1]<<24|...|mac[5]<<56. card_ip is
 * the inet_pton s_addr value, i.e. low IP byte lives in the low byte (card
 * convertIp reads the low byte first). MAC lands at wire bytes 10-15. */
static inline int nnpi_eth_config_encode(uint16_t chan, uint32_t card_ip,
                                         const uint8_t mac[6],
                                         uint64_t *qw0, uint64_t *qw1)
{
    if (!mac || !qw0 || !qw1 || chan > NNPI_ETH_CHANNEL_MASK)
        return NNPI_ETH_INVALID;
    *qw0 = (uint64_t)NNPI_ETH_H2C_CONFIG |
           ((uint64_t)chan << 6) |
           ((uint64_t)card_ip << 32);
    *qw1 = (uint64_t)mac[0] << 16 | (uint64_t)mac[1] << 24 |
           (uint64_t)mac[2] << 32 | (uint64_t)mac[3] << 40 |
           (uint64_t)mac[4] << 48 | (uint64_t)mac[5] << 56;
    return NNPI_ETH_OK;
}

/* h2c_ChanEthernetMsgDscr (host->card) is one QW: opcode|chan<<6|size<<16|
 * skb_handle<<28|is_ack<<36. size is length-1 for a frame, 0 for an ACK.
 * Non-ACK frames must be an Ethernet header up to one page. */
static inline int nnpi_eth_descriptor_encode(uint16_t chan, uint8_t slot,
                                             uint16_t length, int is_ack,
                                             uint64_t *out)
{
    uint64_t size;
    if (!out || chan > NNPI_ETH_CHANNEL_MASK || slot >= NNPI_ETH_SLOTS)
        return NNPI_ETH_INVALID;
    if (!is_ack && (length < NNPI_ETH_MIN_FRAME || length > NNPI_ETH_PAGE_BYTES))
        return NNPI_ETH_INVALID;
    size = is_ack ? 0u : (uint64_t)(length - 1u);
    *out = (uint64_t)NNPI_ETH_H2C_MSG_DSCR |
           ((uint64_t)chan << 6) |
           (size << 16) |
           ((uint64_t)slot << 28) |
           (is_ack ? UINT64_C(1) << 36 : 0u);
    return NNPI_ETH_OK;
}

/* Decode a c2h_ChanEthernetConfig reply (one QW): opcode|chan<<6|reserved<<16
 * |card_ip<<32. card_ip is 0 when the card rejected the setup. */
static inline int nnpi_eth_config_decode(uint64_t value, uint8_t *opcode,
                                         uint16_t *chan, uint32_t *card_ip)
{
    if (!opcode || !chan || !card_ip)
        return NNPI_ETH_INVALID;
    *opcode = (uint8_t)(value & 0x3fu);
    *chan = (uint16_t)((value >> 6) & NNPI_ETH_CHANNEL_MASK);
    *card_ip = (uint32_t)(value >> 32);
    return NNPI_ETH_OK;
}

/* Decode a c2h_ChanEthernetMsgDscr (one QW). *length is size+1. slot bounds are
 * the one structural error here: an out-of-range slot cannot address a page.
 * Frame length range is left to the caller (a bad length still ACKs the page).
 * The opcode is reported but not enforced so the caller can route 39 vs 40. */
static inline int nnpi_eth_descriptor_decode(uint64_t value, uint8_t *opcode,
                                             uint16_t *chan, uint16_t *length,
                                             uint8_t *slot, uint8_t *is_ack)
{
    if (!opcode || !chan || !length || !slot || !is_ack)
        return NNPI_ETH_INVALID;
    *opcode = (uint8_t)(value & 0x3fu);
    *chan = (uint16_t)((value >> 6) & NNPI_ETH_CHANNEL_MASK);
    *length = (uint16_t)(((value >> 16) & NNPI_ETH_SIZE_MASK) + 1u);
    *slot = (uint8_t)((value >> 28) & NNPI_ETH_SLOT_MASK);
    *is_ack = (uint8_t)((value >> 36) & 1u);
    if (*slot >= NNPI_ETH_SLOTS)
        return NNPI_ETH_INVALID;
    return NNPI_ETH_OK;
}

/* c2h_event_report: opcode(6)|event_code(7)<<6|chan_id(8)<<13|obj_id(16)<<21|
 * obj_id_2(16)<<37|event_val(8)<<53|ctx_valid(1)<<61|obj_valid(1)<<62|
 * obj_valid_2(1)<<63. The field at bits 13-20 is 8 bits (context_id in the
 * header, chan_id for channel events). Only the fatal classification and event
 * code are needed for a non-inference channel. */
static inline uint8_t nnpi_eth_event_code(uint64_t value)
{
    return (uint8_t)((value >> 6) & 0x7fu);
}

static inline int nnpi_eth_event_fatal(uint8_t event_code)
{
    return event_code >= NNPI_ETH_EVENT_FATAL_START;
}

/* Random-access page slot pool (16 slots). ACK recycles a slot; a slot is not
 * reusable until its matching C2H40 is_ack=1. */
struct nnpi_eth_slots {
    uint16_t busy; /* bit i set = slot i outstanding */
};

static inline void nnpi_eth_slots_init(struct nnpi_eth_slots *s)
{
    if (s)
        s->busy = 0u;
}

static inline int nnpi_eth_slots_reserve(struct nnpi_eth_slots *s)
{
    unsigned i;
    if (!s)
        return -1;
    for (i = 0; i < NNPI_ETH_SLOTS; ++i)
        if (!(s->busy & (1u << i))) {
            s->busy = (uint16_t)(s->busy | (1u << i));
            return (int)i;
        }
    return -1;
}

static inline int nnpi_eth_slots_release(struct nnpi_eth_slots *s, unsigned slot)
{
    if (!s || slot >= NNPI_ETH_SLOTS || !(s->busy & (1u << slot)))
        return NNPI_ETH_INVALID;
    s->busy = (uint16_t)(s->busy & ~(1u << slot));
    return NNPI_ETH_OK;
}

static inline int nnpi_eth_slots_available(const struct nnpi_eth_slots *s)
{
    return s && s->busy != UINT16_C(0xffff);
}

#endif /* NNPI_ETH_PROTOCOL_H */
