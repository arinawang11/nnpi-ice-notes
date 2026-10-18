/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_STATUS_ABI_H
#define NNPI_STATUS_ABI_H
#include <stdint.h>
#include <stddef.h>
#define NNPI_STATUS_VERSION 1U
#define NNPI_STATUS_IOCTL 0x80006008U /* 0x802, METHOD_BUFFERED, FILE_READ_ACCESS */
#define NNPI_STATUS_ATTRIBUTES 0x0fU /* MMIO, single read, read-only, noncached/NX */
struct nnpi_status_request { uint32_t version, size, reserved[2]; };
struct nnpi_status_response {
    uint32_t version, size;
    uint64_t generation;
    uint32_t valid, attributes, value, read_count;
    uint32_t reserved[4];
};
#define NNPI_STATUS_ASSERT(n, e) typedef char nnpi_status_assert_##n[(e) ? 1 : -1]
NNPI_STATUS_ASSERT(request, sizeof(struct nnpi_status_request) == 16);
NNPI_STATUS_ASSERT(response, sizeof(struct nnpi_status_response) == 48);
NNPI_STATUS_ASSERT(generation, offsetof(struct nnpi_status_response, generation) == 8);
NNPI_STATUS_ASSERT(value, offsetof(struct nnpi_status_response, value) == 24);
NNPI_STATUS_ASSERT(reserved, offsetof(struct nnpi_status_response, reserved) == 32);
#undef NNPI_STATUS_ASSERT
static inline int nnpi_status_request_valid(const struct nnpi_status_request *r, size_t bytes)
{
    return r && bytes == sizeof(*r) && r->version == NNPI_STATUS_VERSION &&
        r->size == sizeof(*r) && !r->reserved[0] && !r->reserved[1];
}
/* Exact output size is part of v1, unlike legacy INFO's minimum-size rule. */
static inline int nnpi_status_buffers_valid(const struct nnpi_status_request *r,
                                           size_t input_bytes, size_t output_bytes)
{
    return nnpi_status_request_valid(r, input_bytes) &&
        output_bytes == sizeof(struct nnpi_status_response);
}
static inline int nnpi_status_response_valid(const struct nnpi_status_response *r, size_t bytes)
{
    uint32_t i;
    if (!r || bytes != sizeof(*r) || r->version != NNPI_STATUS_VERSION ||
        r->size != sizeof(*r) || !r->generation || r->valid != 1 ||
        r->attributes != NNPI_STATUS_ATTRIBUTES || r->read_count != 1) return 0;
    for (i = 0; i < 4; ++i) if (r->reserved[i]) return 0;
    return 1; /* Zero and all-ones are data, NOT health/boot verdicts. */
}
#endif
