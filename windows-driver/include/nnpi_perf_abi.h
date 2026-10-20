/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_PERF_ABI_H
#define NNPI_PERF_ABI_H
#include <stdint.h>
#define NNPI_PERF_VERSION 1u
#define NNPI_PERF_QUERY_IOCTL 0x80006068u /* 0x81a BUFFERED READ; CPU-only */
struct nnpi_perf_request { uint32_t version, size, reserved[2]; };
/* Device-lifetime cumulative, CacheLock serialized, saturating counters. Times
 * are raw QPC ticks at qpc_frequency; no reset IOCTL. Bins: <50us,<250us,<2ms,
 * <10ms,>=10ms. Work budget is advisory, not a real-time execution bound. */
struct nnpi_perf_response {
    uint32_t version, size;
    uint64_t qpc_frequency;
    uint64_t wait_close, wait_work, wait_timeout, wait_error;
    uint64_t wait_bins[5], lock_bins[5];
    uint64_t rounds, empty_rounds, bursts, burst_round_limit, burst_time_limit;
    uint64_t rx_qwords, ack_sent, tx_sent, fifo_full, pending_at_limit;
    uint64_t tx_enqueued, tx_latency_count, tx_latency_ticks, tx_latency_max;
    uint64_t tx_pending_high_water;
};
typedef char nnpi_perf_request_size[(sizeof(struct nnpi_perf_request)==16)?1:-1];
typedef char nnpi_perf_response_size[(sizeof(struct nnpi_perf_response)==248)?1:-1];
static inline uint64_t nnpi_perf_add(uint64_t a,uint64_t b)
{ return UINT64_MAX-a<b?UINT64_MAX:a+b; }
static inline unsigned nnpi_perf_bin(uint64_t ticks,uint64_t frequency)
{
    if (ticks<frequency/20000) return 0;
    if (ticks<frequency/4000) return 1;
    if (ticks<frequency/500) return 2;
    if (ticks<frequency/100) return 3;
    return 4;
}
#endif
