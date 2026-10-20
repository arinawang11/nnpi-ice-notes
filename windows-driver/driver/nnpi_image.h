/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_IMAGE_H
#define NNPI_IMAGE_H
#include <ntddk.h>
#include <wdf.h>
#include "../include/nnpi_boot_protocol.h"

#include "../include/nnpi_image_identity.h"
#define NNPI_IMAGE_BYTES ((size_t)NNPI_APPROVED_IMAGE_BYTES)
#define NNPI_IMAGE_CHUNK_BYTES (1024u * 1024u)
#define NNPI_IMAGE_ALIGNMENT (NNPI_BOOT_PAGE_SIZE - 1u)
#define NNPI_IMAGE_CHUNK_COUNT \
    ((NNPI_IMAGE_BYTES + NNPI_IMAGE_CHUNK_BYTES - 1u) / NNPI_IMAGE_CHUNK_BYTES)
#define NNPI_IMAGE_ROUND_PAGE(n) \
    (((n) + NNPI_BOOT_PAGE_SIZE - 1u) & ~(NNPI_BOOT_PAGE_SIZE - 1u))
#define NNPI_IMAGE_TABLE_BYTES \
    (NNPI_BOOT_HEADER_SIZE + NNPI_BOOT_ENTRY_SIZE * NNPI_IMAGE_CHUNK_COUNT)
#define NNPI_IMAGE_TABLE_MAPPED_BYTES NNPI_IMAGE_ROUND_PAGE(NNPI_IMAGE_TABLE_BYTES)

/* Kernel-private ownership, NOT an IOCTL ABI. Never return this state, its
 * addresses, descriptors or BootMessage to user mode. Keep it in zero-initialized
 * nonpaged device context/pool, NOT on the kernel stack. Do not copy live state.
 * All calls and all direct state access require PASSIVE_LEVEL and the SAME
 * caller-owned exclusive lifecycle lock (including hash verification/publish).
 * No concurrent device teardown, source mutation or state mutation is allowed.
 *
 * Integration prerequisites (NOT implemented or wired into driver/vcxproj):
 * - DeviceAdd sets device alignment >= 4095 BEFORE creating DMA enablers.
 *   Prepare checks this; its separate ScatterGather64 enabler inherits alignment.
 * - Before Seal(TRUE), the trusted kernel integration must verify the COMPLETE
 *   staged image against an independently trusted expected SHA256 and reject
 *   capsule/update images. TRUE is a kernel attestation, NEVER a user assertion.
 *   This module implements neither SHA256 nor capsule classification. Hash the
 *   staged Chunks' ImageBytes in order, not padding or a mutable source file.
 * - MarkPublished must succeed BEFORE the first hardware-visible table address,
 *   FIFO write or doorbell, under the same lock. It does no MMIO. Once marked,
 *   ambiguous/partial MMIO failure still counts as publication.
 * - Only integration may set StopConfirmed, from stop/completion evidence for
 *   THIS publication (confirmed DriverReady/CardReady or proven DMA quiesce).
 *   BOOT_STARTED, timeout, error or removal notification alone are insufficient.
 * - Release's guard cannot prevent WDF PARENT Dispose deleting its children.
 *   Integrate a device ReleaseHardware/parent-destruction barrier retaining all
 *   mappings until stop evidence. A WDF reference is NOT such a barrier.
 *
 * Not integrated, not WDK-compiled, not a deliverable driver or proof of DMA.
 */
typedef struct _NNPI_IMAGE_BUFFER {
    WDFCOMMONBUFFER Handle;
    PVOID VirtualAddress;
    PHYSICAL_ADDRESS LogicalAddress; /* WDF DMA logical, never CPU physical. */
    size_t MappedBytes;
    size_t ImageBytes;
} NNPI_IMAGE_BUFFER;

typedef struct _NNPI_IMAGE_STATE {
    WDFDMAENABLER Enabler;
    NNPI_IMAGE_BUFFER Chunks[NNPI_IMAGE_CHUNK_COUNT];
    NNPI_IMAGE_BUFFER Table;
    /* Persistent encoder workspace avoids a segment/page array on the stack. */
    struct nnpi_boot_segment Segments[NNPI_IMAGE_CHUNK_COUNT];
    ULONG AllocatedChunks;
    size_t WrittenBytes;
    UCHAR BootMessage[NNPI_BOOT_MESSAGE_SIZE];
    BOOLEAN Prepared, TrustedImageVerified, Sealed, Published, StopConfirmed;
} NNPI_IMAGE_STATE;

/* Prepare requires initial zero state or successful Release. Failure cleans up
 * exactly the resources allocated by that attempt, in reverse allocation order. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImagePrepare(WDFDEVICE Device, NNPI_IMAGE_STATE *State);
/* Kernel readable, stable, non-overlapping source only (no raw user pointer).
 * Offset must equal WrittenBytes; 0 < Bytes <= remaining bytes of that chunk.
 * Chunk-boundary crossing is intentionally rejected; split the write instead.
 * No DMA address is accepted. On failure WrittenBytes does not advance; a copy
 * exception may dirty ONLY the unwritten range, which must be retried in full. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImageWriteChunk(NNPI_IMAGE_STATE *State, size_t Offset,
    const VOID *Source, size_t Bytes);
/* Requires all exact image bytes written and trusted verification above.
 * Produces a contiguous descriptor table and private 24-byte boot message. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImageSeal(NNPI_IMAGE_STATE *State, BOOLEAN TrustedImageVerified);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImageMarkPublished(NNPI_IMAGE_STATE *State);
/* Returns STATUS_DEVICE_BUSY, leaving everything intact, if published without
 * stop evidence. Successful release is idempotent and returns state to zero. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiImageRelease(NNPI_IMAGE_STATE *State);
#endif
