/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_BAR_MATCH_H
#define NNPI_BAR_MATCH_H
#include <stdint.h>
#include <stddef.h>
#define NNPI_BAR_MAX_RESOURCES 16U
#define NNPI_BAR_MEMORY 1U
#define NNPI_BAR_PORT 2U
#define NNPI_BAR_OPAQUE 3U
#define NNPI_BAR_PREFETCH 1U
#define NNPI_BAR_OK 0U
#define NNPI_BAR_CONFIG_UNAVAILABLE 1U
#define NNPI_BAR_IDENTITY 2U
#define NNPI_BAR_HEADER 3U
#define NNPI_BAR_RESOURCE_PAIR 4U
#define NNPI_BAR_RANGE 5U
#define NNPI_BAR_ENCODING 6U
#define NNPI_BAR_NO_MATCH 7U
#define NNPI_BAR_AMBIGUOUS 8U
#define NNPI_BAR_UNCLAIMED 9U
#define NNPI_BAR_REQUIRED 10U
struct nnpi_bar_resource {
    uint32_t type, flags;
    uint64_t start, length;
};
struct nnpi_bar_entry {
    uint32_t bar, width, type, prefetch;
    uint32_t raw_index, translated_index;
    uint64_t bus_start, translated_start, length;
};
/* WDK memory descriptor flags, checked against the SDK in nnpi_hw.c.
 * BAR identifies a BAR resource; it carries no access/cache semantics.
 * These are adapter constants, NOT additional flags accepted by the matcher. */
#define NNPI_CM_MEMORY_BAR 0x80U
#define NNPI_CM_MEMORY_PREFETCHABLE 0x04U
static inline int nnpi_bar_normalize_memory_flags(uint32_t flags, uint32_t *out)
{
    if (!out) return 0;
    *out = 0;
    /* Reject every unreviewed bit before converting to matcher flags. */
    if (flags & ~(NNPI_CM_MEMORY_BAR | NNPI_CM_MEMORY_PREFETCHABLE)) return 0;
    *out = (flags & NNPI_CM_MEMORY_PREFETCHABLE) ? NNPI_BAR_PREFETCH : 0;
    return 1;
}
static inline uint32_t nnpi_bar_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int nnpi_bar_range(uint64_t start, uint64_t length)
{
    return length && start <= UINT64_MAX - (length - 1);
}
static inline int nnpi_bar_overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return a <= b + (bn - 1) && b <= a + (an - 1);
}
/* Raw/translated list index correspondence is supplied by Windows PnP, NOT a
 * BAR number. The BAR identity comes exclusively from the config bus address.
 * Caller normalizes supported address descriptors, leaves private unions opaque.
 * Strict subset: reject I/O->memory translation, large-memory descriptors and
 * unclaimed address windows rather than guessing. No sizing writes/probes.
 * Output is transactional: every failure clears ALL entries and count.
 */
