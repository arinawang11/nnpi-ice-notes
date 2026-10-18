#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_sysinfo_policy.h"
struct writer { unsigned fail, calls; uint32_t values[6]; };
static int write_half(void *ctx,uint32_t half,uint32_t value)
{
    struct writer *w=(struct writer *)ctx;
    assert(half==w->calls && half<6);
    w->values[w->calls++]=value;
    return half!=w->fail;
}
static void payload(uint8_t *p)
{
    size_t i; memset(p,0,123); p[0]=1;
    for(i=0;i<32;++i) p[25+2*i]='A';
    p[41]='.'; p[49]='.'; p[59]='.'; p[67]='.';
}
int main(void)
{
    struct nnpi_sysinfo_response s;
    struct nnpi_reset_response r;
    struct nnpi_fifo_command_plan plan;
    struct nnpi_sysinfo_request q={NNPI_SYSINFO_VERSION,sizeof(q),0,0,0,0};
    uint8_t p[123]; unsigned i;
    assert(nnpi_sysinfo_request_valid(&q,sizeof(q),0));
    assert(!nnpi_sysinfo_request_valid(&q,sizeof(q),1));
    payload(p); assert(nnpi_sysinfo_parse(p,123)==NNPI_SYSINFO_PARSE_OK);
    assert(nnpi_sysinfo_parse(p,122)==NNPI_SYSINFO_PARSE_LENGTH);
    p[0]=2; assert(nnpi_sysinfo_parse(p,123)==NNPI_SYSINFO_PARSE_VERSION); p[0]=1;
    for(i=25;i<91;++i) { uint8_t v=p[i]; p[i]=255;
        assert(nnpi_sysinfo_parse(p,123)==NNPI_SYSINFO_PARSE_STRING); p[i]=v; }
    assert(nnpi_sysinfo_plan(1,0,0x1000,&plan));
    assert(plan.count==3 && plan.writes[0].value==0x0010111f);
    assert(plan.writes[1].value==0x1000 && plan.writes[2].value==4096);
    assert(plan.writes[0].low_offset==0x50 && plan.writes[2].high_offset==0x5c);
    assert(!nnpi_sysinfo_plan(10,0,0x1000,&plan));
    assert(!nnpi_sysinfo_plan(1,0x100,0x1000,&plan));
    assert(!nnpi_sysinfo_plan(1,0,0x1001,&plan));
    assert(nnpi_sysinfo_plan(1,0,UINT64_C(0x0000001200001000),&plan));
    for(i=0;i<=6;++i) {
        struct writer w={i,0,{0}};
        unsigned j;
        nnpi_sysinfo_init(&s); assert(nnpi_sysinfo_may_release(&s));
        s.cleanup_reserved=1; s.page_retained=1;
        assert(nnpi_sysinfo_publish(&s,2,3));
        assert(nnpi_sysinfo_emit(&s,&plan,&w,write_half)==(i==6));
        assert(w.calls==(i==6 ? 6 : i+1) && s.writes_attempted==w.calls);
        for(j=0;j<w.calls;++j)
            assert(w.values[j]==(uint32_t)(plan.writes[j/2].value>>((j%2)*32)));
        assert(!nnpi_sysinfo_emit(&s,&plan,&w,write_half));
        assert(!nnpi_sysinfo_publish(&s,2,3));
        assert(!nnpi_sysinfo_may_release(&s));
        if (i==6) {
            assert(nnpi_sysinfo_observe(&s,10)); nnpi_sysinfo_payload(&s,p);
            assert(s.payload_valid);
        } else {
            assert(s.phase==NNPI_SYSINFO_CLEANUP);
            assert(!nnpi_sysinfo_observe(&s,10));
            assert(!s.payload_valid);
        }
        assert(!nnpi_sysinfo_may_release(&s));
        nnpi_sysinfo_invalidate(&s); assert(!nnpi_sysinfo_observe(&s,10));
        nnpi_reset_init(&r); assert(nnpi_reset_begin(&r,2,3,1));
        assert(!nnpi_sysinfo_stopped(&s,1,&r));
        nnpi_sysinfo_reset_returned(&r,NNPI_RESET_PENDING,2);
        assert(!nnpi_sysinfo_stopped(&s,1,&r));
        nnpi_reset_init(&r); assert(nnpi_reset_begin(&r,2,3,1));
        nnpi_sysinfo_reset_returned(&r,0,3); nnpi_reset_invalidate(&r);
        assert(!nnpi_sysinfo_stopped(&s,2,&r));
        assert(nnpi_sysinfo_stopped(&s,1,&r)); assert(nnpi_sysinfo_may_release(&s));
        assert(s.reset.callback_seen==0 && s.reset.callback_count==0 && s.reset.call_status==0);
        assert(nnpi_sysinfo_response_valid(&s,sizeof(s)));
    }
    nnpi_sysinfo_init(&s); s.cleanup_reserved=s.page_retained=1;
    assert(nnpi_sysinfo_publish(&s,2,3));
    for(i=0;i<20;++i) assert(!nnpi_sysinfo_observe(&s,1));
    assert(!nnpi_sysinfo_observe(&s,10)); assert(!nnpi_sysinfo_may_release(&s));
    nnpi_reset_init(&r); assert(nnpi_reset_begin(&r,2,3,1));
    nnpi_reset_returned(&r,0xc0000001U,2); nnpi_reset_completed(&r,0,3);
    assert(!nnpi_sysinfo_stopped(&s,1,&r)); assert(!nnpi_sysinfo_may_release(&s));
    {
        const uint32_t statuses[]={1,NNPI_RESET_PENDING,0xc0000001U};
        for(i=0;i<sizeof(statuses)/sizeof(statuses[0]);++i) {
            nnpi_reset_init(&r); assert(nnpi_reset_begin(&r,2,3,1));
            nnpi_sysinfo_reset_returned(&r,statuses[i],2);
            assert(!nnpi_sysinfo_stopped(&s,1,&r));
            assert(!nnpi_sysinfo_may_release(&s));
        }
    }
    nnpi_reset_init(&r); assert(nnpi_reset_begin(&r,2,3,1));
    nnpi_sysinfo_reset_returned(&r,0,2);
    s.reset_mode=0; assert(!nnpi_sysinfo_stopped(&s,1,&r));
    s.reset_mode=NNPI_SYSINFO_RESET_RETURN_ONLY;
    r.generation=9; assert(!nnpi_sysinfo_stopped(&s,1,&r)); r.generation=2;
    r.epoch=9; assert(!nnpi_sysinfo_stopped(&s,1,&r)); r.epoch=3;
    assert(nnpi_sysinfo_stopped(&s,1,&r));
    puts("SYSINFO ABI/parser/ownership tests passed"); return 0;
}
