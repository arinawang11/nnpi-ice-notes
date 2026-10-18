/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_NET_DATA_H
#define NNPI_NET_DATA_H
#include <string.h>
#include "nnpi_eth_protocol.h"
#include "nnpi_net_data_abi.h"
/* CacheLock serializes all state, page copies and command publication. No heap,
 * MMIO or DMA lifetime authority here. RX stores only CPU-owned frame copies. */
struct nnpi_net_data {
    struct nnpi_eth_slots tx;
    uint16_t tx_pending, tx_sent, rx_pending;
    uint32_t rx_head;
    uint16_t tx_lengths[NNPI_ETH_SLOTS], rx_lengths[NNPI_NET_RX_QUEUE_DEPTH];
    uint8_t rx_frames[NNPI_NET_RX_QUEUE_DEPTH][NNPI_ETH_PAGE_BYTES];
    struct nnpi_net_data_response stats;
};
static inline void nnpi_net_data_inc(uint64_t *v) { if (*v!=UINT64_MAX) ++*v; }
static inline uint32_t nnpi_net_data_bits(uint16_t bits)
{ uint32_t n=0; while(bits) { n+=(bits&1u); bits=(uint16_t)(bits>>1); } return n; }
static inline void nnpi_net_data_snapshot(struct nnpi_net_data *d)
{
    d->stats.rx_ack_pending=nnpi_net_data_bits(d->rx_pending);
    d->stats.tx_busy=nnpi_net_data_bits(d->tx.busy);
    d->stats.tx_pending=nnpi_net_data_bits(d->tx_pending);
}
/* Decode/ownership checks happen BEFORE obtaining any page address. Return 1
 * for frame, 2 for ACK, 0 for invalid/stranger, -1 for duplicate owned RX. ACK
 * size must be zero (decode reports size+1==1); ACKs are never frame payloads. */
static inline int nnpi_net_data_classify(struct nnpi_net_data *d,uint64_t qw,
    uint16_t channel,int current,uint16_t *length,uint8_t *slot)
{
    uint8_t opcode,ack; uint16_t chan;
    if (!current || (qw>>37)!=0 ||
        nnpi_eth_descriptor_decode(qw,&opcode,&chan,length,slot,&ack)!=NNPI_ETH_OK ||
        opcode!=NNPI_ETH_C2H_MSG_DSCR || chan!=channel || (ack && *length!=1)) {
        nnpi_net_data_inc(&d->stats.invalid_descriptor); return 0;
    }
    if (ack) {
        if (!(d->tx_sent&(1u<<*slot)) || nnpi_eth_slots_release(&d->tx,*slot)!=NNPI_ETH_OK) {
            nnpi_net_data_inc(&d->stats.unknown_ack); return 0;
        }
        d->tx_sent=(uint16_t)(d->tx_sent&~(1u<<*slot));
        nnpi_net_data_inc(&d->stats.tx_acked); return 2;
    }
    if (d->rx_pending&(1u<<*slot)) {
        nnpi_net_data_inc(&d->stats.duplicate_rx); return -1;
    }
    return 1;
}
/* Caller provides a valid coherent common-buffer page after its acquire barrier.
 * The descriptor is issued by card DMA completion. Copy finishes before setting
 * ACK pending. Bad Ethernet length still returns a valid owned card page: card
 * ACK handler clears busy[handle], ignores size. No pointer used on bad length. */
static inline int nnpi_net_data_receive(struct nnpi_net_data *d,uint8_t slot,
    uint16_t length,const void *page)
{
    if (slot>=NNPI_ETH_SLOTS || (d->rx_pending&(1u<<slot))) return 0;
    if (length<NNPI_ETH_MIN_FRAME || length>NNPI_ETH_PAGE_BYTES) {
        nnpi_net_data_inc(&d->stats.rx_bad_length);
        nnpi_net_data_inc(&d->stats.rx_dropped);
    } else {
        if (!page) return 0;
        nnpi_net_data_inc(&d->stats.rx_received);
        if (UINT64_MAX-d->stats.rx_bytes<length) d->stats.rx_bytes=UINT64_MAX;
        else d->stats.rx_bytes+=length;
        if (d->stats.rx_queued<NNPI_NET_RX_QUEUE_DEPTH) {
            uint32_t tail=(d->rx_head+d->stats.rx_queued)%NNPI_NET_RX_QUEUE_DEPTH;
            memcpy(d->rx_frames[tail],page,length);
            d->rx_lengths[tail]=length; ++d->stats.rx_queued;
        } else {
            /* Bounded CPU queue overflow; the oldest queued frames are kept.
             * Page is consumed/dropped without reading or exposing its payload. */
            nnpi_net_data_inc(&d->stats.rx_dropped);
        }
    }
    d->rx_pending=(uint16_t)(d->rx_pending|(1u<<slot)); return 1;
}
/* Caller validates identity, ownership and both buffer lengths before this
 * destructive copy. Output must be zeroed first; never revisit ACKed DMA pages. */
static inline void nnpi_net_data_dequeue(struct nnpi_net_data *d,uint32_t max_frames,
    struct nnpi_net_rx_batch_response *out)
{
    uint32_t i,n=d->stats.rx_queued;
    if (n>max_frames) n=max_frames;
    if (n>NNPI_NET_RX_QUEUE_DEPTH) n=NNPI_NET_RX_QUEUE_DEPTH;
    for (i=0;i<n;++i) {
        uint32_t head=d->rx_head;
        out->frames[i].length=d->rx_lengths[head];
        memcpy(out->frames[i].frame,d->rx_frames[head],d->rx_lengths[head]);
        memset(d->rx_frames[head],0,sizeof(d->rx_frames[head]));
        d->rx_lengths[head]=0;
        d->rx_head=(head+1u)%NNPI_NET_RX_QUEUE_DEPTH;
        --d->stats.rx_queued;
    }
    out->count=n; out->remaining=d->stats.rx_queued;
}
static inline int nnpi_net_data_next(struct nnpi_net_data *d,uint16_t channel,
    uint64_t *qw,uint8_t *slot,int *ack)
{
    unsigned i; uint16_t bits=d->rx_pending ? d->rx_pending:d->tx_pending;
    *ack=d->rx_pending!=0;
    for(i=0;i<NNPI_ETH_SLOTS;++i) if (bits&(1u<<i)) {
        *slot=(uint8_t)i;
        return nnpi_eth_descriptor_encode(channel,*slot,d->tx_lengths[i],*ack,qw)==NNPI_ETH_OK;
    }
    return 0;
}
/* Called only after BOTH halves succeeded. Failed/ambiguous MMIO never clears
 * pending or outstanding; driver latches fault-retained and never replays. */
static inline void nnpi_net_data_sent(struct nnpi_net_data *d,uint8_t slot,int ack)
{
    if (ack) {
        d->rx_pending=(uint16_t)(d->rx_pending&~(1u<<slot));
        nnpi_net_data_inc(&d->stats.rx_ack_sent);
    } else {
        d->tx_pending=(uint16_t)(d->tx_pending&~(1u<<slot));
        d->tx_sent=(uint16_t)(d->tx_sent|(1u<<slot));
        nnpi_net_data_inc(&d->stats.tx_submitted);
    }
}
#endif
