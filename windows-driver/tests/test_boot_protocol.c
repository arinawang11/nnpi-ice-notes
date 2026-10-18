#include "../include/nnpi_boot_protocol.h"
#include "../include/nnpi_fifo_protocol.h"
#include <stdio.h>
#include <string.h>

static unsigned int checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)
#define REJECT(expr, result) do { memset(out, 0xa5, sizeof(out)); \
    CHECK((expr) == (result)); \
    for (j = 0; j < sizeof(out); ++j) CHECK(out[j] == 0xa5); \
} while (0)

/* Composition regression for 025: actual existing encoders, no mock DMA or
 * invented drain contract. A transport still needs exact message-length,
 * BIOS_READY, lifecycle, budget and MMIO-ordering gates before using this plan.
 * In particular the generic FIFO helper deliberately accepts aligned prefixes;
 * it cannot be treated as a SYSINFO protocol validator. */
static int test_sysinfo_fifo_composition(void)
{
    uint8_t encoded[NNPI_BOOT_MESSAGE_SIZE + 2u];
    struct nnpi_fifo_command_plan plan, before;
    uint32_t read, write, used, i;
    size_t capacity;
    const uint64_t page = UINT64_C(0x12345000); /* synthetic only */
    for (capacity = 0; capacity < NNPI_BOOT_MESSAGE_SIZE; ++capacity) {
        memset(encoded, 0xa5, sizeof(encoded));
        CHECK(nnpi_boot_sysinfo(encoded + 1, capacity, page) == NNPI_BOOT_BUFFER_TOO_SMALL);
        for (i = 0; i < sizeof(encoded); ++i) CHECK(encoded[i] == 0xa5);
    }
    CHECK(nnpi_boot_sysinfo(encoded + 1, NNPI_BOOT_MESSAGE_SIZE, page) == NNPI_BOOT_OK);
    CHECK(encoded[0] == 0xa5 && encoded[sizeof(encoded) - 1u] == 0xa5);
    for (read = 0; read < 16; ++read) {
        for (write = 0; write < 32; ++write) {
            const uint32_t control = read | (write << 8) | UINT32_C(0xa5008000);
            int result;
            memset(&plan, 0xa5, sizeof(plan));
            memcpy(&before, &plan, sizeof(before));
            result = nnpi_fifo_plan_command(control, encoded + 1,
                                            NNPI_BOOT_MESSAGE_SIZE, &plan);
            if (write < read || write - read > 16) {
                CHECK(result == NNPI_FIFO_BAD_STATE);
                CHECK(memcmp(&before, &plan, sizeof(plan)) == 0);
                continue;
            }
            used = write - read;
            if (used > 13) {
                CHECK(result == NNPI_FIFO_NO_SPACE);
                CHECK(memcmp(&before, &plan, sizeof(plan)) == 0);
                continue;
            }
            CHECK(result == NNPI_FIFO_OK);
            CHECK(plan.count == 3 && plan.free_before == 16 - used && plan.free_after == 13 - used);
            CHECK(plan.writes[0].value == UINT64_C(0x0010111f));
            CHECK(plan.writes[1].value == page);
            CHECK(plan.writes[2].value == 4096);
            for (i = 0; i < 3; ++i) {
                CHECK(plan.writes[i].notify == (i == 2 ? 1u : 0u));
                CHECK(plan.writes[i].low_offset == (i == 2 ? 0x58u : 0x50u));
                CHECK(plan.writes[i].high_offset == (i == 2 ? 0x5cu : 0x54u));
            }
            for (i = 3; i < NNPI_FIFO_DEPTH; ++i) {
                CHECK(plan.writes[i].value == 0 && plan.writes[i].notify == 0);
                CHECK(plan.writes[i].low_offset == 0 && plan.writes[i].high_offset == 0);
            }
        }
    }
    /* Explicitly expose the missing higher-level length gate, rather than
     * claiming that a generic FIFO encoder rejects protocol truncation. */
    for (capacity = 0; capacity < NNPI_BOOT_MESSAGE_SIZE; ++capacity) {
        int result;
        memset(&plan, 0xa5, sizeof(plan));
        memcpy(&before, &plan, sizeof(before));
        result = nnpi_fifo_plan_command(0, encoded + 1, capacity, &plan);
        if (capacity % 8u) {
            CHECK(result == NNPI_FIFO_RANGE);
            CHECK(memcmp(&before, &plan, sizeof(plan)) == 0);
        } else {
            CHECK(result == NNPI_FIFO_OK);
            CHECK(plan.count == capacity / 8u);
        }
    }
    return 0;
}

