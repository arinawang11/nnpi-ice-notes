/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_MAINTENANCE_ABI_H
#define NNPI_MAINTENANCE_ABI_H
#include <stdint.h>
#define NNPI_MAINTENANCE_VERSION 1u
#define NNPI_MAINTENANCE_PREPARE_IOCTL 0x8000e078u
#define NNPI_MAINTENANCE_QUERY_IOCTL 0x8000607cu
#define NNPI_MAINT_IDLE 0u
#define NNPI_MAINT_DRAINING 1u
#define NNPI_MAINT_RESET_PENDING 2u
#define NNPI_MAINT_RETAINED 3u
#define NNPI_MAINT_QUIESCED 4u
#define NNPI_MAINT_STOPPED 1u
#define NNPI_MAINT_BULK_RELEASED 2u
#define NNPI_MAINT_BOOT_RELEASED 4u
#define NNPI_MAINT_NET_RELEASED 8u
#define NNPI_MAINT_PRODUCERS_STOPPED 16u
/* Producers no longer use context/MMIO; work-item device/image reference and
 * rundown still protect callback tails. This is NOT callback-return evidence. */
#define NNPI_MAINT_REFERENCES_PROTECTED 32u
#define NNPI_MAINT_PNP_SAFE 64u
#define NNPI_MAINT_ALL_EVIDENCE 127u
/* QUERY: all fields after size zero. PREPARE: exact observed identity and a
 * caller-generated nonzero attempt. No DMA addresses, no reset parameters. */
struct nnpi_maintenance_request {
 uint32_t version,size;
 uint64_t session,generation,epoch,attempt;
 uint32_t reserved[2];
};
struct nnpi_maintenance_response {
 uint32_t version,size;
 uint64_t session,generation,epoch,attempt;
 uint32_t state,reason,evidence,reset_claims;
 uint32_t reset_returned,reset_status,boot_owned,net_owned;
 uint32_t bulk_owned,producer_active,provider_retained,physical_reclaim_observed;
 uint32_t reserved[4];
};
typedef char nnpi_maintenance_request_layout[sizeof(struct nnpi_maintenance_request)==48?1:-1];
typedef char nnpi_maintenance_response_layout[sizeof(struct nnpi_maintenance_response)==104?1:-1];
#endif
