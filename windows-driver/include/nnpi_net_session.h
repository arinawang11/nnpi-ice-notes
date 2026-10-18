/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_NET_SESSION_H
#define NNPI_NET_SESSION_H
#include "nnpi_chan_protocol.h"
#include "nnpi_eth_protocol.h"
#include "nnpi_fifo_protocol.h"
#include <string.h>

/* Production-shared POLICY ONLY for the 027 minimal PCIe-Ethernet channel
 * lifecycle. No MMIO, DMA, allocation, waits or simulated completion. The
 * owner serializes all calls with PnP/D0, supplies fresh command-FIFO control
 * snapshots, strictly increasing read serials, and DMA page-frame numbers from
 * this device's DMA API. This layer only PLANS command pushes and CLASSIFIES
 * the C2H replies that confirm each step; it never writes a register or reads
 * a FIFO, and never claims SSH/network success.
 *
 * Lifecycle (Linux reference: nnpi-drv-5.15/device_chardev.c:186-412):
 *   CHANNEL_OP(22,create) -> CREATE_CHANNEL_SUCCESS(21)  (obj_id == chan_id)
 *   CHANNEL_RB_OP(23,h2c) -> CHANNEL_SET_RB_SUCCESS(23)
 *   CHANNEL_RB_OP(23,c2h) -> CHANNEL_SET_RB_SUCCESS(23)
 *   ETH_CONFIG(57)         -> C2H ETH_CONFIG(39) card_ip echo (0 = rejected)
 * After READY the owner moves Ethernet frames with nnpi_eth_protocol.h; this
 * session does not carry those frames or their ACK recycling.
 *
 * Phase advance mirrors nnpi_boot_session.h: a PLAN step moves READY->TX and
 * produces the FIFO plan; sent() moves TX->WAIT; observe() moves WAIT->READY.
 * One channel, one H2C pool, one C2H pool (16 x 4096 bytes each), rb_id 0. The
 * non-context channel id is owner-assigned in 256..1023. uid/privileged mirror
 * the Linux CHANNEL_OP credential fields; Windows maps them at the transport.
 */

#define NNPI_NS_NEW          0U
#define NNPI_NS_INITIAL      1U
#define NNPI_NS_CHAN_TX      2U
#define NNPI_NS_WAIT_CHAN    3U
#define NNPI_NS_CHAN_READY   4U
#define NNPI_NS_H2C_RB_TX    5U
#define NNPI_NS_WAIT_H2C_RB  6U
#define NNPI_NS_H2C_RB_READY 7U
#define NNPI_NS_C2H_RB_TX    8U
#define NNPI_NS_WAIT_C2H_RB  9U
#define NNPI_NS_C2H_RB_READY 10U
#define NNPI_NS_ETH_TX       11U
#define NNPI_NS_WAIT_ETH     12U
#define NNPI_NS_READY        13U
#define NNPI_NS_CLEANUP      14U
#define NNPI_NS_FINISHED     15U

/* Published DMA resources: H2C/C2H page pools, released only after stop. */
#define NNPI_NS_H2C_POOL 1U
#define NNPI_NS_C2H_POOL 2U
#define NNPI_NS_ALL_DMA  3U
#define NNPI_NS_OBSERVATION_LIMIT 120U

struct nnpi_net_session_key { uint64_t session_id, generation, epoch; };

struct nnpi_net_session {
    struct nnpi_net_session_key key;
    uint64_t last_read;
    uint16_t chan_id;
    uint32_t phase;
    uint32_t invalidated;
    uint32_t chan_created, h2c_rb_published, c2h_rb_published, eth_configured;
    uint32_t published_dma, stopped_dma;
    uint32_t observations;
    uint32_t pending_qwords; /* expected send size for the in-flight TX step */
    uint32_t card_ip;        /* configured card IP (s_addr), for echo check */
    uint8_t  mac[6];         /* configured card MAC, for replay check */
};

static inline void nnpi_net_session_init(struct nnpi_net_session *s)
{
    if (s) memset(s, 0, sizeof(*s));
}

static inline int nnpi_net_session_key_valid(const struct nnpi_net_session_key *k)
{
    return k && k->session_id && k->session_id != UINT64_MAX &&
        k->generation && k->generation != UINT64_MAX &&
        k->epoch && k->epoch != UINT64_MAX;
}

static inline int nnpi_net_session_matches(const struct nnpi_net_session *s,
                                           const struct nnpi_net_session_key *k)
{
    return s && nnpi_net_session_key_valid(k) &&
        s->key.session_id == k->session_id && s->key.generation == k->generation &&
        s->key.epoch == k->epoch;
}

