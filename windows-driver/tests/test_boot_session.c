#include <stdio.h>
#include <stdlib.h>
#include "../include/nnpi_boot_session.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static const struct nnpi_boot_session_key key={26,2,3};
static void payload(uint8_t *p)
{
    unsigned i;
    memset(p,0,NNPI_SYSINFO_PAYLOAD_BYTES); p[0]=1;
    for (i=0;i<32;++i) p[25+2*i]='A';
    p[41]='.'; p[49]='.'; p[59]='.'; p[67]='.';
}
static void started(struct nnpi_boot_session *s,uint32_t initial)
{
    nnpi_boot_session_init(s);
    CHECK(nnpi_boot_session_begin(s,&key,1,0,initial,0,0));
    CHECK(!nnpi_boot_session_hostready(s,&key));
    CHECK(nnpi_boot_session_reserve_cleanup(s,&key));
    CHECK(!nnpi_boot_session_reserve_cleanup(s,&key));
    CHECK(nnpi_boot_session_hostready(s,&key));
    CHECK(!nnpi_boot_session_hostready(s,&key));
    CHECK(!nnpi_boot_session_observe(s,&key,1,1,NULL,0));
    CHECK(nnpi_boot_session_observe(s,&key,2,1,NULL,0));
}
static void sysinfo_tx(struct nnpi_boot_session *s)
{
    struct nnpi_fifo_command_plan plan;
    started(s,0);
    CHECK(nnpi_boot_session_sysinfo(s,&key,3,1,0,0x1000,&plan));
    CHECK(plan.count==3 && plan.writes[0].value==UINT64_C(0x0010111f));
    CHECK(plan.writes[1].value==0x1000 && plan.writes[2].value==4096);
    CHECK(plan.writes[0].low_offset==0x50 && plan.writes[1].notify==0 &&
          plan.writes[2].high_offset==0x5c && plan.writes[2].notify==1);
    CHECK(!nnpi_boot_session_may_release(s,NNPI_BS_SYSINFO_DMA));
    CHECK(nnpi_boot_session_may_release(s,NNPI_BS_IMAGE_DMA|NNPI_BS_TABLE_DMA));
    CHECK(!nnpi_boot_session_sysinfo(s,&key,4,1,0,0x1000,&plan));
    CHECK(!nnpi_boot_session_observe(s,&key,4,10,NULL,0));
}
static void sysinfo_ready(struct nnpi_boot_session *s)
{
    uint8_t p[NNPI_SYSINFO_PAYLOAD_BYTES];
    payload(p); sysinfo_tx(s);
    CHECK(nnpi_boot_session_sent(s,&key,6));
    CHECK(!nnpi_boot_session_sent(s,&key,6));
    CHECK(nnpi_boot_session_observe(s,&key,4,10,p,sizeof(p)));
    CHECK(s->payload_valid && !nnpi_boot_session_may_release(s,NNPI_BS_SYSINFO_DMA));
}
static void image_tx(struct nnpi_boot_session *s)
{
    struct nnpi_fifo_command_plan plan;
    sysinfo_ready(s);
    CHECK(nnpi_boot_session_image(s,&key,5,10,0,0x2000,4096,1,4096,&plan));
    CHECK(plan.count==3 && plan.writes[0].value==UINT64_C(0x0010101f));
    CHECK(plan.writes[1].value==0x2018 && plan.writes[2].value==UINT64_C(0x100000000008));
    CHECK(s->published_dma==NNPI_BS_ALL_DMA);
    CHECK(!nnpi_boot_session_image(s,&key,6,10,0,0x2000,4096,1,4096,&plan));
}
static void reset_response(struct nnpi_reset_response *r)
{
    nnpi_reset_init(r);
    CHECK(nnpi_reset_begin(r,key.generation,key.epoch,1));
    nnpi_sysinfo_reset_returned(r,0,2);
}
static void baseline_tests(void)
{
    struct nnpi_boot_session s;
    struct nnpi_boot_session_key bad;
    uint32_t i;
    for (i=0;i<256;++i) {
        nnpi_boot_session_init(&s);
        CHECK(nnpi_boot_session_begin(&s,&key,1,0,i,0,0)==(i<=1));
    }
    nnpi_boot_session_init(&s);
    CHECK(!nnpi_boot_session_begin(&s,&key,1,0,UINT32_MAX,0,0));
    CHECK(!nnpi_boot_session_begin(&s,&key,1,0,0x1001,0,0));
    CHECK(!nnpi_boot_session_begin(&s,&key,1,1,0,0,0));
    CHECK(!nnpi_boot_session_begin(&s,&key,1,0,0,0x100,0));
    CHECK(!nnpi_boot_session_begin(&s,&key,1,0,0,0,0x100));
    CHECK(!nnpi_boot_session_begin(&s,&key,1,0,0,UINT32_MAX,0));
    CHECK(!nnpi_boot_session_begin(&s,&key,0,0,0,0,0));
    CHECK(!nnpi_boot_session_begin(&s,&key,UINT64_MAX,0,0,0,0));
    for (i=0;i<6;++i) {
        bad=key;
        if (i<2) bad.session_id=i ? UINT64_MAX : 0;
        else if (i<4) bad.generation=i==3 ? UINT64_MAX : 0;
        else bad.epoch=i==5 ? UINT64_MAX : 0;
        CHECK(!nnpi_boot_session_begin(&s,&bad,1,0,0,0,0));
    }
    started(&s,1);
    CHECK(!nnpi_boot_session_begin(&s,&key,3,0,0,0,0));
    CHECK(!nnpi_boot_session_may_release(&s,0));
    CHECK(!nnpi_boot_session_may_release(&s,8));
    CHECK(nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
    nnpi_boot_session_abort(&s);
    CHECK(!nnpi_boot_session_hostready(&s,&key));
}
static void success_tests(void)
{
    uint32_t ready;
    for (ready=4;ready<=8;ready+=4) {
        struct nnpi_boot_session s;
        struct nnpi_reset_response r;
        image_tx(&s);
        CHECK(nnpi_boot_session_sent(&s,&key,6));
        CHECK(!nnpi_boot_session_observe(&s,&key,6,10,NULL,0));
        CHECK(!nnpi_boot_session_observe(&s,&key,7,3,NULL,0));
        CHECK(s.boot_started && !s.driver_ready && s.phase==NNPI_BS_WAIT_DRIVER);
        CHECK(!nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
        CHECK(nnpi_boot_session_observe(&s,&key,8,ready,NULL,0));
        CHECK(s.driver_ready && s.phase==NNPI_BS_DRIVER_READY);
        CHECK(!nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
        CHECK(nnpi_boot_session_reset_begin(&s,&key));
        CHECK(!nnpi_boot_session_reset_begin(&s,&key));
        reset_response(&r);
        CHECK(nnpi_boot_session_stopped(&s,&key,NNPI_BS_RESET_RETURN_ONLY,0,&r));
        CHECK(nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
        CHECK(!nnpi_boot_session_stopped(&s,&key,NNPI_BS_RESET_RETURN_ONLY,0,&r));
        CHECK(!nnpi_boot_session_observe(&s,&key,9,1,NULL,0));
    }
}
static void failure_tests(void)
{
    struct nnpi_boot_session s;
    struct nnpi_fifo_command_plan plan, before;
    uint8_t p[NNPI_SYSINFO_PAYLOAD_BYTES];
    uint32_t i;
    const uint32_t badcards[]={UINT32_MAX,0x1001,2,5,9,255};
    for (i=0;i<sizeof(badcards)/sizeof(badcards[0]);++i) {
        started(&s,0);
        memset(&plan,0xa5,sizeof(plan)); before=plan;
        CHECK(!nnpi_boot_session_sysinfo(&s,&key,3,badcards[i],0,0x1000,&plan));
        CHECK(s.phase==NNPI_BS_CLEANUP && !s.sysinfo_used);
        CHECK(memcmp(&plan,&before,sizeof(plan))==0);
        CHECK(!nnpi_boot_session_sysinfo(&s,&key,4,1,0,0x1000,&plan));
    }
    started(&s,0);
    CHECK(!nnpi_boot_session_sysinfo(&s,&key,3,1,0x100,0x1000,&plan));
    started(&s,0);
    CHECK(!nnpi_boot_session_sysinfo(&s,&key,3,1,0,0x1001,&plan));
    for (i=0;i<6;++i) {
        sysinfo_tx(&s);
        CHECK(!nnpi_boot_session_sent(&s,&key,i));
        CHECK(s.phase==NNPI_BS_CLEANUP);
        CHECK(!nnpi_boot_session_may_release(&s,NNPI_BS_SYSINFO_DMA));
        CHECK(nnpi_boot_session_may_release(&s,NNPI_BS_IMAGE_DMA|NNPI_BS_TABLE_DMA));
        image_tx(&s);
        CHECK(!nnpi_boot_session_sent(&s,&key,i));
        CHECK(!nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
        CHECK(!nnpi_boot_session_sent(&s,&key,6));
    }
    for (i=0;i<3;++i) {
        sysinfo_tx(&s); CHECK(nnpi_boot_session_sent(&s,&key,6)); payload(p);
        if (i==0) p[0]=2;
        if (i==1) p[90]=1;
        CHECK(!nnpi_boot_session_observe(&s,&key,4,10,p,i==2 ? sizeof(p)-1 : sizeof(p)));
        CHECK(s.phase==NNPI_BS_CLEANUP && !s.payload_valid);
        CHECK(!nnpi_boot_session_may_release(&s,NNPI_BS_SYSINFO_DMA));
    }
    for (i=0;i<256;++i) {
        if (i==1 || i==10) continue;
        sysinfo_tx(&s); CHECK(nnpi_boot_session_sent(&s,&key,6));
        CHECK(!nnpi_boot_session_observe(&s,&key,4,i,NULL,0));
        CHECK(s.phase==NNPI_BS_CLEANUP);
    }
    image_tx(&s); CHECK(nnpi_boot_session_sent(&s,&key,6));
    CHECK(!nnpi_boot_session_observe(&s,&key,6,3,NULL,0));
    CHECK(!nnpi_boot_session_observe(&s,&key,7,10,NULL,0));
    CHECK(s.phase==NNPI_BS_CLEANUP);
    sysinfo_ready(&s); memset(&plan,0xa5,sizeof(plan)); before=plan;
    CHECK(!nnpi_boot_session_image(&s,&key,5,10,0,0x2001,4096,1,4096,&plan));
    CHECK(!memcmp(&plan,&before,sizeof(plan)) && s.phase==NNPI_BS_CLEANUP);
    CHECK(!s.image_used && !nnpi_boot_session_may_release(&s,NNPI_BS_SYSINFO_DMA));
}
static void waiting(struct nnpi_boot_session *s,uint32_t phase)
{
    if (phase==NNPI_BS_WAIT_BIOS) {
        nnpi_boot_session_init(s);
        CHECK(nnpi_boot_session_begin(s,&key,1,0,0,0,0));
        CHECK(nnpi_boot_session_reserve_cleanup(s,&key));
        CHECK(nnpi_boot_session_hostready(s,&key));
    } else {
        if (phase==NNPI_BS_WAIT_SYSINFO) sysinfo_tx(s);
        else image_tx(s);
        CHECK(nnpi_boot_session_sent(s,&key,6));
    }
    CHECK(s->phase==phase && !s->observations && !s->timed_out);
}
static uint32_t waiting_card(uint32_t phase)
{
    return phase==NNPI_BS_WAIT_BIOS ? 0U : (phase==NNPI_BS_WAIT_SYSINFO ? 1U : 3U);
}
static uint32_t ready_card(uint32_t phase)
{
    return phase==NNPI_BS_WAIT_BIOS ? 1U : (phase==NNPI_BS_WAIT_SYSINFO ? 10U : 4U);
}
static void dma_retained(const struct nnpi_boot_session *s,uint32_t published)
{
    uint32_t resource;
    CHECK(s->published_dma==published && !s->stopped_dma);
    for (resource=NNPI_BS_SYSINFO_DMA;resource<=NNPI_BS_TABLE_DMA;resource<<=1)
        CHECK(nnpi_boot_session_may_release(s,resource)==!(published&resource));
}
/* Reach the observation BEFORE the limit using actual, strictly fresh reads. */
static void before_limit(struct nnpi_boot_session *s,uint32_t phase)
{
    uint32_t i,limit=nnpi_boot_session_wait_limit(phase);
    waiting(s,phase);
    for (i=1;i<limit;++i) {
        CHECK(!nnpi_boot_session_observe(s,&key,s->last_read+1,waiting_card(phase),NULL,0));
        CHECK(s->phase==phase && s->observations==i && !s->invalidated && !s->timed_out);
    }
}
static void wait_boundary_tests(void)
{
    const uint32_t phases[]={NNPI_BS_WAIT_BIOS,NNPI_BS_WAIT_SYSINFO,NNPI_BS_WAIT_DRIVER};
    const uint32_t targets[]={NNPI_BS_BIOS_READY,NNPI_BS_SYSINFO_READY,NNPI_BS_DRIVER_READY};
    const uint32_t limits[]={NNPI_BS_BIOS_LIMIT,NNPI_BS_SYSINFO_LIMIT,NNPI_BS_DRIVER_LIMIT};
    const volatile uint32_t contract[]={NNPI_BS_POLL_MS,NNPI_BS_READY_LIMIT,
        NNPI_BS_BIOS_LIMIT,NNPI_BS_SYSINFO_LIMIT,NNPI_BS_DRIVER_LIMIT,
        NNPI_BS_SUPERVISOR_MARGIN_MS};
    uint8_t p[NNPI_SYSINFO_PAYLOAD_BYTES];
    uint32_t stage,owned;
    struct nnpi_boot_session s,before;
    struct nnpi_reset_response r;
    payload(p);
    CHECK(contract[0]==500U && contract[1]==600U);
    CHECK(contract[2]==20U && contract[3]==20U);
    CHECK(contract[4]==1200U && contract[5]==60000U);
    CHECK(!nnpi_boot_session_wait_limit(NNPI_BS_NEW));
    CHECK(!nnpi_boot_session_wait_limit(NNPI_BS_CLEANUP));
    CHECK(!nnpi_boot_session_wait_limit(UINT32_MAX));
    for (stage=0;stage<3;++stage) {
        CHECK(nnpi_boot_session_wait_limit(phases[stage])==limits[stage]);
        CHECK(nnpi_boot_session_wait_limit(targets[stage])==limits[stage]);
        before_limit(&s,phases[stage]); owned=s.published_dma;
        CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,waiting_card(phases[stage]),NULL,0));
        CHECK(s.observations==limits[stage] && s.timed_out && s.invalidated);
        CHECK(s.phase==NNPI_BS_CLEANUP && !s.driver_ready);
        dma_retained(&s,owned); before=s;
        CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,ready_card(phases[stage]),p,sizeof(p)));
        CHECK(!memcmp(&s,&before,sizeof(s))); /* No resurrection after timeout. */
        CHECK(nnpi_boot_session_reset_begin(&s,&key));
        CHECK(s.timed_out); dma_retained(&s,owned);
        reset_response(&r);
        CHECK(nnpi_boot_session_stopped(&s,&key,NNPI_BS_RESET_RETURN_ONLY,0,&r));
        CHECK(nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));

        before_limit(&s,phases[stage]); owned=s.published_dma;
        CHECK(nnpi_boot_session_observe(&s,&key,s.last_read+1,ready_card(phases[stage]),p,sizeof(p)));
        CHECK(s.phase==targets[stage] && !s.timed_out && !s.invalidated);
        CHECK(s.observations==(stage==2 ? limits[stage] : 0U));
        dma_retained(&s,owned); /* Ready at limit does not stop published DMA. */
    }
}
static void phase_budget_reset_tests(void)
{
    struct nnpi_boot_session s;
    struct nnpi_fifo_command_plan plan;
    uint8_t p[NNPI_SYSINFO_PAYLOAD_BYTES];
    uint32_t i;
    before_limit(&s,NNPI_BS_WAIT_BIOS); payload(p);
    CHECK(nnpi_boot_session_observe(&s,&key,s.last_read+1,1,NULL,0));
    CHECK(!s.observations);
    CHECK(nnpi_boot_session_sysinfo(&s,&key,s.last_read+1,1,0,0x1000,&plan));
    CHECK(nnpi_boot_session_sent(&s,&key,6));
    for (i=1;i<NNPI_BS_SYSINFO_LIMIT;++i) {
        CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,1,NULL,0));
        CHECK(s.phase==NNPI_BS_WAIT_SYSINFO && s.observations==i && !s.timed_out);
    }
    CHECK(nnpi_boot_session_observe(&s,&key,s.last_read+1,10,p,sizeof(p)));
    CHECK(!s.observations);
    CHECK(nnpi_boot_session_image(&s,&key,s.last_read+1,10,0,0x2000,4096,1,4096,&plan));
    CHECK(nnpi_boot_session_sent(&s,&key,6));
    for (i=1;i<NNPI_BS_DRIVER_LIMIT;++i) {
        CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,3,NULL,0));
        CHECK(s.phase==NNPI_BS_WAIT_DRIVER && s.observations==i && !s.timed_out);
    }
    CHECK(nnpi_boot_session_observe(&s,&key,s.last_read+1,8,NULL,0));
    CHECK(s.driver_ready && !s.timed_out);
    dma_retained(&s,NNPI_BS_ALL_DMA);
}
static void driver_long_wait_tests(void)
{
    const uint32_t checkpoints[]={119,120,121,1199,1200};
    uint32_t n,i,ready;
    struct nnpi_boot_session s;
    /* Both accepted waiting states survive the old 120-observation cutoff. */
    for (n=0;n<2;++n) {
        waiting(&s,NNPI_BS_WAIT_DRIVER);
        for (i=1;i<NNPI_BS_DRIVER_LIMIT;++i) {
            CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,n ? 3 : 10,NULL,0));
            CHECK(s.phase==NNPI_BS_WAIT_DRIVER && !s.timed_out && !s.invalidated);
            if (i==119 || i==120 || i==121 || i==1199) {
                CHECK(s.observations==i && !s.driver_ready);
                dma_retained(&s,NNPI_BS_ALL_DMA);
            }
        }
        CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,n ? 3 : 10,NULL,0));
        CHECK(s.observations==1200 && s.timed_out && s.phase==NNPI_BS_CLEANUP);
        dma_retained(&s,NNPI_BS_ALL_DMA);
    }
    for (n=0;n<sizeof(checkpoints)/sizeof(checkpoints[0]);++n) {
        for (ready=4;ready<=8;ready+=4) {
            waiting(&s,NNPI_BS_WAIT_DRIVER);
            for (i=1;i<checkpoints[n];++i)
                CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,3,NULL,0));
            CHECK(nnpi_boot_session_observe(&s,&key,s.last_read+1,ready,NULL,0));
            CHECK(s.observations==checkpoints[n] && s.phase==NNPI_BS_DRIVER_READY);
            CHECK(s.driver_ready && !s.timed_out && !s.invalidated);
            dma_retained(&s,NNPI_BS_ALL_DMA);
        }
    }
}
static void boundary_protocol_and_serial_tests(void)
{
    const uint32_t phases[]={NNPI_BS_WAIT_BIOS,NNPI_BS_WAIT_SYSINFO,NNPI_BS_WAIT_DRIVER};
    const uint64_t badserials[]={0,1,UINT64_MAX};
    uint32_t stage,i,owned,card;
    uint8_t p[NNPI_SYSINFO_PAYLOAD_BYTES];
    struct nnpi_boot_session s,before;
    struct nnpi_boot_session_key other;
    payload(p);
    for (stage=0;stage<3;++stage) {
        for (i=0;i<6;++i) {
            before_limit(&s,phases[stage]); owned=s.published_dma;
            card=i<4 ? ready_card(phases[stage])|(1U<<(12+i)) : (i==4 ? UINT32_MAX : 2U);
            CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,card,p,sizeof(p)));
            CHECK(s.invalidated && s.phase==NNPI_BS_CLEANUP && !s.timed_out);
            CHECK(!s.driver_ready); dma_retained(&s,owned);
        }
        before_limit(&s,phases[stage]); before=s;
        for (i=0;i<sizeof(badserials)/sizeof(badserials[0]);++i) {
            CHECK(!nnpi_boot_session_observe(&s,&key,badserials[i],ready_card(phases[stage]),p,sizeof(p)));
            CHECK(!memcmp(&s,&before,sizeof(s)));
        }
        CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read,ready_card(phases[stage]),p,sizeof(p)));
        CHECK(!memcmp(&s,&before,sizeof(s)));
        for (i=0;i<3;++i) {
            other=key;
            if (i==0) ++other.session_id;
            else if (i==1) ++other.generation;
            else ++other.epoch;
            CHECK(!nnpi_boot_session_observe(&s,&other,s.last_read+1,ready_card(phases[stage]),p,sizeof(p)));
            CHECK(!memcmp(&s,&before,sizeof(s)));
        }
        CHECK(nnpi_boot_session_observe(&s,&key,s.last_read+1,ready_card(phases[stage]),p,sizeof(p)));
        CHECK(!s.timed_out && !s.invalidated);
    }
    for (i=0;i<4;++i) {
        before_limit(&s,NNPI_BS_WAIT_SYSINFO); payload(p);
        if (i==0) p[0]=2;
        if (i==1) p[90]=1;
        CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,10,i==3 ? NULL : p,
            i==2 ? sizeof(p)-1 : sizeof(p)));
        CHECK(s.phase==NNPI_BS_CLEANUP && s.invalidated && !s.timed_out && !s.payload_valid);
        dma_retained(&s,NNPI_BS_SYSINFO_DMA);
    }
    before_limit(&s,NNPI_BS_WAIT_DRIVER);
    CHECK(!nnpi_boot_session_observe(&s,&key,s.last_read+1,10,NULL,0));
    CHECK(s.phase==NNPI_BS_CLEANUP && !s.timed_out); /* BOOT_STARTED cannot regress. */
    dma_retained(&s,NNPI_BS_ALL_DMA);
}
static void doorbell_and_freshness_tests(void)
{
    struct nnpi_boot_session s;
    struct nnpi_boot_session_key other;
    uint32_t i,stage;
    uint8_t p[NNPI_SYSINFO_PAYLOAD_BYTES];
    payload(p);
    for (i=0;i<256;++i) {
        image_tx(&s); CHECK(nnpi_boot_session_sent(&s,&key,6));
        CHECK(nnpi_boot_session_observe(&s,&key,6,i,NULL,0)==(i==4 || i==8));
        if (i!=3 && i!=4 && i!=8 && i!=10) CHECK(s.phase==NNPI_BS_CLEANUP);
        CHECK(!nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
    }
    for (stage=0;stage<3;++stage) {
        for (i=0;i<5;++i) {
            uint32_t card=i==4 ? UINT32_MAX : ((uint32_t)1<<(12+i));
            if (stage==0) {
                nnpi_boot_session_init(&s);
                CHECK(nnpi_boot_session_begin(&s,&key,1,0,0,0,0));
                CHECK(nnpi_boot_session_reserve_cleanup(&s,&key));
                CHECK(nnpi_boot_session_hostready(&s,&key));
                card|=1;
            } else if (stage==1) {
                sysinfo_tx(&s); CHECK(nnpi_boot_session_sent(&s,&key,6)); card|=10;
            } else {
                image_tx(&s); CHECK(nnpi_boot_session_sent(&s,&key,6)); card|=4;
            }
            CHECK(!nnpi_boot_session_observe(&s,&key,100,card,p,sizeof(p)));
            CHECK(s.phase==NNPI_BS_CLEANUP);
        }
    }
    sysinfo_tx(&s); CHECK(nnpi_boot_session_sent(&s,&key,6));
    for (i=0;i<3;++i) {
        other=key;
        if (i==0) ++other.session_id;
        if (i==1) ++other.generation;
        if (i==2) ++other.epoch;
        CHECK(!nnpi_boot_session_observe(&s,&other,4,10,p,sizeof(p)));
        CHECK(s.last_read==3 && !s.payload_valid);
    }
    CHECK(!nnpi_boot_session_observe(&s,&key,3,10,p,sizeof(p)));
    CHECK(!nnpi_boot_session_observe(&s,&key,UINT64_MAX,10,p,sizeof(p)));
    CHECK(nnpi_boot_session_observe(&s,&key,4,10,p,sizeof(p)));
    CHECK(!nnpi_boot_session_observe(&s,&key,5,4,NULL,0));
    CHECK(!s.driver_ready);
}
static void reset_tests(void)
{
    unsigned i;
    for (i=0;i<19;++i) {
        struct nnpi_boot_session s;
        struct nnpi_reset_response r;
        struct nnpi_boot_session_key other=key;
        uint32_t mode=NNPI_BS_RESET_RETURN_ONLY, flags=0;
        image_tx(&s); nnpi_boot_session_abort(&s);
        CHECK(!nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
        reset_response(&r);
        CHECK(!nnpi_boot_session_stopped(&s,&key,mode,flags,&r));
        CHECK(nnpi_boot_session_reset_begin(&s,&key));
        switch (i) {
        case 0: ++other.session_id; break;
        case 1: ++other.generation; break;
        case 2: ++other.epoch; break;
        case 3: ++r.generation; break;
        case 4: ++r.epoch; break;
        case 5: ++r.operation_id; break;
        case 6: mode=0; break;
        case 7: flags=1; break;
        case 8: r.call_returned=0; break;
        case 9: r.call_status=NNPI_RESET_PENDING; break;
        case 10: r.call_status=1; break;
        case 11: r.call_status=0xc0000001U; break;
        case 12: r.callback_seen=1; break;
        case 13: r.callback_count=1; break;
        case 14: r.state=NNPI_RESET_FAILED; break;
        case 15: r.completed_100ns=3; break;
        case 16: r.completion_status=0; break;
        case 17: r.version=2; break;
        default: r.reserved=1; break;
        }
        CHECK(!nnpi_boot_session_stopped(&s,&other,mode,flags,&r));
        CHECK(!nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
        CHECK(!nnpi_boot_session_reset_begin(&s,&key));
        if (i>=9 && i<=16) {
            reset_response(&r);
            CHECK(!nnpi_boot_session_stopped(&s,&key,NNPI_BS_RESET_RETURN_ONLY,0,&r));
        }
    }
    {
        struct nnpi_boot_session s;
        struct nnpi_reset_response r;
        struct nnpi_boot_session_key other=key;
        sysinfo_tx(&s); ++other.epoch;
        CHECK(!nnpi_boot_session_sent(&s,&other,6));
        nnpi_boot_session_abort(&s);
        CHECK(!nnpi_boot_session_sent(&s,&key,6));
        CHECK(nnpi_boot_session_reset_begin(&s,&key));
        reset_response(&r); r.epoch_invalidated=1;
        CHECK(nnpi_boot_session_stopped(&s,&key,NNPI_BS_RESET_RETURN_ONLY,0,&r));
        CHECK(nnpi_boot_session_may_release(&s,NNPI_BS_ALL_DMA));
    }
}
int main(void)
{
    baseline_tests(); success_tests(); failure_tests();
    wait_boundary_tests(); phase_budget_reset_tests(); driver_long_wait_tests();
    boundary_protocol_and_serial_tests(); doorbell_and_freshness_tests(); reset_tests();
    printf("PASS boot session: %u checks (pure policy; no MMIO/DMA/reset/hardware boot)\n",checks);
    return 0;
}
