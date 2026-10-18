#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/nnpi_boot_protocol.h"
#include "../include/nnpi_response_fifo.h"
#include "../include/nnpi_fifo_protocol.h"

/* Expected values below use independent literal hardware vectors, NOT helpers. */
static void check_unused(const struct nnpi_response_fifo_plan *p)
{
    uint32_t i;
    for (i = p->read_count; i < 16; ++i) {
        assert(p->reads[i].slot == 0);
        assert(p->reads[i].low_offset == 0);
        assert(p->reads[i].high_offset == 0);
    }
    for (i = p->ack_count; i < 2; ++i) {
        assert(p->acks[i].offset == 0);
        assert(p->acks[i].value == 0);
    }
}

static void fail_plan(uint64_t control, int expected)
{
    struct nnpi_response_fifo_plan out;
    unsigned char before[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(before, &out, sizeof(out));
    assert(nnpi_response_fifo_plan_drain(control, &out) == expected);
    assert(memcmp(before, &out, sizeof(out)) == 0);
}

static void test_golden(void)
{
    static const struct {
        uint64_t control;
        uint32_t r, w, count, final_pointer, ack_count;
        uint32_t ack0, ack1;
    } vectors[] = {
        {0x00000000, 0, 0, 0, 0, 0, 0, 0},
        {0x00000f0f, 15, 15, 0, 15, 0, 0, 0},
        {0x00000100, 0, 1, 1, 1, 1, 0x00000101, 0},
        {0x0000100f, 15, 16, 1, 0, 1, 0x00001000, 0},
        {0x0000110e, 14, 17, 3, 1, 1, 0x00001101, 0},
        {0x00001000, 0, 16, 16, 0, 2, 0x00001001, 0x00001000},
        {0x00001707, 7, 23, 16, 7, 2, 0x00001708, 0x00001707},
        {0x00001f0f, 15, 31, 16, 15, 2, 0x00001f00, 0x00001f0f},
        {0xa5a5f10e, 14, 17, 3, 1, 1, 0xa5a5f101, 0},
        {0xffffffff, 15, 31, 16, 15, 2, 0xfffffff0, 0xffffffff}
    };
    static const uint32_t full_low[16] = {
        0x108, 0x110, 0x118, 0x120, 0x128, 0x130, 0x138, 0x140,
        0x148, 0x150, 0x158, 0x160, 0x168, 0x170, 0x178, 0x100
    };
    static const uint32_t full_high[16] = {
        0x10c, 0x114, 0x11c, 0x124, 0x12c, 0x134, 0x13c, 0x144,
        0x14c, 0x154, 0x15c, 0x164, 0x16c, 0x174, 0x17c, 0x104
    };
    struct nnpi_response_fifo_plan p;
    size_t i;
    assert(NNPI_RESPONSE_CONTROL_OFFSET == 0x60);
    assert(NNPI_RESPONSE_FIFO_BASE_OFFSET == 0x100);
    assert(NNPI_RESPONSE_MAX_ACKS == 2);
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        memset(&p, 0xcc, sizeof(p));
        assert(nnpi_response_fifo_plan_drain(vectors[i].control, &p) == NNPI_FIFO_OK);
        assert(p.read_pointer == vectors[i].r);
        assert(p.write_pointer == vectors[i].w);
        assert(p.read_count == vectors[i].count);
        assert(p.final_read_pointer == vectors[i].final_pointer);
        assert(p.ack_count == vectors[i].ack_count);
        assert(p.acks[0].value == vectors[i].ack0);
        assert(p.acks[1].value == vectors[i].ack1);
        if (p.ack_count > 0)
            assert(p.acks[0].offset == 0x60);
        if (p.ack_count > 1)
            assert(p.acks[1].offset == 0x60);
        check_unused(&p);
    }
    assert(nnpi_response_fifo_plan_drain(0x1000, &p) == NNPI_FIFO_OK);
    for (i = 0; i < 16; ++i) {
        assert(p.reads[i].low_offset == full_low[i]);
        assert(p.reads[i].high_offset == full_high[i]);
    }
    assert(nnpi_response_fifo_plan_drain(0x110e, &p) == NNPI_FIFO_OK);
    assert(p.reads[0].slot == 15 && p.reads[0].low_offset == 0x178);
    assert(p.reads[0].high_offset == 0x17c);
    assert(p.reads[1].slot == 0 && p.reads[1].low_offset == 0x100);
    assert(p.reads[1].high_offset == 0x104);
    assert(p.reads[2].slot == 1 && p.reads[2].low_offset == 0x108);
    assert(p.reads[2].high_offset == 0x10c);
}

