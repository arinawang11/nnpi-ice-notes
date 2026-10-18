#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_fifo_protocol.h"

static void le64(uint64_t v, uint8_t out[8])
{
    unsigned i;
    for (i = 0; i < 8; ++i)
        out[i] = (uint8_t)(v >> (8u * i));
}

static void test_single_qw(void)
{
    uint64_t qw = 22ULL | (256ULL << 6);
    uint8_t wire[8];
    struct nnpi_fifo_command_plan plan;
    struct nnpi_fifo_half_write halves[NNPI_FIFO_HALF_MAX];
    uint32_t count;
    le64(qw, wire);
    assert(nnpi_fifo_plan_command(0, wire, 8, &plan) == NNPI_FIFO_OK);
    assert(plan.count == 1 && plan.writes[0].notify == 1);
    assert(nnpi_fifo_plan_halves(&plan, halves, NNPI_FIFO_HALF_MAX, &count) == NNPI_FIFO_OK);
    assert(count == 2);
    /* Single QW is the notify port: relative offsets 8 (low) and 12 (high). */
    assert(halves[0].relative_offset == 8 && halves[1].relative_offset == 12);
    assert(halves[0].value == (uint32_t)qw);
    assert(halves[1].value == (uint32_t)(qw >> 32));
}

static void test_two_qw(void)
{
    uint64_t qw0 = 57ULL | (256ULL << 6) | (UINT64_C(0x020200c0) << 32);
    uint64_t qw1 = 2ULL << 16; /* mac[0] */
    uint8_t wire[16];
    struct nnpi_fifo_command_plan plan;
    struct nnpi_fifo_half_write halves[NNPI_FIFO_HALF_MAX];
    uint32_t count;
    le64(qw0, wire);
    le64(qw1, wire + 8);
    assert(nnpi_fifo_plan_command(0, wire, 16, &plan) == NNPI_FIFO_OK);
    assert(plan.count == 2);
    assert(nnpi_fifo_plan_halves(&plan, halves, NNPI_FIFO_HALF_MAX, &count) == NNPI_FIFO_OK);
    assert(count == 4);
    /* First QW non-notify (0/4), second QW notify (8/12). */
    assert(halves[0].relative_offset == 0 && halves[1].relative_offset == 4);
    assert(halves[2].relative_offset == 8 && halves[3].relative_offset == 12);
    assert(halves[0].value == (uint32_t)qw0 && halves[1].value == (uint32_t)(qw0 >> 32));
    assert(halves[2].value == (uint32_t)qw1 && halves[3].value == (uint32_t)(qw1 >> 32));
}

static void test_three_qw(void)
{
    uint64_t qws[3] = { 1, 2, 3 };
    uint8_t wire[24];
    struct nnpi_fifo_command_plan plan;
    struct nnpi_fifo_half_write halves[NNPI_FIFO_HALF_MAX];
    uint32_t count, i;
    for (i = 0; i < 3; ++i)
        le64(qws[i], wire + i * 8);
    assert(nnpi_fifo_plan_command(0, wire, 24, &plan) == NNPI_FIFO_OK);
    assert(plan.count == 3);
    assert(nnpi_fifo_plan_halves(&plan, halves, NNPI_FIFO_HALF_MAX, &count) == NNPI_FIFO_OK);
    assert(count == 6);
    /* QW0/QW1 non-notify (0/4, 0/4), QW2 notify (8/12). */
    assert(halves[0].relative_offset == 0 && halves[1].relative_offset == 4);
    assert(halves[2].relative_offset == 0 && halves[3].relative_offset == 4);
    assert(halves[4].relative_offset == 8 && halves[5].relative_offset == 12);
    assert(halves[0].value == 1 && halves[1].value == 0);
    assert(halves[2].value == 2 && halves[3].value == 0);
    assert(halves[4].value == 3 && halves[5].value == 0);
}

static void test_negative(void)
{
    struct nnpi_fifo_command_plan plan;
    struct nnpi_fifo_half_write halves[NNPI_FIFO_HALF_MAX];
    uint32_t count = 99;
    /* NULL arguments. */
    assert(nnpi_fifo_plan_halves(NULL, halves, NNPI_FIFO_HALF_MAX, &count) == NNPI_FIFO_INVALID);
    assert(nnpi_fifo_plan_halves(&plan, NULL, NNPI_FIFO_HALF_MAX, &count) == NNPI_FIFO_INVALID);
    assert(nnpi_fifo_plan_halves(&plan, halves, NNPI_FIFO_HALF_MAX, NULL) == NNPI_FIFO_INVALID);
    /* count > depth. */
    memset(&plan, 0, sizeof(plan));
    plan.count = NNPI_FIFO_DEPTH + 1;
    assert(nnpi_fifo_plan_halves(&plan, halves, NNPI_FIFO_HALF_MAX, &count) == NNPI_FIFO_INVALID);
    /* max_halves too small for count*2. */
    plan.count = 2;
    plan.writes[0].low_offset = NNPI_FIFO_COMMAND_NO_NOTIFY_OFFSET;
    plan.writes[0].high_offset = plan.writes[0].low_offset + 4;
    plan.writes[1].low_offset = NNPI_FIFO_COMMAND_NOTIFY_OFFSET;
    plan.writes[1].high_offset = plan.writes[1].low_offset + 4;
    assert(nnpi_fifo_plan_halves(&plan, halves, 3, &count) == NNPI_FIFO_RANGE);
    assert(nnpi_fifo_plan_halves(&plan, halves, 4, &count) == NNPI_FIFO_OK);
    assert(count == 4);
    /* Illegal low_offset (not 0x050/0x058). */
    plan.writes[0].low_offset = 0x060;
    assert(nnpi_fifo_plan_halves(&plan, halves, 4, &count) == NNPI_FIFO_BAD_STATE);
    /* high != low+4. */
    plan.writes[0].low_offset = NNPI_FIFO_COMMAND_NO_NOTIFY_OFFSET;
    plan.writes[0].high_offset = plan.writes[0].low_offset + 8;
    assert(nnpi_fifo_plan_halves(&plan, halves, 4, &count) == NNPI_FIFO_BAD_STATE);
}

int main(void)
{
    test_single_qw();
    test_two_qw();
    test_three_qw();
    test_negative();
    puts("fifo halves: all positive and negative tests passed");
    return 0;
}
