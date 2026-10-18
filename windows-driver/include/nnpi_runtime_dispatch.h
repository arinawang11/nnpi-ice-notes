/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RUNTIME_DISPATCH_H
#define NNPI_RUNTIME_DISPATCH_H
#include "nnpi_runtime_protocol.h"
#include "nnpi_net_session.h"
/* Only completed parser frames may be routed. Never inspect payload QW low bits.
 * ETH_CONFIG without a current session is evidence, not a boot failure. */
enum nnpi_runtime_route { NNPI_RUNTIME_ROUTE_VERSION=1, NNPI_RUNTIME_ROUTE_EVENT,
    NNPI_RUNTIME_ROUTE_SYSINFO, NNPI_RUNTIME_ROUTE_UNEXPECTED_NET, NNPI_RUNTIME_ROUTE_DESCRIPTOR, NNPI_RUNTIME_ROUTE_INVALID };
static inline uint64_t nnpi_runtime_saturating_increment(uint64_t value)
{ return value==UINT64_MAX ? value : value+1; }
static inline int nnpi_runtime_route_frame(const struct nnpi_runtime_message *m)
{
    if (!m || !m->qword_count || m->qword_count!=nnpi_runtime_message_qwords(m->opcode) ||
        (m->qwords[0]&UINT64_C(0x3f))!=m->opcode) return NNPI_RUNTIME_ROUTE_INVALID;
    switch (m->opcode) {
    case 0: case 1: case 2: return NNPI_RUNTIME_ROUTE_VERSION;
    case 4: return NNPI_RUNTIME_ROUTE_EVENT;
    case 11: return NNPI_RUNTIME_ROUTE_SYSINFO;
    case 39: return NNPI_RUNTIME_ROUTE_UNEXPECTED_NET;
    case 40: return NNPI_RUNTIME_ROUTE_DESCRIPTOR;
    default: return NNPI_RUNTIME_ROUTE_INVALID;
    }
}
enum nnpi_runtime_net_result { NNPI_RUNTIME_NET_IGNORED=0,
    NNPI_RUNTIME_NET_OBSERVED=1, NNPI_RUNTIME_NET_FAILED=2,
    NNPI_RUNTIME_NET_UNEXPECTED=3, NNPI_RUNTIME_NET_FATAL=4 };
static inline int nnpi_runtime_dispatch_net(const struct nnpi_runtime_message *m,
    struct nnpi_net_session *s,const struct nnpi_net_session_key *key,int current)
{
    struct nnpi_chan_event ev;
    int route=nnpi_runtime_route_frame(m);
    if (route==NNPI_RUNTIME_ROUTE_EVENT) {
        if (nnpi_chan_event_decode(m->qwords[0],&ev)!=NNPI_CHAN_OK ||
            nnpi_chan_event_card_fatal(ev.event_code)) return NNPI_RUNTIME_NET_FATAL;
    } else if (route!=NNPI_RUNTIME_ROUTE_UNEXPECTED_NET) return NNPI_RUNTIME_NET_IGNORED;
    if (!current || !s || !nnpi_net_session_matches(s,key))
        return route==NNPI_RUNTIME_ROUTE_UNEXPECTED_NET ?
            NNPI_RUNTIME_NET_UNEXPECTED : NNPI_RUNTIME_NET_IGNORED;
    if (s->invalidated) return NNPI_RUNTIME_NET_IGNORED;
    /* A local 39 outside WAIT_ETH is also a protocol error, not success. */
    if (route==NNPI_RUNTIME_ROUTE_UNEXPECTED_NET && s->phase!=NNPI_NS_WAIT_ETH)
        nnpi_net_session_abort(s);
    else (void)nnpi_net_session_observe(s,key,s->last_read+1,m->qwords[0]);
    return nnpi_net_session_failed(s) ? NNPI_RUNTIME_NET_FAILED : NNPI_RUNTIME_NET_OBSERVED;
}
/* Error/health gate wins over a matching cached phase. */
static inline int nnpi_runtime_net_wait_ready(uint32_t healthy,uint32_t failure,
                                             uint32_t phase,uint32_t desired)
{ return healthy && !failure && phase==desired; }
#endif
