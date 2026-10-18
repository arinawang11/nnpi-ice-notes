/* SPDX-License-Identifier: GPL-2.0-only */
#include "nnpi_net_dma.h"
#include "../include/nnpi_boot_protocol.h"

C_ASSERT(NNPI_NET_POOL_BYTES == 65536u);
C_ASSERT(NNPI_ETH_PAGE_BYTES == PAGE_SIZE);
C_ASSERT(NNPI_NET_CHAIN_BYTES == NNPI_BOOT_PAGE_SIZE);

/* Check the entire DMA interval, not only its first PFN. */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN NnpiNetPoolValid(const NNPI_NET_POOL *Pool, size_t Bytes)
{
    uint64_t dma = (uint64_t)Pool->LogicalAddress.QuadPart;
    return Pool->Handle != NULL && Pool->VirtualAddress != NULL &&
        Pool->MappedBytes == Bytes &&
        ((ULONG_PTR)Pool->VirtualAddress & (NNPI_ETH_PAGE_BYTES - 1u)) == 0 &&
        dma != 0 && nnpi_boot_dma_range(dma, Bytes) == NNPI_BOOT_OK;
}

static BOOLEAN NnpiNetPoolEmpty(const NNPI_NET_POOL *Pool)
{
    return Pool->Handle == NULL && Pool->VirtualAddress == NULL &&
        Pool->LogicalAddress.QuadPart == 0 && Pool->MappedBytes == 0;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID NnpiNetPoolDelete(NNPI_NET_POOL *Pool, size_t Bytes)
{
    if (Pool->Handle != NULL) {
        /* Do not dereference a rejected mapping, including a bogus length. */
        if (NnpiNetPoolValid(Pool, Bytes))
            RtlSecureZeroMemory(Pool->VirtualAddress, Bytes);
        WdfObjectDelete(Pool->Handle);
    }
    RtlZeroMemory(Pool, sizeof(*Pool));
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS NnpiNetPoolAllocate(WDFDMAENABLER Enabler,
    NNPI_NET_POOL *Pool, size_t Bytes)
{
    NTSTATUS status;
    WDFCOMMONBUFFER handle = NULL;
    /* WDF fixes the parent to this enabler. Inherit its alignment. */
    status = WdfCommonBufferCreate(Enabler, Bytes,
        WDF_NO_OBJECT_ATTRIBUTES, &handle);
    if (!NT_SUCCESS(status)) return status;
    Pool->Handle = handle;
    Pool->VirtualAddress = WdfCommonBufferGetAlignedVirtualAddress(handle);
    Pool->LogicalAddress = WdfCommonBufferGetAlignedLogicalAddress(handle);
    Pool->MappedBytes = WdfCommonBufferGetLength(handle);
    if (!NnpiNetPoolValid(Pool, Bytes)) return STATUS_INVALID_ADDRESS;
    RtlZeroMemory(Pool->VirtualAddress, Bytes);
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS NnpiNetChainBuild(const NNPI_NET_POOL *Data, NNPI_NET_POOL *Chain)
{
    struct nnpi_boot_segment segment;
    segment.dma_address = (uint64_t)Data->LogicalAddress.QuadPart;
    segment.byte_length = NNPI_NET_POOL_BYTES;
    return nnpi_boot_block((uint8_t *)Chain->VirtualAddress, Chain->MappedBytes,
        &segment, 1, 1, 0, 0, 1) == NNPI_BOOT_OK ?
        STATUS_SUCCESS : STATUS_INVALID_ADDRESS;
}

/* Validate the header, its one entry, and every reserved tail byte without
 * allocating a 4KiB scratch page on the kernel stack. */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN NnpiNetChainValid(const NNPI_NET_POOL *Data, const NNPI_NET_POOL *Chain)
{
    uint8_t expected[NNPI_BOOT_HEADER_SIZE + NNPI_BOOT_ENTRY_SIZE];
    const uint8_t *actual;
    struct nnpi_boot_segment segment;
    size_t i;
    if (!NnpiNetPoolValid(Data, NNPI_NET_POOL_BYTES) ||
        !NnpiNetPoolValid(Chain, NNPI_NET_CHAIN_BYTES)) return FALSE;
    segment.dma_address = (uint64_t)Data->LogicalAddress.QuadPart;
    segment.byte_length = NNPI_NET_POOL_BYTES;
    /* The single terminal block has the same 32-byte prefix in either form. */
    if (nnpi_boot_block(expected, sizeof(expected), &segment, 1, 1, 0, 0, 0)
        != NNPI_BOOT_OK) return FALSE;
    actual = (const uint8_t *)Chain->VirtualAddress;
    for (i = 0; i < sizeof(expected); ++i)
        if (actual[i] != expected[i]) return FALSE;
    for (; i < NNPI_NET_CHAIN_BYTES; ++i)
        if (actual[i] != 0) return FALSE;
    return TRUE;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiNetDmaRelease(NNPI_NET_DMA_STATE *State)
{
    if (State == NULL) return STATUS_INVALID_PARAMETER;
    if (State->Published && !State->StopConfirmed) return STATUS_DEVICE_BUSY;
    NnpiNetPoolDelete(&State->C2HChain, NNPI_NET_CHAIN_BYTES);
    NnpiNetPoolDelete(&State->C2H, NNPI_NET_POOL_BYTES);
    NnpiNetPoolDelete(&State->H2CChain, NNPI_NET_CHAIN_BYTES);
    NnpiNetPoolDelete(&State->H2C, NNPI_NET_POOL_BYTES);
    if (State->Enabler != NULL) WdfObjectDelete(State->Enabler);
    RtlZeroMemory(State, sizeof(*State));
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiNetDmaPrepare(WDFDEVICE Device, NNPI_NET_DMA_STATE *State)
{
    WDF_DMA_ENABLER_CONFIG config;
    WDFDMAENABLER enabler = NULL;
    NTSTATUS status;
    ULONG alignment;
    if (Device == NULL || State == NULL) return STATUS_INVALID_PARAMETER;
    if (State->Enabler != NULL || !NnpiNetPoolEmpty(&State->H2C) ||
        !NnpiNetPoolEmpty(&State->H2CChain) || !NnpiNetPoolEmpty(&State->C2H) ||
        !NnpiNetPoolEmpty(&State->C2HChain) || State->Prepared ||
        State->Published || State->StopConfirmed)
        return STATUS_INVALID_DEVICE_STATE;
    alignment = WdfDeviceGetAlignmentRequirement(Device);
    if ((alignment & (NNPI_ETH_PAGE_BYTES - 1u)) != (NNPI_ETH_PAGE_BYTES - 1u))
        return STATUS_INVALID_DEVICE_STATE;
    WDF_DMA_ENABLER_CONFIG_INIT(&config, WdfDmaProfileScatterGather64,
        NNPI_NET_POOL_BYTES);
    status = WdfDmaEnablerCreate(Device, &config,
        WDF_NO_OBJECT_ATTRIBUTES, &enabler);
    if (!NT_SUCCESS(status)) return status;
    State->Enabler = enabler;
    status = NnpiNetPoolAllocate(enabler, &State->H2C, NNPI_NET_POOL_BYTES);
    if (!NT_SUCCESS(status)) goto Failed;
    status = NnpiNetPoolAllocate(enabler, &State->H2CChain, NNPI_NET_CHAIN_BYTES);
    if (!NT_SUCCESS(status)) goto Failed;
    status = NnpiNetChainBuild(&State->H2C, &State->H2CChain);
    if (!NT_SUCCESS(status)) goto Failed;
    status = NnpiNetPoolAllocate(enabler, &State->C2H, NNPI_NET_POOL_BYTES);
    if (!NT_SUCCESS(status)) goto Failed;
    status = NnpiNetPoolAllocate(enabler, &State->C2HChain, NNPI_NET_CHAIN_BYTES);
    if (!NT_SUCCESS(status)) goto Failed;
    status = NnpiNetChainBuild(&State->C2H, &State->C2HChain);
    if (!NT_SUCCESS(status)) goto Failed;
    State->Prepared = TRUE;
    return STATUS_SUCCESS;
Failed:
    (void)NnpiNetDmaRelease(State); /* No address has been published. */
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiNetDmaMarkPublished(NNPI_NET_DMA_STATE *State)
{
    if (State == NULL) return STATUS_INVALID_PARAMETER;
    if (!State->Prepared || State->Enabler == NULL || State->Published ||
        State->StopConfirmed || !NnpiNetChainValid(&State->H2C, &State->H2CChain) ||
        !NnpiNetChainValid(&State->C2H, &State->C2HChain))
        return STATUS_INVALID_DEVICE_STATE;
    /* Conservative transition BEFORE MMIO; integration owns FIFO barriers. */
    KeMemoryBarrier();
    State->StopConfirmed = FALSE;
    State->Published = TRUE;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
uint64_t NnpiNetDmaHostPfn(const NNPI_NET_DMA_STATE *State, BOOLEAN H2C)
{
    const NNPI_NET_POOL *data, *chain;
    if (State == NULL || !State->Prepared || State->Enabler == NULL) return 0;
    data = H2C ? &State->H2C : &State->C2H;
    chain = H2C ? &State->H2CChain : &State->C2HChain;
    if (!NnpiNetChainValid(data, chain)) return 0;
    return (uint64_t)chain->LogicalAddress.QuadPart >> NNPI_CHAN_PAGE_SHIFT;
}

_IRQL_requires_(PASSIVE_LEVEL)
PVOID NnpiNetDmaSlotAddress(const NNPI_NET_DMA_STATE *State, BOOLEAN H2C, ULONG Slot)
{
    const NNPI_NET_POOL *pool;
    if (State == NULL || !State->Prepared || State->Enabler == NULL ||
        Slot >= NNPI_ETH_SLOTS) return NULL;
    pool = H2C ? &State->H2C : &State->C2H;
    if (!NnpiNetPoolValid(pool, NNPI_NET_POOL_BYTES)) return NULL;
    return (UCHAR *)pool->VirtualAddress + (size_t)Slot * NNPI_ETH_PAGE_BYTES;
}