int main(void)
{
    /* All addresses below are synthetic protocol test values, not mappings. */
    static const uint8_t sysinfo[] = {
        0x1f,0x11,0x10,0,0,0,0,0, 0,0x50,0x34,0x12,0,0,0,0,
        0,0x10,0,0,0,0,0,0
    };
    static const uint8_t image[] = {
        0x1f,0x10,0x10,0,0,0,0,0, 0x18,0x50,0x34,0x12,0,0,0,0,
        0x10,0,0,0, 0x01,0x20,0,0
    };
    static const uint8_t entry[] = {0x45,0x23,0x01,0,0,0x40,0,0};
    static const uint8_t block[] = {
        0,0,0,0,0,0,0,0, 2,0,0,0, 3,0,0,0,
        1,0x20,0,0,0,0,0,0,
        0x45,0x23,1,0,0,0x20,0,0,
        0,0,2,0,0,0x40,0,0
    };
    static const uint8_t linked_header[] = {
        0,0x90,0x78,0x56,0,0,0,0, 0xfe,1,0,0, 0,0,0,0,
        0,0xd0,0x1f,0,0,0,0,0
    };
    uint8_t out[8192];
    struct nnpi_boot_segment segments[510];
    size_t i, j;
    const uint64_t highest_page = NNPI_BOOT_DMA_MAX - 4095u;

#if defined(__cplusplus)
    static_assert(NNPI_BOOT_ENTRIES_PER_PAGE ==
                  (NNPI_BOOT_PAGE_SIZE - NNPI_BOOT_HEADER_SIZE) / NNPI_BOOT_ENTRY_SIZE,
                  "boot entries per page must match the protocol layout");
#else
    _Static_assert(NNPI_BOOT_ENTRIES_PER_PAGE ==
                   (NNPI_BOOT_PAGE_SIZE - NNPI_BOOT_HEADER_SIZE) / NNPI_BOOT_ENTRY_SIZE,
                   "boot entries per page must match the protocol layout");
#endif
    CHECK(nnpi_boot_sysinfo(out + 1, 24, UINT64_C(0x12345000)) == NNPI_BOOT_OK);
    CHECK(memcmp(out + 1, sysinfo, sizeof(sysinfo)) == 0); /* unaligned output */
    CHECK(nnpi_boot_image(out, 24, UINT64_C(0x12345000), 4096, 2, 8193) == NNPI_BOOT_OK);
    CHECK(memcmp(out, image, sizeof(image)) == 0);
    CHECK(nnpi_boot_entry(out, 8, UINT64_C(0x12345000), 4097) == NNPI_BOOT_OK);
    CHECK(memcmp(out, entry, sizeof(entry)) == 0);
    CHECK(nnpi_boot_entry(out, 8, highest_page, 4096) == NNPI_BOOT_OK);
    CHECK(out[0] == 0xff && out[4] == 0xff && out[5] == 0x3f && out[6] == 0 && out[7] == 0);
    CHECK(nnpi_boot_entry(out, 8, 0, NNPI_BOOT_MAX_PAGES * 4096) == NNPI_BOOT_OK);
    CHECK(out[4] == 0 && out[5] == 0xe0 && out[6] == 0xff && out[7] == 0xff);
    CHECK(nnpi_boot_sysinfo(out, 24, highest_page) == NNPI_BOOT_OK);
    CHECK(nnpi_boot_image(out, 24, 0, NNPI_BOOT_HEADER_SIZE +
          (NNPI_BOOT_U32_MAX / 8) * 8, NNPI_BOOT_U32_MAX / 8,
          NNPI_BOOT_U32_MAX) == NNPI_BOOT_OK);
    CHECK(out[16] == 0xf8 && out[19] == 0xff && out[20] == 0xff && out[23] == 0xff);

    segments[0].dma_address = UINT64_C(0x12345000);
    segments[0].byte_length = 4096;
    segments[1].dma_address = UINT64_C(0x20000000);
    segments[1].byte_length = 4097;
    CHECK(nnpi_boot_block(out, sizeof(out), segments, 2, 2, 3, 0, 0) == NNPI_BOOT_OK);
    CHECK(memcmp(out, block, sizeof(block)) == 0);
    memset(out, 0xa5, sizeof(out));
    CHECK(nnpi_boot_block(out, sizeof(out), segments, 2, 2, 3, 0, 1) == NNPI_BOOT_OK);
    CHECK(memcmp(out, block, sizeof(block)) == 0);
    for (i = sizeof(block); i < 4096; ++i) CHECK(out[i] == 0);
    CHECK(out[4096] == 0xa5);

    REJECT(nnpi_boot_sysinfo(out, 23, 0), NNPI_BOOT_BUFFER_TOO_SMALL);
    REJECT(nnpi_boot_sysinfo(out, 24, 1), NNPI_BOOT_ALIGNMENT);
    REJECT(nnpi_boot_sysinfo(out, 24, UINT64_C(1) << 57), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_entry(out, 7, 0, 4096), NNPI_BOOT_BUFFER_TOO_SMALL);
    REJECT(nnpi_boot_entry(out, 8, 1, 4096), NNPI_BOOT_ALIGNMENT);
    REJECT(nnpi_boot_entry(out, 8, 0, 0), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_entry(out, 8, 0, UINT64_MAX), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_entry(out, 8, 0, NNPI_BOOT_MAX_PAGES * 4096 + 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_entry(out, 8, highest_page, 4097), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_entry(out, 8, UINT64_C(1) << 57, 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_image(out, 23, 0, 4096, 1, 1), NNPI_BOOT_BUFFER_TOO_SMALL);
    REJECT(nnpi_boot_image(out, 24, 1, 4096, 1, 1), NNPI_BOOT_ALIGNMENT);
    REJECT(nnpi_boot_image(out, 24, 0, 31, 1, 1), NNPI_BOOT_BUFFER_TOO_SMALL);
    REJECT(nnpi_boot_image(out, 24, 0, UINT64_MAX, 1, 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_image(out, 24, highest_page, 4097, 1, 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_image(out, 24, 0, 4096, 0, 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_image(out, 24, 0, 4096, NNPI_BOOT_U32_MAX / 8 + 1, 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_image(out, 24, 0, 4096, UINT64_MAX, 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_image(out, 24, 0, 4096, 1, 0), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_image(out, 24, 0, 4096, 1, NNPI_BOOT_U32_MAX + 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_block(out, 39, segments, 2, 2, 0, 0, 0), NNPI_BOOT_BUFFER_TOO_SMALL);
    REJECT(nnpi_boot_block(out, 4095, segments, 2, 2, 0, 0, 1), NNPI_BOOT_BUFFER_TOO_SMALL);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 0, 0, 0, 0, 0), NNPI_BOOT_INVALID);
    REJECT(nnpi_boot_block(out, sizeof(out), NULL, 1, 1, 0, 0, 0), NNPI_BOOT_INVALID);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, 1, 0, 0, 0), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, NNPI_BOOT_U32_MAX + 1, 0, 0, 0), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, 2, 4096, 0, 0), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, 2, 0, 4096, 0), NNPI_BOOT_INVALID);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, 3, 0, 0, 0), NNPI_BOOT_INVALID);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, 3, 0, 4096, 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, 2, 0, 0, 2), NNPI_BOOT_INVALID);
    segments[1].dma_address++;
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, 2, 0, 0, 0), NNPI_BOOT_ALIGNMENT);
    segments[1].dma_address--;
    segments[0].byte_length = 1;
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 2, 2, 1, 0, 0), NNPI_BOOT_RANGE);

    for (i = 0; i < 510; ++i) {
        segments[i].dma_address = UINT64_C(0x10000000) + i * UINT64_C(4096);
        segments[i].byte_length = 4096;
    }
    CHECK(nnpi_boot_block(out, sizeof(out), segments, 509, 510, 0,
                          UINT64_C(0x56789000), 1) == NNPI_BOOT_OK);
    CHECK(memcmp(out, linked_header, sizeof(linked_header)) == 0);
    CHECK(out[4095] == 0);
    CHECK(nnpi_boot_block(out, sizeof(out), segments + 509, 1, 510, 0, 0, 1) == NNPI_BOOT_OK);
    CHECK(out[8] == 0xfe && out[9] == 1 && out[16] == 0 && out[17] == 0x10);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 510, 510, 0, 0, 1), NNPI_BOOT_RANGE);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 509, 510, 0, 1, 1), NNPI_BOOT_ALIGNMENT);
    REJECT(nnpi_boot_block(out, sizeof(out), segments, 509, 510, 0, UINT64_C(1) << 57, 1), NNPI_BOOT_RANGE);
    CHECK(nnpi_boot_block(out, sizeof(out), segments, 510, 510, 0, 0, 0) == NNPI_BOOT_OK);
    CHECK(nnpi_boot_image(out, 24, 0, 4104, 510, 510 * 4096) == NNPI_BOOT_OK);
    CHECK(nnpi_boot_sysinfo(NULL, 24, 0) == NNPI_BOOT_INVALID);
    CHECK(nnpi_boot_image(NULL, 24, 0, 4096, 1, 1) == NNPI_BOOT_INVALID);
    CHECK(nnpi_boot_entry(NULL, 8, 0, 1) == NNPI_BOOT_INVALID);
    CHECK(nnpi_boot_block(NULL, sizeof(out), segments, 1, 1, 0, 0, 0) == NNPI_BOOT_INVALID);
    CHECK(test_sysinfo_fifo_composition() == 0);
    printf("PASS boot protocol: %u checks (offline encoding only; no DMA/boot access)\n", checks);
    return 0;
}
