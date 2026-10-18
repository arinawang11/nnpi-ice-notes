/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_resetcaps_policy.h"
/* Synthetic layout size for pure policy tests, not a Windows structure replica. */
#define TEST_SIZE 56U
static struct nnpi_resetcaps_response sample(uint32_t status, uint32_t size,
    uint32_t version, int dereference, uint32_t bits, int handler)
{
    struct nnpi_resetcaps_response r;
    nnpi_resetcaps_init(&r, 9, TEST_SIZE, 1, status);
    nnpi_resetcaps_record(&r, size, version, dereference, bits, handler);
    assert(nnpi_resetcaps_response_valid(&r, sizeof(r)));
    return r;
}
int main(void)
{
    struct nnpi_resetcaps_request request = {1, 16, {0, 0}};
    struct nnpi_resetcaps_response r, out, bad;
    uint32_t bits, status;
    unsigned cleanup_count = 0;
    assert(nnpi_resetcaps_buffers_valid(&request, 16, 64));
    assert(!nnpi_resetcaps_buffers_valid(&request, 15, 64));
    assert(!nnpi_resetcaps_buffers_valid(&request, 17, 64));
    assert(!nnpi_resetcaps_buffers_valid(&request, 16, 63));
    assert(!nnpi_resetcaps_buffers_valid(&request, 16, 65));
    assert(!nnpi_resetcaps_request_valid(NULL, 16));
    request.reserved[0] = 1;
    assert(!nnpi_resetcaps_request_valid(&request, 16));
    request.reserved[0] = 0; request.reserved[1] = 1;
    assert(!nnpi_resetcaps_request_valid(&request, 16));
    request.reserved[1] = 0; request.version = 2;
    assert(!nnpi_resetcaps_request_valid(&request, 16));
    request.version = 1; request.size = 15;
    assert(!nnpi_resetcaps_request_valid(&request, 16));
    for (bits = 0; bits < 8; ++bits) {
        r = sample(0, TEST_SIZE, 1, 1, bits, 1);
        assert(r.supported == ((bits & 3U) ? 1U : 0U));
        assert(r.supported_reset_types == bits && r.handler_present && r.contract_valid);
        bad = r; bad.supported ^= 1;
        assert(!nnpi_resetcaps_response_valid(&bad, sizeof(bad)));
        r = sample(0, TEST_SIZE, 1, 1, bits, 0);
        assert(!r.supported && !r.handler_present && r.supported_reset_types == bits);
    }
    r = sample(0, TEST_SIZE, 1, 1, 0x80000000U, 1);
    assert(!r.supported && r.supported_reset_types == 0x80000000U);
    /* Enum ordinal 0 corresponds to bit 1; ordinal 1 corresponds to bit 2. */
    assert(NNPI_RESETCAPS_FUNCTION_LEVEL == 1U && NNPI_RESETCAPS_PLATFORM_LEVEL == 2U);
    r = sample(0, TEST_SIZE - 1, 1, 1, 3, 1);
    assert(!r.contract_valid && !r.supported && !r.handler_present && !r.supported_reset_types);
    r = sample(0, 0, 1, 1, 3, 1);
    assert(!r.contract_valid);
    r = sample(0, TEST_SIZE, 2, 1, 3, 1);
    assert(!r.contract_valid);
    r = sample(0, TEST_SIZE, 1, 0, 3, 1);
    assert(!r.contract_valid && !r.supported);
    r = sample(0, TEST_SIZE + 32, 1, 1, 3, 1);
    assert(r.contract_valid && r.supported); /* Ignore unrequested future tail. */
    status = 0xc00000bbU;
    r = sample(status, TEST_SIZE, 1, 1, 3, 1);
    assert(r.query_ntstatus == status && !r.returned_size && !r.returned_version);
    assert(!r.supported && !r.contract_valid);
    /* A successful short/wrong-version result still owns exactly one ref.
     * Failed query with scribbled callback must never call it. */
    if (nnpi_resetcaps_needs_dereference(0, 1)) ++cleanup_count;
    if (nnpi_resetcaps_needs_dereference(0, 1)) ++cleanup_count;
    if (nnpi_resetcaps_needs_dereference(status, 1)) ++cleanup_count;
    if (nnpi_resetcaps_needs_dereference(0, 0)) ++cleanup_count;
    assert(cleanup_count == 2);
    assert(nnpi_resetcaps_query(&r, 3, 0, 9, &out) == NNPI_RESET_GATE_OK);
    assert(out.query_ntstatus == status && !out.supported);
    assert(nnpi_resetcaps_query(&r, 1, 0, 9, &out) == NNPI_RESET_GATE_NOT_READY);
    assert(!out.version);
    assert(nnpi_resetcaps_query(&r, 3, 0, 10, &out) == NNPI_RESET_GATE_NOT_READY);
    assert(nnpi_resetcaps_query(&r, 3, 1, 9, &out) == NNPI_RESET_GATE_REMOVED);
    assert(nnpi_resetcaps_query(&r, 7, 0, 9, &out) == NNPI_RESET_GATE_NOT_READY);
    assert(nnpi_resetcaps_query(&r, 3, 0, 0, &out) == NNPI_RESET_GATE_NOT_READY);
    bad = r; bad.reserved[2] = 1;
    assert(!nnpi_resetcaps_response_valid(&bad, sizeof(bad)));
    bad = r; bad.supported = 1;
    assert(!nnpi_resetcaps_response_valid(&bad, sizeof(bad)));
    assert(!nnpi_resetcaps_response_valid(&r, 63));
    assert(!nnpi_resetcaps_response_valid(&r, 65));
    assert(!nnpi_resetcaps_response_valid(NULL, 64));
    memset(&r, 0, sizeof(r));
    assert(nnpi_resetcaps_query(&r, 3, 0, 9, &out) == NNPI_RESET_GATE_NOT_READY);
    puts("resetcaps ABI/policy tests passed (no Windows interface or hardware operations)");
    return 0;
}
