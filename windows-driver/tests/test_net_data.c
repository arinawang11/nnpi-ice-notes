#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_net_data.h"
#include "../include/nnpi_runtime_dispatch.h"
static uint64_t frame(unsigned slot,unsigned length,unsigned ack)
{ return 40ULL|(256ULL<<6)|((uint64_t)(ack?0:length-1)<<16)|((uint64_t)slot<<28)|((uint64_t)ack<<36); }
int main(void)
{
    static struct nnpi_net_data d;
    unsigned i; uint16_t length; uint8_t slot,page[4096]; int ack,kind; uint64_t qw;
    struct nnpi_runtime_parser p={0}; struct nnpi_runtime_message m={0};
    uint64_t crash[]={39ULL|(256ULL<<6)|(1ULL<<32),0x594028,0x10554028};
    memset(page,0xa5,sizeof(page));
    /* Original crash, independent frames in same/across batches. */
    for(i=0;i<3;++i) { assert(nnpi_runtime_push(&p,crash[i],&m)==NNPI_RUNTIME_MESSAGE);
        assert(nnpi_runtime_route_frame(&m)==(i?NNPI_RUNTIME_ROUTE_DESCRIPTOR:NNPI_RUNTIME_ROUTE_UNEXPECTED_NET)); }
    assert(nnpi_runtime_push(&p,2,&m)==NNPI_RUNTIME_NEED_MORE);
    assert(nnpi_runtime_push(&p,0x594028,&m)==NNPI_RUNTIME_NEED_MORE);
    assert(nnpi_runtime_push(&p,0x10554028,&m)==NNPI_RUNTIME_MESSAGE && m.opcode==2);
    /* Zero/stranger ACK is never a one-byte RX and never gets an ACK. */
    assert(nnpi_net_data_classify(&d,frame(0,0,1),256,1,&length,&slot)==0);
    assert(d.stats.unknown_ack==1 && !d.rx_pending && !d.stats.rx_received);
    for(i=0;i<16;++i) {
        assert(nnpi_net_data_classify(&d,frame(i,90,0),256,1,&length,&slot)==1);
        assert(nnpi_net_data_receive(&d,slot,length,page));
    }
    assert(d.rx_pending==0xffff && d.stats.rx_queued==4 && d.stats.rx_dropped==12);
    assert(d.stats.rx_received==16 && d.stats.rx_bytes==1440);
    assert(d.rx_lengths[0]==90 && memcmp(d.rx_frames[0],page,90)==0);
    assert(nnpi_net_data_classify(&d,frame(0,90,0),256,1,&length,&slot)==-1);
    assert(d.stats.duplicate_rx==1);
    for(i=0;i<16;++i) {
        assert(nnpi_net_data_next(&d,256,&qw,&slot,&ack));
        assert(ack && slot==i && (qw&63)==58 && ((qw>>16)&4095)==0 && ((qw>>36)&1));
        nnpi_net_data_sent(&d,slot,ack);
    }
    assert(!d.rx_pending && d.stats.rx_ack_sent==16 && !nnpi_net_data_next(&d,256,&qw,&slot,&ack));
    /* Bad length with valid ownership returns the card page but does not read. */
    assert(nnpi_net_data_classify(&d,frame(0,1,0),256,1,&length,&slot)==1);
    assert(nnpi_net_data_receive(&d,slot,length,NULL));
    assert(d.stats.rx_bad_length==1 && d.rx_pending==1);
    nnpi_net_data_sent(&d,0,1);
    assert(nnpi_net_data_classify(&d,frame(16,90,0),256,1,&length,&slot)==0);
    assert(nnpi_net_data_classify(&d,frame(0,90,0),257,1,&length,&slot)==0);
    assert(nnpi_net_data_classify(&d,frame(0,90,0),256,0,&length,&slot)==0);
    assert(nnpi_net_data_classify(&d,frame(0,0,1)|(1ULL<<16),256,1,&length,&slot)==0);
    assert(nnpi_net_data_classify(&d,frame(0,90,0)|(1ULL<<37),256,1,&length,&slot)==0);
    assert(d.stats.invalid_descriptor==5 && !d.rx_pending);
    /* Pending TX has not been published: early ACK must not recycle it. */
    for(i=0;i<16;++i) {
        assert(nnpi_eth_slots_reserve(&d.tx)==(int)i);
        d.tx_pending=(uint16_t)(d.tx_pending|(1u<<i)); d.tx_lengths[i]=60;
    }
    assert(nnpi_eth_slots_reserve(&d.tx)==-1);
    assert(nnpi_net_data_classify(&d,frame(0,0,1),256,1,&length,&slot)==0);
    for(i=0;i<16;++i) {
        assert(nnpi_net_data_next(&d,256,&qw,&slot,&ack) && !ack && slot==i);
        assert(((qw>>16)&4095)==59); nnpi_net_data_sent(&d,slot,ack);
    }
    assert(d.tx_sent==0xffff && !d.tx_pending && d.stats.tx_submitted==16);
    for(i=0;i<16;++i) {
        kind=nnpi_net_data_classify(&d,frame(i,0,1),256,1,&length,&slot); assert(kind==2);
    }
    assert(!d.tx.busy && !d.tx_sent && d.stats.tx_acked==16);
    assert(nnpi_net_data_classify(&d,frame(15,0,1),256,1,&length,&slot)==0);
    assert(!d.rx_pending && d.stats.unknown_ack==3);
    d.stats.version=1; d.stats.size=sizeof(d.stats); nnpi_net_data_snapshot(&d);
    assert(nnpi_net_data_response_valid(&d.stats,sizeof(d.stats)));
    d.stats.rx_ack_pending=17; assert(!nnpi_net_data_response_valid(&d.stats,sizeof(d.stats)));
    puts("net data: parser40, real copy, bounded drop+ACK, 16-slot RX/TX ownership passed");
    return 0;
}
