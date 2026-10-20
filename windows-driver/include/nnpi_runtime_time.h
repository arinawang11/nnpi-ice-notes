/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RUNTIME_TIME_H
#define NNPI_RUNTIME_TIME_H
#include <stdint.h>
/* All timestamps use KeQueryInterruptTime's monotonic 100-ns units. */
#define NNPI_RUNTIME_SLOW_TICKS UINT64_C(5000000)
#define NNPI_RUNTIME_READY_TICKS UINT64_C(1000)
#define NNPI_RUNTIME_TIMEOUT_TICKS UINT64_C(100000000)
/* Net RX has no dedicated interrupt. While the data plane is busy the runtime
 * polls with a zero-timeout event check instead of a timed wait:
 * KeWaitForMultipleObjects rounds a 100us relative timeout up to the system
 * tick (1-2 ms on this host), which caps credit return to roughly one frame
 * per tick. The 20us stall measured 42k rounds/s and 25k frames/s, i.e. the
 * stall itself owned about 84% of every round, so busy rounds no longer stall.
 * The poll stops once the path has been quiet for qpc_frequency/10 ticks
 * (100 ms here), so an idle bridge does not keep a core busy. */
#define NNPI_RUNTIME_SPIN_US 0
struct nnpi_runtime_time {
    uint64_t negotiation_start;
    uint64_t keepalive_last;
    uint64_t partial_start;
    int partial_active;
};
static __inline void nnpi_runtime_time_init(struct nnpi_runtime_time *p, uint64_t now)
{
    p->negotiation_start=now; p->keepalive_last=now;
    p->partial_start=0; p->partial_active=0;
}
static __inline uint64_t nnpi_runtime_wait_ticks(int data_ready)
{
    return data_ready ? NNPI_RUNTIME_READY_TICKS : NNPI_RUNTIME_SLOW_TICKS;
}
static __inline void nnpi_runtime_partial_observe(struct nnpi_runtime_time *p,
                                                 uint64_t now, unsigned count)
{
    if (!count) { p->partial_active=0; p->partial_start=0; }
    else if (!p->partial_active) { p->partial_active=1; p->partial_start=now; }
}
static __inline int nnpi_runtime_time_expired(const struct nnpi_runtime_time *p,
                                             uint64_t now, int compatible)
{
    return (!compatible && now-p->negotiation_start>=NNPI_RUNTIME_TIMEOUT_TICKS) ||
        (p->partial_active && now-p->partial_start>=NNPI_RUNTIME_TIMEOUT_TICKS);
}
/* Consume one due transmission, not a backlog: delayed scheduling never bursts. */
static __inline int nnpi_runtime_keepalive_due(struct nnpi_runtime_time *p,
                                              uint64_t now, int compatible)
{
    if (!compatible || now-p->keepalive_last<NNPI_RUNTIME_SLOW_TICKS) return 0;
    p->keepalive_last=now;
    return 1;
}
#endif
