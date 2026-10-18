/* SPDX-License-Identifier: GPL-2.0-only */
/* Portable shared-policy tests, NOT a real WDF/PnP/MMIO lifecycle test. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_status_policy.h"
static void le32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
static struct nnpi_diag_response fixture(void)
{
    struct nnpi_diag_response d;
    struct nnpi_bar_resource raw = {NNPI_BAR_MEMORY, 0, 0x80000000U, 0x1000U};
    struct nnpi_bar_resource trans = {NNPI_BAR_MEMORY, 0, 0x90000000U, 0x1000U};
    memset(&d, 0, sizeof(d));
    d.version = NNPI_DIAG_VERSION; d.size = (uint32_t)sizeof(d);
    d.config_bytes = 64; d.raw_count = d.translated_count = 1;
    le32(d.config, 0x45c68086U); le32(d.config + 44, 0x45ce8086U);
    le32(d.config + 16, 0x80000000U);
    d.reason = nnpi_bar_match(d.config, 64, &raw, &trans, 1, d.bars, &d.bar_count);
    assert(d.reason == NNPI_BAR_OK);
    return d;
}
static uint32_t reads;
static uint32_t read_once(void *context)
{
    ++reads;
    return *(uint32_t *)context;
}
static void failure(uint32_t flags, int removed, int mapped, uint64_t generation, uint32_t reason)
{
    struct nnpi_status_response r;
    struct nnpi_status_response zero;
    uint32_t value = 7, before = reads;
    memset(&r, 0xff, sizeof(r)); memset(&zero, 0, sizeof(zero));
    assert(nnpi_status_sample(flags, removed, mapped, generation, reason, read_once, &value, &r) != NNPI_STATUS_OK);
    assert(reads == before && memcmp(&r, &zero, sizeof(r)) == 0);
    assert(!nnpi_status_response_valid(&r, sizeof(r)));
}
int main(void)
{
    struct nnpi_status_request q = {NNPI_STATUS_VERSION, sizeof(q), {0, 0}};
    struct nnpi_status_response r, copy;
    struct nnpi_diag_response d = fixture(), bad;
    uint64_t address = 0;
    uint32_t i, value;
    void *mapping;
    assert(nnpi_status_request_valid(&q, sizeof(q)));
    assert(nnpi_status_buffers_valid(&q, sizeof(q), sizeof(r)));
    for (i = 0; i < sizeof(r) + 8; ++i)
        if (i != sizeof(r)) assert(!nnpi_status_buffers_valid(&q, sizeof(q), i));
    assert(!nnpi_status_request_valid(NULL, sizeof(q)));
    for (i = 0; i < sizeof(q) + 8; ++i)
        if (i != sizeof(q)) assert(!nnpi_status_request_valid(&q, i));
    q.version++; assert(!nnpi_status_request_valid(&q, sizeof(q))); q.version--;
    q.size--; assert(!nnpi_status_request_valid(&q, sizeof(q))); q.size++;
    for (i = 0; i < 2; ++i) { q.reserved[i] = 1; assert(!nnpi_status_request_valid(&q, sizeof(q))); q.reserved[i] = 0; }
    assert(nnpi_status_window(&d, &address) == NNPI_STATUS_OK && address == 0x900001b8U);
#define BAD_WINDOW(change) do { bad = d; change; address = 99; assert(nnpi_status_window(&bad, &address) != NNPI_STATUS_OK); assert(address == 0); } while (0)
    BAD_WINDOW(bad.config[0] ^= 1);
    BAD_WINDOW(bad.config[44] ^= 1);
    BAD_WINDOW(bad.config[46] ^= 1);
    BAD_WINDOW(bad.config[8] = 1);
    BAD_WINDOW(bad.config_bytes = 63);
    BAD_WINDOW(bad.reason = NNPI_BAR_NO_MATCH);
    BAD_WINDOW(bad.bars[0].bar = 2);
    BAD_WINDOW(bad.bars[0].type = NNPI_BAR_PORT);
    BAD_WINDOW(bad.bars[0].prefetch = 1);
    BAD_WINDOW(bad.bars[0].length = 0x1b8U);
    BAD_WINDOW(bad.bars[0].translated_start = UINT64_MAX - 3);
    BAD_WINDOW(bad.bars[0].translated_start = (uint64_t)INT64_MAX - 0x100);
    BAD_WINDOW(bad.bars[0].translated_start++);
    BAD_WINDOW(bad.bars[0].translated_index++);
    BAD_WINDOW(bad.bar_count = 7);
    BAD_WINDOW(bad.reserved[0] = 1);
#undef BAD_WINDOW
    failure(0, 0, 1, 1, 0); failure(1, 0, 1, 1, 0); failure(2, 0, 1, 1, 0);
    failure(3, 1, 1, 1, 0); failure(3, 0, 0, 1, 0); failure(3, 0, 1, 0, 0);
    for (i = NNPI_STATUS_IDENTITY; i <= NNPI_STATUS_NOT_READY; ++i) failure(3, 0, 1, 1, i);
    value = 0;
    assert(nnpi_status_sample(3, 0, 1, 9, 0, read_once, &value, &r) == 0 && reads == 1);
    assert(nnpi_status_response_valid(&r, sizeof(r)) && r.value == 0 && r.generation == 9);
    value = UINT32_MAX;
    assert(nnpi_status_sample(3, 0, 1, 10, 0, read_once, &value, &r) == 0 && reads == 2);
    assert(nnpi_status_response_valid(&r, sizeof(r)) && r.value == UINT32_MAX);
#define BAD_RESPONSE(change) do { copy = r; change; assert(!nnpi_status_response_valid(&copy, sizeof(copy))); } while (0)
    BAD_RESPONSE(copy.version++); BAD_RESPONSE(copy.size--); BAD_RESPONSE(copy.generation = 0);
    BAD_RESPONSE(copy.valid = 0); BAD_RESPONSE(copy.valid = 2); BAD_RESPONSE(copy.attributes ^= 16);
    BAD_RESPONSE(copy.read_count = 0); BAD_RESPONSE(copy.read_count = 2);
    for (i = 0; i < 4; ++i) { BAD_RESPONSE(copy.reserved[i] = 1); }
#undef BAD_RESPONSE
    for (i = 0; i < sizeof(r) + 8; ++i)
        if (i != sizeof(r)) assert(!nnpi_status_response_valid(&r, i));
    assert(!nnpi_status_response_valid(NULL, sizeof(r)));
    assert(nnpi_status_sample(3, 0, 1, 1, 0, NULL, NULL, &r) != 0 && r.valid == 0);
    mapping = &value;
    assert(nnpi_status_detach(&mapping) == &value && mapping == NULL);
    assert(nnpi_status_detach(&mapping) == NULL);
    failure(3, 0, mapping != NULL, 10, 0);
    assert(reads == 2);
    puts("status ABI/window/lifecycle shared-policy tests passed (no real WDF/hardware)");
    return 0;
}