/* Begin a new lifecycle. chan_id is owner-assigned (non-context 256..1023).
 * read_serial is the owner's strictly-increasing observation token. */
static inline int nnpi_net_session_begin(struct nnpi_net_session *s,
                                         const struct nnpi_net_session_key *k,
                                         uint64_t read_serial, uint16_t chan_id)
{
    if (!s || s->phase != NNPI_NS_NEW || !nnpi_net_session_key_valid(k) ||
        !read_serial || read_serial == UINT64_MAX ||
        chan_id < NNPI_CHAN_MIN_NONCTX || chan_id > NNPI_CHAN_ID_MASK)
        return 0;
    s->key = *k;
    s->last_read = read_serial;
    s->chan_id = chan_id;
    s->phase = NNPI_NS_INITIAL;
    return 1;
}

/* Abort is an ownership-independent terminal latch. Never frees DMA or resets
 * budgets; the owner decides release via may_release. */
static inline void nnpi_net_session_abort(struct nnpi_net_session *s)
{
    if (!s || s->phase == NNPI_NS_NEW || s->phase == NNPI_NS_FINISHED) return;
    s->invalidated = 1;
    s->phase = NNPI_NS_CLEANUP;
}

static inline int nnpi_net_session_failed(const struct nnpi_net_session *s)
{
    return s && (s->invalidated || s->phase == NNPI_NS_CLEANUP);
}

/* Little-endian QW serialization; the transport owns byte order on the wire. */
static inline void nnpi_net_le64(uint64_t v, uint8_t out[8])
{
    unsigned i;
    for (i = 0; i < 8; ++i)
        out[i] = (uint8_t)(v >> (8u * i));
}

/* Plan the CHANNEL_OP(22) create push (1 QW). INITIAL -> CHAN_TX. A plan is NOT
 * a send. uid/privileged mirror Linux CAP_SYS_ADMIN/euid at the transport. */
static inline int nnpi_net_session_chan_op(struct nnpi_net_session *s,
    const struct nnpi_net_session_key *k, uint64_t read_serial,
    uint32_t command_snapshot, uint32_t uid, int privileged,
    struct nnpi_fifo_command_plan *out)
{
    uint64_t qw;
    uint8_t wire[8];
    if (!nnpi_net_session_matches(s, k) || s->phase != NNPI_NS_INITIAL ||
        s->invalidated || !out || read_serial <= s->last_read ||
        read_serial == UINT64_MAX)
        return 0;
    if (nnpi_chan_op_encode(s->chan_id, 0, privileged, uid, &qw) != NNPI_CHAN_OK) {
        nnpi_net_session_abort(s);
        return 0;
    }
    nnpi_net_le64(qw, wire);
    if (nnpi_fifo_plan_command(command_snapshot, wire, 8, out) != NNPI_FIFO_OK) {
        nnpi_net_session_abort(s);
        return 0;
    }
    s->last_read = read_serial;
    s->pending_qwords = 1;
    s->phase = NNPI_NS_CHAN_TX;
    return 1;
}

/* Plan the CHANNEL_RB_OP(23) push (1 QW) for one direction (h2c=1 host->card
 * pool, h2c=0 card->host pool), rb_id 0. host_pfn is the chain-header DMA
 * logical address >> 12; the 16 data pages are described by the chain, not
 * directly published as the command PFN.
 * CHAN_READY -> H2C_RB_TX (h2c) or H2C_RB_READY -> C2H_RB_TX (!h2c). */
static inline int nnpi_net_session_rb_op(struct nnpi_net_session *s,
    const struct nnpi_net_session_key *k, uint64_t read_serial,
    uint32_t command_snapshot, int h2c, uint64_t host_pfn,
    struct nnpi_fifo_command_plan *out)
{
    uint64_t qw;
    uint8_t wire[8];
    uint32_t want = h2c ? NNPI_NS_CHAN_READY : NNPI_NS_H2C_RB_READY;
    if (!nnpi_net_session_matches(s, k) || s->phase != want || s->invalidated ||
        !out || read_serial <= s->last_read || read_serial == UINT64_MAX)
        return 0;
    if (nnpi_chan_rb_op_encode(s->chan_id, h2c, 0, 0, host_pfn, &qw) != NNPI_CHAN_OK) {
        nnpi_net_session_abort(s);
        return 0;
    }
    nnpi_net_le64(qw, wire);
    if (nnpi_fifo_plan_command(command_snapshot, wire, 8, out) != NNPI_FIFO_OK) {
        nnpi_net_session_abort(s);
        return 0;
    }
    s->last_read = read_serial;
    s->pending_qwords = 1;
    s->published_dma |= h2c ? NNPI_NS_H2C_POOL : NNPI_NS_C2H_POOL;
    s->phase = h2c ? NNPI_NS_H2C_RB_TX : NNPI_NS_C2H_RB_TX;
    return 1;
}

