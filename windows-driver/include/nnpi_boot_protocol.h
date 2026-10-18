#ifndef NNPI_BOOT_PROTOCOL_H
#define NNPI_BOOT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Pure wire encoding: no allocation, MMIO, DMA mapping or address discovery.
 * All dma_* inputs MUST be device-visible addresses returned by the platform
 * DMA API for this device, NEVER virtual addresses or CPU physical addresses.
 * Success is NNPI_BOOT_OK. On any error, output bytes are left unchanged.
 * Input/output regions must not overlap; callers own buffer lifetime/locking.
 * This validates representation, not DMA ownership, permissions or residency.
 */
#define NNPI_BOOT_PAGE_SIZE 4096u
#define NNPI_BOOT_HEADER_SIZE 24u
#define NNPI_BOOT_ENTRY_SIZE 8u
#define NNPI_BOOT_MESSAGE_SIZE 24u
#define NNPI_BOOT_ENTRIES_PER_PAGE 509u
#define NNPI_BOOT_PFN_MASK UINT64_C(0x1fffffffffff)
#define NNPI_BOOT_DMA_MAX UINT64_C(0x1ffffffffffffff)
#define NNPI_BOOT_MAX_PAGES UINT64_C(0x7ffff)
#define NNPI_BOOT_U32_MAX UINT64_C(0xffffffff)

enum nnpi_boot_result {
    NNPI_BOOT_OK = 0,
    NNPI_BOOT_INVALID = 1,
    NNPI_BOOT_BUFFER_TOO_SMALL = 2,
    NNPI_BOOT_ALIGNMENT = 3,
    NNPI_BOOT_RANGE = 4
};

/* A mapped SG segment; byte_length is the actual SG length, not rounded size. */
struct nnpi_boot_segment {
    uint64_t dma_address;
    uint64_t byte_length;
};

static inline void nnpi_boot_put_le64(uint8_t *out, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        out[i] = (uint8_t)((value >> (i * 8u)) & UINT64_C(0xff));
}

