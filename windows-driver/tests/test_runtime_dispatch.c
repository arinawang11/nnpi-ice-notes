#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_runtime_dispatch.h"
int main(void)
{
    struct nnpi_runtime_parser p={0};
    struct nnpi_runtime_message m={0};
    uint64_t words[]={4,39,UINT64_C(0x40802),39,4,39,4,11,63};
    unsigned i,versions=0,events=0,unexpected=0,sysinfo=0;
    for(i=0;i<sizeof(words)/sizeof(words[0]);++i) {
        int result=nnpi_runtime_push(&p,words[i],&m);
        if(i==2 || i==3) { assert(result==NNPI_RUNTIME_NEED_MORE); continue; }
        if(i==8) { assert(result==NNPI_RUNTIME_UNKNOWN && p.stopped); continue; }
        assert(result==NNPI_RUNTIME_MESSAGE);
        switch(nnpi_runtime_route_frame(&m)) {
        case NNPI_RUNTIME_ROUTE_VERSION: ++versions; assert(nnpi_runtime_compatible_1150(&m)); break;
        case NNPI_RUNTIME_ROUTE_EVENT: ++events; break;
        case NNPI_RUNTIME_ROUTE_UNEXPECTED_NET: ++unexpected; break;
        case NNPI_RUNTIME_ROUTE_SYSINFO: ++sysinfo; break;
        default: assert(0);
        }
    }
    assert(versions==1 && events==2 && unexpected==2 && sysinfo==1);
    assert(nnpi_runtime_push(&p,4,&m)==NNPI_RUNTIME_STOPPED);
    assert(nnpi_runtime_route_frame(NULL)==NNPI_RUNTIME_ROUTE_INVALID);
    m.qword_count=0; assert(nnpi_runtime_route_frame(&m)==NNPI_RUNTIME_ROUTE_INVALID);
    assert(nnpi_runtime_saturating_increment(UINT64_MAX)==UINT64_MAX);
    assert(nnpi_runtime_saturating_increment(UINT64_MAX-1)==UINT64_MAX);
    assert(nnpi_runtime_saturating_increment(0)==1);
    assert(nnpi_runtime_net_wait_ready(1,0,5,5));
    assert(!nnpi_runtime_net_wait_ready(0,0,5,5));
    assert(!nnpi_runtime_net_wait_ready(1,1,5,5));
    assert(!nnpi_runtime_net_wait_ready(1,0,4,5));
    {
        struct nnpi_net_session n={0};
        struct nnpi_net_session_key k={1,2,3};
        m.opcode=39; m.qword_count=1; m.qwords[0]=39;
        assert(nnpi_runtime_dispatch_net(&m,NULL,NULL,0)==NNPI_RUNTIME_NET_UNEXPECTED);
        assert(nnpi_net_session_begin(&n,&k,1,256));
        n.phase=NNPI_NS_WAIT_ETH; n.card_ip=1;
        m.qwords[0]=39ULL|(256ULL<<6); /* rejected zero IP */
        assert(nnpi_runtime_dispatch_net(&m,&n,&k,1)==NNPI_RUNTIME_NET_FAILED);
        m.opcode=4; m.qwords[0]=4ULL|(112ULL<<6);
        assert(nnpi_runtime_dispatch_net(&m,NULL,NULL,0)==NNPI_RUNTIME_NET_FATAL);
    }
    puts("runtime dispatch: passed"); return 0;
}
