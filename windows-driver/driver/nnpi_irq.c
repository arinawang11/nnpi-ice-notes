/* SPDX-License-Identifier: GPL-2.0-only */
#include "nnpi_irq.h"

/* Exactly two fixed 32-bit registers. No public offset/value accessors. */
static uint32_t NnpiIrqReadMask(void *Context)
{
    NNPI_IRQ_MAPPING *mapping = (NNPI_IRQ_MAPPING *)Context;
    return (uint32_t)READ_REGISTER_ULONG((volatile ULONG *)
        ((PUCHAR)mapping->Address + (NNPI_IRQ_MASK_OFFSET - NNPI_IRQ_STATUS_OFFSET)));
}
static uint32_t NnpiIrqReadStatus(void *Context)
{
    NNPI_IRQ_MAPPING *mapping = (NNPI_IRQ_MAPPING *)Context;
    return (uint32_t)READ_REGISTER_ULONG((volatile ULONG *)mapping->Address);
}
static void NnpiIrqMaskAll(void *Context)
{
    NNPI_IRQ_MAPPING *mapping = (NNPI_IRQ_MAPPING *)Context;
    WRITE_REGISTER_ULONG((volatile ULONG *)
        ((PUCHAR)mapping->Address + (NNPI_IRQ_MASK_OFFSET - NNPI_IRQ_STATUS_OFFSET)),
        NNPI_IRQ_MASK_ALL);
}
static const struct nnpi_irq_io NnpiIrqIo = {
    NnpiIrqReadMask, NnpiIrqReadStatus, NnpiIrqMaskAll
};

_Use_decl_annotations_
VOID NnpiIrqInvalidate(NNPI_IRQ_MAPPING *Mapping)
{
    if (Mapping != NULL) (void)nnpi_irq_fail(&Mapping->Policy, NNPI_IRQ_INVALIDATED);
}

_Use_decl_annotations_
VOID NnpiIrqUnmap(NNPI_IRQ_MAPPING *Mapping)
{
    PVOID old;
    if (Mapping == NULL) return;
    NnpiIrqInvalidate(Mapping);
    old = nnpi_status_detach(&Mapping->Address);
    Mapping->Reason = NNPI_STATUS_NOT_READY;
    /* Keep the claim/failure and last diagnostic sample; never unmask on exit. */
    if (old != NULL) MmUnmapIoSpace(old, NNPI_IRQ_WINDOW_BYTES);
}

_Use_decl_annotations_
uint32_t NnpiIrqMap(const struct nnpi_diag_response *Diagnostic, NNPI_IRQ_MAPPING *Mapping)
{
    uint64_t address;
    PHYSICAL_ADDRESS physical;
    if (Mapping == NULL) return NNPI_STATUS_NOT_READY;
    /* A repeated Map must not silently reset a live one-shot/failure ledger. */
    if (Mapping->Address != NULL) return NNPI_STATUS_NOT_READY;
    Mapping->Reason = nnpi_irq_window(Diagnostic, &address);
    if (Mapping->Reason != NNPI_IRQ_OK) return Mapping->Reason;
    physical.QuadPart = (LONGLONG)address;
    Mapping->Address = MmMapIoSpaceEx(physical, NNPI_IRQ_WINDOW_BYTES,
                                     PAGE_READWRITE | PAGE_NOCACHE);
    if (Mapping->Address == NULL) {
        Mapping->Reason = NNPI_STATUS_MAP_FAILED;
        return Mapping->Reason;
    }
    /* PrepareHardware owns this new mapping incarnation. No hardware access. */
    nnpi_irq_init(&Mapping->Policy);
    return NNPI_IRQ_OK;
}

_Use_decl_annotations_
uint32_t NnpiIrqArm(NNPI_IRQ_MAPPING *Mapping, const struct nnpi_irq_scope *Scope,
                   const struct nnpi_irq_evidence *Evidence)
{
    uint32_t reason;
    if (Mapping == NULL) return NNPI_STATUS_NOT_READY;
    __try {
        reason = nnpi_irq_arm(&Mapping->Policy, Scope, Mapping->Address != NULL,
                             Mapping->Reason, Evidence, &NnpiIrqIo, Mapping);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reason = nnpi_irq_fail(&Mapping->Policy, NNPI_IRQ_IO_ERROR);
    }
    return reason;
}

_Use_decl_annotations_
uint32_t NnpiIrqCheck(NNPI_IRQ_MAPPING *Mapping, const struct nnpi_irq_scope *Scope,
                     const struct nnpi_irq_evidence *Evidence)
{
    uint32_t reason;
    if (Mapping == NULL) return NNPI_STATUS_NOT_READY;
    __try {
        reason = nnpi_irq_check(&Mapping->Policy, Scope, Mapping->Address != NULL,
                               Mapping->Reason, Evidence, &NnpiIrqIo, Mapping);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reason = nnpi_irq_fail(&Mapping->Policy, NNPI_IRQ_IO_ERROR);
    }
    return reason;
}
