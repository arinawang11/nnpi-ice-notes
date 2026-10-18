#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_net_control.h"
#include "../include/nnpi_runtime_dispatch.h"
static const uint8_t mac[6]={2,1,2,3,4,5};
static const struct nnpi_net_session_key nk={10,2,3};
static int receive(struct nnpi_runtime_parser *p,struct nnpi_net_session *n,uint64_t word)
{
    struct nnpi_runtime_message m={0};
    assert(nnpi_runtime_push(p,word,&m)==NNPI_RUNTIME_MESSAGE);
    return nnpi_runtime_dispatch_net(&m,n,&nk,1);
}
int main(void)
{
    struct nnpi_runtime_message v={0};
    struct nnpi_net_gate g={0},bad;
    struct nnpi_net_session n={0};
    struct nnpi_runtime_parser parser={0};
    struct nnpi_fifo_command_plan plan={0};
    struct nnpi_boot_session b={0};
    struct nnpi_net_owner o={0},wrong;
    uint64_t create=4ULL|(21ULL<<6)|(256ULL<<21)|(1ULL<<62);
    uint64_t rb=4ULL|(23ULL<<6)|(256ULL<<21)|(1ULL<<62)|(1ULL<<63);
    uint64_t eth=39ULL|(256ULL<<6)|(1ULL<<32);
    uint32_t statuses[]={0x103,0x40000000,0xc0000001};
    unsigned i;
    v.channel_version=0x04c0; v.response_sizes[7]=1; v.command_sizes[25]=2;
    g.compatible=g.healthy=g.boot_worker=g.driver_ready=g.available=1;
    g.boot_phase=NNPI_BS_DRIVER_READY; g.generation=g.boot_generation=2;
    g.epoch=g.boot_epoch=3; g.version=&v;
    assert(nnpi_net_control_gate(&g,2,3,256,1,mac,0)==0);
    bad=g; bad.healthy=0; assert(nnpi_net_control_gate(&bad,2,3,256,1,mac,0)!=0);
    bad=g; bad.compatible=0; assert(nnpi_net_control_gate(&bad,2,3,256,1,mac,0)!=0);
    bad=g; bad.started=1; assert(nnpi_net_control_gate(&bad,2,3,256,1,mac,0)==NNPI_NET_GATE_SESSION_USED);
    bad=g; bad.closing=1; assert(nnpi_net_control_gate(&bad,2,3,256,1,mac,1)!=0);
    assert(nnpi_net_control_gate(&g,2,4,256,1,mac,0)!=0);
    assert(nnpi_net_control_gate(&g,2,3,256,0,mac,0)!=0);
    assert(nnpi_net_control_gate(&g,2,3,257,1,mac,0)!=0);
    assert(nnpi_net_control_gate(&g,2,3,256,1,NULL,0)!=0);
    v.command_sizes[25]=1; assert(nnpi_net_control_gate(&g,2,3,256,1,mac,0)==NNPI_NET_GATE_CAP_MISSING);
    v.command_sizes[25]=2;
    /* Actual session planners + complete parser frames, one receiver to READY. */
    assert(nnpi_net_session_begin(&n,&nk,1,256));
    assert(nnpi_net_session_chan_op(&n,&nk,n.last_read+1,0,0,0,&plan));
    assert(!n.published_dma);
    assert(nnpi_net_session_sent(&n,&nk,2));
    assert(receive(&parser,&n,create)==NNPI_RUNTIME_NET_OBSERVED);
    assert(n.phase==NNPI_NS_CHAN_READY);
    assert(nnpi_net_session_rb_op(&n,&nk,n.last_read+1,0,1,0x12345,&plan));
    assert(n.published_dma==NNPI_NS_H2C_POOL);
    assert(nnpi_net_session_sent(&n,&nk,2));
    assert(receive(&parser,&n,rb)==NNPI_RUNTIME_NET_OBSERVED);
    assert(nnpi_net_session_rb_op(&n,&nk,n.last_read+1,0,0,0x54321,&plan));
    assert(nnpi_net_session_sent(&n,&nk,2));
    assert(receive(&parser,&n,rb)==NNPI_RUNTIME_NET_OBSERVED);
    assert(nnpi_net_session_eth_config(&n,&nk,n.last_read+1,0,1,mac,&plan));
    assert(nnpi_net_session_sent(&n,&nk,4));
    assert(receive(&parser,&n,eth)==NNPI_RUNTIME_NET_OBSERVED);
    assert(nnpi_net_session_ready(&n));
    /* Wrong-phase 39 is local failure, never a global fatal/reset. */
    assert(receive(&parser,&n,eth)==NNPI_RUNTIME_NET_FAILED);
    assert(!nnpi_net_session_may_release(&n,NNPI_NS_ALL_DMA));
    b.key.session_id=20; b.key.generation=2; b.key.epoch=3;
    b.published_dma=b.stopped_dma=NNPI_BS_ALL_DMA;
    b.reset_used=b.reset_returned=1; b.phase=NNPI_BS_FINISHED;
    o.boot=b.key; o.net=nk; o.channel_issued=1;
    assert(!nnpi_net_owner_may_release(&o));
    nnpi_net_owner_record_stop(&o,&b.key,1,0,1);
    assert(!nnpi_net_owner_finalize(&o,&b,&nk,0,1,3,1));
    assert(!nnpi_net_owner_finalize(&o,&b,&nk,1,0,3,1));
    assert(!nnpi_net_owner_finalize(&o,&b,&nk,1,1,0,1));
    for(i=0;i<sizeof(statuses)/sizeof(statuses[0]);++i) {
        wrong=o; wrong.reset_status=statuses[i];
        assert(!nnpi_net_owner_finalize(&wrong,&b,&nk,1,1,3,1));
    }
    wrong=o; wrong.reset_returned=0;
    assert(!nnpi_net_owner_finalize(&wrong,&b,&nk,1,1,3,1));
    wrong=o; wrong.stopped_boot.session_id++;
    assert(!nnpi_net_owner_finalize(&wrong,&b,&nk,1,1,3,1));
    wrong=o; wrong.net.epoch++;
    assert(!nnpi_net_owner_finalize(&wrong,&b,&nk,1,1,3,1));
    assert(nnpi_net_owner_finalize(&o,&b,&nk,1,1,3,1));
    assert(nnpi_net_owner_may_release(&o));
    wrong=o; wrong.stopped=0;
    assert(nnpi_net_owner_finalize(&wrong,&b,&nk,1,1,0,0)); /* channel-only responsibility */
    puts("net control: passed"); return 0;
}
