/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RESET_POLICY_H
#define NNPI_RESET_POLICY_H
#include "nnpi_reset_abi.h"
#include <string.h>
/* Production transitions: caller serializes with a short spinlock. No WDF,
 * request, BAR, allocation, waiting or device pointer belongs in this state. */
static inline void nnpi_reset_init(struct nnpi_reset_response *r)
{
    memset(r,0,sizeof(*r)); r->version=NNPI_RESET_VERSION; r->size=(uint32_t)sizeof(*r);
}
static inline int nnpi_reset_begin(struct nnpi_reset_response *r, uint64_t gen, uint64_t epoch, uint64_t now)
{
    if (r->state!=NNPI_RESET_IDLE || !gen || !epoch) return 0;
    r->state=NNPI_RESET_ACTIVE; r->operation_id=1; r->generation=gen; r->epoch=epoch;
    r->submitted_100ns=now; r->hardware_blocked=1;
    r->call_status=NNPI_RESET_PENDING; r->completion_status=NNPI_RESET_PENDING;
    return 1;
}
static inline void nnpi_reset_finish_state(struct nnpi_reset_response *r)
{
    if (r->callback_count>1) { r->state=NNPI_RESET_FAILED; return; }
    if (!r->call_returned || !r->callback_seen) return;
    r->state=(!(r->call_status & 0x80000000U) && r->call_status!=NNPI_RESET_PENDING &&
        !r->completion_status) || (r->call_status==NNPI_RESET_PENDING && !r->completion_status) ?
        NNPI_RESET_COMPLETE : NNPI_RESET_FAILED;
}
static inline void nnpi_reset_returned(struct nnpi_reset_response *r, uint32_t status, uint64_t now)
{
    r->call_status=status; r->returned_100ns=now; r->call_returned=1;
    nnpi_reset_finish_state(r);
}
/* Returns ownership of exactly the FIRST future-callback rundown release. */
static inline int nnpi_reset_completed(struct nnpi_reset_response *r, uint32_t status, uint64_t now)
{
    if (r->callback_count!=UINT32_MAX) ++r->callback_count;
    if (r->callback_seen) { r->hardware_blocked=1; r->revalidated=0; r->state=NNPI_RESET_FAILED; return 0; }
    r->callback_seen=1; r->completion_status=status; r->completed_100ns=now;
    nnpi_reset_finish_state(r); return 1;
}
static inline void nnpi_reset_invalidate(struct nnpi_reset_response *r)
{
    if (r->state!=NNPI_RESET_IDLE) { r->epoch_invalidated=1; r->hardware_blocked=1; r->revalidated=0; }
}
static inline int nnpi_reset_can_revalidate(const struct nnpi_reset_response *r, uint64_t gen, uint64_t epoch)
{
    return r->state==NNPI_RESET_COMPLETE && r->callback_count==1 && !r->epoch_invalidated &&
        r->generation==gen && r->epoch==epoch && !r->revalidated;
}
#endif
