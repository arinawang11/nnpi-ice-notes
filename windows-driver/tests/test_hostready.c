/* SPDX-License-Identifier: GPL-2.0-only */
#include "../include/nnpi_hostready.h"
#include <stdio.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    struct nnpi_hostready_request q;
    struct nnpi_hostready_response r, before;
    uint32_t i, rd, wr;
    size_t n;
    memset(&q,0,sizeof(q)); q.version=1; q.size=sizeof(q); q.generation=1; q.epoch=1; q.confirmation=NNPI_HOSTREADY_CONFIRM;
    CHECK(nnpi_hostready_request_valid(&q,32,64));
    for(n=0;n<96;++n) {
        CHECK(nnpi_hostready_request_valid(&q,n,64)==(n==32));
        CHECK(nnpi_hostready_request_valid(&q,32,n)==(n==64));
    }
    q.reserved=1; CHECK(!nnpi_hostready_request_valid(&q,32,64)); q.reserved=0;
    q.epoch=UINT64_MAX; CHECK(!nnpi_hostready_request_valid(&q,32,64)); q.epoch=1;
    q.confirmation=0; CHECK(!nnpi_hostready_request_valid(&q,32,64));
    for(i=0;i<32;++i) CHECK(!nnpi_hostready_baseline(UINT32_C(1)<<i,0,0,0));
    for(i=0;i<65536;++i) CHECK(nnpi_hostready_baseline(0,i,0,0)==(i<=1));
    for(rd=0;rd<16;++rd) for(wr=0;wr<32;++wr) {
        uint32_t control=rd|(wr<<8);
        CHECK(nnpi_hostready_baseline(0,0,control,0)==(rd==wr));
        CHECK(nnpi_hostready_baseline(0,1,0,control)==(rd==wr));
    }
    memset(&r,0,sizeof(r)); r.version=1; r.size=sizeof(r); r.generation=1; r.epoch=1;
    r.writes=1; r.reads=4; r.phase=NNPI_HR_WRITTEN;
    CHECK(nnpi_hostready_response_valid(&r,sizeof(r)));
    before=r;
    CHECK(!nnpi_hostready_can_observe(&r,1,1,4999999,0));
    CHECK(nnpi_hostready_can_observe(&r,1,1,5000000,0));
    CHECK(!nnpi_hostready_can_observe(&r,2,1,5000000,0));
    CHECK(!nnpi_hostready_can_observe(&r,1,2,5000000,0));
    CHECK(!nnpi_hostready_can_observe(&r,1,1,0,1));
    CHECK(!nnpi_hostready_can_observe(NULL,1,1,5000000,0));
    for(i=1;i<=20;++i) {
        CHECK(nnpi_hostready_observed(&r,0));
        CHECK(r.observations==i && r.reads==4+i);
        CHECK(nnpi_hostready_response_valid(&r,sizeof(r)));
    }
    CHECK(r.phase==NNPI_HR_EXHAUSTED);
    CHECK(!nnpi_hostready_observed(&r,1));
    r=before; CHECK(nnpi_hostready_observed(&r,1)); CHECK(r.phase==NNPI_HR_READY);
    CHECK(nnpi_hostready_response_valid(&r,sizeof(r))); CHECK(!nnpi_hostready_observed(&r,0));
    for(i=2;i<65536;++i) {
        r=before; CHECK(nnpi_hostready_observed(&r,i)); CHECK(r.phase==NNPI_HR_STOPPED);
        CHECK(nnpi_hostready_response_valid(&r,sizeof(r))); CHECK(!nnpi_hostready_observed(&r,0));
    }
    r=before; r.writes=2; CHECK(!nnpi_hostready_response_valid(&r,sizeof(r)));
    r=before; r.reads=0; CHECK(!nnpi_hostready_response_valid(&r,sizeof(r)));
    r=before; r.host_before=9; CHECK(!nnpi_hostready_response_valid(&r,sizeof(r)));
    CHECK(!nnpi_hostready_observed(NULL,0));
    q.confirmation=NNPI_HOSTREADY_CONFIRM;
    r=before; CHECK(nnpi_hostready_reply_matches(&r,sizeof(r),&q,0));
    CHECK(!nnpi_hostready_reply_matches(&r,sizeof(r),&q,1));
    r.epoch=2; CHECK(!nnpi_hostready_reply_matches(&r,sizeof(r),&q,0));
    r=before; r.generation=2; CHECK(!nnpi_hostready_reply_matches(&r,sizeof(r),&q,0));
    r=before; r.epoch=UINT64_MAX; CHECK(!nnpi_hostready_response_valid(&r,sizeof(r)));
    r=before; r.generation=UINT64_MAX; CHECK(!nnpi_hostready_response_valid(&r,sizeof(r)));
    r=before; CHECK(nnpi_hostready_observed(&r,1));
    CHECK(!nnpi_hostready_reply_matches(&r,sizeof(r),&q,0));
    CHECK(nnpi_hostready_reply_matches(&r,sizeof(r),&q,1));
    printf("PASS host-ready: %u checks; offline policy only, no hardware\n",checks);
    return 0;
}