/* Plan the ETH_CONFIG(57) push (2 QW): card_ip and MAC. Stored for the echo
 * check against the C2H39 reply. C2H_RB_READY -> ETH_TX. */
static inline int nnpi_net_session_eth_config(struct nnpi_net_session *s,
    const struct nnpi_net_session_key *k, uint64_t read_serial,
    uint32_t command_snapshot, uint32_t card_ip, const uint8_t mac[6],
    struct nnpi_fifo_command_plan *out)
{
    uint64_t qw0, qw1;
    uint8_t wire[16];
    if (!nnpi_net_session_matches(s, k) || s->phase != NNPI_NS_C2H_RB_READY ||
        s->invalidated || !out || !mac || read_serial <= s->last_read ||
        read_serial == UINT64_MAX)
        return 0;
    if (nnpi_eth_config_encode(s->chan_id, card_ip, mac, &qw0, &qw1) != NNPI_ETH_OK) {
        nnpi_net_session_abort(s);
        return 0;
    }
    nnpi_net_le64(qw0, wire);
    nnpi_net_le64(qw1, wire + 8);
    if (nnpi_fifo_plan_command(command_snapshot, wire, 16, out) != NNPI_FIFO_OK) {
        nnpi_net_session_abort(s);
        return 0;
    }
    s->last_read = read_serial;
    s->pending_qwords = 2;
    s->card_ip = card_ip;
    memcpy(s->mac, mac, 6);
    s->phase = NNPI_NS_ETH_TX;
    return 1;
}

/* Owner has executed the in-flight plan. halves_completed must equal the
 * pending QW count times two (low+high per QW); a partial send is an abort. */
static inline int nnpi_net_session_sent(struct nnpi_net_session *s,
    const struct nnpi_net_session_key *k, uint32_t halves_completed)
{
    if (!nnpi_net_session_matches(s, k) || s->invalidated) return 0;
    if (halves_completed != s->pending_qwords * 2u) {
        nnpi_net_session_abort(s);
        return 0;
    }
    switch (s->phase) {
    case NNPI_NS_CHAN_TX:   s->phase = NNPI_NS_WAIT_CHAN;   break;
    case NNPI_NS_H2C_RB_TX: s->phase = NNPI_NS_WAIT_H2C_RB; break;
    case NNPI_NS_C2H_RB_TX: s->phase = NNPI_NS_WAIT_C2H_RB; break;
    case NNPI_NS_ETH_TX:    s->phase = NNPI_NS_WAIT_ETH;    break;
    default: nnpi_net_session_abort(s); return 0;
    }
    s->observations = 0;
    return 1;
}

/* Classify a C2H EVENT_REPORT(4) against the current waiting phase. Returns 1
 * on an accepted transition, 0 otherwise. A card-fatal event aborts globally.
 * SET_RB replies echo channel/rb_id only, NOT direction, sequence or destroy.
 * The owner must serialize operations on the same channel. No pending WAIT
 * means no SET_RB phase advance; a late ACK with the same tuple during a later
 * WAIT cannot be distinguished from that WAIT's ACK by this wire protocol. */
static inline int nnpi_net_session_observe_event(struct nnpi_net_session *s,
    const struct nnpi_net_session_key *k, uint64_t read_serial, uint64_t qword)
{
    struct nnpi_chan_event ev;
    if (!nnpi_net_session_matches(s, k) || s->invalidated ||
        !read_serial || read_serial <= s->last_read ||
        (qword & 0x3fu) != NNPI_ETH_C2H_EVENT)
        return 0;
    s->last_read = read_serial;
    ++s->observations;
    if (nnpi_chan_event_decode(qword, &ev) != NNPI_CHAN_OK) {
        nnpi_net_session_abort(s);
        return 0;
    }
    if (nnpi_chan_event_card_fatal(ev.event_code)) {
        nnpi_net_session_abort(s);
        return 0;
    }
    switch (s->phase) {
    case NNPI_NS_WAIT_CHAN:
        if (nnpi_chan_event_create_failed(ev.event_code)) {
            nnpi_net_session_abort(s); return 0;
        }
        if (nnpi_chan_event_create_success(ev.event_code) &&
            ev.obj_valid && ev.obj_id == s->chan_id) {
            s->chan_created = 1;
            s->phase = NNPI_NS_CHAN_READY;
            s->observations = 0;
            return 1;
        }
        break;
    case NNPI_NS_WAIT_H2C_RB:
    case NNPI_NS_WAIT_C2H_RB:
        if (nnpi_chan_event_set_rb_success(ev.event_code) ||
            nnpi_chan_event_set_rb_failed(ev.event_code)) {
            /* A valid foreign identity belongs to another operation, even if
             * the rest of that event is malformed. Do not fail this session. */
            if ((ev.obj_valid && ev.obj_id != s->chan_id) ||
                (ev.obj_valid_2 && ev.obj_id_2 != 0))
                break;
            /* Fail closed on missing identities or malformed local ACKs.
             * SET_RB_FAILED accepts any nonzero card error; zero is malformed
             * but also rejected. Neither kind of failure advances a phase. */
            if (!ev.obj_valid || !ev.obj_valid_2 || ev.ctx_valid ||
                ev.event_val != 0 || nnpi_chan_event_set_rb_failed(ev.event_code)) {
                nnpi_net_session_abort(s); return 0;
            }
            if (s->phase == NNPI_NS_WAIT_H2C_RB) {
                s->h2c_rb_published = 1;
                s->phase = NNPI_NS_H2C_RB_READY;
            } else {
                s->c2h_rb_published = 1;
                s->phase = NNPI_NS_C2H_RB_READY;
            }
            s->observations = 0;
            return 1;
        }
        break;
    default:
        break;
    }
    if (s->observations >= NNPI_NS_OBSERVATION_LIMIT)
        nnpi_net_session_abort(s);
    return 0;
}

