/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_NET_CONTROL_H
#define NNPI_NET_CONTROL_H
#include "nnpi_boot_session.h"
#include "nnpi_net_session.h"
#include "nnpi_runtime_protocol.h"
/* CPU-only admission and reset-owner evidence. Not an IOCTL ABI. */
enum nnpi_net_gate_reason {
    NNPI_NET_GATE_READY=0, NNPI_NET_GATE_NOT_READY=3,
    NNPI_NET_GATE_CAP_MISSING=4, NNPI_NET_GATE_SESSION_USED=5,
    NNPI_NET_GATE_CLOSING=6, NNPI_NET_GATE_UNEXPECTED_CONFIG=7
};
struct nnpi_net_gate {
    uint32_t compatible, healthy, boot_worker, boot_phase, driver_ready;
    uint32_t closing, reset, failed, started, available;
    uint64_t generation, epoch, boot_generation, boot_epoch;
    const struct nnpi_runtime_message *version;
};
static inline uint32_t nnpi_net_control_gate(const struct nnpi_net_gate *g,
    uint64_t generation,uint64_t epoch,uint32_t channel,uint32_t ip,
    const uint8_t mac[6],int continuing)
{
    unsigned i; uint8_t any=0;
    if (!g) return NNPI_NET_GATE_NOT_READY;
    if (g->closing) return NNPI_NET_GATE_CLOSING;
    if (g->started && !continuing) return NNPI_NET_GATE_SESSION_USED;
    if (!g->available || !g->compatible || !g->healthy || !g->boot_worker ||
        g->boot_phase!=NNPI_BS_DRIVER_READY || !g->driver_ready || g->reset || g->failed ||
        !generation || generation==UINT64_MAX || !epoch || epoch==UINT64_MAX ||
        generation!=g->generation || epoch!=g->epoch ||
        generation!=g->boot_generation || epoch!=g->boot_epoch)
        return NNPI_NET_GATE_NOT_READY;
    if (!g->version || g->version->channel_version!=0x04c0 ||
        g->version->response_sizes[7]!=1 || g->version->command_sizes[25]!=2 ||
        g->version->response_sizes[8]!=1 || g->version->command_sizes[26]!=1)
        return NNPI_NET_GATE_CAP_MISSING;
    if (mac) for (i=0;i<6;++i) any|=mac[i];
    if (!ip || !mac || !any || (mac[0]&1) || (channel && channel!=256))
        return NNPI_NET_GATE_UNEXPECTED_CONFIG;
    return NNPI_NET_GATE_READY;
}
struct nnpi_net_owner {
    struct nnpi_boot_session_key boot;
    struct nnpi_net_session_key net;
    struct nnpi_boot_session_key stopped_boot;
    uint32_t channel_issued, reset_returned, reset_status, boot_stop_accepted, stopped;
};
static inline int nnpi_net_owner_matches(const struct nnpi_net_owner *o,
    const struct nnpi_boot_session *b,const struct nnpi_net_session_key *n)
{
    return o && nnpi_boot_session_matches(b,&o->boot) &&
        nnpi_net_session_key_valid(n) && o->net.session_id==n->session_id &&
        o->net.generation==n->generation && o->net.epoch==n->epoch &&
        o->boot.generation==n->generation && o->boot.epoch==n->epoch;
}
/* Record only the actual boot worker's associated, normally returned reset. */
static inline void nnpi_net_owner_record_stop(struct nnpi_net_owner *o,
    const struct nnpi_boot_session_key *key,uint32_t returned,uint32_t status,
    uint32_t accepted)
{
    if (!o || !key) return;
    o->stopped_boot=*key; o->reset_returned=returned;
    o->reset_status=status; o->boot_stop_accepted=accepted;
}
static inline int nnpi_net_owner_finalize(struct nnpi_net_owner *o,
    const struct nnpi_boot_session *b,const struct nnpi_net_session_key *n,
    uint32_t boot_done,uint32_t net_done,uint32_t published_dma,uint32_t objects_published)
{
    if ((published_dma & ~NNPI_NS_ALL_DMA) ||
        (objects_published && !published_dma) ||
        ((published_dma || objects_published) && (!o || !o->channel_issued)) ||
        !boot_done || !net_done || !nnpi_net_owner_matches(o,b,n) ||
        !nnpi_boot_session_matches(b,&o->stopped_boot) ||
        o->reset_returned!=1 || o->reset_status!=0 || o->boot_stop_accepted!=1 ||
        !b->reset_used || !b->reset_returned || b->phase!=NNPI_BS_FINISHED ||
        !nnpi_boot_session_may_release(b,NNPI_BS_ALL_DMA)) return 0;
    o->stopped=1; return 1;
}
static inline int nnpi_net_owner_may_release(const struct nnpi_net_owner *o)
{ return o && (!o->channel_issued || o->stopped); }
#endif
