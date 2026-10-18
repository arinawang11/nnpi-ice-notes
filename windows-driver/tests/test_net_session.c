#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_net_session.h"

static const struct nnpi_net_session_key KEY = { 1, 2, 3 };
static const uint32_t CARD_IP = UINT32_C(0x020200c0); /* 192.0.2.2 s_addr */
static const uint8_t MAC[6] = { 2, 0x4e, 0x4e, 0x50, 1, 2 };

/* A fresh, begun session at read serial 1, channel id 256. */
static void fresh(struct nnpi_net_session *s)
{
    nnpi_net_session_init(s);
    assert(nnpi_net_session_begin(s, &KEY, 1, 256));
    assert(s->phase == NNPI_NS_INITIAL && s->chan_id == 256);
}

static void test_begin(void)
{
    struct nnpi_net_session s;
    nnpi_net_session_init(&s);
    /* Invalid chan_id range, serial, key, duplicate begin. */
    assert(!nnpi_net_session_begin(&s, &KEY, 1, 255));
    assert(!nnpi_net_session_begin(&s, &KEY, 1, 1024));
    assert(!nnpi_net_session_begin(&s, &KEY, 0, 256));
    assert(!nnpi_net_session_begin(&s, &KEY, UINT64_MAX, 256));
    assert(!nnpi_net_session_begin(&s, NULL, 1, 256));
    assert(nnpi_net_session_begin(&s, &KEY, 1, 256));
    assert(!nnpi_net_session_begin(&s, &KEY, 2, 256)); /* already begun */
}

static void test_chan_op(void)
{
    struct nnpi_net_session s;
    struct nnpi_fifo_command_plan plan;
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    assert(s.phase == NNPI_NS_CHAN_TX && s.pending_qwords == 1);
    assert(plan.count == 1 && plan.writes[0].notify == 1);
    /* wire value: opcode 22 | chan_id<<6 | privileged<<31 | uid<<32. */
    assert((plan.writes[0].value & 0x3f) == NNPI_CHAN_OP_H2C);
    assert(((plan.writes[0].value >> 6) & NNPI_CHAN_ID_MASK) == 256);
    assert(((plan.writes[0].value >> 16) & 1) == 0); /* create, not destroy */
    /* uid + privileged land in the credential fields. */
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, UINT32_C(0xdeadbeef), 1, &plan));
    assert((uint32_t)(plan.writes[0].value >> 32) == UINT32_C(0xdeadbeef));
    assert(((plan.writes[0].value >> 31) & 1) == 1);
    /* Wrong phase or stale serial rejects without corrupting the plan. */
    fresh(&s);
    assert(!nnpi_net_session_chan_op(&s, &KEY, 1, 0, 0, 0, &plan)); /* stale serial */
    assert(s.phase == NNPI_NS_INITIAL && !s.invalidated);
}

static void test_rb_op(void)
{
    struct nnpi_net_session s;
    struct nnpi_fifo_command_plan plan;
    fresh(&s);
    /* h2c rb requires CHAN_READY, not INITIAL. */
    assert(!nnpi_net_session_rb_op(&s, &KEY, 2, 0, 1, 0x12345, &plan));
    /* Drive to CHAN_READY. */
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 3,
        4ULL | (21ULL << 6) | (256ULL << 21) | (1ULL << 62)));
    assert(s.phase == NNPI_NS_CHAN_READY);
    /* h2c rb: CHAN_READY -> H2C_RB_TX, publishes H2C pool. */
    assert(nnpi_net_session_rb_op(&s, &KEY, 4, 0, 1, 0x12345, &plan));
    assert(s.phase == NNPI_NS_H2C_RB_TX && (s.published_dma & NNPI_NS_H2C_POOL));
    assert((plan.writes[0].value & 0x3f) == NNPI_CHAN_RB_OP_H2C);
    assert(((plan.writes[0].value >> 16) & 1) == 1); /* h2c */
    assert(((plan.writes[0].value >> 19) & NNPI_CHAN_PFN_MASK) == 0x12345);
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 5,
        4ULL | (23ULL << 6) | (256ULL << 21) | (1ULL << 62) | (1ULL << 63)));
    assert(s.phase == NNPI_NS_H2C_RB_READY);
    /* c2h rb: H2C_RB_READY -> C2H_RB_TX, publishes C2H pool. */
    assert(nnpi_net_session_rb_op(&s, &KEY, 6, 0, 0, 0x54321, &plan));
    assert(s.phase == NNPI_NS_C2H_RB_TX && (s.published_dma & NNPI_NS_C2H_POOL));
    assert(((plan.writes[0].value >> 16) & 1) == 0); /* c2h */
    assert(((plan.writes[0].value >> 19) & NNPI_CHAN_PFN_MASK) == 0x54321);
}