static inline void nnpi_boot_put_le32(uint8_t *out, uint32_t value)
{
    unsigned int i;
    for (i = 0; i < 4; ++i)
        out[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
}

static inline int nnpi_boot_dma_range(uint64_t dma_address, uint64_t bytes)
{
    if ((dma_address & (NNPI_BOOT_PAGE_SIZE - 1u)) != 0)
        return NNPI_BOOT_ALIGNMENT;
    if (!bytes || dma_address > NNPI_BOOT_DMA_MAX ||
        bytes - 1u > NNPI_BOOT_DMA_MAX - dma_address)
        return NNPI_BOOT_RANGE;
    return NNPI_BOOT_OK;
}

/* 45-bit PFN in bits 44:0, 19-bit rounded page count in bits 63:45.
 * The DMA mapping must cover the rounded final page, even for a short segment.
 */
static inline int nnpi_boot_entry(uint8_t *out, size_t capacity,
                                  uint64_t dma_address, uint64_t byte_length)
{
    uint64_t pages;
    int rc;
    if (!out)
        return NNPI_BOOT_INVALID;
    if (capacity < NNPI_BOOT_ENTRY_SIZE)
        return NNPI_BOOT_BUFFER_TOO_SMALL;
    if (!byte_length || byte_length > NNPI_BOOT_MAX_PAGES * NNPI_BOOT_PAGE_SIZE)
        return NNPI_BOOT_RANGE;
    pages = (byte_length + NNPI_BOOT_PAGE_SIZE - 1u) >> 12;
    rc = nnpi_boot_dma_range(dma_address, pages << 12);
    if (rc != NNPI_BOOT_OK)
        return rc;
    nnpi_boot_put_le64(out, ((dma_address >> 12) & NNPI_BOOT_PFN_MASK) |
                           ((pages & NNPI_BOOT_MAX_PAGES) << 45));
    return NNPI_BOOT_OK;
}

/* BIOS SYSTEM_INFO_REQ: opcode 31, type 0x11, payload length 16 (not 24).
 * The supplied mapping must be a writable, device-visible 4KiB page.
 */
static inline int nnpi_boot_sysinfo(uint8_t *out, size_t capacity,
                                    uint64_t dma_page)
{
    int rc;
    if (!out)
        return NNPI_BOOT_INVALID;
    if (capacity < NNPI_BOOT_MESSAGE_SIZE)
        return NNPI_BOOT_BUFFER_TOO_SMALL;
    rc = nnpi_boot_dma_range(dma_page, NNPI_BOOT_PAGE_SIZE);
    if (rc != NNPI_BOOT_OK)
        return rc;
    nnpi_boot_put_le64(out, (UINT64_C(31) & UINT64_C(0x3f)) |
                           (UINT64_C(0x11) << 8) | (UINT64_C(16) << 16));
    nnpi_boot_put_le64(out + 8, dma_page);
    nnpi_boot_put_le64(out + 16, NNPI_BOOT_PAGE_SIZE);
    return NNPI_BOOT_OK;
}

/* BIOS BOOT_IMAGE_READY requires ONE DMA-contiguous descriptor array, NOT
 * the 4KiB linked-block form when it spans multiple blocks. dma_table points
 * to its 24-byte header; the wire message points to dma_table + 24.
 * mapped_table_bytes describes the contiguous mapping beginning at dma_table.
 * Caller must check table entries cover image_bytes and preserve their order.
 */
static inline int nnpi_boot_image(uint8_t *out, size_t capacity,
                                  uint64_t dma_table, uint64_t mapped_table_bytes,
                                  uint64_t entry_count, uint64_t image_bytes)
{
    uint64_t descriptor_bytes;
    int rc;
    if (!out)
        return NNPI_BOOT_INVALID;
    if (capacity < NNPI_BOOT_MESSAGE_SIZE)
        return NNPI_BOOT_BUFFER_TOO_SMALL;
    if (!entry_count || entry_count > NNPI_BOOT_U32_MAX / NNPI_BOOT_ENTRY_SIZE ||
        !image_bytes || image_bytes > NNPI_BOOT_U32_MAX)
        return NNPI_BOOT_RANGE;
    descriptor_bytes = entry_count * NNPI_BOOT_ENTRY_SIZE;
    if (mapped_table_bytes < NNPI_BOOT_HEADER_SIZE + descriptor_bytes)
        return NNPI_BOOT_BUFFER_TOO_SMALL;
    rc = nnpi_boot_dma_range(dma_table, mapped_table_bytes);
    if (rc != NNPI_BOOT_OK)
        return rc;
    nnpi_boot_put_le64(out, (UINT64_C(31) & UINT64_C(0x3f)) |
                           (UINT64_C(0x10) << 8) | (UINT64_C(16) << 16));
    nnpi_boot_put_le64(out + 8, dma_table + NNPI_BOOT_HEADER_SIZE);
    nnpi_boot_put_le64(out + 16, (descriptor_bytes & NNPI_BOOT_U32_MAX) |
                                ((image_bytes & NNPI_BOOT_U32_MAX) << 32));
    return NNPI_BOOT_OK;
}

/* Encode one block: [next DMA byte address LE64, GLOBAL total_nents LE32,
 * start_offset LE32, THIS BLOCK's sum of actual SG byte lengths LE64], entries.
 * Use page_block=1 for 4KiB linked blocks (509 entries, zero-filled tail).
 * Use page_block=0 for a single contiguous BIOS table (no next, all entries).
 * Only the first block may have start_offset != 0; subsequent callers pass 0.
 * dma_next=0 is the end sentinel. Caller validates linkage, no cycles, global
 * count consistency, and mapping sizes; this function sees only one block.
 */
static inline int nnpi_boot_block(uint8_t *out, size_t capacity,
                                  const struct nnpi_boot_segment *segments,
                                  size_t count, uint64_t total_nents,
                                  uint32_t start_offset, uint64_t dma_next,
                                  int page_block)
{
    uint64_t size = 0;
    size_t i, required;
    uint8_t scratch[NNPI_BOOT_ENTRY_SIZE];
    int rc;
    if (!out || !segments || !count || (page_block != 0 && page_block != 1))
        return NNPI_BOOT_INVALID;
    if (!total_nents || total_nents > NNPI_BOOT_U32_MAX || count > total_nents ||
        start_offset >= NNPI_BOOT_PAGE_SIZE ||
        count > (SIZE_MAX - NNPI_BOOT_HEADER_SIZE) / NNPI_BOOT_ENTRY_SIZE)
        return NNPI_BOOT_RANGE;
    if (page_block) {
        if (count > NNPI_BOOT_ENTRIES_PER_PAGE ||
            (dma_next && (count != NNPI_BOOT_ENTRIES_PER_PAGE || total_nents <= count)))
            return NNPI_BOOT_RANGE;
        required = NNPI_BOOT_PAGE_SIZE;
    } else {
        if (dma_next || count != total_nents)
            return NNPI_BOOT_INVALID;
        required = NNPI_BOOT_HEADER_SIZE + count * NNPI_BOOT_ENTRY_SIZE;
    }
    if (capacity < required)
        return NNPI_BOOT_BUFFER_TOO_SMALL;
    if (dma_next) {
        rc = nnpi_boot_dma_range(dma_next, NNPI_BOOT_PAGE_SIZE);
        if (rc != NNPI_BOOT_OK)
            return rc;
    }
    /* Validate every entry before touching output, including aggregate overflow. */
    for (i = 0; i < count; ++i) {
        rc = nnpi_boot_entry(scratch, sizeof(scratch), segments[i].dma_address,
                             segments[i].byte_length);
        if (rc != NNPI_BOOT_OK)
            return rc;
        if (segments[i].byte_length > UINT64_MAX - size)
            return NNPI_BOOT_RANGE;
        size += segments[i].byte_length;
    }
    if (start_offset >= segments[0].byte_length)
        return NNPI_BOOT_RANGE;
    nnpi_boot_put_le64(out, dma_next);
    nnpi_boot_put_le32(out + 8, (uint32_t)total_nents);
    nnpi_boot_put_le32(out + 12, start_offset);
    nnpi_boot_put_le64(out + 16, size);
    for (i = 0; i < count; ++i)
        (void)nnpi_boot_entry(out + NNPI_BOOT_HEADER_SIZE + i * NNPI_BOOT_ENTRY_SIZE,
                              NNPI_BOOT_ENTRY_SIZE, segments[i].dma_address,
                              segments[i].byte_length);
    for (i = NNPI_BOOT_HEADER_SIZE + count * NNPI_BOOT_ENTRY_SIZE; i < required; ++i)
        out[i] = 0;
    return NNPI_BOOT_OK;
}

/* Ownership rule: BOOT_STARTED is NOT DMA completion. Keep image, descriptor
 * and DMA mappings alive until confirmed DriverReady/CardReady, or a proven
 * safe DMA-stop/quiesce path. Timeout/error/removal notification alone is not
 * proof that DMA stopped. This header implements neither boot nor teardown.
 */
#endif /* NNPI_BOOT_PROTOCOL_H */