/* Classify the C2H ETH_CONFIG(39) reply: card_ip echo, 0 means rejected. */
static inline int nnpi_net_session_observe_eth(struct nnpi_net_session *s,
    const struct nnpi_net_session_key *k, uint64_t read_serial, uint64_t qword)
{
    uint8_t opcode; uint16_t chan; uint32_t card_ip;
    if (!nnpi_net_session_matches(s, k) || s->invalidated ||
        s->phase != NNPI_NS_WAIT_ETH ||
        !read_serial || read_serial <= s->last_read)
        return 0;
    s->last_read = read_serial;
    ++s->observations;
    if (nnpi_eth_config_decode(qword, &opcode, &chan, &card_ip) != NNPI_ETH_OK ||
        opcode != NNPI_ETH_C2H_CONFIG || chan != s->chan_id) {
        nnpi_net_session_abort(s);
        return 0;
    }
    if (card_ip == 0) { /* card rejected the setup */
        nnpi_net_session_abort(s);
        return 0;
    }
    if (card_ip != s->card_ip) { /* echo mismatch is a protocol error */
        nnpi_net_session_abort(s);
        return 0;
    }
    s->eth_configured = 1;
    s->phase = NNPI_NS_READY;
    s->observations = 0;
    return 1;
}

/* Route one decoded C2H qword (already read from the response FIFO) by opcode.
 * EVENT_REPORT(4) and ETH_CONFIG(39) advance the lifecycle; other opcodes are
 * ignored here (they are Ethernet data frames, handled outside this session). */
static inline int nnpi_net_session_observe(struct nnpi_net_session *s,
    const struct nnpi_net_session_key *k, uint64_t read_serial, uint64_t qword)
{
    uint8_t opcode;
    if (!nnpi_net_session_matches(s, k) || s->invalidated) return 0;
    opcode = (uint8_t)(qword & 0x3fu);
    if (opcode == NNPI_ETH_C2H_EVENT)
        return nnpi_net_session_observe_event(s, k, read_serial, qword);
    if (opcode == NNPI_ETH_C2H_CONFIG)
        return nnpi_net_session_observe_eth(s, k, read_serial, qword);
    return 0;
}

static inline int nnpi_net_session_ready(const struct nnpi_net_session *s)
{
    return s && s->phase == NNPI_NS_READY && s->chan_created &&
        s->h2c_rb_published && s->c2h_rb_published && s->eth_configured &&
        !s->invalidated;
}

/* Owner has device-stop evidence (reset complete / channel destroy confirmed)
 * for the named published pools. Marks them stopped so may_release can pass. */
static inline int nnpi_net_session_stopped(struct nnpi_net_session *s,
                                           uint32_t resources)
{
    if (!s || !resources || (resources & ~NNPI_NS_ALL_DMA)) return 0;
    s->stopped_dma |= resources;
    return 1;
}

/* Each resource is evaluated independently; published pools require an
 * associated device stop. READY is NOT sufficient to release a pool. */
static inline int nnpi_net_session_may_release(const struct nnpi_net_session *s,
                                               uint32_t resources)
{
    return s && resources && !(resources & ~NNPI_NS_ALL_DMA) &&
        !(s->published_dma & resources & ~s->stopped_dma);
}

#endif /* NNPI_NET_SESSION_H */
