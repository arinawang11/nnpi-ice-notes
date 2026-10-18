/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_dmacheck_policy.h"
/* Fake platform ONLY; production shares the orchestration, gates and CPU test. */
struct fake {
    uint8_t page[NNPI_DMACHECK_BYTES];
    uint64_t logical, bytes;
    uint32_t allocation_status;
    int live, has_va, allocations, deletes, reads, writes, phase, fault;
};
static uint32_t allocate_page(void *c, uint64_t *logical, uint64_t *bytes, int *has_va)
{
    struct fake *f = (struct fake *)c;
    ++f->allocations;
    f->live = 1; /* Even injected failure has a partial object to clean. */
    *logical = f->logical; *bytes = f->bytes; *has_va = f->has_va;
    f->phase = 0;
    return f->allocation_status;
}
static void write_byte(void *c, size_t offset, uint8_t value)
{
    struct fake *f = (struct fake *)c;
    assert(f->live && offset < sizeof(f->page));
    if (!offset) ++f->phase;
    ++f->writes;
    f->page[offset] = value;
}
static uint8_t read_byte(void *c, size_t offset)
{
    struct fake *f = (struct fake *)c;
    assert(f->live && offset < sizeof(f->page));
    ++f->reads;
    if (offset == 77 && f->phase == f->fault) return (uint8_t)(f->page[offset] ^ 1U);
    return f->page[offset];
}
static void release_page(void *c)
{
    struct fake *f = (struct fake *)c;
    if (f->live) {
        (void)nnpi_dmacheck_zero(c, sizeof(f->page), write_byte, read_byte);
        f->live = 0;
        ++f->deletes;
    }
}
static void init_fake(struct fake *f)
{
    memset(f, 0, sizeof(*f));
    f->logical = 0x100000U; f->bytes = NNPI_DMACHECK_BYTES; f->has_va = 1;
}
static void prepare(struct fake *f, struct nnpi_dmacheck_response *r, uint64_t gen, uint32_t status)
{
    nnpi_dmacheck_prepare(r, gen, status, f, allocate_page, release_page, write_byte, read_byte);
    assert(nnpi_dmacheck_response_valid(r, sizeof(*r)));
}
static void test_layout(void)
{
    assert(nnpi_dmacheck_layout(0, 4096, 1) == NNPI_DMA_READY);
    assert(nnpi_dmacheck_layout(4096, 4096, 0) == NNPI_DMA_VIRTUAL_ADDRESS);
    assert(nnpi_dmacheck_layout(4096, 0, 1) == NNPI_DMA_LENGTH);
    assert(nnpi_dmacheck_layout(4096, 4095, 1) == NNPI_DMA_LENGTH);
    assert(nnpi_dmacheck_layout(4096, 4097, 1) == NNPI_DMA_LENGTH);
    assert(nnpi_dmacheck_layout(4097, 4096, 1) == NNPI_DMA_ALIGNMENT);
    assert(nnpi_dmacheck_layout(NNPI_BOOT_DMA_MAX - 4095U, 4096, 1) == NNPI_DMA_READY);
    assert(nnpi_dmacheck_layout(NNPI_BOOT_DMA_MAX + 1U, 4096, 1) == NNPI_DMA_RANGE);
    assert(nnpi_dmacheck_layout(UINT64_MAX - 4095U, 4096, 1) == NNPI_DMA_RANGE);
    assert(nnpi_dmacheck_layout(0, UINT64_MAX, 1) == NNPI_DMA_LENGTH);
}
static void test_lifetime(void)
{
    struct fake f;
    struct nnpi_dmacheck_response r, out;
    int reads, writes, allocations;
    init_fake(&f);
    prepare(&f, &r, 1, 0);
    assert(r.passed && r.retained && r.checks == NNPI_DMACHECK_ALL && f.live);
    assert(f.reads == 3 * 4096 && f.writes == 3 * 4096);
    reads = f.reads; writes = f.writes; allocations = f.allocations;
    assert(nnpi_dmacheck_query(&r, 3, 0, 1, &out) == NNPI_DMA_GATE_OK);
    assert(out.passed && f.reads == reads && f.writes == writes && f.allocations == allocations);
    assert(nnpi_dmacheck_query(&r, 1, 0, 1, &out) == NNPI_DMA_GATE_NOT_READY); /* D0Exit */
    assert(!out.passed && !out.generation);
    assert(nnpi_dmacheck_query(&r, 2, 0, 1, &out) == NNPI_DMA_GATE_NOT_READY);
    assert(nnpi_dmacheck_query(&r, 0, 0, 1, &out) == NNPI_DMA_GATE_NOT_READY);
    assert(nnpi_dmacheck_query(&r, 7, 0, 1, &out) == NNPI_DMA_GATE_NOT_READY);
    assert(nnpi_dmacheck_query(&r, 3, 1, 1, &out) == NNPI_DMA_GATE_REMOVED);
    assert(nnpi_dmacheck_query(&r, 3, 0, 0, &out) == NNPI_DMA_GATE_NOT_READY);
    assert(nnpi_dmacheck_query(&r, 3, 0, 2, &out) == NNPI_DMA_GATE_NOT_READY);
    prepare(&f, &r, 2, 0); /* Reprepare destroys old page before allocating. */
    assert(f.allocations == 2 && f.deletes == 1 && r.generation == 2);
    assert(nnpi_dmacheck_query(&r, 3, 0, 1, &out) == NNPI_DMA_GATE_NOT_READY);
    nnpi_dmacheck_release(&r, &f, release_page);
    nnpi_dmacheck_release(&r, &f, release_page); /* Surprise then Release is idempotent. */
    assert(f.deletes == 2 && !f.live && !r.passed && !r.generation);
    assert(nnpi_dmacheck_query(&r, 3, 0, 2, &out) == NNPI_DMA_GATE_NOT_READY);
}
static void test_failures(void)
{
    struct fake f;
    struct nnpi_dmacheck_response r, out;
    uint32_t stage;
    size_t i;
    for (stage = NNPI_DMA_ENABLER; stage < NNPI_DMA_READY; ++stage) {
        init_fake(&f);
        if (stage == NNPI_DMA_ALLOCATE) f.allocation_status = 0xc000009aU;
        if (stage == NNPI_DMA_VIRTUAL_ADDRESS) f.has_va = 0;
        if (stage == NNPI_DMA_LENGTH) f.bytes = 4095;
        if (stage == NNPI_DMA_ALIGNMENT) f.logical++;
        if (stage == NNPI_DMA_RANGE) f.logical = NNPI_BOOT_DMA_MAX + 1;
        if (stage >= NNPI_DMA_INITIAL_ZERO) f.fault = (int)(stage - NNPI_DMA_INITIAL_ZERO + 1);
        prepare(&f, &r, 5, stage == NNPI_DMA_ENABLER ? 0xc00000bbU : 0U);
        assert(r.stage == stage && !r.passed && !r.retained && !f.live);
        assert(f.deletes == (stage == NNPI_DMA_ENABLER ? 0 : 1));
        assert(nnpi_dmacheck_query(&r, 3, 0, 5, &out) == NNPI_DMA_GATE_OK);
        assert(out.stage == stage && !out.passed && out.ntstatus == r.ntstatus);
        out.passed = 1; assert(!nnpi_dmacheck_response_valid(&out, sizeof(out))); out = r;
        out.retained = 1; assert(!nnpi_dmacheck_response_valid(&out, sizeof(out))); out = r;
        out.ntstatus = 0; assert(!nnpi_dmacheck_response_valid(&out, sizeof(out))); out = r;
        out.checks ^= 1U; assert(!nnpi_dmacheck_response_valid(&out, sizeof(out)));
        if (stage == NNPI_DMA_LENGTH) {
            out = r; out.actual_bytes = NNPI_DMACHECK_BYTES;
            assert(!nnpi_dmacheck_response_valid(&out, sizeof(out)));
        }
        for (i = 0; i < sizeof(f.page); ++i) assert(!f.page[i]);
        nnpi_dmacheck_release(&r, &f, release_page);
        assert(f.deletes == (stage == NNPI_DMA_ENABLER ? 0 : 1));
    }
}
static void test_abi(void)
{
    struct nnpi_dmacheck_request q = {NNPI_DMACHECK_VERSION, sizeof(q), {0, 0}};
    struct nnpi_dmacheck_response r, bad;
    struct fake f;
    size_t i;
    assert(nnpi_dmacheck_buffers_valid(&q, 16, 64));
    assert(!nnpi_dmacheck_buffers_valid(&q, 15, 64));
    assert(!nnpi_dmacheck_buffers_valid(&q, 17, 64));
    assert(!nnpi_dmacheck_buffers_valid(&q, 16, 63));
    assert(!nnpi_dmacheck_buffers_valid(&q, 16, 65));
    assert(!nnpi_dmacheck_request_valid(NULL, 16));
    q.reserved[0] = 1; assert(!nnpi_dmacheck_request_valid(&q, 16)); q.reserved[0] = 0;
    q.reserved[1] = 1; assert(!nnpi_dmacheck_request_valid(&q, 16)); q.reserved[1] = 0;
    q.version++; assert(!nnpi_dmacheck_request_valid(&q, 16)); q.version--;
    q.size++; assert(!nnpi_dmacheck_request_valid(&q, 16));
    init_fake(&f); prepare(&f, &r, 1, 0);
    assert(!nnpi_dmacheck_response_valid(NULL, 64));
    assert(!nnpi_dmacheck_response_valid(&r, 63));
    assert(!nnpi_dmacheck_response_valid(&r, 65));
    /* Every scalar except generation must reject an increment in a success record. */
    for (i = 0; i < sizeof(r); i += 4) {
        uint32_t word;
        if (i == 8 || i == 12) continue;
        bad = r;
        memcpy(&word, (const uint8_t *)&bad + i, 4); ++word;
        memcpy((uint8_t *)&bad + i, &word, 4);
        assert(!nnpi_dmacheck_response_valid(&bad, sizeof(bad)));
    }
    bad = r; bad.generation = 0; assert(!nnpi_dmacheck_response_valid(&bad, sizeof(bad)));
    bad = r; bad.stage = NNPI_DMA_PATTERN; assert(!nnpi_dmacheck_response_valid(&bad, sizeof(bad)));
    nnpi_dmacheck_release(&r, &f, release_page);
}
int main(void)
{
    test_layout(); test_lifetime(); test_failures(); test_abi();
    puts("dmacheck shared ABI/policy tests passed (fake platform, NOT WDF or device DMA validation)");
    return 0;
}
