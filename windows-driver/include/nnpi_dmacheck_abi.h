/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_DMACHECK_ABI_H
#define NNPI_DMACHECK_ABI_H
#include <stdint.h>
#include <stddef.h>
#define NNPI_DMACHECK_VERSION 1U
#define NNPI_DMACHECK_IOCTL 0x80006010U
#define NNPI_DMACHECK_BYTES 4096U
#define NNPI_DMACHECK_ALIGNMENT 4095U
/* ABI identifier, not the numerical value of WDF_DMA_PROFILE. */
#define NNPI_DMACHECK_PROFILE_SG64 1U
#define NNPI_DMACHECK_ATTRIBUTES 3U /* CPU-only test, address never published */
#define NNPI_DMACHECK_ENABLER 1U
#define NNPI_DMACHECK_ALLOCATED 2U
#define NNPI_DMACHECK_LAYOUT 4U
#define NNPI_DMACHECK_INITIAL_ZERO 8U
#define NNPI_DMACHECK_PATTERN 16U
#define NNPI_DMACHECK_FINAL_ZERO 32U
#define NNPI_DMACHECK_ALL 63U
enum nnpi_dmacheck_stage {
    NNPI_DMA_ENABLER = 1, NNPI_DMA_ALLOCATE, NNPI_DMA_VIRTUAL_ADDRESS,
    NNPI_DMA_LENGTH, NNPI_DMA_ALIGNMENT, NNPI_DMA_RANGE,
    NNPI_DMA_INITIAL_ZERO, NNPI_DMA_PATTERN, NNPI_DMA_FINAL_ZERO, NNPI_DMA_READY
};
struct nnpi_dmacheck_request { uint32_t version, size, reserved[2]; };
/* No handles, virtual, physical or DMA logical addresses on this wire. */
struct nnpi_dmacheck_response {
    uint32_t version, size;
    uint64_t generation, actual_bytes;
    uint32_t attributes, profile, requested_bytes, alignment_mask;
    uint32_t checks, stage, ntstatus, passed, retained, reserved;
};
#define NNPI_DMA_ASSERT(n, e) typedef char nnpi_dma_assert_##n[(e) ? 1 : -1]
NNPI_DMA_ASSERT(request, sizeof(struct nnpi_dmacheck_request) == 16);
NNPI_DMA_ASSERT(response, sizeof(struct nnpi_dmacheck_response) == 64);
NNPI_DMA_ASSERT(actual, offsetof(struct nnpi_dmacheck_response, actual_bytes) == 16);
NNPI_DMA_ASSERT(checks, offsetof(struct nnpi_dmacheck_response, checks) == 40);
NNPI_DMA_ASSERT(reserved, offsetof(struct nnpi_dmacheck_response, reserved) == 60);
#undef NNPI_DMA_ASSERT
static inline int nnpi_dmacheck_request_valid(const struct nnpi_dmacheck_request *r, size_t n)
{
    return r && n == sizeof(*r) && r->version == NNPI_DMACHECK_VERSION &&
        r->size == sizeof(*r) && !r->reserved[0] && !r->reserved[1];
}
static inline int nnpi_dmacheck_buffers_valid(const struct nnpi_dmacheck_request *r, size_t in, size_t out)
{
    return nnpi_dmacheck_request_valid(r, in) && out == sizeof(struct nnpi_dmacheck_response);
}
static inline const char *nnpi_dmacheck_stage_name(uint32_t stage)
{
    switch (stage) {
    case NNPI_DMA_ENABLER: return "enabler";
    case NNPI_DMA_ALLOCATE: return "allocate";
    case NNPI_DMA_VIRTUAL_ADDRESS: return "virtual-address";
    case NNPI_DMA_LENGTH: return "length";
    case NNPI_DMA_ALIGNMENT: return "alignment";
    case NNPI_DMA_RANGE: return "range";
    case NNPI_DMA_INITIAL_ZERO: return "initial-zero";
    case NNPI_DMA_PATTERN: return "pattern";
    case NNPI_DMA_FINAL_ZERO: return "final-zero";
    case NNPI_DMA_READY: return "ready";
    default: return "unknown";
    }
}
static inline int nnpi_dmacheck_response_valid(const struct nnpi_dmacheck_response *r, size_t n)
{
    uint32_t expected;
    if (!r || n != sizeof(*r) || r->version != NNPI_DMACHECK_VERSION ||
        r->size != sizeof(*r) || !r->generation ||
        r->attributes != NNPI_DMACHECK_ATTRIBUTES || r->profile != NNPI_DMACHECK_PROFILE_SG64 ||
        r->requested_bytes != NNPI_DMACHECK_BYTES || r->alignment_mask != NNPI_DMACHECK_ALIGNMENT ||
        r->reserved || r->stage < NNPI_DMA_ENABLER || r->stage > NNPI_DMA_READY) return 0;
    if (r->stage == NNPI_DMA_READY)
        return !r->ntstatus && r->passed == 1 && r->retained == 1 &&
            r->checks == NNPI_DMACHECK_ALL && r->actual_bytes == NNPI_DMACHECK_BYTES;
    if (!(r->ntstatus & 0x80000000U) || r->passed || r->retained) return 0;
    expected = r->stage == NNPI_DMA_ENABLER ? 0U :
        r->stage == NNPI_DMA_ALLOCATE ? 1U :
        r->stage <= NNPI_DMA_RANGE ? 3U :
        r->stage == NNPI_DMA_INITIAL_ZERO ? 7U :
        r->stage == NNPI_DMA_PATTERN ? 15U : 31U;
    if (r->checks != expected) return 0;
    if (r->stage <= NNPI_DMA_ALLOCATE && r->actual_bytes) return 0;
    if (r->stage == NNPI_DMA_LENGTH && r->actual_bytes == NNPI_DMACHECK_BYTES) return 0;
    if (r->stage >= NNPI_DMA_ALIGNMENT && r->actual_bytes != NNPI_DMACHECK_BYTES) return 0;
    return 1;
}
#endif
