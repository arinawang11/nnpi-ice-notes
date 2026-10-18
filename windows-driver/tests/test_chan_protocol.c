#include <assert.h>
#include <stdio.h>
#include "../include/nnpi_chan_protocol.h"

static void test_channel_op(void)
{
    uint64_t v;
    /* create: opcode 22, chan_id 256, destroy 0, privileged 0, uid 0. */
    assert(nnpi_chan_op_encode(256, 0, 0, 0, &v) == NNPI_CHAN_OK);
    assert((v & 0x3f) == NNPI_CHAN_OP_H2C);
    assert(((v >> 6) & NNPI_CHAN_ID_MASK) == 256);
    assert(((v >> 16) & 1) == 0);
    assert(((v >> 31) & 1) == 0);
    assert((v >> 32) == 0);
    /* destroy: destroy=1 flips bit 16, everything else unchanged. */
    assert(nnpi_chan_op_encode(256, 1, 0, 0, &v) == NNPI_CHAN_OK);
    assert(((v >> 16) & 1) == 1);
    assert((v & 0x3f) == NNPI_CHAN_OP_H2C && ((v >> 6) & NNPI_CHAN_ID_MASK) == 256);
    /* privileged + uid fill bits 31 and 32-63. */
    assert(nnpi_chan_op_encode(0, 0, 1, UINT32_C(0xdeadbeef), &v) == NNPI_CHAN_OK);
    assert(((v >> 31) & 1) == 1);
    assert((uint32_t)(v >> 32) == UINT32_C(0xdeadbeef));
    /* chan_id is 10 bits: 1024 must be rejected. */
    assert(nnpi_chan_op_encode(1024, 0, 0, 0, &v) == NNPI_CHAN_INVALID);
    assert(nnpi_chan_op_encode(256, 0, 0, 0, NULL) == NNPI_CHAN_INVALID);
    /* Non-context network channel starts at id 256. */
    assert(NNPI_CHAN_MIN_NONCTX == 256);
}

static void test_ringbuf_op(void)
{
    uint64_t v;
    /* H2C pool: opcode 23, chan_id 256, h2c=1, rb_id=0, destroy=0, pfn. */
    assert(nnpi_chan_rb_op_encode(256, 1, 0, 0, UINT64_C(0x12345), &v) == NNPI_CHAN_OK);
    assert((v & 0x3f) == NNPI_CHAN_RB_OP_H2C);
    assert(((v >> 6) & NNPI_CHAN_ID_MASK) == 256);
    assert(((v >> 16) & 1) == 1); /* h2c */
    assert(((v >> 17) & 1) == 0); /* rb_id */
    assert(((v >> 18) & 1) == 0); /* destroy */
    assert(((v >> 19) & NNPI_CHAN_PFN_MASK) == UINT64_C(0x12345));
    /* C2H pool: h2c=0. */
    assert(nnpi_chan_rb_op_encode(256, 0, 0, 0, UINT64_C(0xabcde), &v) == NNPI_CHAN_OK);
    assert(((v >> 16) & 1) == 0);
    assert(((v >> 19) & NNPI_CHAN_PFN_MASK) == UINT64_C(0xabcde));
    /* destroy: bit 18 set. */
    assert(nnpi_chan_rb_op_encode(256, 1, 0, 1, UINT64_C(0x12345), &v) == NNPI_CHAN_OK);
    assert(((v >> 18) & 1) == 1);
    /* Full-width PFN (45 bits) is accepted; bit 45 must be rejected. */
    assert(nnpi_chan_rb_op_encode(0, 0, 0, 0, NNPI_CHAN_PFN_MASK, &v) == NNPI_CHAN_OK);
    assert(((v >> 19) & NNPI_CHAN_PFN_MASK) == NNPI_CHAN_PFN_MASK);
    assert(nnpi_chan_rb_op_encode(0, 0, 0, 0, UINT64_C(1) << NNPI_CHAN_PFN_BITS, &v) == NNPI_CHAN_INVALID);
    /* rb_id is 1 bit; chan_id 10 bits. */
    assert(nnpi_chan_rb_op_encode(0, 0, 2, 0, 0, &v) == NNPI_CHAN_INVALID);
    assert(nnpi_chan_rb_op_encode(1024, 0, 0, 0, 0, &v) == NNPI_CHAN_INVALID);
    assert(nnpi_chan_rb_op_encode(0, 0, 0, 0, 0, NULL) == NNPI_CHAN_INVALID);
}