static void test_eth_config(void)
{
    struct nnpi_net_session s;
    struct nnpi_fifo_command_plan plan;
    uint64_t reply;
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 3,
        4ULL | (21ULL << 6) | (256ULL << 21) | (1ULL << 62)));
    assert(nnpi_net_session_rb_op(&s, &KEY, 4, 0, 1, 0x12345, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 5,
        4ULL | (23ULL << 6) | (256ULL << 21) | (1ULL << 62) | (1ULL << 63)));
    assert(nnpi_net_session_rb_op(&s, &KEY, 6, 0, 0, 0x54321, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 7,
        4ULL | (23ULL << 6) | (256ULL << 21) | (1ULL << 62) | (1ULL << 63)));
    assert(s.phase == NNPI_NS_C2H_RB_READY);
    /* eth config: C2H_RB_READY -> ETH_TX, 2 QW, card_ip+MAC stored. */
    assert(nnpi_net_session_eth_config(&s, &KEY, 8, 0, CARD_IP, MAC, &plan));
    assert(s.phase == NNPI_NS_ETH_TX && s.pending_qwords == 2);
    assert(plan.count == 2 && plan.writes[1].notify == 1);
    assert((plan.writes[0].value & 0x3f) == NNPI_ETH_H2C_CONFIG);
    assert(s.card_ip == CARD_IP);
    /* 4 halves for a 2-QW message. */
    assert(nnpi_net_session_sent(&s, &KEY, 4));
    assert(s.phase == NNPI_NS_WAIT_ETH);
    /* C2H39 echo advances to READY. */
    reply = (uint64_t)NNPI_ETH_C2H_CONFIG | (256ULL << 6) |
            ((uint64_t)CARD_IP << 32);
    assert(nnpi_net_session_observe(&s, &KEY, 9, reply));
    assert(nnpi_net_session_ready(&s));
}

static void test_negative_events(void)
{
    struct nnpi_net_session s;
    struct nnpi_fifo_command_plan plan;
    /* CREATE_CHANNEL_FAILED aborts while waiting for the channel. */
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(!nnpi_net_session_observe_event(&s, &KEY, 3, 4ULL | (70ULL << 6)));
    assert(nnpi_net_session_failed(&s));
    /* Card-fatal event aborts immediately. */
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(!nnpi_net_session_observe_event(&s, &KEY, 3, 4ULL | (112ULL << 6)));
    assert(nnpi_net_session_failed(&s));
    /* obj_id mismatch on CREATE_SUCCESS is rejected, not accepted. */
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(!nnpi_net_session_observe_event(&s, &KEY, 3,
        4ULL | (21ULL << 6) | (300ULL << 21) | (1ULL << 62)));
    assert(s.phase == NNPI_NS_WAIT_CHAN && !s.invalidated);
}

/* SET_RB replies deliberately have no direction/sequence/destroy echo. */
static uint64_t rb_event(unsigned code, unsigned channel, unsigned rb,
                         unsigned value, unsigned valid, unsigned valid2,
                         unsigned ctx)
{
    return 4ULL | ((uint64_t)code << 6) | ((uint64_t)channel << 21) |
        ((uint64_t)rb << 37) | ((uint64_t)value << 53) |
        ((uint64_t)ctx << 61) | ((uint64_t)valid << 62) |
        ((uint64_t)valid2 << 63);
}

/* Real lifecycle to either pending RB WAIT; do not fabricate pending state. */
static void wait_rb(struct nnpi_net_session *s, int h2c)
{
    struct nnpi_fifo_command_plan plan;
    fresh(s);
    assert(nnpi_net_session_chan_op(s, &KEY, 2, 0, 0, 0, &plan));
    assert(nnpi_net_session_sent(s, &KEY, 2));
    assert(nnpi_net_session_observe(s, &KEY, 3,
        4ULL | (21ULL << 6) | (256ULL << 21) | (1ULL << 62)));
    assert(nnpi_net_session_rb_op(s, &KEY, 4, 0, 1, 0x12345, &plan));
    assert(nnpi_net_session_sent(s, &KEY, 2));
    if (!h2c) {
        assert(nnpi_net_session_observe(s, &KEY, 5,
            rb_event(23, 256, 0, 0, 1, 1, 0)));
        assert(nnpi_net_session_rb_op(s, &KEY, 6, 0, 0, 0x54321, &plan));
        assert(nnpi_net_session_sent(s, &KEY, 2));
    }
}

