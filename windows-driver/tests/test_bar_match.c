/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_diag_abi.h"
static uint8_t config[64];
static struct nnpi_bar_resource raw[4], trans[4];
static struct nnpi_bar_entry out[6];
static uint32_t count;
static unsigned cases;
static void put32(unsigned offset, uint32_t value)
{
    unsigned i; for (i = 0; i < 4; ++i) config[offset + i] = (uint8_t)(value >> (8 * i));
}
static void fixture(void)
{
    memset(config, 0, sizeof(config)); memset(raw, 0, sizeof(raw));
    put32(0, 0x45c68086); put32(16, 0x100004); put32(20, 1); /* BAR0 64-bit */
    put32(24, 0x200000); /* BAR2 32-bit */
    /* Resource order deliberately NOT BAR order, including opaque private data. */
    raw[0].type = NNPI_BAR_MEMORY; raw[0].start = 0x200000; raw[0].length = 0x1000;
    raw[1].type = NNPI_BAR_OPAQUE;
    raw[2].type = NNPI_BAR_MEMORY; raw[2].start = UINT64_C(0x100100000); raw[2].length = 0x1000;
    raw[3].type = NNPI_BAR_OPAQUE;
    memcpy(trans, raw, sizeof(raw));
    trans[0].start = 0x800000; trans[2].start = UINT64_C(0x200000000);
}
static void check(uint32_t reason)
{
    unsigned i; memset(out, 0xa5, sizeof(out)); count = 99;
    assert(nnpi_bar_match(config, 64, raw, trans, 4, out, &count) == reason);
    if (reason) {
        const uint8_t *p = (const uint8_t *)out;
        assert(count == 0); for (i = 0; i < sizeof(out); ++i) assert(p[i] == 0);
    }
    ++cases;
}
static void test_memory_flags(void)
{
    static const uint32_t accepted[] = {0x00U, 0x04U, 0x80U, 0x84U};
    uint32_t flags, normalized, bit;
    unsigned i, j;
    /* Exhaust all USHORT descriptor flags, including access/cache flags.
     * Seed output to verify that failure cannot publish stale prefetch state. */
    for (flags = 0; flags <= 0xffffU; ++flags) {
        int expected = flags == 0 || flags == 4 || flags == 0x80 || flags == 0x84;
        normalized = 0xa5a5a5a5U;
        assert(nnpi_bar_normalize_memory_flags(flags, &normalized) == expected);
        assert(normalized == (expected && (flags & 4U) ? NNPI_BAR_PREFETCH : 0U));
    }
    /* The pure helper also refuses bits above the WDK descriptor width. */
    for (bit = 16; bit < 32; ++bit) {
        for (i = 0; i < 4; ++i) {
            normalized = 99;
            assert(!nnpi_bar_normalize_memory_flags(accepted[i] | (UINT32_C(1) << bit),
                                                     &normalized));
            assert(normalized == 0);
        }
    }
    assert(!nnpi_bar_normalize_memory_flags(0x84U, NULL));
    /* BAR metadata may differ across raw/translated; prefetch may not.
     * Exercise the same adapter called by NnpiNormalize, then the real matcher. */
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            fixture();
            if (accepted[i] & 4U) put32(24, 0x200008);
            assert(nnpi_bar_normalize_memory_flags(accepted[i], &raw[0].flags));
            assert(nnpi_bar_normalize_memory_flags(accepted[j], &trans[0].flags));
            check((accepted[i] & 4U) == (accepted[j] & 4U) ?
                  NNPI_BAR_OK : NNPI_BAR_RESOURCE_PAIR);
            if (count) {
                assert(count == 2 && out[1].bar == 2 && out[1].raw_index == 0);
                assert(out[1].prefetch == ((accepted[i] & 4U) ? 1U : 0U));
                assert(out[1].bus_start == raw[0].start);
                assert(out[1].translated_start == trans[0].start);
            }
        }
    }
    /* Do not teach the matcher to accept WDK bits or ignore config prefetch. */
    fixture(); raw[0].flags = trans[0].flags = 0x80U; check(NNPI_BAR_RESOURCE_PAIR);
    fixture(); raw[0].flags = trans[0].flags = 0x84U; check(NNPI_BAR_RESOURCE_PAIR);
    fixture();
    assert(nnpi_bar_normalize_memory_flags(0x84U, &raw[0].flags));
    assert(nnpi_bar_normalize_memory_flags(0x84U, &trans[0].flags));
    check(NNPI_BAR_RESOURCE_PAIR);
    puts("Memory flag adapter: exhaustive 16-bit whitelist, high-bit rejection and matcher regressions passed");
}
int main(void)
{
    struct nnpi_diag_response response, bad;
    struct nnpi_diag_request request = {NNPI_DIAG_VERSION, sizeof(request), {0, 0}};
    fixture(); check(NNPI_BAR_OK);
    assert(count == 2 && out[0].bar == 0 && out[0].width == 64 && out[0].raw_index == 2);
    assert(out[1].bar == 2 && out[1].width == 32 && out[1].raw_index == 0);
    memset(&response, 0, sizeof(response));
    response.version = NNPI_DIAG_VERSION; response.size = sizeof(response);
    response.flags = NNPI_DIAG_PREPARED_D0; response.generation = 1;
    response.config_bytes = 64; response.raw_count = response.translated_count = 4;
    response.bar_count = count; memcpy(response.bars, out, sizeof(out));
    memcpy(response.config, config, sizeof(config));
    assert(nnpi_diag_request_valid(&request, sizeof(request)));
    request.reserved[1] = 1; assert(!nnpi_diag_request_valid(&request, sizeof(request)));
    assert(nnpi_diag_response_valid(&response, sizeof(response)));
    bad = response; bad.bars[0].bar = 1; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    bad = response; bad.bars[0].raw_index = 16; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    bad = response; bad.bars[0].translated_index = 1; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    bad = response; bad.bars[2].length = 1; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    bad = response; bad.reserved[4] = 1; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    bad = response; bad.flags = 1; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    bad = response; bad.generation = 0; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    bad = response; bad.reason = NNPI_BAR_AMBIGUOUS; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    assert(!nnpi_diag_response_valid(&response, sizeof(response) - 1));
    assert(!nnpi_diag_response_valid(&response, sizeof(response) + 1));
    memset(&bad, 0, sizeof(bad)); bad.version = 1; bad.size = sizeof(bad);
    bad.flags = 3; bad.generation = 1; bad.reason = NNPI_BAR_CONFIG_UNAVAILABLE;
    assert(nnpi_diag_response_valid(&bad, sizeof(bad)));
    bad.config[63] = 1; assert(!nnpi_diag_response_valid(&bad, sizeof(bad)));
    fixture(); put32(0, 0xffffffff); check(NNPI_BAR_IDENTITY);
    fixture(); config[14] = 1; check(NNPI_BAR_HEADER);
    fixture(); config[14] = 0x80; check(NNPI_BAR_OK); /* multifunction endpoint */
    fixture(); put32(16, 0x100002); check(NNPI_BAR_ENCODING); /* below-1M rejected */
    fixture(); put32(36, 4); check(NNPI_BAR_ENCODING); /* truncated 64-bit BAR5 */
    fixture(); put32(24, 0x200003); check(NNPI_BAR_ENCODING);
    fixture(); trans[0].type = NNPI_BAR_PORT; check(NNPI_BAR_RESOURCE_PAIR);
    fixture(); raw[1].start = 1; check(NNPI_BAR_RESOURCE_PAIR); /* opaque union refused */
    fixture(); trans[0].length = 0; check(NNPI_BAR_RANGE);
    fixture(); raw[0].length = trans[0].length = 0; check(NNPI_BAR_RANGE);
    fixture(); trans[0].start = UINT64_MAX - 1; check(NNPI_BAR_RANGE);
    fixture(); raw[0].start = UINT64_MAX - 1; check(NNPI_BAR_RANGE);
    fixture(); raw[1] = raw[0]; trans[1] = trans[0]; check(NNPI_BAR_AMBIGUOUS);
    fixture(); trans[0].start = trans[2].start; check(NNPI_BAR_AMBIGUOUS);
    fixture(); put32(24, 0x300000); check(NNPI_BAR_NO_MATCH);
    fixture(); put32(28, 0x200000); check(NNPI_BAR_AMBIGUOUS); /* two BARs same raw */
    fixture(); put32(24, 0); check(NNPI_BAR_UNCLAIMED);
    fixture(); put32(16, 0); put32(20, 0); raw[2].type = trans[2].type = NNPI_BAR_OPAQUE;
    raw[2].start = trans[2].start = raw[2].length = trans[2].length = 0; check(NNPI_BAR_REQUIRED);
    fixture(); raw[2].length = trans[2].length = 0x100; check(NNPI_BAR_REQUIRED);
    fixture(); raw[0].length = trans[0].length = 0x1800; check(NNPI_BAR_RANGE);
    fixture(); raw[0].start += 16; trans[0].start += 16; put32(24, 0x200010); check(NNPI_BAR_RANGE);
    fixture(); put32(24, 0x200008); check(NNPI_BAR_RESOURCE_PAIR);
    fixture(); put32(24, 0x200008); raw[0].flags = trans[0].flags = 1; check(NNPI_BAR_OK);
    fixture(); put32(24, 0x200001); raw[0].type = trans[0].type = NNPI_BAR_PORT; check(NNPI_BAR_OK);
    fixture(); raw[0].length = trans[0].length = 8; check(NNPI_BAR_RANGE);
    fixture(); put32(16, 0x100000); put32(20, 0); raw[2].start = 0x100000; check(NNPI_BAR_OK);
    fixture(); put32(16, 4); put32(20, 0); check(NNPI_BAR_UNCLAIMED); /* unassigned BAR0 */
    fixture(); put32(24, 0xfffffff0); raw[0].start = 0xfffffff0;
    raw[0].length = trans[0].length = 0x20; check(NNPI_BAR_RANGE);
    fixture(); put32(16, 0xfffff004); put32(20, 0xffffffff);
    raw[2].start = UINT64_MAX - 0xfff; check(NNPI_BAR_OK); /* inclusive end UINT64_MAX */
    fixture(); assert(nnpi_bar_match(config, 63, raw, trans, 4, out, &count) == NNPI_BAR_CONFIG_UNAVAILABLE);
    assert(count == 0);
    assert(nnpi_bar_match(config, 64, raw, trans, 17, out, &count) == NNPI_BAR_RESOURCE_PAIR);
    assert(count == 0);
    test_memory_flags();
    printf("BAR association: %u scenarios passed; diagnostic ABI validation passed\n", cases);
    return 0;
}
