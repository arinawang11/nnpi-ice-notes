#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/nnpi_fifo_protocol.h"
#include "../include/nnpi_boot_protocol.h"

/* Independent literals: do not derive golden expectations from helper masks,
 * capacities, offsets, or boot encoder output. */
static const uint8_t golden_message[24] = {
    0x1f, 0x11, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x50, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void check_plan(const struct nnpi_fifo_command_plan *plan,
                       uint32_t count, uint32_t free_before)
{
    uint32_t i;
    assert(plan->count == count);
    assert(plan->free_before == free_before);
    assert(plan->free_after == free_before - count);
    for (i = 0; i < count; ++i) {
        assert(plan->writes[i].notify == (uint32_t)(i + 1u == count));
        assert(plan->writes[i].low_offset ==
               (i + 1u == count ? UINT32_C(0x58) : UINT32_C(0x50)));
        assert(plan->writes[i].high_offset ==
               (i + 1u == count ? UINT32_C(0x5c) : UINT32_C(0x54)));
    }
    for (; i < 16; ++i) {
        assert(plan->writes[i].value == 0);
        assert(plan->writes[i].low_offset == 0);
        assert(plan->writes[i].high_offset == 0);
        assert(plan->writes[i].notify == 0);
    }
}

static void fail_decode(uint64_t control, int expected)
{
    struct nnpi_fifo_snapshot out;
    unsigned char saved[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(saved, &out, sizeof(out));
    assert(nnpi_fifo_decode(control, &out) == expected);
    assert(memcmp(saved, &out, sizeof(out)) == 0);
}

static void fail_plan(uint64_t control, const uint8_t *bytes,
                      size_t size, int expected)
{
    struct nnpi_fifo_command_plan out;
    unsigned char saved[sizeof(out)];
    memset(&out, 0x5a, sizeof(out));
    memcpy(saved, &out, sizeof(out));
    assert(nnpi_fifo_plan_command(control, bytes, size, &out) == expected);
    assert(memcmp(saved, &out, sizeof(out)) == 0);
}

static void test_golden(void)
{
    static const struct {
        uint64_t control;
        uint32_t read_pointer, write_pointer, used, free_slots;
    } vectors[] = {
        {UINT64_C(0x00000000), 0, 0, 0, 16},
        {UINT64_C(0x00000f0f), 15, 15, 0, 16},
        {UINT64_C(0x00001000), 0, 16, 16, 0},
        {UINT64_C(0x00001f0f), 15, 31, 16, 0},
        {UINT64_C(0x0000100f), 15, 16, 1, 15},
        {UINT64_C(0x00001307), 7, 19, 12, 4},
        {UINT64_C(0xffffffff), 15, 31, 16, 0}
    };
    struct nnpi_fifo_snapshot snapshot;
    struct nnpi_fifo_command_plan plan;
    uint8_t boot_message[24];
    uint8_t extremes[16] = {0};
    size_t i;
    assert(NNPI_FIFO_DEPTH == 16);
    assert(NNPI_FIFO_QWORD_BYTES == 8);
    assert(NNPI_FIFO_COMMAND_CONTROL_OFFSET == 0x44);
    assert(NNPI_FIFO_READ_MASK == 0xf);
    assert(NNPI_FIFO_WRITE_MASK == 0x1f00);
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        assert(nnpi_fifo_decode(vectors[i].control, &snapshot) == NNPI_FIFO_OK);
        assert(snapshot.read_pointer == vectors[i].read_pointer);
        assert(snapshot.write_pointer == vectors[i].write_pointer);
        assert(snapshot.used_qwords == vectors[i].used);
        assert(snapshot.free_qwords == vectors[i].free_slots);
    }
    assert(nnpi_fifo_plan_command(0x1307, golden_message, 24, &plan) == NNPI_FIFO_OK);
    check_plan(&plan, 3, 4);
    assert(plan.writes[0].value == UINT64_C(0x000000000010111f));
    assert(plan.writes[1].value == UINT64_C(0x0000000012345000));
    assert(plan.writes[2].value == UINT64_C(0x0000000000001000));
    /* Existing codec integration is checked AGAINST independent bytes first. */
    assert(nnpi_boot_sysinfo(boot_message, sizeof(boot_message),
                            UINT64_C(0x12345000)) == NNPI_BOOT_OK);
    assert(memcmp(boot_message, golden_message, sizeof(boot_message)) == 0);
    assert(nnpi_fifo_plan_command(0, boot_message, sizeof(boot_message), &plan)
           == NNPI_FIFO_OK);
    check_plan(&plan, 3, 16);
    assert(plan.writes[0].value == UINT64_C(0x10111f));
    assert(plan.writes[1].value == UINT64_C(0x12345000));
    assert(plan.writes[2].value == UINT64_C(0x1000));
    memset(extremes + 8, 0xff, 8);
    assert(nnpi_fifo_plan_command(0, extremes, sizeof(extremes), &plan) == NNPI_FIFO_OK);
    assert(plan.writes[0].value == 0);
    assert(plan.writes[1].value == UINT64_MAX);
    check_plan(&plan, 2, 16);
    extremes[7] = 0x80;
    assert(nnpi_fifo_plan_command(0, extremes, 8, &plan) == NNPI_FIFO_OK);
    assert(plan.writes[0].value == UINT64_C(0x8000000000000000));
    check_plan(&plan, 1, 16);
}

static void test_exhaustive(void)
{
    uint32_t r, w, count, valid = 0, invalid = 0;
    uint8_t bytes[128];
    memset(bytes, 0x96, sizeof(bytes));
    for (r = 0; r < 16; ++r) {
        for (w = 0; w < 32; ++w) {
            struct nnpi_fifo_snapshot snapshot;
            uint64_t control = (uint64_t)w * 256u + r;
            if (w < r || w > r + 16) {
                ++invalid;
                fail_decode(control, NNPI_FIFO_BAD_STATE);
                fail_plan(control, bytes, 8, NNPI_FIFO_BAD_STATE);
                fail_plan(control, NULL, 0, NNPI_FIFO_BAD_STATE);
                continue;
            }
            ++valid;
            assert(nnpi_fifo_decode(control, &snapshot) == NNPI_FIFO_OK);
            assert(snapshot.read_pointer == r && snapshot.write_pointer == w);
            assert(snapshot.used_qwords == w - r);
            assert(snapshot.free_qwords == 16 - (w - r));
            /* Other bits are opaque: setting ALL of them must not change fields. */
            assert(nnpi_fifo_decode(control | UINT64_C(0xffffe0f0), &snapshot)
                   == NNPI_FIFO_OK);
            assert(snapshot.read_pointer == r && snapshot.write_pointer == w);
            assert(snapshot.used_qwords + snapshot.free_qwords == 16);
            for (count = 0; count <= 16; ++count) {
                struct nnpi_fifo_command_plan plan;
                uint32_t i;
                if (count > 16 - (w - r)) {
                    fail_plan(control, bytes, (size_t)count * 8, NNPI_FIFO_NO_SPACE);
                    continue;
                }
                assert(nnpi_fifo_plan_command(control, bytes, (size_t)count * 8,
                                              &plan) == NNPI_FIFO_OK);
                check_plan(&plan, count, 16 - (w - r));
                for (i = 0; i < count; ++i)
                    assert(plan.writes[i].value == UINT64_C(0x9696969696969696));
            }
        }
        for (count = 0; count <= 16; ++count) {
            uint32_t next = UINT32_MAX;
            uint32_t expected = r + count;
            if (expected >= 16)
                expected -= 16;
            assert(nnpi_fifo_ring_advance(r, count, &next) == NNPI_FIFO_OK);
            assert(next == expected);
        }
    }
    assert(valid == 272 && invalid == 240);
}

static void test_rejections(void)
{
    uint8_t bytes[136] = {0};
    struct nnpi_fifo_command_plan plan;
    uint32_t next = UINT32_C(0xdeadbeef);
    assert(nnpi_fifo_decode(0, NULL) == NNPI_FIFO_INVALID);
    fail_decode(UINT64_C(0x100000000), NNPI_FIFO_RANGE);
    fail_decode(UINT64_MAX, NNPI_FIFO_RANGE);
    fail_decode(0x00000f, NNPI_FIFO_BAD_STATE); /* Not modulo wrap. */
    fail_decode(0x001100, NNPI_FIFO_BAD_STATE); /* 17 occupied. */
    assert(nnpi_fifo_plan_command(0, bytes, 8, NULL) == NNPI_FIFO_INVALID);
    fail_plan(0, NULL, 8, NNPI_FIFO_INVALID);
    fail_plan(0, bytes, 1, NNPI_FIFO_RANGE);
    fail_plan(0, bytes, 7, NNPI_FIFO_RANGE);
    fail_plan(0, bytes, 9, NNPI_FIFO_RANGE);
    fail_plan(0, bytes, 127, NNPI_FIFO_RANGE);
    fail_plan(0, bytes, 129, NNPI_FIFO_RANGE);
    fail_plan(0, bytes, 136, NNPI_FIFO_RANGE);
    fail_plan(0, bytes, SIZE_MAX, NNPI_FIFO_RANGE);
    fail_plan(UINT64_C(0x100000000), bytes, 8, NNPI_FIFO_RANGE);
    fail_plan(UINT64_MAX, bytes, 8, NNPI_FIFO_RANGE);
    fail_plan(0x1000, bytes, 8, NNPI_FIFO_NO_SPACE);
    fail_plan(0x0f00, bytes, 16, NNPI_FIFO_NO_SPACE);
    assert(nnpi_fifo_plan_command(0x1000, NULL, 0, &plan) == NNPI_FIFO_OK);
    check_plan(&plan, 0, 0);
    assert(nnpi_fifo_plan_command(0, NULL, 0, &plan) == NNPI_FIFO_OK);
    check_plan(&plan, 0, 16);
    assert(nnpi_fifo_ring_advance(0, 0, NULL) == NNPI_FIFO_INVALID);
    assert(nnpi_fifo_ring_advance(16, 0, &next) == NNPI_FIFO_RANGE);
    assert(next == UINT32_C(0xdeadbeef));
    assert(nnpi_fifo_ring_advance(UINT32_MAX, 0, &next) == NNPI_FIFO_RANGE);
    assert(next == UINT32_C(0xdeadbeef));
    assert(nnpi_fifo_ring_advance(0, 17, &next) == NNPI_FIFO_RANGE);
    assert(next == UINT32_C(0xdeadbeef));
    assert(nnpi_fifo_ring_advance(0, UINT64_MAX, &next) == NNPI_FIFO_RANGE);
    assert(next == UINT32_C(0xdeadbeef));
}

int main(void)
{
    test_golden();
    test_exhaustive();
    test_rejections();
    puts("PASS fifo protocol: golden vectors; 272 valid/240 invalid snapshots;"
         " 4624 valid-state length cases; 272 ring advances; failure atomicity");
    puts("OFFLINE PURE LOGIC ONLY: no MMIO, DMA, driver, firmware or boot validation");
    return 0;
}