static void test_rb_event_correlation(void)
{
    struct nnpi_net_session s;
    unsigned h2c, failed, value, bits;
    for (h2c = 0; h2c != 2; ++h2c) {
        uint32_t waiting = h2c ? NNPI_NS_WAIT_H2C_RB : NNPI_NS_WAIT_C2H_RB;
        /* Foreign success/failure never completes or fails our pending RB. */
        for (failed = 0; failed != 2; ++failed) {
            unsigned code = failed ? 72 : 23;
            wait_rb(&s, (int)h2c);
            assert(!nnpi_net_session_observe(&s, &KEY, 10,
                rb_event(code, 257, 0, failed, 1, 1, 0)));
            assert(s.phase == waiting && !s.invalidated);
            assert(!nnpi_net_session_observe(&s, &KEY, 11,
                rb_event(code, 256, 1, failed, 1, 1, 0)));
            assert(s.phase == waiting && !s.invalidated);
            assert(!nnpi_net_session_observe(&s, &KEY, 12,
                rb_event(code, 256, 65535, failed, 1, 1, 0)));
            assert(s.phase == waiting && !s.invalidated);
            /* Explicitly foreign identity wins over malformed other fields. */
            assert(!nnpi_net_session_observe(&s, &KEY, 13,
                rb_event(code, 257, 0, 255, 1, 0, 1)));
            assert(s.phase == waiting && !s.invalidated);
            assert(!nnpi_net_session_observe(&s, &KEY, 14,
                rb_event(code, 256, 1, 255, 0, 1, 1)));
            assert(s.phase == waiting && !s.invalidated);
            assert(!s.c2h_rb_published && s.h2c_rb_published == (h2c ? 0u : 1u));
            assert(nnpi_net_session_observe(&s, &KEY, 15,
                rb_event(23, 256, 0, 0, 1, 1, 0)));
            assert(!s.invalidated);
        }
        /* All missing-valid combinations reject by terminal abort, including
         * old golden replies missing bit 63, and failure replies too. */
        for (failed = 0; failed != 2; ++failed) {
            for (bits = 0; bits != 3; ++bits) {
                wait_rb(&s, (int)h2c);
                assert(!nnpi_net_session_observe(&s, &KEY, 10,
                    rb_event(failed ? 72 : 23, 256, 0, failed,
                             bits & 1u, (bits >> 1) & 1u, 0)));
                assert(nnpi_net_session_failed(&s));
                assert(!s.c2h_rb_published && s.h2c_rb_published == (h2c ? 0u : 1u));
                assert(!nnpi_net_session_observe(&s, &KEY, 11,
                    rb_event(23, 256, 0, 0, 1, 1, 0)));
            }
            wait_rb(&s, (int)h2c);
            assert(!nnpi_net_session_observe(&s, &KEY, 10,
                rb_event(failed ? 72 : 23, 256, 0, failed, 1, 1, 1)));
            assert(nnpi_net_session_failed(&s));
        }
        /* Every nonzero error is accepted as failure, not just one enumerant;
         * success+nonzero is malformed and follows the same abort policy. */
        for (failed = 0; failed != 2; ++failed) {
            for (value = 1; value <= 255; ++value) {
                wait_rb(&s, (int)h2c);
                assert(!nnpi_net_session_observe(&s, &KEY, 10,
                    rb_event(failed ? 72 : 23, 256, 0, value, 1, 1, 0)));
                assert(nnpi_net_session_failed(&s));
                assert(!s.c2h_rb_published && s.h2c_rb_published == (h2c ? 0u : 1u));
            }
        }
        wait_rb(&s, (int)h2c);
        assert(!nnpi_net_session_observe(&s, &KEY, 10,
            rb_event(72, 256, 0, 0, 1, 1, 0))); /* malformed failure */
        assert(nnpi_net_session_failed(&s));
        wait_rb(&s, (int)h2c);
        {
            struct nnpi_net_session_key stale = KEY;
            ++stale.epoch;
            assert(!nnpi_net_session_observe(&s, &stale, 10,
                rb_event(23, 256, 0, 0, 1, 1, 0)));
            assert(!nnpi_net_session_observe(&s, &KEY, s.last_read,
                rb_event(23, 256, 0, 0, 1, 1, 0)));
            assert(s.phase == waiting && !s.invalidated);
        }
        assert(!nnpi_net_session_observe_event(&s, &KEY, 10,
            rb_event(23, 256, 0, 0, 1, 1, 0) ^ 1ULL)); /* not opcode 4 */
        assert(s.phase == waiting && !s.invalidated);
        /* Context field is not the non-context channel id; when invalid its
         * raw bits must not be used to reject a valid obj_id=256 ACK. */
        assert(nnpi_net_session_observe(&s, &KEY, 11,
            rb_event(23, 256, 0, 0, 1, 1, 0) | (255ULL << 13)));
        wait_rb(&s, (int)h2c);
        assert(!nnpi_net_session_observe(&s, &KEY, 10,
            rb_event(112, 257, 1, 0, 1, 1, 0)));
        assert(nnpi_net_session_failed(&s)); /* fatal ignores tuple */
    }
}

