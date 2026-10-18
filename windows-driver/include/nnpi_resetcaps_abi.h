/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RESETCAPS_ABI_H
#define NNPI_RESETCAPS_ABI_H
#include <stdint.h>
#include <stddef.h>
#define NNPI_RESETCAPS_VERSION 1U
#define NNPI_RESETCAPS_IOCTL 0x80006014U
/* WDK DEVICE_RESET_TYPE ordinals are bit POSITIONS, not masks. */
#define NNPI_RESETCAPS_FUNCTION_LEVEL (1U << 0)
#define NNPI_RESETCAPS_PLATFORM_LEVEL (1U << 1)
#define NNPI_RESETCAPS_KNOWN_TYPES 3U
struct nnpi_resetcaps_request { uint32_t version, size, reserved[2]; };
/* No interface context, callable pointers, addresses or new WDK tail fields. */
struct nnpi_resetcaps_response {
    uint32_t version, size;
    uint64_t generation;
    uint32_t requested_size, requested_version, query_ntstatus;
    uint32_t returned_size, returned_version, supported_reset_types;
    uint32_t supported, handler_present, contract_valid, reserved[3];
};
#define NNPI_RESET_ASSERT(n, e) typedef char nnpi_reset_assert_##n[(e) ? 1 : -1]
NNPI_RESET_ASSERT(request, sizeof(struct nnpi_resetcaps_request) == 16);
NNPI_RESET_ASSERT(response, sizeof(struct nnpi_resetcaps_response) == 64);
NNPI_RESET_ASSERT(generation, offsetof(struct nnpi_resetcaps_response, generation) == 8);
NNPI_RESET_ASSERT(requested, offsetof(struct nnpi_resetcaps_response, requested_size) == 16);
NNPI_RESET_ASSERT(reserved, offsetof(struct nnpi_resetcaps_response, reserved) == 52);
#undef NNPI_RESET_ASSERT
static inline int nnpi_resetcaps_request_valid(const struct nnpi_resetcaps_request *r, size_t n)
{
    return r && n == sizeof(*r) && r->version == NNPI_RESETCAPS_VERSION &&
        r->size == sizeof(*r) && !r->reserved[0] && !r->reserved[1];
}
static inline int nnpi_resetcaps_buffers_valid(const struct nnpi_resetcaps_request *r, size_t in, size_t out)
{
    return nnpi_resetcaps_request_valid(r, in) && out == sizeof(struct nnpi_resetcaps_response);
}
static inline int nnpi_resetcaps_response_valid(const struct nnpi_resetcaps_response *r, size_t n)
{
    uint32_t supported;
    if (!r || n != sizeof(*r) || r->version != NNPI_RESETCAPS_VERSION ||
        r->size != sizeof(*r) || !r->generation || !r->requested_size ||
        r->requested_size > UINT16_MAX || r->requested_version != 1U ||
        r->returned_size > UINT16_MAX || r->returned_version > UINT16_MAX ||
        r->supported > 1U || r->handler_present > 1U || r->contract_valid > 1U ||
        r->reserved[0] || r->reserved[1] || r->reserved[2]) return 0;
    if (r->query_ntstatus & 0x80000000U)
        return !r->returned_size && !r->returned_version && !r->supported_reset_types &&
            !r->supported && !r->handler_present && !r->contract_valid;
    if (!r->contract_valid)
        return !r->supported_reset_types && !r->handler_present && !r->supported;
    if (r->returned_size < r->requested_size || r->returned_version != r->requested_version) return 0;
    supported = (r->handler_present && (r->supported_reset_types & NNPI_RESETCAPS_KNOWN_TYPES)) ? 1U : 0U;
    return r->supported == supported;
}
#endif
