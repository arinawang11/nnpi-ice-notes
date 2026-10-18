/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RESET_ABI_H
#define NNPI_RESET_ABI_H
#include <stdint.h>
#include <stddef.h>
#define NNPI_RESET_VERSION 1U
#define NNPI_RESET_SUBMIT_IOCTL 0x8000e018U
#define NNPI_RESET_QUERY_IOCTL 0x8000601cU
#define NNPI_RESET_REVALIDATE_IOCTL 0x8000e020U
#define NNPI_RESET_CONFIRM 0x46524c31U
#define NNPI_RESET_IDLE 0U
#define NNPI_RESET_ACTIVE 1U
#define NNPI_RESET_COMPLETE 2U
#define NNPI_RESET_FAILED 3U
#define NNPI_RESET_PENDING 0x00000103U
struct nnpi_reset_request {
    uint32_t version, size;
    uint64_t generation, epoch;
    uint32_t confirmation, reserved;
};
struct nnpi_reset_response {
    uint32_t version, size;
    uint64_t operation_id, generation, epoch;
    uint64_t submitted_100ns, returned_100ns, completed_100ns;
    uint32_t state, call_returned, call_status, callback_seen;
    uint32_t completion_status, callback_count, epoch_invalidated, revalidated;
    uint32_t hardware_blocked, reserved;
};
#define NNPI_RST_ASSERT(n,e) typedef char nnpi_rst_assert_##n[(e)?1:-1]
NNPI_RST_ASSERT(request, sizeof(struct nnpi_reset_request)==32);
NNPI_RST_ASSERT(response, sizeof(struct nnpi_reset_response)==96);
NNPI_RST_ASSERT(state, offsetof(struct nnpi_reset_response,state)==56);
#undef NNPI_RST_ASSERT
static inline int nnpi_reset_request_valid(const struct nnpi_reset_request *r, size_t n, int submit)
{
    return r && n==sizeof(*r) && r->version==NNPI_RESET_VERSION && r->size==sizeof(*r) &&
        !r->reserved && (submit ? (r->confirmation==NNPI_RESET_CONFIRM && r->generation && r->epoch) :
        (!r->confirmation && !r->generation && !r->epoch));
}
static inline int nnpi_reset_response_valid(const struct nnpi_reset_response *r, size_t n)
{
    return r && n==sizeof(*r) && r->version==NNPI_RESET_VERSION && r->size==sizeof(*r) &&
        r->state<=NNPI_RESET_FAILED && r->call_returned<=1 && r->callback_seen<=1 &&
        r->epoch_invalidated<=1 && r->revalidated<=1 && r->hardware_blocked<=1 && !r->reserved;
}
#endif
