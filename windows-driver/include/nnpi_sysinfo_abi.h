/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_SYSINFO_ABI_H
#define NNPI_SYSINFO_ABI_H
#include <stdint.h>
#include <stddef.h>
#include "nnpi_reset_abi.h"
#define NNPI_SYSINFO_VERSION 2U
#define NNPI_SYSINFO_RESET_RETURN_ONLY 1U
#define NNPI_SYSINFO_SUBMIT_IOCTL 0x8000e02cU
#define NNPI_SYSINFO_QUERY_IOCTL 0x80006030U
#define NNPI_SYSINFO_CONFIRM 0x53594931U
#define NNPI_SYSINFO_BYTES 4096U
#define NNPI_SYSINFO_PAYLOAD_BYTES 123U
#define NNPI_SYSINFO_IDLE 0U
#define NNPI_SYSINFO_OBSERVING 1U
#define NNPI_SYSINFO_CLEANUP 2U
#define NNPI_SYSINFO_FINISHED 3U
#define NNPI_SYSINFO_STOP_UNCONFIRMED 4U
#define NNPI_SYSINFO_UNPUBLISHED 0U
#define NNPI_SYSINFO_PUBLISHED 1U
#define NNPI_SYSINFO_PROTOCOL_COMPLETE 2U
#define NNPI_SYSINFO_STOPPED_BY_RESET 3U
#define NNPI_SYSINFO_PARSE_NOT_RUN 0U
#define NNPI_SYSINFO_PARSE_OK 1U
#define NNPI_SYSINFO_PARSE_LENGTH 2U
#define NNPI_SYSINFO_PARSE_VERSION 3U
#define NNPI_SYSINFO_PARSE_STRING 4U
struct nnpi_sysinfo_request {
    uint32_t version, size;
    uint64_t generation, epoch;
    uint32_t confirmation, reserved;
};
/* Fixed cache-only response, no pointers or DMA addresses. reset belongs to
 * publication_id even after epoch invalidation; reset success grants no MMIO. */
struct nnpi_sysinfo_response {
    uint32_t version, size;
    uint64_t publication_id, generation, epoch;
    uint32_t phase, ownership, writes_attempted, observations;
    uint32_t ready_seen, payload_valid, parse_result, epoch_invalidated;
    uint32_t budget_consumed, cleanup_reserved, page_retained, timed_out;
    uint32_t card_before, card_last, command_before, reset_mode;
    struct nnpi_reset_response reset;
    uint8_t payload[NNPI_SYSINFO_PAYLOAD_BYTES];
    uint8_t padding[5];
};
#define NNPI_SI_ASSERT(n,e) typedef char nnpi_si_assert_##n[(e)?1:-1]
NNPI_SI_ASSERT(request,sizeof(struct nnpi_sysinfo_request)==32);
NNPI_SI_ASSERT(response,sizeof(struct nnpi_sysinfo_response)==320);
NNPI_SI_ASSERT(reset,offsetof(struct nnpi_sysinfo_response,reset)==96);
NNPI_SI_ASSERT(payload,offsetof(struct nnpi_sysinfo_response,payload)==192);
#undef NNPI_SI_ASSERT
static inline int nnpi_sysinfo_request_valid(const struct nnpi_sysinfo_request *r,size_t n,int submit)
{
    return r && n==sizeof(*r) && r->version==NNPI_SYSINFO_VERSION && r->size==sizeof(*r) &&
        !r->reserved && (submit ? (r->confirmation==NNPI_SYSINFO_CONFIRM && r->generation && r->epoch) :
        (!r->confirmation && !r->generation && !r->epoch));
}
static inline int nnpi_sysinfo_response_valid(const struct nnpi_sysinfo_response *r,size_t n)
{
    size_t i;
    if (!r || n!=sizeof(*r) || r->version!=NNPI_SYSINFO_VERSION || r->size!=sizeof(*r) ||
        r->phase>NNPI_SYSINFO_STOP_UNCONFIRMED || r->ownership>NNPI_SYSINFO_STOPPED_BY_RESET ||
        r->writes_attempted>6 || r->observations>20 || r->ready_seen>1 || r->payload_valid>1 ||
        r->parse_result>NNPI_SYSINFO_PARSE_STRING || r->epoch_invalidated>1 || r->budget_consumed>1 ||
        r->cleanup_reserved>1 || r->page_retained>1 || r->timed_out>1 || r->reset_mode!=NNPI_SYSINFO_RESET_RETURN_ONLY ||
        !nnpi_reset_response_valid(&r->reset,sizeof(r->reset))) return 0;
    for (i=0;i<sizeof(r->padding);++i) if (r->padding[i]) return 0;
    return 1;
}
#endif
