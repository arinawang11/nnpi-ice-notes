/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_IRQ_H
#define NNPI_IRQ_H
#include <ntddk.h> /* Kernel header first; never Windows.h. */
#include "../include/nnpi_irq_policy.h"

typedef struct _NNPI_IRQ_MAPPING {
    PVOID Address;
    uint32_t Reason;
    struct nnpi_irq_policy Policy; /* CPU-only internal diagnostics. */
} NNPI_IRQ_MAPPING;

/* All calls require PASSIVE_LEVEL and the SAME device lifecycle/CacheLock,
 * from caller's fresh baseline/card read through the call. Initialize storage
 * to zero once. Map only in PrepareHardware for a new resource incarnation;
 * never map/reinitialize to recover a failed Arm/Check. Basic info recovery
 * remains safe: neither Map nor Unmap performs MMIO. No ISR is installed.
 *
 * Scope must describe CURRENT Cache.flags/Removed/Cache.generation/Epoch.
 * Evidence must be freshly read in that scope, not cached 025/previous boot
 * data. Arm requires a normal host/card/FIFO baseline AFTER BootReadyWait and
 * BEFORE nnpi_boot_session_hostready/NnpiWriteHostReady. Check reuses the fresh
 * card from an existing boot/runtime poll and a monotonic owner read serial.
 *
 * Return is an IRQ/status policy reason (zero success), NOT NTSTATUS: never
 * use NT_SUCCESS.
 * Any failed check blocks later host-ready/FIFO/runtime writes; caller follows
 * its existing abort/cleanup path. Neither Verified nor mask-all proves DMA
 * stop. D0 loss, reset start and surprise removal call Invalidate under lock;
 * ReleaseHardware owns Unmap after workers are drained. No cleanup MMIO.
 */
_IRQL_requires_(PASSIVE_LEVEL)
uint32_t NnpiIrqMap(const struct nnpi_diag_response *Diagnostic, NNPI_IRQ_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiIrqUnmap(NNPI_IRQ_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiIrqInvalidate(NNPI_IRQ_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
uint32_t NnpiIrqArm(NNPI_IRQ_MAPPING *Mapping, const struct nnpi_irq_scope *Scope,
                   const struct nnpi_irq_evidence *Evidence);
_IRQL_requires_(PASSIVE_LEVEL)
uint32_t NnpiIrqCheck(NNPI_IRQ_MAPPING *Mapping, const struct nnpi_irq_scope *Scope,
                     const struct nnpi_irq_evidence *Evidence);
#endif
