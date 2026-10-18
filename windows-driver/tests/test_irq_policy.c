/* SPDX-License-Identifier: GPL-2.0-only */
/* Real production policy with mock MMIO only; no device/IRQ/DMA-stop claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/nnpi_irq_policy.h"

struct mock {
    uint32_t Mask, Status, PostStatus, Readback;
    uint32_t Reads, Writes, Events;
    char Order[16];
    struct nnpi_irq_policy *Policy;
};
static void event(struct mock *m, char value)
{
    assert(m->Events < sizeof(m->Order));
    m->Order[m->Events++] = value;
}
static uint32_t read_mask(void *context)
{
    struct mock *m = (struct mock *)context;
    ++m->Reads; event(m, 'm'); return m->Mask;
}
static uint32_t read_status(void *context)
{
    struct mock *m = (struct mock *)context;
    ++m->Reads; event(m, 's'); return m->Status;
}
static void mask_all(void *context)
{
    struct mock *m = (struct mock *)context;
    assert(m->Policy->Claimed && !m->Policy->Verified && !m->Writes);
    ++m->Writes; event(m, 'w');
    m->Mask = m->Readback;
    m->Status = m->PostStatus;
}
static const struct nnpi_irq_io io = {read_mask, read_status, mask_all};
static const struct nnpi_irq_scope scope = {3, 0, 7, 9};
static struct nnpi_irq_evidence evidence(void)
{
    struct nnpi_irq_evidence e = {7, 9, 1, 0, 1, 0, 0};
    return e;
}
static void init(struct nnpi_irq_policy *p, struct mock *m)
{
    nnpi_irq_init(p); memset(m, 0, sizeof(*m)); m->Policy = p;
    m->Mask = 0x12345678U; m->Status = m->PostStatus = 0x11U;
    m->Readback = UINT32_MAX;
}
static uint32_t arm(struct nnpi_irq_policy *p, struct mock *m,
                    const struct nnpi_irq_evidence *e)
{
    return nnpi_irq_arm(p, &scope, 1, NNPI_STATUS_OK, e, &io, m);
}
static uint32_t check(struct nnpi_irq_policy *p, struct mock *m,
                      const struct nnpi_irq_evidence *e)
{
    return nnpi_irq_check(p, &scope, 1, NNPI_STATUS_OK, e, &io, m);
}
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
static void windows(void)
{
    struct nnpi_diag_response d = fixture(), bad;
    uint64_t address;
    assert(NNPI_IRQ_STATUS_OFFSET == 0x008U && NNPI_IRQ_MASK_OFFSET == 0x00cU);
    assert(NNPI_IRQ_WINDOW_BYTES == 8U && NNPI_IRQ_MASK_ALL == UINT32_MAX);
    assert(nnpi_irq_window(&d, &address) == NNPI_IRQ_OK && address == 0x90000008U);
    assert(nnpi_irq_window(NULL, &address) != NNPI_IRQ_OK && address == 0);
    assert(nnpi_irq_window(&d, NULL) != NNPI_IRQ_OK);
#define BAD_WINDOW(change) do { bad = d; change; address = 99; \
    assert(nnpi_irq_window(&bad, &address) != NNPI_IRQ_OK && address == 0); } while (0)
    BAD_WINDOW(bad.config[0] ^= 1); BAD_WINDOW(bad.config[44] ^= 1);
    BAD_WINDOW(bad.config[8] = 1); BAD_WINDOW(bad.config_bytes = 63);
    BAD_WINDOW(bad.reason = NNPI_BAR_NO_MATCH); BAD_WINDOW(bad.bar_count = 7);
    BAD_WINDOW(bad.bar_count = 0); BAD_WINDOW(bad.bars[0].bar = 2);
    BAD_WINDOW(bad.bars[0].type = NNPI_BAR_PORT); BAD_WINDOW(bad.bars[0].prefetch = 1);
    BAD_WINDOW(bad.bars[0].translated_index++); BAD_WINDOW(bad.bars[0].length = 0x0f);
    BAD_WINDOW(bad.bars[0].length = 0x10); BAD_WINDOW(bad.bars[0].length = 0x100);
    BAD_WINDOW(bad.bars[0].translated_start++);
    BAD_WINDOW(bad.bars[0].translated_start = UINT64_MAX - 7);
    BAD_WINDOW(bad.bars[0].translated_start = (uint64_t)INT64_MAX - 0x60);
    BAD_WINDOW(bad.bars[0].length = UINT64_MAX);
    BAD_WINDOW(bad.bar_count = 2; bad.bars[1] = bad.bars[0]);
#undef BAD_WINDOW
    d.bars[0].length = 0x200; /* Conservative shared target aperture floor. */
    assert(nnpi_irq_window(&d, &address) == NNPI_IRQ_OK);
    d.bars[0].translated_start = (uint64_t)INT64_MAX - 0x63;
    assert(nnpi_irq_window(&d, &address) == NNPI_IRQ_OK);
    assert(address == (uint64_t)INT64_MAX - 0x5b);
}
static void success_and_drift(void)
{
    struct nnpi_irq_policy p;
    struct nnpi_irq_evidence e = evidence();
    struct mock m;
    init(&p, &m);
    assert(arm(&p, &m, &e) == NNPI_IRQ_OK);
    assert(m.Reads == 4 && m.Writes == 1 && m.Events == 5);
    assert(memcmp(m.Order, "mswms", 5) == 0);
    assert(p.Claimed && p.Verified && !p.Failed && !p.Checks);
    assert(p.BeforeMask == 0x12345678U && p.AfterMask == UINT32_MAX && p.Status == 0x11U);
    assert(arm(&p, &m, &e) == NNPI_IRQ_ALREADY_ARMED);
    assert(m.Reads == 4 && m.Writes == 1 && p.Verified);
    ++e.Serial; e.Card = 10;
    assert(check(&p, &m, &e) == NNPI_IRQ_OK);
    assert(p.Checks == 1 && p.Status == 0x11U && m.Writes == 1 && m.Reads == 6);
    ++e.Serial; e.Card = 8; m.Mask = 0xfffffff7U;
    assert(check(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
    assert(p.Checks == 2 && p.Failed && !p.Verified && p.AfterMask == 0xfffffff7U);
    assert(m.Reads == 8 && m.Writes == 1); /* No repair, unmask or W1C callback. */
    ++e.Serial; m.Mask = UINT32_MAX;
    assert(check(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
    assert(arm(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
    assert(m.Reads == 8 && m.Writes == 1);
}
/* Exact readback whitelist, not a low-bit containment test. */
static void reject_readback(uint32_t value)
{
    struct nnpi_irq_policy p;
    struct nnpi_irq_evidence e = evidence();
    struct mock m;
    init(&p, &m); m.Readback = value;
    assert(arm(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
    assert(p.Claimed && p.Failed && !p.Verified && !p.ArmedMask);
    assert(p.AfterMask == value && p.Reason == NNPI_IRQ_MASK_DRIFT);
    assert(m.Reads == 4 && m.Writes == 1 && m.Events == 5);
    assert(memcmp(m.Order, "mswms", 5) == 0);
    m.Mask = UINT32_MAX; ++e.Serial;
    assert(arm(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
    assert(check(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
    assert(m.Reads == 4 && m.Writes == 1); /* Failure cannot replenish writes. */
}
static void exact_readbacks(void)
{
    const uint32_t accepted[] = {UINT32_MAX, UINT32_C(0x0007ffff)};
    struct nnpi_irq_policy p;
    struct nnpi_irq_evidence e;
    struct mock m;
    uint32_t i, bit, high, stage;
    assert(NNPI_IRQ_LOCAL_READBACK == UINT32_C(0x0007ffff));
    for (i = 0; i < 2; ++i) {
        init(&p, &m); e = evidence();
        m.Mask = m.Readback = accepted[i];
        m.Status = m.PostStatus = 0x800U; /* Observed pending status is retained. */
        assert(arm(&p, &m, &e) == NNPI_IRQ_OK);
        assert(p.BeforeMask == accepted[i] && p.AfterMask == accepted[i]);
        assert(p.ArmedMask == accepted[i] && p.Status == 0x800U);
        assert(p.Claimed && p.Verified && !p.Failed && p.Reason == NNPI_IRQ_OK);
        assert(m.Reads == 4 && m.Writes == 1 && m.Events == 5);
        assert(memcmp(m.Order, "mswms", 5) == 0);
        assert(arm(&p, &m, &e) == NNPI_IRQ_ALREADY_ARMED);
        assert(m.Reads == 4 && m.Writes == 1);
        ++e.Serial;
        assert(check(&p, &m, &e) == NNPI_IRQ_OK);
        assert(p.Checks == 1 && p.Verified && p.Status == 0x800U);
        assert(m.Reads == 6 && m.Writes == 1 && m.Events == 7);
        assert(memcmp(m.Order, "mswmsms", 7) == 0);
        ++e.Serial; m.Mask = accepted[1U - i];
        assert(check(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
        assert(p.ArmedMask == accepted[i] && p.AfterMask == accepted[1U - i]);
        assert(p.Failed && !p.Verified && p.Checks == 2);
        ++e.Serial; m.Mask = accepted[i];
        assert(check(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
        assert(arm(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
        assert(m.Reads == 8 && m.Writes == 1); /* No repair in either direction. */
        for (bit = 0; bit < 19; ++bit)
            reject_readback(accepted[i] & ~(UINT32_C(1) << bit));
        for (stage = 0; stage < 3; ++stage) {
            init(&p, &m); e = evidence(); m.Mask = m.Readback = accepted[i];
            if (stage == 0) m.Status = UINT32_MAX;
            if (stage == 1) m.PostStatus = UINT32_MAX;
            if (stage < 2) {
                assert(arm(&p, &m, &e) == NNPI_IRQ_BAD_STATUS);
            } else {
                assert(arm(&p, &m, &e) == NNPI_IRQ_OK);
                ++e.Serial; m.Status = UINT32_MAX;
                assert(check(&p, &m, &e) == NNPI_IRQ_BAD_STATUS);
            }
            assert(p.Status == UINT32_MAX && p.Failed && !p.Verified);
            assert(arm(&p, &m, &e) == NNPI_IRQ_BAD_STATUS);
            assert(check(&p, &m, &e) == NNPI_IRQ_BAD_STATUS);
            assert(m.Writes == (stage == 0 ? 0U : 1U));
            assert(m.Reads == (stage == 0 ? 2U : stage == 1 ? 4U : 6U));
        }
    }
    reject_readback(0x0000ffffU);
    /* Exhaust all 13 upper-bit combinations except the two exact allowed values. */
    for (high = 1; high < 0x1fffU; ++high)
        reject_readback(UINT32_C(0x0007ffff) | (high << 19));
    /* Matching a corrupted ArmedMask is insufficient if it is outside the whitelist. */
    init(&p, &m); e = evidence();
    assert(arm(&p, &m, &e) == NNPI_IRQ_OK);
    p.ArmedMask = m.Mask = 0x0000ffffU; ++e.Serial;
    assert(check(&p, &m, &e) == NNPI_IRQ_MASK_DRIFT);
    assert(p.Failed && !p.Verified && m.Reads == 6 && m.Writes == 1);
}
static void failures(void)
{
    struct nnpi_irq_policy p;
    struct nnpi_irq_evidence e;
    struct mock m;
    uint32_t i;
#define BAD_BASELINE(change) do { init(&p, &m); e = evidence(); change; \
    assert(arm(&p, &m, &e) != NNPI_IRQ_OK); \
    assert(!m.Reads && !m.Writes && p.Failed && !p.Verified); } while (0)
    BAD_BASELINE(e.Host = UINT32_MAX); BAD_BASELINE(e.Card = UINT32_MAX);
    BAD_BASELINE(e.Command = UINT32_MAX); BAD_BASELINE(e.Response = UINT32_MAX);
    BAD_BASELINE(e.Host = 9); BAD_BASELINE(e.Card = 2);
    BAD_BASELINE(e.Command = 0x100); BAD_BASELINE(e.Response = 0x100);
    BAD_BASELINE(e.Serial = 0); BAD_BASELINE(e.Serial = UINT64_MAX);
    BAD_BASELINE(e.Generation++); BAD_BASELINE(e.Epoch++);
#undef BAD_BASELINE
    for (i = 0; i < 4; ++i) {
        init(&p, &m); e = evidence();
        if (i == 0) m.Status = UINT32_MAX;
        if (i == 1) m.PostStatus = UINT32_MAX;
        if (i == 2) m.Readback = 0;
        if (i == 3) m.Readback = 0xfffffffeU;
        assert(arm(&p, &m, &e) != NNPI_IRQ_OK);
        assert(p.Claimed && p.Failed && !p.Verified);
        assert(m.Writes == (i == 0 ? 0U : 1U));
        assert(arm(&p, &m, &e) != NNPI_IRQ_OK);
        assert(m.Reads == (i == 0 ? 2U : 4U));
    }
    init(&p, &m); e = evidence(); m.Mask = UINT32_MAX;
    assert(arm(&p, &m, &e) == NNPI_IRQ_OK && m.Writes == 1);
    ++e.Serial; m.Status = UINT32_MAX;
    assert(check(&p, &m, &e) == NNPI_IRQ_BAD_STATUS && !p.Verified);
    assert(m.Writes == 1 && p.Status == UINT32_MAX);
#define BAD_CHECK(change) do { init(&p, &m); e = evidence(); \
    assert(arm(&p, &m, &e) == NNPI_IRQ_OK); ++e.Serial; change; \
    assert(check(&p, &m, &e) != NNPI_IRQ_OK); \
    assert(m.Reads == 4 && m.Writes == 1 && p.Failed && !p.Verified); } while (0)
    BAD_CHECK(e.Card = UINT32_MAX); BAD_CHECK(e.Card = 0x1001);
    BAD_CHECK(e.Card = 2); BAD_CHECK(e.Serial = 1);
    BAD_CHECK(e.Generation++); BAD_CHECK(e.Epoch++);
#undef BAD_CHECK
    init(&p, &m); e = evidence();
    assert(check(&p, &m, &e) == NNPI_IRQ_NOT_ARMED && !m.Reads && !m.Writes);
    init(&p, &m); e = evidence();
    assert(nnpi_irq_arm(&p, &scope, 1, 0, &e, NULL, &m) != NNPI_IRQ_OK);
    assert(!m.Reads && !m.Writes);
    init(&p, &m); assert(arm(&p, &m, &e) == NNPI_IRQ_OK);
    p.Checks = UINT32_MAX; ++e.Serial;
    assert(check(&p, &m, &e) == NNPI_IRQ_OK && p.Checks == UINT32_MAX);
}
static void gates(void)
{
    struct nnpi_irq_policy p;
    struct nnpi_irq_scope s;
    struct nnpi_irq_evidence e;
    struct mock m;
    uint32_t i, reason;
    int mapped;
    for (i = 0; i < 7; ++i) {
        init(&p, &m); e = evidence(); s = scope; mapped = 1; reason = 0;
        if (i == 0) s.Removed = 1;
        if (i == 1) s.Flags = 1;
        if (i == 2) s.Generation = 0;
        if (i == 3) s.Epoch = 0;
        if (i == 4) mapped = 0;
        if (i == 5) reason = NNPI_STATUS_MAP_FAILED;
        if (i == 6) s.Generation = UINT64_MAX;
        assert(nnpi_irq_arm(&p, &s, mapped, reason, &e, &io, &m) != NNPI_IRQ_OK);
        assert(nnpi_irq_check(&p, &s, mapped, reason, &e, &io, &m) != NNPI_IRQ_OK);
        assert(!m.Reads && !m.Writes);
        init(&p, &m); assert(arm(&p, &m, &e) == NNPI_IRQ_OK); ++e.Serial;
        assert(nnpi_irq_check(&p, &s, mapped, reason, &e, &io, &m) != NNPI_IRQ_OK);
        assert(m.Reads == 4 && m.Writes == 1 && !p.Verified);
    }
    /* Even internally matching new G/E cannot reuse an old mapping claim. */
    init(&p, &m); e = evidence(); s = scope;
    assert(arm(&p, &m, &e) == NNPI_IRQ_OK);
    ++s.Epoch; ++e.Epoch; ++e.Serial;
    assert(nnpi_irq_arm(&p, &s, 1, 0, &e, &io, &m) == NNPI_IRQ_STALE);
    assert(m.Reads == 4 && m.Writes == 1 && !p.Verified);
    init(&p, &m); e = evidence(); s = scope;
    assert(arm(&p, &m, &e) == NNPI_IRQ_OK);
    ++s.Generation; ++e.Generation; ++e.Serial;
    assert(nnpi_irq_check(&p, &s, 1, 0, &e, &io, &m) == NNPI_IRQ_STALE);
    assert(m.Reads == 4 && m.Writes == 1 && !p.Verified);
    init(&p, &m); e = evidence();
    assert(nnpi_irq_arm(&p, NULL, 1, 0, &e, &io, &m) != NNPI_IRQ_OK);
    assert(!m.Reads && !m.Writes);
    init(&p, &m);
    assert(nnpi_irq_arm(&p, &scope, 1, 0, NULL, &io, &m) != NNPI_IRQ_OK);
    assert(!m.Reads && !m.Writes);
    init(&p, &m); e = evidence();
    assert(arm(&p, &m, &e) == NNPI_IRQ_OK);
    (void)nnpi_irq_fail(&p, NNPI_IRQ_INVALIDATED); ++e.Serial;
    assert(check(&p, &m, &e) == NNPI_IRQ_INVALIDATED);
    assert(arm(&p, &m, &e) == NNPI_IRQ_INVALIDATED);
    assert(m.Reads == 4 && m.Writes == 1);
}
int main(void)
{
    windows(); success_and_drift(); exact_readbacks(); failures(); gates();
    puts("IRQ policy: map bounds, baseline, mask/readback, pending status, drift, one-shot and removed-no-IO passed (mock only)");
    return 0;
}
