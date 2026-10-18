/* SPDX-License-Identifier: GPL-2.0-only */
#include "nnpi_dma.h"

_IRQL_requires_(PASSIVE_LEVEL)
static void NnpiDmaWriteByte(void *Context, size_t Offset, uint8_t Value)
{
    NNPI_DMA_STATE *state = (NNPI_DMA_STATE *)Context;
    ((volatile uint8_t *)state->VirtualAddress)[Offset] = Value;
}
_IRQL_requires_(PASSIVE_LEVEL)
static uint8_t NnpiDmaReadByte(void *Context, size_t Offset)
{
    NNPI_DMA_STATE *state = (NNPI_DMA_STATE *)Context;
    return ((volatile uint8_t *)state->VirtualAddress)[Offset];
}

/* Caller holds CacheLock. Explicit deletion requires UNPUBLISHED or the
 * publication-associated successful reset stop evidence. The device's
 * ReleaseHardware barrier additionally prevents PARENT Dispose from freeing
 * this WDF child before stop evidence; a reference is NOT that barrier. */
_IRQL_requires_(PASSIVE_LEVEL)
static void NnpiDmaReleasePage(void *Context)
{
    NNPI_DMA_STATE *state = (NNPI_DMA_STATE *)Context;
    NT_ASSERT(!state->Published || state->StopConfirmed);
    if (state->Published && !state->StopConfirmed) return;
    if (state->Buffer != NULL) {
        if (state->VirtualAddress != NULL && state->Length != 0)
            (void)nnpi_dmacheck_zero(state, state->Length, NnpiDmaWriteByte, NnpiDmaReadByte);
        WdfObjectDelete(state->Buffer);
    }
    state->Buffer = NULL;
    state->VirtualAddress = NULL;
    state->LogicalAddress.QuadPart = 0;
    state->Length = 0;
}

_IRQL_requires_(PASSIVE_LEVEL)
static uint32_t NnpiDmaAllocatePage(void *Context, uint64_t *Logical, uint64_t *Bytes, int *HasVa)
{
    NNPI_DMA_STATE *state = (NNPI_DMA_STATE *)Context;
    NTSTATUS status;
    /* Inherit enabler alignment (device/HAL maximum) without a buffer config
     * structure version entry point. Runtime validation is still required.
     * Common-buffer parent is fixed by WDF; do not supply ParentObject. */
    status = WdfCommonBufferCreate(state->Enabler, NNPI_DMACHECK_BYTES,
        WDF_NO_OBJECT_ATTRIBUTES, &state->Buffer);
    if (!NT_SUCCESS(status)) return (uint32_t)status;
    state->VirtualAddress = WdfCommonBufferGetAlignedVirtualAddress(state->Buffer);
    state->LogicalAddress = WdfCommonBufferGetAlignedLogicalAddress(state->Buffer);
    state->Length = WdfCommonBufferGetLength(state->Buffer);
    *Logical = (uint64_t)state->LogicalAddress.QuadPart;
    *Bytes = (uint64_t)state->Length;
    *HasVa = state->VirtualAddress != NULL;
    return (uint32_t)status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiDmaInitialize(WDFDEVICE Device, NNPI_DMA_STATE *State)
{
    WDF_DMA_ENABLER_CONFIG config;
    /* Linux host driver requests DMA_BIT_MASK(64); no duplex transfers here.
     * This profile choice is NOT evidence of successful device DMA. */
    WdfDeviceSetAlignmentRequirement(Device, NNPI_DMACHECK_ALIGNMENT);
    WDF_DMA_ENABLER_CONFIG_INIT(&config, WdfDmaProfileScatterGather64, NNPI_DMACHECK_BYTES);
    State->EnablerStatus = WdfDmaEnablerCreate(Device, &config,
        WDF_NO_OBJECT_ATTRIBUTES, &State->Enabler);
    /* Device is the enabler's parent; failed optional capability cannot fail INFO. */
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiDmaPrepare(NNPI_DMA_STATE *State, uint64_t Generation)
{
    NT_ASSERT(!State->Published);
    if (State->Published) return;
    nnpi_dmacheck_prepare(&State->Cache, Generation, (uint32_t)State->EnablerStatus,
        State, NnpiDmaAllocatePage, NnpiDmaReleasePage, NnpiDmaWriteByte, NnpiDmaReadByte);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiDmaRelease(NNPI_DMA_STATE *State)
{
    NT_ASSERT(!State->Published || State->StopConfirmed);
    if (State->Published && !State->StopConfirmed) return;
    nnpi_dmacheck_release(&State->Cache, State, NnpiDmaReleasePage);
    /* Keep device-owned enabler until WDF deletes the device, after its buffers. */
}
