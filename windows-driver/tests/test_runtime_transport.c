#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_runtime_transport.h"

struct fake_write {
    uint32_t address, width, value;
};
struct fake_mmio {
    uint32_t base, calls, fail_call, fail_after_write, writes;
    struct fake_write trace[2];
    unsigned char bytes[0x180];
};

/* Model one write32 including failure before or after its side effect. The
 * tested sender is the exact helper used by production NnpiQueryVersion. */
static int write32(void *context, uint32_t relative, uint32_t value)
{
    struct fake_mmio *mmio = (struct fake_mmio *)context;
    uint32_t address = mmio->base + relative;
    ++mmio->calls;
    assert(mmio->calls <= 2);
    assert(relative == (mmio->calls == 1 ? 8u : 12u));
    assert(address == (mmio->calls == 1 ? 0x58u : 0x5cu));
    assert(value == 0);
    if (mmio->calls == mmio->fail_call && !mmio->fail_after_write)
        return 0;
    mmio->trace[mmio->writes].address = address;
    mmio->trace[mmio->writes].width = 4;
    mmio->trace[mmio->writes].value = value;
    ++mmio->writes;
    memcpy(mmio->bytes + address, &value, 4);
    return mmio->calls != mmio->fail_call;
}
static void init(struct fake_mmio *mmio)
{
    memset(mmio, 0, sizeof(*mmio));
    memset(mmio->bytes, 0xa5, sizeof(mmio->bytes));
    mmio->base = 0x50;
}
static void unchanged_outside_ports(const struct fake_mmio *mmio)
{
    uint32_t i;
    for (i = 0; i < sizeof(mmio->bytes); ++i) {
        if (i >= 0x58 && i < 0x58 + mmio->writes * 4)
            assert(mmio->bytes[i] == 0);
        else
            assert(mmio->bytes[i] == 0xa5);
    }
    /* Explicit regression: response/control space must not receive query. */
    for (i = 0x60; i < 0x70; ++i) assert(mmio->bytes[i] == 0xa5);
}
static void test_success(void)
{
    struct nnpi_runtime_query_state state = {0}, saved;
    struct fake_mmio mmio;
    init(&mmio);
    assert(nnpi_runtime_query_send(&state, mmio.base, 1, &mmio, write32) == NNPI_RUNTIME_QUERY_OK);
    assert(state.used && state.attempted_halves == 2 && state.completed_halves == 2);
    assert(state.last_relative_offset == 12 && state.last_value == 0);
    assert(state.result == NNPI_RUNTIME_QUERY_OK);
    assert(mmio.calls == 2 && mmio.writes == 2);
    assert(mmio.trace[0].address == 0x58 && mmio.trace[1].address == 0x5c);
    assert(mmio.trace[0].width == 4 && mmio.trace[1].width == 4);
    assert(mmio.trace[0].value == 0 && mmio.trace[1].value == 0);
    saved = state;
    assert(nnpi_runtime_query_send(&state, mmio.base, 1, &mmio, write32) == NNPI_RUNTIME_QUERY_USED);
    assert(memcmp(&state, &saved, sizeof(state)) == 0);
    assert(mmio.calls == 2 && mmio.writes == 2);
    unchanged_outside_ports(&mmio);
}
static void test_partial_never_replays(void)
{
    uint32_t fail, after;
    for (fail = 1; fail <= 2; ++fail) for (after = 0; after <= 1; ++after) {
        struct nnpi_runtime_query_state state = {0}, saved;
        struct fake_mmio mmio;
        init(&mmio);
        mmio.fail_call = fail;
        mmio.fail_after_write = after;
        assert(nnpi_runtime_query_send(&state, mmio.base, 1, &mmio, write32) == NNPI_RUNTIME_QUERY_WRITE_FAILED);
        assert(state.used && state.attempted_halves == fail && state.completed_halves == fail - 1);
        assert(mmio.calls == fail && mmio.writes == fail - 1 + after);
        assert(state.result == NNPI_RUNTIME_QUERY_WRITE_FAILED);
        saved = state;
        assert(nnpi_runtime_query_send(&state, mmio.base, 1, &mmio, write32) == NNPI_RUNTIME_QUERY_USED);
        assert(memcmp(&state, &saved, sizeof(state)) == 0 && mmio.calls == fail);
        unchanged_outside_ports(&mmio);
    }
}
static void test_wrong_mapping_and_arguments(void)
{
    struct nnpi_runtime_query_state state = {0};
    struct fake_mmio mmio;
    uint32_t base;
    init(&mmio);
    /* Exact original bug: response mapping is 0x060, not command 0x050. */
    mmio.base = 0x60;
    assert(nnpi_runtime_query_send(&state, mmio.base, 1, &mmio, write32) == NNPI_RUNTIME_QUERY_INVALID);
    assert(!state.used && !state.attempted_halves && !mmio.calls);
    for (base = 0; base < 0x180; ++base) {
        if (base == 0x50) continue;
        assert(nnpi_runtime_query_send(&state, base, 1, &mmio, write32) == NNPI_RUNTIME_QUERY_INVALID);
    }
    assert(nnpi_runtime_query_send(&state, 0x50, 0, &mmio, write32) == NNPI_RUNTIME_QUERY_INVALID);
    assert(nnpi_runtime_query_send(&state, 0x50, 1, NULL, write32) == NNPI_RUNTIME_QUERY_INVALID);
    assert(nnpi_runtime_query_send(&state, 0x50, 1, &mmio, NULL) == NNPI_RUNTIME_QUERY_INVALID);
    assert(nnpi_runtime_query_send(NULL, 0x50, 1, &mmio, write32) == NNPI_RUNTIME_QUERY_INVALID);
    assert(!state.used && !mmio.calls && !mmio.writes);
    unchanged_outside_ports(&mmio);
    state.completed_halves = 1; /* Corrupt or partial state is not a fresh grant. */
    assert(nnpi_runtime_query_send(&state, 0x50, 1, &mmio, write32) == NNPI_RUNTIME_QUERY_USED);
    assert(!mmio.calls);
}
int main(void)
{
    test_success();
    test_partial_never_replays();
    test_wrong_mapping_and_arguments();
    puts("runtime transport: fixed write32 addresses/order/values/counts and fail-stop passed");
    return 0;
}