static inline uint32_t nnpi_bar_match(const uint8_t *config, size_t bytes,
    const struct nnpi_bar_resource *raw, const struct nnpi_bar_resource *translated,
    uint32_t count, struct nnpi_bar_entry out[6], uint32_t *out_count)
{
    struct nnpi_bar_entry candidate[6] = {{0, 0, 0, 0, 0, 0, 0, 0, 0}};
    uint32_t used[NNPI_BAR_MAX_RESOURCES] = {0};
    uint32_t i, j, n = 0;
    if (!out || !out_count) return NNPI_BAR_RESOURCE_PAIR;
    for (i = 0; i < 6; ++i) { struct nnpi_bar_entry zero = {0, 0, 0, 0, 0, 0, 0, 0, 0}; out[i] = zero; }
    *out_count = 0;
    if (!config || bytes != 64) return NNPI_BAR_CONFIG_UNAVAILABLE;
    if (nnpi_bar_le32(config) != 0x45c68086U) return NNPI_BAR_IDENTITY;
    if ((config[14] & 0x7fU) != 0) return NNPI_BAR_HEADER;
    if (!raw || !translated || count > NNPI_BAR_MAX_RESOURCES) return NNPI_BAR_RESOURCE_PAIR;
    for (i = 0; i < count; ++i) {
        if (raw[i].type != translated[i].type ||
            raw[i].flags != translated[i].flags || raw[i].flags > NNPI_BAR_PREFETCH ||
            raw[i].type < NNPI_BAR_MEMORY || raw[i].type > NNPI_BAR_OPAQUE)
            return NNPI_BAR_RESOURCE_PAIR;
        if (raw[i].type == NNPI_BAR_OPAQUE) {
            if (raw[i].start || raw[i].length || translated[i].start || translated[i].length || raw[i].flags)
                return NNPI_BAR_RESOURCE_PAIR;
            continue;
        }
        if (raw[i].length != translated[i].length ||
            !nnpi_bar_range(raw[i].start, raw[i].length) ||
            !nnpi_bar_range(translated[i].start, translated[i].length)) return NNPI_BAR_RANGE;
        for (j = 0; j < i; ++j) {
            if (raw[j].type != raw[i].type) continue;
            if (nnpi_bar_overlap(raw[i].start, raw[i].length, raw[j].start, raw[j].length) ||
                nnpi_bar_overlap(translated[i].start, translated[i].length,
                                 translated[j].start, translated[j].length)) return NNPI_BAR_AMBIGUOUS;
        }
    }
    for (i = 0; i < 6; ++i) {
        uint32_t low = nnpi_bar_le32(config + 16 + i * 4), bar = i;
        uint32_t type, width = 32, prefetch = 0, match = count, matches = 0;
        uint64_t base;
        if (low & 1U) {
            if (low & 2U) return NNPI_BAR_ENCODING;
            type = NNPI_BAR_PORT; base = low & ~3U;
        } else {
            type = NNPI_BAR_MEMORY; prefetch = (low >> 3) & 1U;
            base = low & ~15U;
            if ((low & 6U) == 4U) {
                if (i == 5) return NNPI_BAR_ENCODING;
                width = 64;
                base |= (uint64_t)nnpi_bar_le32(config + 16 + (++i) * 4) << 32;
            } else if (low & 6U) return NNPI_BAR_ENCODING;
        }
        /* Zero address can mean unimplemented OR unassigned: never infer a map. */
        if (!base) continue;
        for (j = 0; j < count; ++j) {
            if (raw[j].type == type && raw[j].start == base) { match = j; ++matches; }
        }
        if (!matches) return NNPI_BAR_NO_MATCH;
        if (matches != 1 || used[match]) return NNPI_BAR_AMBIGUOUS;
        if (raw[match].flags != prefetch) return NNPI_BAR_RESOURCE_PAIR;
        /* PCI BAR apertures are aligned power-of-two ranges. Length is from PnP,
         * never obtained by writing all ones to the config BAR register. */
        if (raw[match].length < (type == NNPI_BAR_MEMORY ? 16U : 4U) ||
            (raw[match].length & (raw[match].length - 1)) ||
            (base & (raw[match].length - 1)) ||
            (width == 32 && raw[match].length - 1 > UINT32_MAX - base)) return NNPI_BAR_RANGE;
        used[match] = 1;
        candidate[n].bar = bar; candidate[n].width = width;
        candidate[n].type = type; candidate[n].prefetch = prefetch;
        candidate[n].raw_index = match; candidate[n].translated_index = match;
        candidate[n].bus_start = base; candidate[n].translated_start = translated[match].start;
        candidate[n].length = raw[match].length; ++n;
    }
    for (i = 0; i < count; ++i)
        if (raw[i].type != NNPI_BAR_OPAQUE && !used[i]) return NNPI_BAR_UNCLAIMED;
    if (!n || candidate[0].bar != 0 || candidate[0].type != NNPI_BAR_MEMORY ||
        candidate[0].length < 0x1c0U) return NNPI_BAR_REQUIRED;
    for (i = 0; i < n; ++i) out[i] = candidate[i];
    *out_count = n;
    return NNPI_BAR_OK;
}
static inline const char *nnpi_bar_reason(uint32_t reason)
{
    switch (reason) {
    case NNPI_BAR_OK: return "unique-association";
    case NNPI_BAR_CONFIG_UNAVAILABLE: return "pci-config-unavailable-or-short-read";
    case NNPI_BAR_IDENTITY: return "pci-identity-mismatch";
    case NNPI_BAR_HEADER: return "not-type0-pci-header";
    case NNPI_BAR_RESOURCE_PAIR: return "unsupported-or-inconsistent-resource-pair";
    case NNPI_BAR_RANGE: return "invalid-overflowing-or-unaligned-range";
    case NNPI_BAR_ENCODING: return "unsupported-bar-encoding";
    case NNPI_BAR_NO_MATCH: return "bar-has-no-raw-address-match";
    case NNPI_BAR_AMBIGUOUS: return "ambiguous-or-overlapping-resources";
    case NNPI_BAR_UNCLAIMED: return "address-resource-not-claimed-by-bar";
    case NNPI_BAR_REQUIRED: return "bar0-memory-range-unavailable";
    default: return "unknown";
    }
}
#endif
