/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_BULK_DMA_H
#define NNPI_BULK_DMA_H
#include "nnpi_net_dma.h"
#include "../include/nnpi_bulk.h"
typedef struct _NNPI_BULK_DMA {
 WDFDMAENABLER Enabler;
 NNPI_NET_POOL Control, ControlChain, Data, DataChain;
} NNPI_BULK_DMA;
/* All operations under CacheLock at PASSIVE; release requires both HOSTRES
 * unmap acknowledgements and dedicated channel destruction, not STOP alone.
 * Integration blocks parent disposal on any unresolved publication. */
NTSTATUS NnpiBulkDmaPrepare(WDFDEVICE Device,NNPI_BULK_DMA *Dma,uint64_t Run,uint32_t Chunk,uint32_t Qd,uint32_t Direction);
NTSTATUS NnpiBulkDmaRelease(NNPI_BULK_DMA *Dma,const struct nnpi_bulk_state *State);
#endif