static void test_events(void)
{
    struct nnpi_chan_event ev;
    uint64_t wire;
    assert(nnpi_chan_event_code((uint64_t)NNPI_CHAN_EVENT_CREATE_SUCCESS << 6) ==
           NNPI_CHAN_EVENT_CREATE_SUCCESS);
    assert(nnpi_chan_event_create_success(NNPI_CHAN_EVENT_CREATE_SUCCESS));
    assert(!nnpi_chan_event_create_success(NNPI_CHAN_EVENT_CREATE_FAILED));
    assert(nnpi_chan_event_create_failed(NNPI_CHAN_EVENT_CREATE_FAILED));
    assert(!nnpi_chan_event_create_failed(NNPI_CHAN_EVENT_CREATE_SUCCESS));
    assert(nnpi_chan_event_channel_destroyed(NNPI_CHAN_EVENT_CHANNEL_DESTROYED));
    assert(nnpi_chan_event_set_rb_success(NNPI_CHAN_EVENT_SET_RB_SUCCESS));
    assert(nnpi_chan_event_destroy_failed(NNPI_CHAN_EVENT_DESTROY_FAILED));
    assert(nnpi_chan_event_set_rb_failed(NNPI_CHAN_EVENT_SET_RB_FAILED));
    assert(nnpi_chan_event_card_fatal(NNPI_CHAN_EVENT_CARD_FATAL_START));
    assert(!nnpi_chan_event_card_fatal(NNPI_CHAN_EVENT_CARD_FATAL_START - 1));
    /* Full event report decode: opcode|event_code|chan_id|obj_id|obj_id_2|
     * event_val|ctx_valid|obj_valid|obj_valid_2. chan_id is 8 bits; the 10-bit
     * channel protocol_id lives in the 16-bit obj_id for channel events. */
    wire = 4ULL | (21ULL << 6) | (0x7aULL << 13) | (256ULL << 21) |
           (0ULL << 37) | (0ULL << 53) | (1ULL << 62);
    assert(nnpi_chan_event_decode(wire, &ev) == NNPI_CHAN_OK);
    assert(ev.opcode == 4 && ev.event_code == 21 && ev.chan_id == 0x7a);
    assert(ev.obj_id == 256 && ev.obj_id_2 == 0);
    assert(ev.event_val == 0 && ev.ctx_valid == 0 && ev.obj_valid == 1);
    assert(ev.obj_valid_2 == 0);
    /* CREATE_CHANNEL_SUCCESS: obj_id carries the channel protocol_id. */
    assert(nnpi_chan_event_create_success(ev.event_code) && ev.obj_id == 256);
    /* SET_RB_SUCCESS: obj_id_2 carries rb_id, both obj_valid bits set. */
    wire = 4ULL | (23ULL << 6) | (256ULL << 21) | (1ULL << 37) |
           (1ULL << 62) | (1ULL << 63);
    assert(nnpi_chan_event_decode(wire, &ev) == NNPI_CHAN_OK);
    assert(nnpi_chan_event_set_rb_success(ev.event_code));
    assert(ev.obj_id == 256 && ev.obj_id_2 == 1);
    assert(ev.obj_valid == 1 && ev.obj_valid_2 == 1);
    assert(ev.ctx_valid == 0 && ev.event_val == 0);
    /* Actual network tuple: channel 256/rb 0, both object-valid bits even
     * though rb_id is zero. Decoding preserves missing bits for rejection. */
    wire = UINT64_C(0xc0000000200005c4);
    assert(nnpi_chan_event_decode(wire, &ev) == NNPI_CHAN_OK);
    assert(ev.opcode == 4 && ev.event_code == 23);
    assert(ev.obj_id == 256 && ev.obj_id_2 == 0 && ev.ctx_valid == 0);
    assert(ev.obj_valid == 1 && ev.obj_valid_2 == 1 && ev.event_val == 0);
    assert(nnpi_chan_event_decode(wire & ~(1ULL << 63), &ev) == NNPI_CHAN_OK);
    assert(ev.obj_valid == 1 && ev.obj_valid_2 == 0);
    assert(nnpi_chan_event_decode(wire & ~(1ULL << 62), &ev) == NNPI_CHAN_OK);
    assert(ev.obj_valid == 0 && ev.obj_valid_2 == 1);
    wire = 4ULL | (72ULL << 6) | (256ULL << 21) | (255ULL << 53) |
        (1ULL << 62) | (1ULL << 63);
    assert(nnpi_chan_event_decode(wire, &ev) == NNPI_CHAN_OK);
    assert(ev.opcode == 4 && ev.event_code == 72 && ev.event_val == 255);
    assert(ev.obj_id == 256 && ev.obj_id_2 == 0 && ev.ctx_valid == 0);
    assert(ev.obj_valid == 1 && ev.obj_valid_2 == 1);
    assert(nnpi_chan_event_decode(wire, NULL) == NNPI_CHAN_INVALID);
    /* Protocol version 1.6.0 = 0x04c0. */
    assert(NNPI_CHAN_PROTOCOL_VERSION == UINT16_C(0x04c0));
}

int main(void)
{
    test_channel_op();
    test_ringbuf_op();
    test_events();
    puts("chan protocol: all positive and negative tests passed");
    return 0;
}