static void test_rb_no_pending_and_late_ack(void)
{
    struct nnpi_net_session s;
    struct nnpi_fifo_command_plan plan;
    uint64_t ack = rb_event(23, 256, 0, 0, 1, 1, 0);
    uint64_t nack = rb_event(72, 256, 0, 1, 1, 1, 0);
    uint32_t phase;
    /* Every non-RB-WAIT phase must refuse both local success and failure.
     * This is a phase-dispatch test; realistic transition coverage follows. */
    for (phase = NNPI_NS_INITIAL; phase <= NNPI_NS_FINISHED; ++phase) {
        if (phase == NNPI_NS_WAIT_H2C_RB || phase == NNPI_NS_WAIT_C2H_RB)
            continue;
        fresh(&s);
        s.phase = phase;
        s.pending_qwords = 0;
        assert(!nnpi_net_session_observe(&s, &KEY, 10, ack));
        assert(!nnpi_net_session_observe(&s, &KEY, 11, nack));
        assert(s.phase == phase && !s.invalidated);
        assert(!s.h2c_rb_published && !s.c2h_rb_published);
    }
    wait_rb(&s, 1);
    assert(nnpi_net_session_observe(&s, &KEY, 10, ack));
    assert(!nnpi_net_session_observe(&s, &KEY, 11, ack));
    assert(s.phase == NNPI_NS_H2C_RB_READY && !s.c2h_rb_published);
    assert(nnpi_net_session_rb_op(&s, &KEY, 12, 0, 0, 0x54321, &plan));
    assert(!nnpi_net_session_observe(&s, &KEY, 13, ack));
    assert(s.phase == NNPI_NS_C2H_RB_TX && !s.c2h_rb_published);
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    /* Once C2H WAIT exists, a late H2C ACK with this identical tuple is
     * indistinguishable. This acceptance documents the wire limitation, not
     * direction/sequence replay protection; serialize same-channel commands. */
    assert(nnpi_net_session_observe(&s, &KEY, 14, ack));
    assert(s.phase == NNPI_NS_C2H_RB_READY && s.c2h_rb_published);
    assert(!nnpi_net_session_observe(&s, &KEY, 15, ack));
    assert(s.phase == NNPI_NS_C2H_RB_READY);
}

