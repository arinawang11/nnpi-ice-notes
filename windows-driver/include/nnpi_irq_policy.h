/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_IRQ_POLICY_H
#define NNPI_IRQ_POLICY_H
#include "nnpi_snapshot_policy.h"
#include "nnpi_hostready.h"

/* Linux BAR0 interrupt status/mask, NOT CPU_STATUS0 (0x1b8).
 * Polling only. Never unmask, acknowledge W1C, touch AER/SERR or infer DMA stop.
 * This is an internal policy, not a user-visible ABI/register access interface. */
#define NNPI_IRQ_STATUS_OFFSET 0x008U
#define NNPI_IRQ_MASK_OFFSET 0x00cU
#define NNPI_IRQ_WINDOW_BYTES 8U
#define NNPI_IRQ_MASK_ALL UINT32_MAX
/* Exact readback observed on this 8086:45c6/45ce rev0 test card after a
 * write-all. Not a claim that other hardware implements only 19 bits. */
#define NNPI_IRQ_LOCAL_READBACK UINT32_C(0x0007ffff)
#define NNPI_IRQ_OK NNPI_STATUS_OK
#define NNPI_IRQ_BAD_BASELINE 7U
#define NNPI_IRQ_BAD_STATUS 8U
#define NNPI_IRQ_MASK_DRIFT 9U
#define NNPI_IRQ_ALREADY_ARMED 10U
#define NNPI_IRQ_STALE 11U
#define NNPI_IRQ_NOT_ARMED 12U
#define NNPI_IRQ_IO_ERROR 13U
#define NNPI_IRQ_INVALIDATED 14U

struct nnpi_irq_scope {
    uint32_t Flags, Removed;
    uint64_t Generation, Epoch;
};
/* Owner supplies actual fresh reads in this same locked G/E incarnation.
 * Serial is a strictly increasing, nonzero owner read serial, not a timestamp
 * or readiness claim. Tokens cannot prove freshness without this contract.
 * Arm uses all four values; Check uses Card only, no new doorbell read here. */
struct nnpi_irq_evidence {
    uint64_t Generation, Epoch, Serial;
    uint32_t Host, Card, Command, Response;
};
struct nnpi_irq_policy {
    uint32_t BeforeMask, AfterMask, Status, Claimed, Verified, Checks;
    uint32_t Failed, Reason;
    uint32_t ArmedMask; /* Exact accepted first readback; later drift is denied. */
    uint64_t Generation, Epoch, LastSerial;
};
/* Fixed callbacks only, no offset/value argument and no W1C write callback. */
struct nnpi_irq_io {
    uint32_t (*ReadMask)(void *context);
    uint32_t (*ReadStatus)(void *context);
    void (*MaskAll)(void *context);
};

/* Pure resource validation; no map, read, write or PCI config transaction.
 * snapshot_window revalidates target identity and the unique BAR0 association.
 * Retain its conservative aperture requirement, then independently bound our
 * narrower 0x008..0x00f request before forming the physical address. */
static inline uint32_t nnpi_irq_window(const struct nnpi_diag_response *d,
                                      uint64_t *address)
{
    const struct nnpi_bar_entry *bar;
    uint64_t snapshot, base;
    uint32_t reason;
    if (!address) return NNPI_STATUS_RANGE;
    *address = 0;
    reason = nnpi_snapshot_window(d, &snapshot);
    if (reason != NNPI_STATUS_OK) return reason;
    if (snapshot < NNPI_SNAPSHOT_WINDOW_OFFSET) return NNPI_STATUS_RANGE;
    base = snapshot - NNPI_SNAPSHOT_WINDOW_OFFSET;
    bar = &d->bars[0];
    if (bar->bar != 0 || base != bar->translated_start || (base & 3U) ||
        bar->length < NNPI_IRQ_STATUS_OFFSET + NNPI_IRQ_WINDOW_BYTES ||
        !nnpi_bar_range(base, bar->length) ||
        base > (uint64_t)INT64_MAX - NNPI_IRQ_STATUS_OFFSET - (NNPI_IRQ_WINDOW_BYTES - 1U))
        return NNPI_STATUS_RANGE;
    *address = base + NNPI_IRQ_STATUS_OFFSET;
    return NNPI_IRQ_OK;
}

/* Init ONLY when creating a new mapping ownership incarnation. Map is not a
 * repair/re-arm API; never clear a live failure/claim to replenish writes. */
static inline void nnpi_irq_init(struct nnpi_irq_policy *p)
{
    if (p) memset(p, 0, sizeof(*p));
}
static inline uint32_t nnpi_irq_fail(struct nnpi_irq_policy *p, uint32_t reason)
{
    if (p) {
        if (!p->Failed) p->Reason = reason;
        p->Failed = 1;
        p->Verified = 0;
    }
    return reason;
}
static inline uint32_t nnpi_irq_gate(const struct nnpi_irq_scope *scope,
    int mapped, uint32_t map_reason, const struct nnpi_irq_evidence *e)
{
    uint32_t reason;
    if (!scope) return NNPI_STATUS_NOT_READY;
    reason = nnpi_status_gate(scope->Flags, scope->Removed != 0, mapped,
                             scope->Generation, map_reason);
    if (reason != NNPI_STATUS_OK) return reason;
    if (!scope->Epoch || scope->Epoch == UINT64_MAX || scope->Generation == UINT64_MAX ||
        !e || e->Generation != scope->Generation || e->Epoch != scope->Epoch ||
        !e->Serial || e->Serial == UINT64_MAX) return NNPI_IRQ_STALE;
    return NNPI_IRQ_OK;
}
static inline int nnpi_irq_card_valid(uint32_t card)
{
    if (card == UINT32_MAX || (card & 0xf000U)) return 0;
    switch (card & 0xffU) {
    case 0: case 1: case 3: case 4: case 8: case 10: return 1;
    default: return 0;
    }
}

