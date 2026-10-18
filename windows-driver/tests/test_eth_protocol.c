#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_eth_protocol.h"

/* Little-endian QW serialization: the transport layer owns byte order; the
 * self-test re-derives nnpi-tap.cpp's byte-level assertions through it. */
static void le64(uint64_t v, uint8_t out[8])
{
    unsigned i;
    for (i = 0; i < 8; ++i)
        out[i] = (uint8_t)(v >> (8u * i));
}

static void test_slots(void)
{
    struct nnpi_eth_slots pool;
    unsigned i;
    nnpi_eth_slots_init(&pool);
    for (i = 0; i < NNPI_ETH_SLOTS; ++i)
        assert(nnpi_eth_slots_reserve(&pool) == (int)i);
    assert(nnpi_eth_slots_reserve(&pool) == -1);
    assert(!nnpi_eth_slots_available(&pool));
    /* Out-of-range and double release are both rejected. */
    assert(nnpi_eth_slots_release(&pool, NNPI_ETH_SLOTS) == NNPI_ETH_INVALID);
    assert(nnpi_eth_slots_release(&pool, 7) == NNPI_ETH_OK);
    assert(nnpi_eth_slots_release(&pool, 7) == NNPI_ETH_INVALID);
    assert(nnpi_eth_slots_available(&pool));
    assert(nnpi_eth_slots_reserve(&pool) == 7);
    assert(nnpi_eth_slots_release(NULL, 0) == NNPI_ETH_INVALID);
    assert(nnpi_eth_slots_reserve(NULL) == -1);
}

static void test_config(void)
{
    const uint8_t mac[6] = {2, 0x4e, 0x4e, 0x50, 1, 2};
    const uint32_t card_ip = UINT32_C(0x020200c0); /* 192.0.2.2 s_addr */
    uint8_t cfg[16];
    uint64_t qw0, qw1;
    assert(nnpi_eth_config_encode(256, card_ip, mac, &qw0, &qw1) == NNPI_ETH_OK);
    le64(qw0, cfg);
    le64(qw1, cfg + 8);
    /* card_ip lands at wire bytes 4-7, low IP byte first (card convertIp). */
    assert(cfg[4] == 192 && cfg[5] == 0 && cfg[6] == 2 && cfg[7] == 2);
    assert(memcmp(cfg + 10, mac, 6) == 0);
    /* opcode + chan occupy the low 16 bits of QW0. */
    assert((qw0 & 0x3f) == NNPI_ETH_H2C_CONFIG);
    assert(((qw0 >> 6) & NNPI_ETH_CHANNEL_MASK) == 256);
    assert(nnpi_eth_config_encode(1024, card_ip, mac, &qw0, &qw1) == NNPI_ETH_INVALID);
    assert(nnpi_eth_config_encode(256, card_ip, NULL, &qw0, &qw1) == NNPI_ETH_INVALID);
    assert(nnpi_eth_config_encode(256, card_ip, mac, NULL, &qw1) == NNPI_ETH_INVALID);
    /* c2h config reply: card_ip echo, 0 means the card rejected setup. */
    {
        uint8_t opcode; uint16_t chan; uint32_t ip;
        assert(nnpi_eth_config_decode((uint64_t)NNPI_ETH_C2H_CONFIG |
                                      (256ULL << 6) | ((uint64_t)card_ip << 32),
                                      &opcode, &chan, &ip) == NNPI_ETH_OK);
        assert(opcode == NNPI_ETH_C2H_CONFIG && chan == 256 && ip == card_ip);
        assert(nnpi_eth_config_decode(0, &opcode, &chan, &ip) == NNPI_ETH_OK);
        assert(opcode == 0 && chan == 0 && ip == 0);
        assert(nnpi_eth_config_decode(0, NULL, &chan, &ip) == NNPI_ETH_INVALID);
    }
}

static void test_descriptor_encode(void)
{
    uint64_t tx, ack;
    assert(nnpi_eth_descriptor_encode(256, 15, 1514, 0, &tx) == NNPI_ETH_OK);
    assert((tx & 0x3f) == NNPI_ETH_H2C_MSG_DSCR);
    assert(((tx >> 6) & 1023) == 256);
    assert(((tx >> 16) & 4095) == 1513); /* size = length - 1 */
    assert(((tx >> 28) & 255) == 15);
    assert(((tx >> 36) & 1) == 0);
    assert(nnpi_eth_descriptor_encode(256, 15, 0, 1, &ack) == NNPI_ETH_OK);
    assert(((ack >> 36) & 1) == 1);
    assert(((ack >> 16) & 4095) == 0); /* ACK carries no length */
    /* Bound and argument rejection. */
    assert(nnpi_eth_descriptor_encode(256, NNPI_ETH_SLOTS, 14, 0, &tx) == NNPI_ETH_INVALID);
    assert(nnpi_eth_descriptor_encode(256, 0, 13, 0, &tx) == NNPI_ETH_INVALID);
    assert(nnpi_eth_descriptor_encode(256, 0, NNPI_ETH_PAGE_BYTES + 1, 0, &tx) == NNPI_ETH_INVALID);
    assert(nnpi_eth_descriptor_encode(1024, 0, 14, 0, &tx) == NNPI_ETH_INVALID);
    assert(nnpi_eth_descriptor_encode(256, 0, 14, 0, NULL) == NNPI_ETH_INVALID);
}

