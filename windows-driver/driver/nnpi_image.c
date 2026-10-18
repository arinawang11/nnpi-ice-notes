/* SPDX-License-Identifier: GPL-2.0-only */
#include "nnpi_image.h"

C_ASSERT(NNPI_IMAGE_CHUNK_COUNT == 181u);
C_ASSERT(NNPI_IMAGE_BYTES - (NNPI_IMAGE_CHUNK_COUNT - 1u) *
    NNPI_IMAGE_CHUNK_BYTES == 512u);
C_ASSERT(NNPI_IMAGE_TABLE_BYTES == 1472u);
C_ASSERT(NNPI_IMAGE_TABLE_MAPPED_BYTES == 4096u);
C_ASSERT(PAGE_SIZE == NNPI_BOOT_PAGE_SIZE);

_IRQL_requires_(PASSIVE_LEVEL)
static size_t NnpiImageChunkLength(ULONG Index)
{
    size_t remaining = (size_t)NNPI_IMAGE_BYTES -
        (size_t)Index * NNPI_IMAGE_CHUNK_BYTES;
    return remaining < NNPI_IMAGE_CHUNK_BYTES ? remaining : NNPI_IMAGE_CHUNK_BYTES;
}

_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN NnpiImageBufferValid(const NNPI_IMAGE_BUFFER *Buffer, size_t Bytes)
{
    UCHAR entry[NNPI_BOOT_ENTRY_SIZE];
    if (Buffer->Handle == NULL || Buffer->VirtualAddress == NULL ||
        Buffer->ImageBytes != Bytes || Bytes == 0 ||
        Bytes > NNPI_IMAGE_CHUNK_BYTES ||
        Buffer->MappedBytes < NNPI_IMAGE_ROUND_PAGE(Bytes) ||
        (Buffer->MappedBytes & NNPI_IMAGE_ALIGNMENT) != 0 ||
        ((ULONG_PTR)Buffer->VirtualAddress & NNPI_IMAGE_ALIGNMENT) != 0)
        return FALSE;
    /* Check the entire real mapping, not just the unrounded valid image bytes.
     * Entry encoder additionally validates the PFN/page-count representation. */
    return nnpi_boot_dma_range((uint64_t)Buffer->LogicalAddress.QuadPart,
               (uint64_t)Buffer->MappedBytes) == NNPI_BOOT_OK &&
        nnpi_boot_entry(entry, sizeof(entry),
               (uint64_t)Buffer->LogicalAddress.QuadPart, (uint64_t)Bytes) == NNPI_BOOT_OK;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID NnpiImageDeleteBuffer(NNPI_IMAGE_BUFFER *Buffer)
{
    if (Buffer->Handle != NULL) {
        if (Buffer->VirtualAddress != NULL && Buffer->MappedBytes != 0)
            RtlSecureZeroMemory(Buffer->VirtualAddress, Buffer->MappedBytes);
        WdfObjectDelete(Buffer->Handle);
    }
    RtlZeroMemory(Buffer, sizeof(*Buffer));
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS NnpiImageAllocateBuffer(WDFDMAENABLER Enabler,
    NNPI_IMAGE_BUFFER *Buffer, size_t Bytes)
{
    NTSTATUS status;
    WDFCOMMONBUFFER handle = NULL;
    /* No ParentObject: WDF fixes the common buffer's parent to its enabler.
     * Inherit the enabler/device/HAL alignment; never override it downwards. */
    status = WdfCommonBufferCreate(Enabler, NNPI_IMAGE_ROUND_PAGE(Bytes),
        WDF_NO_OBJECT_ATTRIBUTES, &handle);
    if (!NT_SUCCESS(status)) return status;
    Buffer->Handle = handle;
    Buffer->VirtualAddress = WdfCommonBufferGetAlignedVirtualAddress(handle);
    Buffer->LogicalAddress = WdfCommonBufferGetAlignedLogicalAddress(handle);
    Buffer->MappedBytes = WdfCommonBufferGetLength(handle);
    Buffer->ImageBytes = Bytes;
    if (!NnpiImageBufferValid(Buffer, Bytes)) {
        /* This allocation is newest and not yet included in AllocatedChunks. */
        NnpiImageDeleteBuffer(Buffer);
        return STATUS_INVALID_ADDRESS;
    }
    RtlZeroMemory(Buffer->VirtualAddress, Buffer->MappedBytes);
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImageRelease(NNPI_IMAGE_STATE *State)
{
    if (State == NULL) return STATUS_INVALID_PARAMETER;
    if (State->Published && !State->StopConfirmed) return STATUS_DEVICE_BUSY;
    /* Table was allocated last. Touch only committed allocation records. */
    NnpiImageDeleteBuffer(&State->Table);
    while (State->AllocatedChunks != 0) {
        --State->AllocatedChunks;
        NnpiImageDeleteBuffer(&State->Chunks[State->AllocatedChunks]);
    }
    if (State->Enabler != NULL) WdfObjectDelete(State->Enabler);
    RtlZeroMemory(State, sizeof(*State));
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImagePrepare(WDFDEVICE Device, NNPI_IMAGE_STATE *State)
{
    WDF_DMA_ENABLER_CONFIG config;
    WDFDMAENABLER enabler = NULL;
    NTSTATUS status;
    ULONG i;
    ULONG alignment;
    if (Device == NULL || State == NULL) return STATUS_INVALID_PARAMETER;
    if (State->Enabler != NULL || State->Table.Handle != NULL ||
        State->AllocatedChunks != 0 || State->WrittenBytes != 0 ||
        State->Prepared || State->Sealed || State->Published ||
        State->TrustedImageVerified || State->StopConfirmed)
        return STATUS_INVALID_DEVICE_STATE;
    /* Check instead of changing a device shared with the existing DMA module. */
    alignment = WdfDeviceGetAlignmentRequirement(Device);
    if ((alignment & NNPI_IMAGE_ALIGNMENT) != NNPI_IMAGE_ALIGNMENT)
        return STATUS_INVALID_DEVICE_STATE;
    WDF_DMA_ENABLER_CONFIG_INIT(&config, WdfDmaProfileScatterGather64,
        NNPI_IMAGE_CHUNK_BYTES);
    status = WdfDmaEnablerCreate(Device, &config,
        WDF_NO_OBJECT_ATTRIBUTES, &enabler);
    if (!NT_SUCCESS(status)) return status;
    State->Enabler = enabler;
    for (i = 0; i < NNPI_IMAGE_CHUNK_COUNT; ++i) {
        status = NnpiImageAllocateBuffer(enabler, &State->Chunks[i],
            NnpiImageChunkLength(i));
        if (!NT_SUCCESS(status)) goto Failed;
        ++State->AllocatedChunks;
    }
    status = NnpiImageAllocateBuffer(enabler, &State->Table, NNPI_IMAGE_TABLE_BYTES);
    if (!NT_SUCCESS(status)) goto Failed;
    State->Prepared = TRUE;
    return STATUS_SUCCESS;
Failed:
    (void)NnpiImageRelease(State); /* No address has been published. */
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImageWriteChunk(NNPI_IMAGE_STATE *State, size_t Offset,
    const VOID *Source, size_t Bytes)
{
    NNPI_IMAGE_BUFFER *chunk;
    size_t index, within;
    if (State == NULL || Source == NULL || Bytes == 0) return STATUS_INVALID_PARAMETER;
    if (!State->Prepared || State->Sealed || State->Published ||
        State->AllocatedChunks != NNPI_IMAGE_CHUNK_COUNT)
        return STATUS_INVALID_DEVICE_STATE;
    if (Offset != State->WrittenBytes || Offset >= NNPI_IMAGE_BYTES)
        return STATUS_INVALID_PARAMETER;
    index = Offset / NNPI_IMAGE_CHUNK_BYTES;
    within = Offset % NNPI_IMAGE_CHUNK_BYTES;
    chunk = &State->Chunks[index];
    if (!NnpiImageBufferValid(chunk, NnpiImageChunkLength((ULONG)index)))
        return STATUS_INVALID_DEVICE_STATE;
    if (within >= chunk->ImageBytes || Bytes > chunk->ImageBytes - within)
        return STATUS_INVALID_BUFFER_SIZE;
    State->TrustedImageVerified = FALSE;
    /* Source must be a captured kernel buffer. SEH is failure containment, not
     * permission to pass arbitrary user addresses or race a freed source. */
    __try {
        RtlCopyMemory((UCHAR *)chunk->VirtualAddress + within, Source, Bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    State->WrittenBytes += Bytes; /* Commit ONLY after the complete copy. */
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImageSeal(NNPI_IMAGE_STATE *State, BOOLEAN TrustedImageVerified)
{
    ULONG i;
    uint64_t total = 0;
    UCHAR message[NNPI_BOOT_MESSAGE_SIZE];
    int rc;
    if (State == NULL) return STATUS_INVALID_PARAMETER;
    if (!State->Prepared || State->Published || State->Sealed ||
        State->AllocatedChunks != NNPI_IMAGE_CHUNK_COUNT ||
        State->WrittenBytes != NNPI_IMAGE_BYTES)
        return STATUS_INVALID_DEVICE_STATE;
    State->TrustedImageVerified = FALSE;
    if (!TrustedImageVerified) return STATUS_ACCESS_DENIED;
    if (!NnpiImageBufferValid(&State->Table, NNPI_IMAGE_TABLE_BYTES))
        return STATUS_INVALID_DEVICE_STATE;
    for (i = 0; i < NNPI_IMAGE_CHUNK_COUNT; ++i) {
        NNPI_IMAGE_BUFFER *chunk = &State->Chunks[i];
        size_t bytes = NnpiImageChunkLength(i);
        if (!NnpiImageBufferValid(chunk, bytes)) return STATUS_INVALID_DEVICE_STATE;
        State->Segments[i].dma_address = (uint64_t)chunk->LogicalAddress.QuadPart;
        State->Segments[i].byte_length = (uint64_t)bytes;
        total += bytes;
    }
    if (total != NNPI_IMAGE_BYTES) return STATUS_INVALID_BUFFER_SIZE;
    rc = nnpi_boot_image(message, sizeof(message),
        (uint64_t)State->Table.LogicalAddress.QuadPart,
        (uint64_t)State->Table.MappedBytes, NNPI_IMAGE_CHUNK_COUNT, total);
    if (rc != NNPI_BOOT_OK) return STATUS_INVALID_ADDRESS;
    /* All payload bytes have been verified. Padding is backed by the same real
     * mappings, zeroed here again, and excluded from the exact image length. */
    for (i = 0; i < NNPI_IMAGE_CHUNK_COUNT; ++i) {
        NNPI_IMAGE_BUFFER *chunk = &State->Chunks[i];
        RtlZeroMemory((UCHAR *)chunk->VirtualAddress + chunk->ImageBytes,
            chunk->MappedBytes - chunk->ImageBytes);
    }
    RtlZeroMemory(State->Table.VirtualAddress, State->Table.MappedBytes);
    rc = nnpi_boot_block((uint8_t *)State->Table.VirtualAddress,
        State->Table.MappedBytes, State->Segments, NNPI_IMAGE_CHUNK_COUNT,
        NNPI_IMAGE_CHUNK_COUNT, 0, 0, 0); /* ONE contiguous table, not linked pages. */
    if (rc != NNPI_BOOT_OK) return STATUS_INVALID_ADDRESS;
    RtlCopyMemory(State->BootMessage, message, sizeof(message));
    State->TrustedImageVerified = TRUE;
    State->Sealed = TRUE;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImageMarkPublished(NNPI_IMAGE_STATE *State)
{
    if (State == NULL) return STATUS_INVALID_PARAMETER;
    if (!State->Prepared || !State->Sealed || !State->TrustedImageVerified ||
        State->Published || State->WrittenBytes != NNPI_IMAGE_BYTES ||
        State->AllocatedChunks != NNPI_IMAGE_CHUNK_COUNT)
        return STATUS_INVALID_DEVICE_STATE;
    /* Conservative ownership transition BEFORE any future MMIO. Integration
     * still owns architecture/device-specific DMA ordering and FIFO barriers. */
    KeMemoryBarrier();
    State->StopConfirmed = FALSE;
    State->Published = TRUE;
    return STATUS_SUCCESS;
}