static void test_eth_rejection(void)
{
    struct nnpi_net_session s;
    struct nnpi_fifo_command_plan plan;
    uint64_t reply;
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 3,
        4ULL | (21ULL << 6) | (256ULL << 21) | (1ULL << 62)));
    assert(nnpi_net_session_rb_op(&s, &KEY, 4, 0, 1, 0x12345, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 5,
        4ULL | (23ULL << 6) | (256ULL << 21) | (1ULL << 62) | (1ULL << 63)));
    assert(nnpi_net_session_rb_op(&s, &KEY, 6, 0, 0, 0x54321, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 7,
        4ULL | (23ULL << 6) | (256ULL << 21) | (1ULL << 62) | (1ULL << 63)));
    assert(nnpi_net_session_eth_config(&s, &KEY, 8, 0, CARD_IP, MAC, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 4));
    /* card_ip 0 = rejected. */
    assert(!nnpi_net_session_observe_eth(&s, &KEY, 9,
        (uint64_t)NNPI_ETH_C2H_CONFIG | (256ULL << 6)));
    assert(nnpi_net_session_failed(&s));
    /* echo mismatch aborts a second fresh session. */
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 3,
        4ULL | (21ULL << 6) | (256ULL << 21) | (1ULL << 62)));
    assert(nnpi_net_session_rb_op(&s, &KEY, 4, 0, 1, 0x12345, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 5,
        4ULL | (23ULL << 6) | (256ULL << 21) | (1ULL << 62) | (1ULL << 63)));
    assert(nnpi_net_session_rb_op(&s, &KEY, 6, 0, 0, 0x54321, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 2));
    assert(nnpi_net_session_observe_event(&s, &KEY, 7,
        4ULL | (23ULL << 6) | (256ULL << 21) | (1ULL << 62) | (1ULL << 63)));
    assert(nnpi_net_session_eth_config(&s, &KEY, 8, 0, CARD_IP, MAC, &plan));
    assert(nnpi_net_session_sent(&s, &KEY, 4));
    reply = (uint64_t)NNPI_ETH_C2H_CONFIG | (256ULL << 6) |
            (UINT64_C(0x0101a8c0) << 32); /* different IP */
    assert(!nnpi_net_session_observe_eth(&s, &KEY, 9, reply));
    assert(nnpi_net_session_failed(&s));
}

static void test_partial_send(void)
{
    struct nnpi_net_session s;
    struct nnpi_fifo_command_plan plan;
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0, 0, 0, &plan));
    /* 1 half instead of 2 is a partial send -> abort. */
    assert(!nnpi_net_session_sent(&s, &KEY, 1));
    assert(nnpi_net_session_failed(&s));
}

static void test_fifo_space(void)
{
    struct nnpi_net_session s;
    struct nnpi_fifo_command_plan plan;
    fresh(&s);
    /* command FIFO control write=16 read=0 => used 16, free 0: the 1-QW
     * CHANNEL_OP plan cannot fit, so it aborts rather than over-commit. */
    assert(!nnpi_net_session_chan_op(&s, &KEY, 2, 0x00001000, 0, 0, &plan));
    assert(nnpi_net_session_failed(&s));
    /* write=15 read=0 => free 1: exactly enough for the 1-QW CHANNEL_OP. */
    fresh(&s);
    assert(nnpi_net_session_chan_op(&s, &KEY, 2, 0x00000f00, 0, 0, &plan));
    assert(s.phase == NNPI_NS_CHAN_TX);
}

static void test_release(void)
{
    struct nnpi_net_session s;
    nnpi_net_session_init(&s);
    assert(nnpi_net_session_begin(&s, &KEY, 1, 256));
    assert(nnpi_net_session_may_release(&s, NNPI_NS_H2C_POOL)); /* unpublished */
    /* Simulate published H2C/C2H pools; release requires stop. */
    s.published_dma = NNPI_NS_ALL_DMA;
    assert(!nnpi_net_session_may_release(&s, NNPI_NS_H2C_POOL));
    assert(!nnpi_net_session_may_release(&s, NNPI_NS_ALL_DMA));
    /* stopped() latches resources and rejects bad input. */
    assert(!nnpi_net_session_stopped(NULL, NNPI_NS_H2C_POOL));
    assert(!nnpi_net_session_stopped(&s, 0));
    assert(!nnpi_net_session_stopped(&s, NNPI_NS_ALL_DMA | 4));
    assert(nnpi_net_session_stopped(&s, NNPI_NS_H2C_POOL));
    assert(s.stopped_dma == NNPI_NS_H2C_POOL);
    assert(nnpi_net_session_may_release(&s, NNPI_NS_H2C_POOL));
    assert(!nnpi_net_session_may_release(&s, NNPI_NS_ALL_DMA)); /* c2h still up */
    assert(!nnpi_net_session_may_release(&s, NNPI_NS_H2C_POOL | NNPI_NS_C2H_POOL));
    /* Both directions stop independently, then all release. */
    assert(nnpi_net_session_stopped(&s, NNPI_NS_C2H_POOL));
    assert(nnpi_net_session_may_release(&s, NNPI_NS_ALL_DMA));
}

int main(void)
{
    test_begin();
    test_chan_op();
    test_rb_op();
    test_eth_config();
    test_negative_events();
    test_rb_event_correlation();
    test_rb_no_pending_and_late_ack();
    test_eth_rejection();
    test_partial_send();
    test_fifo_space();
    test_release();
    puts("net session: all positive and negative tests passed");
    return 0;
}
