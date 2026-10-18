#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_runtime_protocol.h"
#include "../include/nnpi_response_fifo.h"

static uint64_t version_word(uint32_t opcode, uint16_t protocol,
                             uint16_t firmware, uint16_t channel)
{
    return opcode | ((uint64_t)protocol << 6) | ((uint64_t)firmware << 22) |
           ((uint64_t)channel << 38);
}

static void test_versions(void)
{
    struct nnpi_runtime_parser parser;
    struct nnpi_runtime_message message, before;
    uint32_t opcode, i;
    /* Independent bit-pattern vector: 4.1.0, firmware=0, channel=1.6.0. */
    const uint64_t wire = UINT64_C(0x0001300000040802);
    assert(wire == version_word(2, 0x1020, 0, 0x04c0));
    for (opcode = 0; opcode <= 2; ++opcode) {
        nnpi_runtime_init(&parser);
        memset(&message, 0xa5, sizeof(message));
        before = message;
        assert(nnpi_runtime_push(&parser,
            version_word(opcode, 0x1020, 0xabcd, 0x04c0) |
                (UINT64_C(0x3ff) << 54), &message) ==
            (opcode == 0 ? NNPI_RUNTIME_MESSAGE : NNPI_RUNTIME_NEED_MORE));
        if (opcode != 0) {
            assert(memcmp(&message, &before, sizeof(message)) == 0);
            assert(parser.count == 1);
            assert(nnpi_runtime_push(&parser, UINT64_C(0xe4e4e4e4e4e4e4e4), &message) ==
                (opcode == 1 ? NNPI_RUNTIME_MESSAGE : NNPI_RUNTIME_NEED_MORE));
        }
        if (opcode == 2) {
            assert(memcmp(&message, &before, sizeof(message)) == 0);
            assert(parser.count == 2);
            assert(nnpi_runtime_push(&parser, UINT64_C(0x1b1b1b1b1b1b1b1b), &message) ==
                   NNPI_RUNTIME_MESSAGE);
        }
        assert(message.opcode == opcode && message.qword_count == opcode + 1);
        assert(message.protocol_version == 0x1020);
        assert(message.firmware_version == 0xabcd);
        assert(message.channel_version == 0x04c0);
        for (i = 0; i < 32; ++i) {
            assert(message.response_sizes[i] == (opcode >= 1 ? i % 4 : 0));
            assert(message.command_sizes[i] == (opcode == 2 ? 3 - i % 4 : 0));
        }
        assert(nnpi_runtime_compatible_1150(&message) == (opcode == 2));
        assert(parser.count == 0 && parser.expected == 0 && parser.stopped == 0);
    }
    nnpi_runtime_init(&parser);
    /* Exhaust all 16-bit fields: masks must neither drop bit15 nor bleed. */
    assert(nnpi_runtime_push(&parser, version_word(2, 0xffff, 0xffff, 0xffff), &message) == NNPI_RUNTIME_NEED_MORE);
    assert(nnpi_runtime_push(&parser, UINT64_MAX, &message) == NNPI_RUNTIME_NEED_MORE);
    assert(nnpi_runtime_push(&parser, UINT64_MAX, &message) == NNPI_RUNTIME_MESSAGE);
    assert(message.protocol_version == 0xffff && message.firmware_version == 0xffff && message.channel_version == 0xffff);
    assert(!nnpi_runtime_compatible_1150(&message));
    message.protocol_version = 0x1021; /* patch differs: Linux accepts */
    assert(nnpi_runtime_compatible_1150(&message));
    message.protocol_version = 0x1000; /* stale 4.0.0 header: reject */
    assert(!nnpi_runtime_compatible_1150(&message));
    message.protocol_version = 0x1020;
    message.response_sizes_raw = 0;
    assert(!nnpi_runtime_compatible_1150(&message));
    assert(!nnpi_runtime_compatible_1150(NULL));
    /* A zero QW is a legacy opcode-0 reply with zero versions, never a
     * compatible reply3. Fixing the command address must not relax this gate. */
    nnpi_runtime_init(&parser);
    assert(nnpi_runtime_push(&parser, 0, &message) == NNPI_RUNTIME_MESSAGE);
    assert(message.opcode == 0 && message.qword_count == 1);
    assert(message.protocol_version == 0 && message.response_sizes_raw == 0);
    assert(!nnpi_runtime_compatible_1150(&message));
}

