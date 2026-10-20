/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_BULK_ABI_H
#define NNPI_BULK_ABI_H
#include <stdint.h>
#define NNPI_BULK_ABI_VERSION 4u
#define NNPI_BULK_BEGIN_IOCTL 0x8000e070u /* 0x81c buffered read/write */
#define NNPI_BULK_QUERY_IOCTL 0x80006074u /* 0x81d buffered read */
/* v4 only: H2C repeated slots or C2H unique 16MiB target; no addresses.
 * Both directions accept chunk=65536/262144/1048576 and QD=1/4.
 * reserved bit0 selects FAST sentinel verification; other bits must be zero.
 * QUERY fields after size must be zero. Reject all historical ABI versions. */
struct nnpi_bulk_request {
 uint32_t version,size;
 uint64_t session,generation,epoch,run;
 uint32_t chunk_bytes,queue_depth;
 uint32_t direction,reserved; /* 0 H2C, 1 C2H; reserved bit0 FAST */
};
struct nnpi_bulk_response {
 uint32_t version,size;
 /* Host wall is BEGIN-to-EXEC-response (includes channel/map/control).
  * It is deliberately not DMA-only duration or PCIe bandwidth. */
 uint64_t session,generation,epoch,run,host_wall_100ns;
 uint32_t phase,fault,published,mapped,completed_bytes,card_us;
 uint32_t exec_status,completion_seen,stop_seen,image_approved;
 uint32_t channel_issued,channel_destroyed;
};
#define NNPI_BULK_QUERY_VERSION 4u
/* Historical struct name retained internally; card wire version is 4. */
struct nnpi_bulk_response_v2 {
 struct nnpi_bulk_response base;
 uint32_t fault_from_phase,destroy_sent,destroy_seen,channel;
 uint64_t exec_submit_100ns,exec_complete_100ns,exec_wall_100ns,cleanup_wall_100ns;
 uint64_t phase_elapsed_100ns[15];
 uint64_t card_setup_ns,card_window_ns,card_verify_ns;
 uint32_t chunk_bytes,queue_depth,total_bytes,stats_seen;
 uint32_t direction,verified_bytes,verify_status,first_bad_offset;
 uint32_t expected_byte,actual_byte,buffer_ownership_released,physical_reclaim_observed;
 uint64_t host_verify_100ns,end_to_end_100ns;
};
typedef char nnpi_bulk_response_v2_size[(sizeof(struct nnpi_bulk_response_v2)==352)?1:-1];
typedef char nnpi_bulk_request_size[(sizeof(struct nnpi_bulk_request)==56)?1:-1];
typedef char nnpi_bulk_response_size[(sizeof(struct nnpi_bulk_response)==96)?1:-1];
#endif
