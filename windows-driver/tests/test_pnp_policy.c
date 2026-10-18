/* SPDX-License-Identifier: GPL-2.0-only */
/* CPU-only production-policy regressions; no driver/device/boot/reset calls. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_pnp_policy.h"
#include "../include/nnpi_boot_session.h"
#include "../include/nnpi_net_session.h"

static void expect(const struct nnpi_pnp_snapshot *s,uint32_t reasons)
{
    struct nnpi_pnp_snapshot before=*s;
    assert(nnpi_pnp_query_veto_reasons(s)==reasons);
    assert(memcmp(s,&before,sizeof(before))==0);
}
static struct nnpi_pnp_snapshot idle(void)
{
    struct nnpi_pnp_snapshot s;
    memset(&s,0,sizeof(s));
    s.boot_done=s.sysinfo_done=s.net_done=1;
    s.boot_may_release=s.sysinfo_may_release=s.net_may_release=1;
    return s;
}
static void idle_and_load_only(void)
{
    struct nnpi_pnp_snapshot s=idle();
    expect(&s,0); /* Idle or allocated/sealed image: no published DMA. */
    s.boot_may_release=s.sysinfo_may_release=s.net_may_release=0;
    s.boot_done=s.sysinfo_done=s.net_done=0;
    expect(&s,0); /* NEW, no start or ownership: release=false is irrelevant. */
}
static void workers(void)
{
    uint32_t bits;
    for (bits=0;bits<256;++bits) {
        struct nnpi_pnp_snapshot s=idle();
        uint32_t reasons=0;
        s.boot_started=bits&1; s.boot_active=bits&2; s.boot_done=bits&4;
        s.sysinfo_started=bits&8; s.sysinfo_done=bits&16;
        s.net_started=bits&32; s.net_active=bits&64; s.net_done=bits&128;
        if (s.boot_active || (s.boot_started && !s.boot_done)) reasons|=NNPI_PNP_BOOT_WORKER;
        if (s.sysinfo_started && !s.sysinfo_done) reasons|=NNPI_PNP_SYSINFO_WORKER;
        if (s.net_active || (s.net_started && !s.net_done)) reasons|=NNPI_PNP_NET_WORKER;
        expect(&s,reasons); /* Includes queued and active=0/Done=0 tail gaps. */
    }
}
static void session_ownership(void)
{
    uint32_t resource,started;
    for (started=0;started<2;++started) {
        for (resource=1;resource<=NNPI_BS_TABLE_DMA;resource<<=1) {
            struct nnpi_boot_session boot;
            struct nnpi_pnp_snapshot s=idle();
            nnpi_boot_session_init(&boot);
            boot.published_dma=resource;
            s.boot_started=started;
            s.boot_owned=boot.published_dma!=0;
            s.boot_may_release=nnpi_boot_session_may_release(&boot,NNPI_BS_ALL_DMA);
            expect(&s,NNPI_PNP_BOOT_OWNERSHIP);
            boot.stopped_dma=resource;
            s.boot_may_release=nnpi_boot_session_may_release(&boot,NNPI_BS_ALL_DMA);
            expect(&s,0);
        }
        for (resource=1;resource<=NNPI_NS_C2H_POOL;resource<<=1) {
            struct nnpi_net_session net;
            struct nnpi_pnp_snapshot s=idle();
            nnpi_net_session_init(&net);
            net.published_dma=resource;
            s.net_started=started;
            s.net_owned=net.published_dma!=0;
            s.net_may_release=nnpi_net_session_may_release(&net,NNPI_NS_ALL_DMA);
            expect(&s,NNPI_PNP_NET_OWNERSHIP);
            net.stopped_dma=resource;
            s.net_may_release=nnpi_net_session_may_release(&net,NNPI_NS_ALL_DMA);
            expect(&s,0);
        }
        for (resource=NNPI_SYSINFO_UNPUBLISHED;resource<=NNPI_SYSINFO_STOPPED_BY_RESET;++resource) {
            struct nnpi_sysinfo_response sysinfo;
            struct nnpi_pnp_snapshot s=idle();
            nnpi_sysinfo_init(&sysinfo); sysinfo.ownership=resource;
            s.sysinfo_started=started;
            s.sysinfo_owned=sysinfo.ownership!=NNPI_SYSINFO_UNPUBLISHED;
            s.sysinfo_may_release=nnpi_sysinfo_may_release(&sysinfo);
            expect(&s,(resource==NNPI_SYSINFO_PUBLISHED || resource==NNPI_SYSINFO_PROTOCOL_COMPLETE) ?
                NNPI_PNP_SYSINFO_OWNERSHIP : 0);
        }
    }
}
static void actual_publications(void)
{
    uint32_t bits,started;
    for (started=0;started<2;++started) for (bits=0;bits<64;++bits) {
        struct nnpi_pnp_snapshot s=idle();
        uint32_t reasons=0;
        s.boot_started=s.sysinfo_started=s.net_started=started;
        s.dma_published=bits&1; s.dma_stop_confirmed=bits&2;
        s.image_published=bits&4; s.image_stop_confirmed=bits&8;
        s.netdma_published=bits&16; s.netdma_stop_confirmed=bits&32;
        if (s.dma_published && !s.dma_stop_confirmed) reasons|=NNPI_PNP_DMA_UNSTOPPED;
        if (s.image_published && !s.image_stop_confirmed) reasons|=NNPI_PNP_IMAGE_UNSTOPPED;
        if (s.netdma_published && !s.netdma_stop_confirmed) reasons|=NNPI_PNP_NETDMA_UNSTOPPED;
        expect(&s,reasons); /* No session ledger can waive actual ownership. */
    }
}
static void contradictory_stop_evidence_and_all_safe(void)
{
    struct nnpi_pnp_snapshot s=idle();
    s.boot_started=s.sysinfo_started=s.net_started=1;
    s.boot_owned=s.sysinfo_owned=s.net_owned=1;
    s.dma_published=s.image_published=s.netdma_published=1;
    /* All workers done and sessions stopped, actual mappings NOT stopped. */
    expect(&s,NNPI_PNP_DMA_UNSTOPPED|NNPI_PNP_IMAGE_UNSTOPPED|NNPI_PNP_NETDMA_UNSTOPPED);
    s.dma_stop_confirmed=s.image_stop_confirmed=s.netdma_stop_confirmed=1;
    s.boot_may_release=s.sysinfo_may_release=s.net_may_release=0;
    /* Actual mappings stopped cannot waive unstopped session ownership. */
    expect(&s,NNPI_PNP_BOOT_OWNERSHIP|NNPI_PNP_SYSINFO_OWNERSHIP|NNPI_PNP_NET_OWNERSHIP);
    s.boot_may_release=s.sysinfo_may_release=s.net_may_release=1;
    expect(&s,0); /* All independent stop evidence present, workers complete. */
    s.boot_active=s.net_active=1; s.sysinfo_done=0;
    s.boot_may_release=s.sysinfo_may_release=s.net_may_release=0;
    s.dma_stop_confirmed=s.image_stop_confirmed=s.netdma_stop_confirmed=0;
    expect(&s,NNPI_PNP_BOOT_WORKER|NNPI_PNP_SYSINFO_WORKER|NNPI_PNP_NET_WORKER|
        NNPI_PNP_BOOT_OWNERSHIP|NNPI_PNP_SYSINFO_OWNERSHIP|NNPI_PNP_NET_OWNERSHIP|
        NNPI_PNP_DMA_UNSTOPPED|NNPI_PNP_IMAGE_UNSTOPPED|NNPI_PNP_NETDMA_UNSTOPPED);
}
int main(void)
{
    idle_and_load_only(); workers(); session_ownership(); actual_publications();
    contradictory_stop_evidence_and_all_safe();
    puts("PnP policy regressions passed (CPU-only)");
    return 0;
}
