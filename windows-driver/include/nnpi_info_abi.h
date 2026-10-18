/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_WIN_INFO_ABI_H
#define NNPI_WIN_INFO_ABI_H
#include <stdint.h>
#include <stddef.h>

#define NNPI_INFO_VERSION 1U
#define NNPI_INFO_DEVICE_TYPE 0x8000U
#define NNPI_INFO_IOCTL ((NNPI_INFO_DEVICE_TYPE << 16) | (1U << 14) | (0x800U << 2))
#define NNPI_INFO_MAX_RESOURCES 16U
#define NNPI_INFO_ID_PNP_HARDWARE_IDS 1U
#define NNPI_INFO_FLAG_PREPARED 1U
#define NNPI_INFO_FLAG_D0 2U
#define NNPI_INFO_RESOURCE_ADDRESS 1U
#define NNPI_INFO_RESOURCE_PORT 1U
#define NNPI_INFO_RESOURCE_INTERRUPT 2U
#define NNPI_INFO_RESOURCE_MEMORY 3U
/* System-defined private descriptor; opaque, not a memory address range. */
#define NNPI_INFO_RESOURCE_DEVICE_PRIVATE 0x81U

struct nnpi_info_request {
    uint32_t version;
    uint32_t size;
    uint32_t reserved[2];
};
struct nnpi_info_resource {
    uint32_t translated_resource_index;
    uint32_t type;
    uint32_t flags;
    uint32_t valid;
    uint64_t start;
    uint64_t length;
};
struct nnpi_info_response {
    uint32_t version;
    uint32_t size;
    uint32_t driver_version;
    uint32_t flags;
    uint32_t vendor;
    uint32_t device;
    uint32_t identity_source;
    uint32_t resource_count;
    uint64_t generation;
    uint32_t reserved[6];
    struct nnpi_info_resource resources[NNPI_INFO_MAX_RESOURCES];
};
#define NNPI_ASSERT(name, expr) typedef char nnpi_assert_##name[(expr) ? 1 : -1]
NNPI_ASSERT(request_size, sizeof(struct nnpi_info_request) == 16);
NNPI_ASSERT(resource_size, sizeof(struct nnpi_info_resource) == 32);
NNPI_ASSERT(resource_start, offsetof(struct nnpi_info_resource, start) == 16);
NNPI_ASSERT(resource_length, offsetof(struct nnpi_info_resource, length) == 24);
NNPI_ASSERT(response_size, sizeof(struct nnpi_info_response) == 576);
NNPI_ASSERT(generation_offset, offsetof(struct nnpi_info_response, generation) == 32);
NNPI_ASSERT(reserved_offset, offsetof(struct nnpi_info_response, reserved) == 40);
NNPI_ASSERT(resources_offset, offsetof(struct nnpi_info_response, resources) == 64);
NNPI_ASSERT(ioctl_code, NNPI_INFO_IOCTL == 0x80006000U);
#undef NNPI_ASSERT

/* Shared pure validation; no WDK types, physical access or device operations. */
static inline int nnpi_info_request_valid(const struct nnpi_info_request *r, size_t bytes)
{
    return r && bytes == sizeof(*r) && r->version == NNPI_INFO_VERSION &&
           r->size == sizeof(*r) && !r->reserved[0] && !r->reserved[1];
}
static inline int nnpi_info_response_valid(const struct nnpi_info_response *r, size_t bytes)
{
    unsigned int i;
    if (!r || bytes != sizeof(*r) || r->version != NNPI_INFO_VERSION ||
        r->size != sizeof(*r) || r->driver_version != 1U ||
        (r->flags & ~(NNPI_INFO_FLAG_PREPARED | NNPI_INFO_FLAG_D0)) ||
        !(r->flags & NNPI_INFO_FLAG_PREPARED) || r->vendor != 0x8086U || r->device != 0x45c6U ||
        r->identity_source != NNPI_INFO_ID_PNP_HARDWARE_IDS || !r->generation ||
        r->resource_count > NNPI_INFO_MAX_RESOURCES)
        return 0;
    for (i = 0; i < 6; ++i)
        if (r->reserved[i]) return 0;
    for (i = 0; i < NNPI_INFO_MAX_RESOURCES; ++i) {
        const struct nnpi_info_resource *e = &r->resources[i];
        if (i >= r->resource_count) {
            if (e->translated_resource_index || e->type || e->flags || e->valid || e->start || e->length)
                return 0;
            continue;
        }
        if (e->translated_resource_index != i || e->flags > 0xffffU)
            return 0;
        if (e->type == NNPI_INFO_RESOURCE_MEMORY || e->type == NNPI_INFO_RESOURCE_PORT) {
            if (e->valid != NNPI_INFO_RESOURCE_ADDRESS || !e->length ||
                e->start > UINT64_MAX - (e->length - 1)) return 0;
        } else if (e->type == NNPI_INFO_RESOURCE_INTERRUPT ||
                   e->type == NNPI_INFO_RESOURCE_DEVICE_PRIVATE) {
            if (e->valid || e->start || e->length) return 0;
        } else return 0;
    }
    return 1;
}
#endif
