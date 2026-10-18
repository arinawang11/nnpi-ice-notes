/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_DMA_H
#define NNPI_DMA_H
#include <ntddk.h>
#include <wdf.h>
#include "../include/nnpi_dmacheck_policy.h"
/* Private state only. No address publication or DMA transaction exists in 021.
 * All operations after DeviceAdd require CacheLock and PASSIVE_LEVEL. */
typedef struct _NNPI_DMA_STATE {
    WDFDMAENABLER Enabler; /* Device-owned; created once, not per Prepare. */
    NTSTATUS EnablerStatus;
    WDFCOMMONBUFFER Buffer; /* Enabler child, explicitly deleted on Release. */
    PVOID VirtualAddress;
    PHYSICAL_ADDRESS LogicalAddress; /* WDF logical address, NOT CPU physical. */
    size_t Length;
    BOOLEAN Published, StopConfirmed;
    struct nnpi_dmacheck_response Cache;
} NNPI_DMA_STATE;
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiDmaInitialize(WDFDEVICE Device, NNPI_DMA_STATE *State);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiDmaPrepare(NNPI_DMA_STATE *State, uint64_t Generation);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiDmaRelease(NNPI_DMA_STATE *State);
#endif