/* One shot after ReadyWait's NORMAL baseline, before host-ready, under the
 * lifecycle lock. Claim precedes first MMIO so exceptions cannot allow retries.
 * Accept only full readback or the exact locally observed readback; neither
 * proves accessibility without the normal baseline and non-all-ones status.
 * Pending status bits are retained, never cleared. */
static inline uint32_t nnpi_irq_arm(struct nnpi_irq_policy *p,
    const struct nnpi_irq_scope *scope, int mapped, uint32_t map_reason,
    const struct nnpi_irq_evidence *e, const struct nnpi_irq_io *io, void *context)
{
    uint32_t reason;
    if (!p) return NNPI_STATUS_NOT_READY;
    reason = nnpi_irq_gate(scope, mapped, map_reason, e);
    if (reason != NNPI_IRQ_OK) return nnpi_irq_fail(p, reason);
    if (p->Failed) return p->Reason;
    if (p->Claimed) {
        if (e->Generation != p->Generation || e->Epoch != p->Epoch)
            return nnpi_irq_fail(p, NNPI_IRQ_STALE);
        return NNPI_IRQ_ALREADY_ARMED; /* No reads or writes. */
    }
    if (!nnpi_hostready_baseline(e->Host, e->Card, e->Command, e->Response))
        return nnpi_irq_fail(p, NNPI_IRQ_BAD_BASELINE);
    if (!io || !io->ReadMask || !io->ReadStatus || !io->MaskAll)
        return nnpi_irq_fail(p, NNPI_STATUS_NOT_READY);
    p->Generation = e->Generation;
    p->Epoch = e->Epoch;
    p->LastSerial = e->Serial;
    p->Claimed = 1;
    p->BeforeMask = io->ReadMask(context);
    p->Status = io->ReadStatus(context);
    if (p->Status == UINT32_MAX) return nnpi_irq_fail(p, NNPI_IRQ_BAD_STATUS);
    io->MaskAll(context);
    p->AfterMask = io->ReadMask(context); /* Posted-write completion/readback. */
    p->Status = io->ReadStatus(context);
    if (p->Status == UINT32_MAX) return nnpi_irq_fail(p, NNPI_IRQ_BAD_STATUS);
    if (p->AfterMask != NNPI_IRQ_MASK_ALL && p->AfterMask != NNPI_IRQ_LOCAL_READBACK)
        return nnpi_irq_fail(p, NNPI_IRQ_MASK_DRIFT);
    p->ArmedMask = p->AfterMask;
    p->Verified = 1;
    p->Reason = NNPI_IRQ_OK;
    return NNPI_IRQ_OK;
}

/* One bounded read-only sample per existing caller poll. No independent poll
 * loop, repair, implicit re-arm or ACK. First failure latches until teardown.
 * AfterMask and Status are the last attempted sample; Checks counts attempts,
 * saturating rather than wrapping. Verified means only interrupt isolation. */
static inline uint32_t nnpi_irq_check(struct nnpi_irq_policy *p,
    const struct nnpi_irq_scope *scope, int mapped, uint32_t map_reason,
    const struct nnpi_irq_evidence *e, const struct nnpi_irq_io *io, void *context)
{
    uint32_t reason;
    if (!p) return NNPI_STATUS_NOT_READY;
    reason = nnpi_irq_gate(scope, mapped, map_reason, e);
    if (reason != NNPI_IRQ_OK) return nnpi_irq_fail(p, reason);
    if (p->Failed) return p->Reason;
    if (!p->Claimed || !p->Verified) return nnpi_irq_fail(p, NNPI_IRQ_NOT_ARMED);
    if (e->Generation != p->Generation || e->Epoch != p->Epoch || e->Serial <= p->LastSerial)
        return nnpi_irq_fail(p, NNPI_IRQ_STALE);
    if (!nnpi_irq_card_valid(e->Card)) return nnpi_irq_fail(p, NNPI_IRQ_BAD_BASELINE);
    if (!io || !io->ReadMask || !io->ReadStatus)
        return nnpi_irq_fail(p, NNPI_STATUS_NOT_READY);
    p->LastSerial = e->Serial;
    if (p->Checks != UINT32_MAX) ++p->Checks;
    p->AfterMask = io->ReadMask(context);
    p->Status = io->ReadStatus(context);
    if (p->Status == UINT32_MAX) return nnpi_irq_fail(p, NNPI_IRQ_BAD_STATUS);
    if ((p->ArmedMask != NNPI_IRQ_MASK_ALL && p->ArmedMask != NNPI_IRQ_LOCAL_READBACK) ||
        p->AfterMask != p->ArmedMask) return nnpi_irq_fail(p, NNPI_IRQ_MASK_DRIFT);
    return NNPI_IRQ_OK;
}
#endif
