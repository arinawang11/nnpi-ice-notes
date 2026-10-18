/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_NET_DMA_H
#define NNPI_NET_DMA_H
#include <ntddk.h>
#include <wdf.h>
#include "../include/nnpi_eth_protocol.h"
#include "../include/nnpi_chan_protocol.h"

/* Each direction owns a 64KiB contiguous frame buffer PLUS a 4KiB page-list
 * chain buffer, all children of the same DMA enabler. CHANNEL_RB_OP host_pfn
 * names the chain, never the frame buffer. Its one 16-page entry names data.
 * Allocation order is H2C, H2CChain, C2H, C2HChain; cleanup reverses it. */
#define NNPI_NET_POOL_BYTES ((size_t)NNPI_ETH_PAGE_BYTES * NNPI_ETH_SLOTS)
#define NNPI_NET_CHAIN_BYTES ((size_t)NNPI_ETH_PAGE_BYTES)

/* Kernel-private ownership, NOT an IOCTL ABI. Never return this state or its
 * addresses/PFNs to user mode. Keep it in zero-initialized nonpaged device
 * context, NOT on the kernel stack. All calls and direct state access require
 * PASSIVE_LEVEL and the SAME caller-owned exclusive lifecycle lock.
 *
 * Integration prerequisites (mirror nnpi_image.h):
 * - DeviceAdd sets device alignment >= 4095 BEFORE creating DMA enablers; this
 *   module only CHECKS the inherited alignment, never changes it.
 * - MarkPublished must succeed BEFORE the first hardware-visible PFN, FIFO
 *   write or doorbell, under the same lock. It does no MMIO. Once marked,
 *   ambiguous/partial MMIO failure still counts as publication.
 * - Only integration may set StopConfirmed, from stop/completion evidence for
 *   THIS publication (confirmed channel destroy / proven DMA quiesce). Timeout,
 *   error or removal notification alone are insufficient.
 * - Release's guard cannot prevent WDF PARENT Dispose deleting its children;
 *   integration must retain mappings until stop evidence (a WDF reference is
 *   NOT that barrier).
 */
typedef struct _NNPI_NET_POOL {
    WDFCOMMONBUFFER Handle;
    PVOID VirtualAddress;
    PHYSICAL_ADDRESS LogicalAddress; /* WDF DMA logical, never CPU physical. */
    size_t MappedBytes;
} NNPI_NET_POOL;

typedef struct _NNPI_NET_DMA_STATE {
    WDFDMAENABLER Enabler;
    NNPI_NET_POOL H2C; /* host -> card frames */
    NNPI_NET_POOL H2CChain; /* one terminal page-list block */
    NNPI_NET_POOL C2H; /* card -> host frames */
    NNPI_NET_POOL C2HChain; /* one terminal page-list block */
    BOOLEAN Prepared, Published, StopConfirmed;
} NNPI_NET_DMA_STATE;

/* Prepare requires initial zero state or successful Release. Failure cleans up
 * exactly the resources allocated by that attempt, in reverse order. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiNetDmaPrepare(WDFDEVICE Device, NNPI_NET_DMA_STATE *State);
/* Conservative ownership transition before any MMIO; no DMA address leaves the
 * driver until this returns. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiNetDmaMarkPublished(NNPI_NET_DMA_STATE *State);
/* Returns STATUS_DEVICE_BUSY, leaving everything intact, if published without
 * stop evidence. Successful release is idempotent and returns state to zero. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiNetDmaRelease(NNPI_NET_DMA_STATE *State);

/* host_pfn = CHAIN logical address >> 12, bounded to NNPI_CHAN_PFN_BITS.
 * Returns 0 unless prepared with valid data/chain mappings and matching page
 * list. SlotAddress still names data VA. Never expose either address to users. */
_IRQL_requires_(PASSIVE_LEVEL)
uint64_t NnpiNetDmaHostPfn(const NNPI_NET_DMA_STATE *State, BOOLEAN H2C);
/* Slot i virtual address within the direction pool, or NULL for a bad index or
 * unprepared pool. */
_IRQL_requires_(PASSIVE_LEVEL)
PVOID NnpiNetDmaSlotAddress(const NNPI_NET_DMA_STATE *State, BOOLEAN H2C, ULONG Slot);
#endif
