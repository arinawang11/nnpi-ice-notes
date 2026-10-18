#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_runtime_info_abi.h"
int main(void)
{
    struct nnpi_runtime_info_request q={NNPI_RUNTIME_INFO_VERSION,sizeof(q),{0,0}};
    struct nnpi_runtime_info_response r={0};
    size_t i;
    assert(NNPI_RUNTIME_INFO_IOCTL==((0x8000u<<16)|(1u<<14)|(0x814u<<2)));
    assert(sizeof(q)==16 && sizeof(r)==128);
    assert(nnpi_runtime_info_buffers_valid(&q,sizeof(q),sizeof(r)));
    for (i=0;i<256;++i) {
        if(i!=sizeof(q)) assert(!nnpi_runtime_info_request_valid(&q,i));
        if(i!=sizeof(r)) assert(!nnpi_runtime_info_buffers_valid(&q,sizeof(q),i));
    }
    assert(!nnpi_runtime_info_request_valid(NULL,sizeof(q)));
    q.reserved[1]=1; assert(!nnpi_runtime_info_request_valid(&q,sizeof(q))); q.reserved[1]=0;
    q.version=2; assert(!nnpi_runtime_info_request_valid(&q,sizeof(q))); q.version=1;
    q.size=0; assert(!nnpi_runtime_info_request_valid(&q,sizeof(q)));
    r.version=1; r.size=sizeof(r); r.net_disabled_reason=NNPI_RUNTIME_NET_DISABLED_PAGE_LIST_UNIMPLEMENTED;
    assert(nnpi_runtime_info_response_valid(&r,sizeof(r))); /* pre-prepare cache */
    r.generation=UINT64_MAX; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.generation=0;
    r.epoch=UINT64_MAX; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.epoch=0;
    r.healthy=1; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.compatible=1; assert(nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.keepalive_updates=UINT64_MAX; r.unexpected_net_config_count=UINT64_MAX;
    assert(nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.net_disabled_reason=NNPI_RUNTIME_NET_DISABLED_LIFECYCLE_UNVERIFIED;
    assert(nnpi_runtime_info_response_valid(&r,sizeof(r)));
    for (i=1;i<=NNPI_RUNTIME_NET_UNEXPECTED_CONFIG;++i) {
        r.net_disabled_reason=(uint32_t)i;
        assert(nnpi_runtime_info_response_valid(&r,sizeof(r)));
        r.net_enabled=1; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.net_enabled=0;
    }
    r.net_disabled_reason=8; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.net_disabled_reason=UINT32_MAX; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.net_disabled_reason=NNPI_RUNTIME_NET_READY;
    assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); /* disabled READY is inconsistent */
    r.net_enabled=1; assert(nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.healthy=0; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.compatible=0; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.healthy=1; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.compatible=1;
    r.net_enabled=2; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.net_enabled=0;
    r.net_disabled_reason=NNPI_RUNTIME_NET_NOT_READY;
    /* No coupling to advisory counters, partial QWs, or IRQ evidence. */
    r.partial_count=3; r.irq_valid=0; r.unexpected_net_config_count=UINT64_MAX;
    r.net_enabled=1; r.net_disabled_reason=NNPI_RUNTIME_NET_READY;
    assert(nnpi_runtime_info_response_valid(&r,sizeof(r)));
    r.net_enabled=0; r.net_disabled_reason=NNPI_RUNTIME_NET_NOT_READY;
    r.irq_valid=4; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.irq_valid=3;
    r.partial_count=4; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.partial_count=2;
    r.reserved0=1; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.reserved0=0;
    r.reserved[1]=1; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r))); r.reserved[1]=0;
    r.protocol_version=0x10000; assert(!nnpi_runtime_info_response_valid(&r,sizeof(r)));
    assert(!nnpi_runtime_info_response_valid(NULL,sizeof(r)));
    puts("runtime info ABI: passed"); return 0;
}