static void test_stream_and_rejection(void)
{
    struct nnpi_runtime_parser parser, saved;
    struct nnpi_runtime_message message, before;
    uint32_t op;
    for (op = 0; op < 64; ++op) {
        nnpi_runtime_init(&parser);
        memset(&message, 0x5a, sizeof(message));
        before = message;
        if (nnpi_runtime_message_qwords(op) != 0)
            continue;
        assert(nnpi_runtime_push(&parser, op, &message) == NNPI_RUNTIME_UNKNOWN);
        assert(parser.count == 1 && parser.partial[0] == op && parser.stopped);
        saved = parser;
        assert(nnpi_runtime_push(&parser, 2, &message) == NNPI_RUNTIME_STOPPED);
        assert(memcmp(&saved, &parser, sizeof(parser)) == 0);
        assert(memcmp(&before, &message, sizeof(message)) == 0);
    }
    nnpi_runtime_init(&parser);
    assert(nnpi_runtime_push(&parser, 4, &message) == NNPI_RUNTIME_MESSAGE);
    assert(message.opcode == 4 && !nnpi_runtime_compatible_1150(&message));
    assert(nnpi_runtime_push(&parser, 11, &message) == NNPI_RUNTIME_MESSAGE);
    assert(message.opcode == 11 && !nnpi_runtime_compatible_1150(&message));
    assert(nnpi_runtime_message_qwords(39) == 1);
    assert(nnpi_runtime_push(&parser, UINT64_C(0x12340027), &message) == NNPI_RUNTIME_MESSAGE);
    assert(message.opcode == 39 && message.qwords[0] == UINT64_C(0x12340027));
    assert(nnpi_runtime_push(&parser, version_word(2,0x1020,0,0), &message) == NNPI_RUNTIME_NEED_MORE);
    assert(nnpi_runtime_push(&parser, 39, &message) == NNPI_RUNTIME_NEED_MORE);
    assert(nnpi_runtime_push(&parser, 4, &message) == NNPI_RUNTIME_MESSAGE);
    assert(message.opcode == 2 && message.response_sizes_raw == 39 && message.command_sizes_raw == 4);
    assert(nnpi_runtime_compatible_1150(&message));
    assert(nnpi_runtime_push(&parser, 2, &message) == NNPI_RUNTIME_NEED_MORE);
    /* Payload low bits must NOT be mistaken for unknown opcode or new reply. */
    assert(nnpi_runtime_push(&parser, 63, &message) == NNPI_RUNTIME_NEED_MORE);
    assert(nnpi_runtime_push(&parser, 31, &message) == NNPI_RUNTIME_MESSAGE);
    assert(message.response_sizes_raw == 63 && message.command_sizes_raw == 31);
    saved = parser;
    assert(nnpi_runtime_push(NULL, 0, &message) == NNPI_RUNTIME_INVALID);
    assert(nnpi_runtime_push(&parser, 0, NULL) == NNPI_RUNTIME_INVALID);
    assert(memcmp(&saved, &parser, sizeof(parser)) == 0);
    parser.count = UINT32_MAX;
    saved = parser;
    assert(nnpi_runtime_push(&parser, 0, &message) == NNPI_RUNTIME_INVALID);
    assert(memcmp(&saved, &parser, sizeof(parser)) == 0);
    nnpi_runtime_init(&parser);
    parser.expected = 3;
    assert(nnpi_runtime_push(&parser, 0, &message) == NNPI_RUNTIME_INVALID);
    parser.count = 1;
    parser.partial[0] = 0;
    assert(nnpi_runtime_push(&parser, 0, &message) == NNPI_RUNTIME_INVALID);
    nnpi_runtime_init(NULL);
}

