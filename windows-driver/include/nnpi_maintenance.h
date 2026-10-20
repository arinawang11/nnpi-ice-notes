/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_MAINTENANCE_H
#define NNPI_MAINTENANCE_H
#include "nnpi_maintenance_abi.h"
/* Caller serializes on the same lock as every external submission. */
static __inline int nnpi_maintenance_closed(const struct nnpi_maintenance_response *s)
{ return s->state!=NNPI_MAINT_IDLE; }
static __inline int nnpi_maintenance_prepare(struct nnpi_maintenance_response *s,
 const struct nnpi_maintenance_request *r,uint64_t session,uint64_t generation,uint64_t epoch)
{
 if(r->version!=NNPI_MAINTENANCE_VERSION || r->size!=sizeof(*r) ||
    r->reserved[0] || r->reserved[1] || !r->attempt || r->attempt==UINT64_MAX ||
    !generation || !epoch || generation==UINT64_MAX || epoch==UINT64_MAX ||
    r->session!=session || r->generation!=generation || r->epoch!=epoch) return 0;
 if(nnpi_maintenance_closed(s)) return s->session==r->session &&
    s->generation==r->generation && s->epoch==r->epoch && s->attempt==r->attempt;
 s->version=NNPI_MAINTENANCE_VERSION; s->size=sizeof(*s);
 s->session=session; s->generation=generation; s->epoch=epoch;
 s->attempt=r->attempt; s->state=NNPI_MAINT_DRAINING;
 return 1;
}
static __inline int nnpi_maintenance_valid(const struct nnpi_maintenance_response *s)
{
 return s->version==NNPI_MAINTENANCE_VERSION && s->size==sizeof(*s) &&
 s->generation && s->generation!=UINT64_MAX && s->epoch && s->epoch!=UINT64_MAX &&
 s->attempt && s->attempt!=UINT64_MAX && s->state==NNPI_MAINT_QUIESCED && !s->reason &&
 s->evidence==NNPI_MAINT_ALL_EVIDENCE && !s->boot_owned && !s->net_owned &&
 !s->bulk_owned && !s->producer_active && !s->provider_retained &&
 !s->physical_reclaim_observed && !s->reserved[0] && !s->reserved[1] &&
 !s->reserved[2] && !s->reserved[3] &&
 ((s->session && s->reset_claims==1 && s->reset_returned==1 && !s->reset_status) ||
 (!s->session && !s->reset_claims && !s->reset_returned && !s->reset_status));
}
#endif