static void test_descriptor_decode(void)
{
    uint8_t opcode, slot, is_ack;
    uint16_t chan, length;
    uint64_t incoming = 40ULL | (256ULL << 6) | (1513ULL << 16) | (15ULL << 28);
    assert(nnpi_eth_descriptor_decode(incoming, &opcode, &chan, &length, &slot, &is_ack) == NNPI_ETH_OK);
    assert(opcode == NNPI_ETH_C2H_MSG_DSCR && chan == 256);
    assert(length == 1514 && slot == 15 && is_ack == 0);
    /* ACK reply: card sends size=0, so size+1 collapses to 1. */
    incoming = 40ULL | (256ULL << 6) | (15ULL << 28) | (1ULL << 36);
    assert(nnpi_eth_descriptor_decode(incoming, &opcode, &chan, &length, &slot, &is_ack) == NNPI_ETH_OK);
    assert(opcode == NNPI_ETH_C2H_MSG_DSCR && chan == 256);
    assert(is_ack == 1 && length == 1 && slot == 15);
    /* Out-of-range slot cannot address a page: structural error. */
    assert(nnpi_eth_descriptor_decode(40ULL | (16ULL << 28), &opcode, &chan, &length, &slot, &is_ack) == NNPI_ETH_INVALID);
    assert(nnpi_eth_descriptor_decode(incoming, NULL, &chan, &length, &slot, &is_ack) == NNPI_ETH_INVALID);
}

static void test_page_addressing(void)
{
    /* Random-access pool: a card frame lives at slot * PAGE_BYTES. */
    static uint8_t memory[NNPI_ETH_SLOTS * NNPI_ETH_PAGE_BYTES];
    uint8_t packet[1514];
    uint8_t opcode, slot, is_ack;
    uint16_t chan, length;
    uint64_t incoming = 40ULL | (256ULL << 6) | (1513ULL << 16) | (15ULL << 28);
    memset(packet, 0xa5, sizeof(packet));
    assert(nnpi_eth_descriptor_decode(incoming, &opcode, &chan, &length, &slot, &is_ack) == NNPI_ETH_OK);
    memcpy(memory + (size_t)slot * NNPI_ETH_PAGE_BYTES, packet, length);
    assert(memcmp(memory + 15 * NNPI_ETH_PAGE_BYTES, packet, sizeof(packet)) == 0);
}

static void test_event(void)
{
    /* wire layout: opcode(6)|event_code(7)<<6|chan_id(8)<<13|obj_id(16)<<21|
     * obj_id_2(16)<<37|event_val(8)<<53|ctx_valid(1)<<61|obj_valid(1)<<62|
     * obj_valid_2(1)<<63. */
    const unsigned codes[] = {111u, 112u, 119u, 120u, 127u};
    unsigned i;
    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        uint64_t wire = 4ULL | ((uint64_t)codes[i] << 6) | (0x5aULL << 13) |
                        (0x37ULL << 53) | (1ULL << 61);
        assert(nnpi_eth_event_code(wire) == codes[i]);
        assert(nnpi_eth_event_fatal((uint8_t)codes[i]) == (codes[i] >= 112));
        /* Independent field confirmation of the same word. chan_id is 8 bits. */
        assert(((wire >> 13) & 0xff) == 0x5a);
        assert(((wire >> 21) & 0xffff) == 0);
        assert(((wire >> 37) & 0xffff) == 0);
        assert(((wire >> 53) & 0xff) == 0x37);
        assert(((wire >> 61) & 1) == 1);
    }
    assert(!nnpi_eth_event_fatal(111));
    assert(nnpi_eth_event_fatal(NNPI_ETH_EVENT_FATAL_START));
}

int main(void)
{
    test_slots();
    test_config();
    test_descriptor_encode();
    test_descriptor_decode();
    test_page_addressing();
    test_event();
    puts("eth protocol: all positive and negative tests passed");
    return 0;
}
