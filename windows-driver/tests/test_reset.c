/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_reset_policy.h"
/* Model the OS rundown ownership around PRODUCTION transitions. This is not
 * WDF emulation or evidence that an actual provider/hardware reset works. */
struct model { struct nnpi_reset_response r; unsigned refs, calls; int closing; };
static void init(struct model *m) { nnpi_reset_init(&m->r); m->refs=0; m->calls=0; m->closing=0; }
static int submit(struct model *m) {
    if (m->closing || !nnpi_reset_begin(&m->r,7,9,1)) return 0;
    m->refs+=2; ++m->calls; return 1;
}
static void callback(struct model *m, uint32_t status) {
    int release=nnpi_reset_completed(&m->r,status,3);
    if (release) { assert(m->refs); --m->refs; }
}
static void returned(struct model *m, uint32_t status) {
    nnpi_reset_returned(&m->r,status,2); assert(m->refs); --m->refs;
}
int main(void) {
    struct model m;
    struct nnpi_reset_request q={NNPI_RESET_VERSION,sizeof(q),0,0,0,0};
    assert(nnpi_reset_request_valid(&q,sizeof(q),0));
    assert(!nnpi_reset_request_valid(&q,sizeof(q)-1,0));
    assert(!nnpi_reset_request_valid(&q,sizeof(q),1));
    q.generation=7; q.epoch=9; q.confirmation=NNPI_RESET_CONFIRM;
    assert(nnpi_reset_request_valid(&q,sizeof(q),1));
    q.reserved=1; assert(!nnpi_reset_request_valid(&q,sizeof(q),1));
    /* inline, submit reference keeps storage alive after inline completion */
    init(&m); assert(submit(&m)); callback(&m,0); assert(m.refs==1);
    assert(m.r.state==NNPI_RESET_ACTIVE); returned(&m,0); assert(!m.refs);
    assert(m.r.state==NNPI_RESET_COMPLETE && m.r.hardware_blocked);
    assert(nnpi_reset_can_revalidate(&m.r,7,9));
    /* pending success, no inference of success from STATUS_PENDING */
    init(&m); assert(submit(&m)); returned(&m,NNPI_RESET_PENDING);
    assert(m.r.state==NNPI_RESET_ACTIVE && m.refs==1);
    callback(&m,0); assert(m.r.state==NNPI_RESET_COMPLETE && !m.refs);
    /* pending failure */
    init(&m); assert(submit(&m)); returned(&m,NNPI_RESET_PENDING); callback(&m,0xc0000001U);
    assert(m.r.state==NNPI_RESET_FAILED && !nnpi_reset_can_revalidate(&m.r,7,9));
    /* immediate failure DOES NOT prove no future callback; never + unload */
    init(&m); assert(submit(&m)); returned(&m,0xc0000001U); m.closing=1;
    assert(m.refs==1 && !submit(&m));
    /* timeout/cancel observation is deliberately NOT a transition. late callback */
    assert(m.r.hardware_blocked && m.calls==1); callback(&m,0);
    assert(!m.refs && m.r.state==NNPI_RESET_FAILED);
    /* never: unload remains waiting, one future owner retained */
    init(&m); assert(submit(&m)); returned(&m,0); m.closing=1;
    assert(m.refs==1 && m.r.state==NNPI_RESET_ACTIVE);
    /* multiclient/duplicate request shares LOAD gate, no second provider call */
    init(&m); assert(submit(&m)); assert(!submit(&m)); assert(m.calls==1);
    callback(&m,0); callback(&m,0); returned(&m,0);
    assert(!m.refs && m.r.callback_count==2 && m.r.state==NNPI_RESET_FAILED);
    assert(!nnpi_reset_can_revalidate(&m.r,7,9));
    /* D0 exit, Prepare epoch, surprise/remove all invalidate; callback cannot reopen */
    init(&m); assert(submit(&m)); nnpi_reset_invalidate(&m.r);
    returned(&m,NNPI_RESET_PENDING); callback(&m,0);
    assert(m.r.hardware_blocked && !nnpi_reset_can_revalidate(&m.r,7,9));
    assert(!nnpi_reset_can_revalidate(&m.r,8,10));
    /* same epoch required even absent a PnP event */
    init(&m); assert(submit(&m)); returned(&m,0); callback(&m,0);
    assert(!nnpi_reset_can_revalidate(&m.r,8,9)); assert(!nnpi_reset_can_revalidate(&m.r,7,10));
    assert(nnpi_reset_response_valid(&m.r,sizeof(m.r)));
    m.r.reserved=1; assert(!nnpi_reset_response_valid(&m.r,sizeof(m.r)));
    puts("reset ABI/production transitions: inline pending late never error double multiclient epoch remove unload PASS; hardware NOT RUN");
    return 0;
}