static void test_fixed_encoders(void)
{
    uint32_t out = 0xdeadbeef, next = 99, previous, i;
    uint8_t query[8] = {0};
    struct nnpi_fifo_command_plan plan;
    assert(nnpi_runtime_query_version() == 0);
    assert(nnpi_fifo_plan_command(0, query, sizeof(query), &plan) == NNPI_FIFO_OK);
    assert(plan.count == 1 && plan.writes[0].value == nnpi_runtime_query_version());
    assert(plan.writes[0].low_offset == 0x58 && plan.writes[0].notify == 1);
    assert(nnpi_runtime_host_ready_value(0, &out) == NNPI_RUNTIME_MESSAGE && out == 0x100);
    assert(nnpi_runtime_host_ready_value(UINT32_MAX, &out) == NNPI_RUNTIME_MESSAGE);
    assert(out == UINT32_C(0xfffff1ff));
    out = 0xdeadbeef;
    assert(nnpi_runtime_host_ready_value(UINT64_C(0x100000000), &out) == NNPI_RUNTIME_INVALID);
    assert(out == 0xdeadbeef);
    assert(nnpi_runtime_host_ready_value(0, NULL) == NNPI_RUNTIME_INVALID);
    previous = 0;
    for (i = 0; i < 100; ++i) {
        assert(nnpi_runtime_keepalive_value(UINT32_MAX, previous, &next, &out) == NNPI_RUNTIME_MESSAGE);
        assert(next == i % 14 + 1);
        assert((out & NNPI_RUNTIME_KEEPALIVE_MASK) == next << 20);
        assert((out & ~NNPI_RUNTIME_KEEPALIVE_MASK) == (UINT32_MAX & ~NNPI_RUNTIME_KEEPALIVE_MASK));
        previous = next;
    }
    out = 0xdeadbeef;
    next = 99;
    assert(nnpi_runtime_keepalive_value(0, 15, &next, &out) == NNPI_RUNTIME_INVALID);
    assert(nnpi_runtime_keepalive_value(0, UINT32_MAX, &next, &out) == NNPI_RUNTIME_INVALID);
    assert(nnpi_runtime_keepalive_value(UINT64_C(0x100000000), 0, &next, &out) == NNPI_RUNTIME_INVALID);
    assert(nnpi_runtime_keepalive_value(0, 0, NULL, &out) == NNPI_RUNTIME_INVALID);
    assert(nnpi_runtime_keepalive_value(0, 0, &next, NULL) == NNPI_RUNTIME_INVALID);
    assert(nnpi_runtime_keepalive_value(0, 0, &out, &out) == NNPI_RUNTIME_INVALID);
    assert(out == 0xdeadbeef && next == 99);
}

static void test_across_fifo_rounds(void)
{
    struct nnpi_response_fifo_plan plan;
    struct nnpi_runtime_parser parser;
    struct nnpi_runtime_message message;
    uint32_t i;
    nnpi_runtime_init(&parser);
    /* A full retained FIFO snapshot ends in first QW of a partial reply3.
     * Planner ACK count must stay independent of parser results. */
    assert(nnpi_response_fifo_plan_drain(0x1000, &plan) == NNPI_FIFO_OK);
    assert(plan.read_count == 16 && plan.ack_count == 2);
    assert(plan.acks[0].value == 0x1001 && plan.acks[1].value == 0x1000);
    for (i = 0; i < 15; ++i)
        assert(nnpi_runtime_push(&parser, 4, &message) == NNPI_RUNTIME_MESSAGE);
    assert(nnpi_runtime_push(&parser, version_word(2, 0x1020, 0, 0x04c0), &message) == NNPI_RUNTIME_NEED_MORE);
    assert(parser.count == 1);
    /* Empty polling round: no push/no reset. Next snapshot contains tail. */
    assert(nnpi_response_fifo_plan_drain(0, &plan) == NNPI_FIFO_OK && plan.ack_count == 0);
    assert(parser.count == 1);
    assert(nnpi_response_fifo_plan_drain(0x200, &plan) == NNPI_FIFO_OK);
    assert(plan.read_count == 2 && plan.ack_count == 1);
    assert(nnpi_runtime_push(&parser, 1, &message) == NNPI_RUNTIME_NEED_MORE);
    assert(nnpi_runtime_push(&parser, 3, &message) == NNPI_RUNTIME_MESSAGE);
    assert(nnpi_runtime_compatible_1150(&message));
}

int main(void)
{
    test_versions();
    test_stream_and_rejection();
    test_fixed_encoders();
    test_across_fifo_rounds();
    puts("runtime protocol: all positive and negative tests passed");
    return 0;
}
