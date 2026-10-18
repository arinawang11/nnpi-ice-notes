/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_NET_DATA_ABI_H
#define NNPI_NET_DATA_ABI_H
#include <stdint.h>
#include <stddef.h>
#define NNPI_NET_DATA_VERSION 1u
#define NNPI_NET_DATA_QUERY_IOCTL 0x80006054u /* 0x815 BUFFERED READ */
#define NNPI_NET_TX_IOCTL 0x8000e058u /* 0x816 BUFFERED READ|WRITE */
#define NNPI_NET_RX_QUEUE_DEPTH 4u
/* Independent RX ABI; legacy diagnostic/TX layouts and version stay unchanged.
 * Exact sizes are required. All reserved fields and unused output bytes are zero.
 * A claim belongs to the file object, not PID or numeric user handle. It excludes
 * other RX consumers AND TX senders. Same-file repeated claim is idempotent.
 * New claims require no outstanding TX; legacy TX remains usable without claim.
 * RX is nonblocking: success/count=0 means empty. Completion loss is packet loss,
 * never permission to replay. Cleanup revokes only the CPU consumer, not DMA.
 */
#define NNPI_NET_RX_VERSION 1u
#define NNPI_NET_RX_CLAIM_IOCTL 0x8000e05cu
#define NNPI_NET_RX_RELEASE_IOCTL 0x8000e060u
#define NNPI_NET_RX_BATCH_IOCTL 0x8000e064u
#define NNPI_NET_RX_FRAME_BYTES 4096u
struct nnpi_net_rx_claim_request {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint32_t reserved[4];
};
struct nnpi_net_rx_claim_response {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint32_t claimed, queue_depth, max_frame_bytes, reserved;
};
struct nnpi_net_rx_batch_request {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint32_t max_frames, reserved[3];
};
struct nnpi_net_rx_frame {
    uint32_t length, reserved;
    uint8_t frame[NNPI_NET_RX_FRAME_BYTES];
};
struct nnpi_net_rx_batch_response {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint32_t count, remaining, reserved[2];
    struct nnpi_net_rx_frame frames[NNPI_NET_RX_QUEUE_DEPTH];
};
typedef char nnpi_rx_claim_req_size[(sizeof(struct nnpi_net_rx_claim_request)==48)?1:-1];
typedef char nnpi_rx_claim_resp_size[(sizeof(struct nnpi_net_rx_claim_response)==48)?1:-1];
typedef char nnpi_rx_batch_req_size[(sizeof(struct nnpi_net_rx_batch_request)==48)?1:-1];
typedef char nnpi_rx_frame_size[(sizeof(struct nnpi_net_rx_frame)==4104)?1:-1];
typedef char nnpi_rx_batch_resp_size[(sizeof(struct nnpi_net_rx_batch_response)==16464)?1:-1];
typedef char nnpi_rx_batch_frame_offset[(offsetof(struct nnpi_net_rx_batch_response,frames)==48)?1:-1];
struct nnpi_net_data_request { uint32_t version, size, reserved[2]; };
/* Cached counters only, never DMA addresses or packet contents. No implied link. */
struct nnpi_net_data_response {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint64_t rx_received, rx_bytes, rx_dropped, rx_bad_length, rx_ack_sent;
    uint64_t tx_submitted, tx_acked, invalid_descriptor, unknown_ack, duplicate_rx, fifo_full;
    uint32_t rx_ack_pending, tx_busy, rx_queued, fault_retained, last_emit_halves, tx_pending;
    uint32_t reserved[4];
};
/* Single explicit frame submission; no automatic probe/retry. Buffer includes a
 * zero tail. Accepted is queued, NOT card ACK or delivery. No sequence on wire. */
struct nnpi_net_tx_request {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint32_t length, reserved[3];
    uint8_t frame[4096];
};
struct nnpi_net_tx_response {
    uint32_t version, size;
    uint64_t session_id, generation, epoch;
    uint32_t accepted, slot, reserved[2];
};
typedef char nnpi_data_size[(sizeof(struct nnpi_net_data_response)==160)?1:-1];
typedef char nnpi_tx_req_size[(sizeof(struct nnpi_net_tx_request)==4144)?1:-1];
typedef char nnpi_tx_resp_size[(sizeof(struct nnpi_net_tx_response)==48)?1:-1];
static inline int nnpi_net_data_response_valid(const struct nnpi_net_data_response *r,size_t bytes)
{
    return r && bytes==sizeof(*r) && r->version==NNPI_NET_DATA_VERSION && r->size==sizeof(*r) &&
        r->session_id!=UINT64_MAX && r->generation!=UINT64_MAX && r->epoch!=UINT64_MAX &&
        r->rx_ack_pending<=16 && r->tx_busy<=16 && r->tx_pending<=r->tx_busy &&
        r->rx_queued<=NNPI_NET_RX_QUEUE_DEPTH && r->fault_retained<=1 && r->last_emit_halves<=2 &&
        !r->reserved[0] && !r->reserved[1] && !r->reserved[2] && !r->reserved[3];
}
#endif
