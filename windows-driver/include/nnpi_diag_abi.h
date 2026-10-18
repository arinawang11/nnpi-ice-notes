/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_DIAG_ABI_H
#define NNPI_DIAG_ABI_H
#include "nnpi_bar_match.h"
#define NNPI_DIAG_VERSION 1U
#define NNPI_DIAG_IOCTL 0x80006004U /* function 0x801, BUFFERED, READ_ACCESS */
#define NNPI_DIAG_PREPARED_D0 3U
struct nnpi_diag_request { uint32_t version, size, reserved[2]; };
/* PCI config captured during PrepareHardware. IOCTL returns a CPU-only cache.
 * No MMIO/status values exist in this ABI. A successful association is NOT
 * permission to map/access hardware and is NOT boot/SSH readiness.
 */
struct nnpi_diag_response {
    uint32_t version, size, flags, reason;
    uint32_t config_bytes, pci_status, raw_count, translated_count;
    uint64_t generation;
    uint32_t bar_count, reserved[5];
    uint8_t config[64];
    struct nnpi_bar_entry bars[6];
};
#define NNPI_DIAG_ASSERT(n, e) typedef char nnpi_diag_assert_##n[(e) ? 1 : -1]
NNPI_DIAG_ASSERT(request, sizeof(struct nnpi_diag_request) == 16);
NNPI_DIAG_ASSERT(entry, sizeof(struct nnpi_bar_entry) == 48);
NNPI_DIAG_ASSERT(response, sizeof(struct nnpi_diag_response) == 416);
NNPI_DIAG_ASSERT(config, offsetof(struct nnpi_diag_response, config) == 64);
NNPI_DIAG_ASSERT(bars, offsetof(struct nnpi_diag_response, bars) == 128);
#undef NNPI_DIAG_ASSERT
static inline int nnpi_diag_request_valid(const struct nnpi_diag_request *r, size_t bytes)
{
    return r && bytes == sizeof(*r) && r->version == NNPI_DIAG_VERSION &&
        r->size == sizeof(*r) && !r->reserved[0] && !r->reserved[1];
}
static inline int nnpi_diag_response_valid(const struct nnpi_diag_response *r, size_t bytes)
{
    uint32_t i, j;
    struct nnpi_bar_resource raw[NNPI_BAR_MAX_RESOURCES] = {{0, 0, 0, 0}};
    struct nnpi_bar_resource trans[NNPI_BAR_MAX_RESOURCES] = {{0, 0, 0, 0}};
    struct nnpi_bar_entry expected[6];
    uint32_t count = 0, seen = 0;
    if (!r || bytes != sizeof(*r) || r->version != NNPI_DIAG_VERSION ||
        r->size != sizeof(*r) || r->flags != NNPI_DIAG_PREPARED_D0 || !r->generation ||
        r->reason > NNPI_BAR_REQUIRED || r->config_bytes > 64 || r->bar_count > 6)
        return 0;
    for (i = 0; i < 5; ++i) if (r->reserved[i]) return 0;
    for (i = r->config_bytes; i < 64; ++i) if (r->config[i]) return 0;
    if (r->reason != NNPI_BAR_CONFIG_UNAVAILABLE && (r->config_bytes != 64 || r->pci_status)) return 0;
    if (r->reason && r->bar_count) return 0;
    for (i = r->bar_count; i < 6; ++i) {
        const struct nnpi_bar_entry *b = &r->bars[i];
        if (b->bar || b->width || b->type || b->prefetch || b->raw_index ||
            b->translated_index || b->bus_start || b->translated_start || b->length) return 0;
    }
    if (r->reason) return 1;
    if (r->raw_count != r->translated_count || r->raw_count > NNPI_BAR_MAX_RESOURCES) return 0;
    for (i = 0; i < r->raw_count; ++i) raw[i].type = trans[i].type = NNPI_BAR_OPAQUE;
    for (i = 0; i < r->bar_count; ++i) {
        const struct nnpi_bar_entry *b = &r->bars[i];
        j = b->raw_index;
        if (j >= r->raw_count || j != b->translated_index || (seen & (1U << j))) return 0;
        seen |= 1U << j;
        raw[j].type = trans[j].type = b->type;
        raw[j].flags = trans[j].flags = b->prefetch;
        raw[j].start = b->bus_start; trans[j].start = b->translated_start;
        raw[j].length = trans[j].length = b->length;
    }
    if (nnpi_bar_match(r->config, 64, raw, trans, r->raw_count, expected, &count) || count != r->bar_count) return 0;
    for (i = 0; i < count; ++i) {
        const struct nnpi_bar_entry *a = &expected[i], *b = &r->bars[i];
        if (a->bar != b->bar || a->width != b->width || a->type != b->type ||
            a->prefetch != b->prefetch || a->raw_index != b->raw_index ||
            a->translated_index != b->translated_index || a->bus_start != b->bus_start ||
            a->translated_start != b->translated_start || a->length != b->length) return 0;
    }
    return 1;
}
#endif
