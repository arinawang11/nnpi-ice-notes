/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_PNP_POLICY_H
#define NNPI_PNP_POLICY_H
#include <stdint.h>

/* Internal POLICY ONLY, not a public ABI. Capture every field under the same
 * lifecycle lock, then classify without closing admission, signaling, refreshing
 * stop evidence or touching hardware. Worker completion is NOT DMA stop proof. */
#define NNPI_PNP_BOOT_WORKER      (1U << 0)
#define NNPI_PNP_SYSINFO_WORKER   (1U << 1)
#define NNPI_PNP_NET_WORKER       (1U << 2)
#define NNPI_PNP_BOOT_OWNERSHIP   (1U << 3)
#define NNPI_PNP_SYSINFO_OWNERSHIP (1U << 4)
#define NNPI_PNP_NET_OWNERSHIP    (1U << 5)
#define NNPI_PNP_DMA_UNSTOPPED    (1U << 6)
#define NNPI_PNP_IMAGE_UNSTOPPED  (1U << 7)
#define NNPI_PNP_NETDMA_UNSTOPPED (1U << 8)

struct nnpi_pnp_snapshot {
    uint32_t boot_started, boot_active, boot_done;
    uint32_t sysinfo_started, sysinfo_done;
    uint32_t net_started, net_active, net_done;
    uint32_t boot_owned, boot_may_release;
    uint32_t sysinfo_owned, sysinfo_may_release;
    uint32_t net_owned, net_may_release;
    uint32_t dma_published, dma_stop_confirmed;
    uint32_t image_published, image_stop_confirmed;
    uint32_t netdma_published, netdma_stop_confirmed;
};

static inline uint32_t nnpi_pnp_query_veto_reasons(const struct nnpi_pnp_snapshot *s)
{
    uint32_t reasons=0;
    if (s->boot_active || (s->boot_started && !s->boot_done))
        reasons|=NNPI_PNP_BOOT_WORKER;
    if (s->sysinfo_started && !s->sysinfo_done)
        reasons|=NNPI_PNP_SYSINFO_WORKER;
    if (s->net_active || (s->net_started && !s->net_done))
        reasons|=NNPI_PNP_NET_WORKER;
    /* A NEW, never-started session without ownership is not a veto, even if
     * its release predicate says false. Ownership never depends on Started. */
    if ((s->boot_started || s->boot_owned) && !s->boot_may_release)
        reasons|=NNPI_PNP_BOOT_OWNERSHIP;
    if ((s->sysinfo_started || s->sysinfo_owned) && !s->sysinfo_may_release)
        reasons|=NNPI_PNP_SYSINFO_OWNERSHIP;
    if ((s->net_started || s->net_owned) && !s->net_may_release)
        reasons|=NNPI_PNP_NET_OWNERSHIP;
    /* Check actual mappings independently in both directions: session stop
     * cannot excuse an unstopped mapping, nor can mapping stop excuse session
     * ownership. Neither Started nor worker_done can bypass these guards. */
    if (s->dma_published && !s->dma_stop_confirmed)
        reasons|=NNPI_PNP_DMA_UNSTOPPED;
    if (s->image_published && !s->image_stop_confirmed)
        reasons|=NNPI_PNP_IMAGE_UNSTOPPED;
    if (s->netdma_published && !s->netdma_stop_confirmed)
        reasons|=NNPI_PNP_NETDMA_UNSTOPPED;
    return reasons;
}
#endif