static void test_exhaustive(void)
{
    uint32_t r, w, valid = 0, invalid = 0;
    /* Each opaque bit alone, all opaque bits, and all-clear: 25 variants. */
    uint32_t variants[34];
    uint32_t n = 0, bit, v;
    variants[n++] = 0;
    variants[n++] = UINT32_C(0xffffe0f0);
    for (bit = 0; bit < 32; ++bit) {
        uint32_t mask = UINT32_C(1) << bit;
        if ((mask & UINT32_C(0xffffe0f0)) != 0)
            variants[n++] = mask;
    }
    assert(n == 25); /* 32 - 4 - 5 = 23 opaque bits, plus two extremes. */
    for (r = 0; r < 16; ++r) {
        for (w = 0; w < 32; ++w) {
            int good = w >= r && w <= r + 16;
            if (good)
                ++valid;
            else
                ++invalid;
            for (v = 0; v < n; ++v) {
                uint32_t control = w * 256u + r + variants[v];
                struct nnpi_response_fifo_plan p;
                uint32_t count, current, i, seen = 0;
                if (!good) {
                    fail_plan(control, NNPI_FIFO_BAD_STATE);
                    continue;
                }
                count = w - r;
                current = r;
                memset(&p, 0x55, sizeof(p));
                assert(nnpi_response_fifo_plan_drain(control, &p) == NNPI_FIFO_OK);
                assert(p.read_pointer == r && p.write_pointer == w);
                assert(p.read_count == count);
                for (i = 0; i < count; ++i) {
                    current = current == 15 ? 0 : current + 1;
                    assert(p.reads[i].slot == current);
                    assert((seen & (UINT32_C(1) << current)) == 0);
                    seen |= UINT32_C(1) << current;
                    assert(p.reads[i].low_offset == 256 + current * 8);
                    assert(p.reads[i].high_offset == 260 + current * 8);
                    assert(p.reads[i].high_offset <= 0x17c);
                }
                assert(p.final_read_pointer == current);
                assert(p.ack_count == (count == 0 ? 0u : count == 16 ? 2u : 1u));
                if (count == 16) {
                    uint32_t intermediate = r == 15 ? 0 : r + 1;
                    assert(seen == 0xffff);
                    assert(current == r);
                    assert(p.acks[0].value == control - r + intermediate);
                    assert(p.acks[1].value == control);
                    assert(p.acks[0].value != p.acks[1].value);
                } else if (count != 0) {
                    assert(current != r);
                    assert(p.acks[0].value == control - r + current);
                }
                for (i = 0; i < p.ack_count; ++i) {
                    assert(p.acks[i].offset == 0x60);
                    assert((p.acks[i].value & UINT32_C(0xfffffff0)) ==
                           (control & UINT32_C(0xfffffff0)));
                }
                check_unused(&p);
            }
        }
    }
    assert(valid == 272 && invalid == 240);
}

static void test_rejections_and_coexistence(void)
{
    uint32_t bit;
    uint8_t bytes[24];
    struct nnpi_fifo_command_plan command;
    struct nnpi_response_fifo_plan response;
    assert(nnpi_response_fifo_plan_drain(0, NULL) == NNPI_FIFO_INVALID);
    fail_plan(0x0000000f, NNPI_FIFO_BAD_STATE); /* w<r is NOT modulo wrap. */
    fail_plan(0x00001100, NNPI_FIFO_BAD_STATE); /* 17 occupied. */
    fail_plan(0x00001f00, NNPI_FIFO_BAD_STATE); /* 31 occupied. */
    fail_plan(UINT64_C(0x100000000), NNPI_FIFO_RANGE);
    fail_plan(UINT64_MAX, NNPI_FIFO_RANGE);
    for (bit = 32; bit < 64; ++bit)
        fail_plan((UINT64_C(1) << bit) | UINT64_C(0x1f0f), NNPI_FIFO_RANGE);
    /* Command and response plans are independent; no response opcode is inferred. */
    assert(nnpi_boot_sysinfo(bytes, sizeof(bytes), UINT64_C(0x12345000)) == NNPI_BOOT_OK);
    assert(nnpi_fifo_plan_command(0, bytes, sizeof(bytes), &command) == NNPI_FIFO_OK);
    assert(command.count == 3 && command.writes[2].low_offset == 0x58);
    assert(command.writes[0].value == UINT64_C(0x10111f));
    assert(nnpi_response_fifo_plan_drain(0x0300, &response) == NNPI_FIFO_OK);
    assert(response.read_count == 3 && response.reads[0].low_offset == 0x108);
    assert(response.acks[0].value == 0x0303);
}

int main(void)
{
    test_golden();
    test_exhaustive();
    test_rejections_and_coexistence();
    puts("PASS response FIFO: independent golden vectors; 272 valid/240 invalid states;"
         " 25 opaque-bit variants (12800 cases); range/failure atomicity; codec coexistence");
    puts("OFFLINE PURE LOGIC ONLY: no MMIO, hardware FIFO, firmware, boot or SSH validation");
    return 0;
}
