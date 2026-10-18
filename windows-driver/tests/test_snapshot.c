/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared production policy; mock reads only, not a WDF/physical unplug test. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_snapshot_policy.h"
static void le32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
static struct nnpi_diag_response fixture(void)
{
    struct nnpi_diag_response d;
    struct nnpi_bar_resource raw = {NNPI_BAR_MEMORY, 0, 0x80000000U, 0x1000U};
    struct nnpi_bar_resource trans = {NNPI_BAR_MEMORY, 0, 0x90000000U, 0x1000U};
    memset(&d, 0, sizeof(d));
    d.version = NNPI_DIAG_VERSION; d.size = (uint32_t)sizeof(d);
    d.config_bytes = 64; d.raw_count = d.translated_count = 1;
    le32(d.config, 0x45c68086U); le32(d.config + 44, 0x45ce8086U);
    le32(d.config + 16, 0x80000000U);
    d.reason = nnpi_bar_match(d.config, 64, &raw, &trans, 1, d.bars, &d.bar_count);
    assert(d.reason == NNPI_BAR_OK);
    return d;
}
struct mock { uint32_t calls, values[3]; };
static uint32_t read_once(void *context, uint32_t offset)
{
    const uint32_t order[3] = {0x044, 0x060, 0x038};
    struct mock *m = (struct mock *)context;
    assert(m->calls < 3 && offset == order[m->calls]);
    assert(offset >= NNPI_SNAPSHOT_WINDOW_OFFSET &&
           offset + 4 <= NNPI_SNAPSHOT_WINDOW_OFFSET + NNPI_SNAPSHOT_WINDOW_BYTES);
    return m->values[m->calls++];
}
static uint32_t sample(struct mock *m, struct nnpi_snapshot_response *r)
{
    const struct nnpi_snapshot_request q = {NNPI_SNAPSHOT_VERSION, sizeof(q), {0, 0}};
    m->calls = 0;
    memset(r, 0xff, sizeof(*r));
    return nnpi_snapshot_sample(&q, sizeof(q), sizeof(*r), 3, 0, 1, 9, 0, read_once, m, r);
}
static void failure(const struct nnpi_snapshot_request *q, size_t in, size_t out,
                    uint32_t flags, int removed, int mapped, uint64_t generation, uint32_t reason)
{
    struct mock m = {0, {0, 0, 0}};
    struct nnpi_snapshot_response r, zero;
    memset(&r, 0xff, sizeof(r)); memset(&zero, 0, sizeof(zero));
    assert(nnpi_snapshot_sample(q, in, out, flags, removed, mapped, generation,
                               reason, read_once, &m, &r) != NNPI_STATUS_OK);
    assert(m.calls == 0 && memcmp(&r, &zero, sizeof(r)) == 0);
    assert(!nnpi_snapshot_response_valid(&r, sizeof(r)));
}
int main(void)
{
    struct nnpi_snapshot_request q = {NNPI_SNAPSHOT_VERSION, sizeof(q), {0, 0}};
    struct nnpi_snapshot_response r, copy;
    struct nnpi_diag_response d = fixture(), bad;
    struct mock m = {0, {0, 0x1000, 0xffffffffU}};
    uint64_t address, status_address;
    uint32_t i, rp, wp;
    void *mapping, *status_mapping;
    assert(NNPI_SNAPSHOT_IOCTL != NNPI_STATUS_IOCTL && NNPI_SNAPSHOT_IOCTL != NNPI_DIAG_IOCTL);
    assert(nnpi_snapshot_buffers_valid(&q, sizeof(q), sizeof(r)));
#define BAD_BUFFER() failure(&q, sizeof(q), sizeof(r), 3, 0, 1, 9, 0)
    for (i = 0; i < sizeof(q) + 8; ++i)
        if (i != sizeof(q)) failure(&q, i, sizeof(r), 3, 0, 1, 9, 0);
    for (i = 0; i < sizeof(r) + 8; ++i)
        if (i != sizeof(r)) failure(&q, sizeof(q), i, 3, 0, 1, 9, 0);
    failure(NULL, sizeof(q), sizeof(r), 3, 0, 1, 9, 0);
    q.version++; BAD_BUFFER(); q.version--;
    q.size--; BAD_BUFFER(); q.size++;
    for (i = 0; i < 2; ++i) { q.reserved[i] = 1; BAD_BUFFER(); q.reserved[i] = 0; }
#undef BAD_BUFFER
    for (i = 0; i < 8; ++i)
        if (i != 3) failure(&q, sizeof(q), sizeof(r), i, 0, 1, 9, 0);
    failure(&q, sizeof(q), sizeof(r), 3, 1, 1, 9, 0);
    failure(&q, sizeof(q), sizeof(r), 3, 0, 0, 9, 0);
    failure(&q, sizeof(q), sizeof(r), 3, 0, 1, 0, 0);
    for (i = 1; i <= 7; ++i) failure(&q, sizeof(q), sizeof(r), 3, 0, 1, 9, i);
    assert(nnpi_snapshot_sample(&q, sizeof(q), sizeof(r), 3, 0, 1, 9, 0, NULL, &m, &r) != 0 && m.calls == 0);
    assert(nnpi_snapshot_sample(&q, sizeof(q), sizeof(r), 3, 0, 1, 9, 0, read_once, &m, NULL) != 0 && m.calls == 0);
    assert(nnpi_snapshot_window(&d, &address) == 0 && address == 0x90000038U);
    assert(nnpi_status_window(&d, &status_address) == 0 && status_address == 0x900001b8U);
    assert(nnpi_snapshot_window(NULL, &address) != 0 && address == 0);
    assert(nnpi_snapshot_window(&d, NULL) != 0);
#define BAD_WINDOW(change) do { bad = d; change; address = 99; assert(nnpi_snapshot_window(&bad, &address) != 0 && address == 0); } while (0)
    BAD_WINDOW(bad.config[0] ^= 1); BAD_WINDOW(bad.config[44] ^= 1);
    BAD_WINDOW(bad.config[46] ^= 1); BAD_WINDOW(bad.config[8] = 1);
    BAD_WINDOW(bad.config_bytes = 63); BAD_WINDOW(bad.reason = NNPI_BAR_NO_MATCH);
    BAD_WINDOW(bad.bars[0].bar = 2); BAD_WINDOW(bad.bars[0].type = NNPI_BAR_PORT);
    BAD_WINDOW(bad.bars[0].prefetch = 1); BAD_WINDOW(bad.bars[0].length = 0x63);
    BAD_WINDOW(bad.bars[0].translated_start = UINT64_MAX - 3);
    BAD_WINDOW(bad.bars[0].translated_start = (uint64_t)INT64_MAX - 0x60);
    BAD_WINDOW(bad.bars[0].translated_start++); BAD_WINDOW(bad.bars[0].translated_index++);
    BAD_WINDOW(bad.bar_count = 7); BAD_WINDOW(bad.reserved[0] = 1);
#undef BAD_WINDOW
    bad = d; bad.bars[0].length = 0x64;
    /* Existing trusted matcher additionally requires a power-of-two aperture
     * covering 0x1c0; a mere 0x64-byte descriptor is not a trusted NNPI BAR. */
    assert(nnpi_snapshot_window(&bad, &address) != 0);
    bad.bars[0].length = 0x200;
    assert(nnpi_snapshot_window(&bad, &address) == 0);
    bad.bars[0].length = 0x100;
    assert(nnpi_snapshot_window(&bad, &address) != 0);
    bad = d;
    bad.bars[0].translated_start = (uint64_t)INT64_MAX - 0x63;
    assert(nnpi_snapshot_window(&bad, &address) == 0);
    assert(address + NNPI_SNAPSHOT_WINDOW_BYTES - 1 == (uint64_t)INT64_MAX);
    assert(sample(&m, &r) == 0 && m.calls == 3);
    assert(nnpi_snapshot_response_valid(&r, sizeof(r)) && r.generation == 9);
    assert(r.fifo_valid == 3 && r.command_used == 0 && r.command_free == 16);
    assert(r.response_used == 16 && r.response_free == 0);
    assert(r.boot_state == 255 && r.error_code == 15 && r.card_doorbell == UINT32_MAX);
    /* Exhaustive pointer domain, independently bad command/response FIFO. */
    for (rp = 0; rp < 16; ++rp) for (wp = 0; wp < 32; ++wp) {
        const int valid = wp >= rp && wp - rp <= 16;
        m.values[0] = rp | (wp << 8); m.values[1] = 0; m.values[2] = 0;
        assert(sample(&m, &r) == 0 && m.calls == 3);
        assert(nnpi_snapshot_response_valid(&r, sizeof(r)));
        assert(r.command_control == m.values[0] && r.fifo_valid == (valid ? 3U : 2U));
        if (!valid) assert(!r.command_used && !r.command_free && r.response_free == 16);
        m.values[1] = m.values[0]; m.values[0] = 0;
        assert(sample(&m, &r) == 0 && m.calls == 3);
        assert(nnpi_snapshot_response_valid(&r, sizeof(r)));
        assert(r.response_control == m.values[1] && r.fifo_valid == (valid ? 3U : 1U));
        if (!valid) assert(!r.response_used && !r.response_free && r.command_free == 16);
    }
    m.values[0] = 1; m.values[1] = 0x1f00;
    assert(sample(&m, &r) == 0 && m.calls == 3 && r.fifo_valid == 0);
    assert(nnpi_snapshot_response_valid(&r, sizeof(r)));
#define BAD_RESPONSE(change) do { copy = r; change; assert(!nnpi_snapshot_response_valid(&copy, sizeof(copy))); } while (0)
    BAD_RESPONSE(copy.version++); BAD_RESPONSE(copy.size--); BAD_RESPONSE(copy.generation = 0);
    BAD_RESPONSE(copy.attributes ^= 1); BAD_RESPONSE(copy.read_count = 2);
    BAD_RESPONSE(copy.read_count = 4); BAD_RESPONSE(copy.raw_valid = 0);
    BAD_RESPONSE(copy.raw_valid = 15); BAD_RESPONSE(copy.fifo_valid = 3);
    BAD_RESPONSE(copy.fifo_valid = 4); BAD_RESPONSE(copy.boot_state++); BAD_RESPONSE(copy.error_code++);
    BAD_RESPONSE(copy.command_used = 1); BAD_RESPONSE(copy.command_free = 1);
    BAD_RESPONSE(copy.response_used = 1); BAD_RESPONSE(copy.response_free = 1);
    for (i = 0; i < 3; ++i) { BAD_RESPONSE(copy.reserved[i] = 1); }
    m.values[0] = 0; m.values[1] = 0;
    assert(sample(&m, &r) == 0 && nnpi_snapshot_response_valid(&r, sizeof(r)));
    BAD_RESPONSE(copy.command_used++); BAD_RESPONSE(copy.command_free--);
    BAD_RESPONSE(copy.response_used++); BAD_RESPONSE(copy.response_free--);
#undef BAD_RESPONSE
    for (i = 0; i < sizeof(r) + 8; ++i)
        if (i != sizeof(r)) assert(!nnpi_snapshot_response_valid(&r, i));
    assert(!nnpi_snapshot_response_valid(NULL, sizeof(r)));
    /* Idempotent independent detach mirrors adapter ownership before unmap. */
    mapping = &m; status_mapping = &q;
    assert(nnpi_status_detach(&mapping) == &m && mapping == NULL && status_mapping == &q);
    assert(nnpi_status_detach(&mapping) == NULL && status_mapping == &q);
    failure(&q, sizeof(q), sizeof(r), 3, 0, mapping != NULL, 9, 0);
    assert(nnpi_status_window(&d, &address) == 0 && address == status_address);
    assert(nnpi_status_detach(&status_mapping) == &q && status_mapping == NULL);
    assert(nnpi_status_detach(&status_mapping) == NULL);
    puts("snapshot ABI/window/gates/order/three-reads/partial-FIFO/cleanup passed (mock only)");
    return 0;
}
