/* SPDX-License-Identifier: GPL-2.0-only */
#include "nnpi_bulk_dma.h"
static NTSTATUS BulkPool(WDFDMAENABLER enabler,NNPI_NET_POOL *pool,size_t bytes)
{
 NTSTATUS status=WdfCommonBufferCreate(enabler,bytes,WDF_NO_OBJECT_ATTRIBUTES,&pool->Handle);
 if (!NT_SUCCESS(status)) return status;
 pool->VirtualAddress=WdfCommonBufferGetAlignedVirtualAddress(pool->Handle);
 pool->LogicalAddress=WdfCommonBufferGetAlignedLogicalAddress(pool->Handle);
 pool->MappedBytes=WdfCommonBufferGetLength(pool->Handle);
 if (!pool->VirtualAddress || pool->MappedBytes!=bytes ||
  ((ULONG_PTR)pool->VirtualAddress&4095u) || !pool->LogicalAddress.QuadPart ||
  nnpi_boot_dma_range((uint64_t)pool->LogicalAddress.QuadPart,bytes)!=NNPI_BOOT_OK)
  return STATUS_INVALID_ADDRESS;
 RtlZeroMemory(pool->VirtualAddress,bytes); return STATUS_SUCCESS;
}
static NTSTATUS BulkChain(NNPI_NET_POOL *data,NNPI_NET_POOL *chain)
{
 struct nnpi_boot_segment segment;
 segment.dma_address=(uint64_t)data->LogicalAddress.QuadPart;
 segment.byte_length=data->MappedBytes;
 return nnpi_boot_block((uint8_t *)chain->VirtualAddress,chain->MappedBytes,
  &segment,1,1,0,0,1)==NNPI_BOOT_OK?STATUS_SUCCESS:STATUS_INVALID_ADDRESS;
}
NTSTATUS NnpiBulkDmaRelease(NNPI_BULK_DMA *d,const struct nnpi_bulk_state *s)
{
 if (!nnpi_bulk_may_release(s)) return STATUS_DEVICE_BUSY;
 /* Enabler deletion owns all four commonbuffer children. */
 if (d->Enabler) WdfObjectDelete(d->Enabler);
 RtlZeroMemory(d,sizeof(*d)); return STATUS_SUCCESS;
}
NTSTATUS NnpiBulkDmaPrepare(WDFDEVICE device,NNPI_BULK_DMA *d,uint64_t run,uint32_t chunk,uint32_t qd,uint32_t direction)
{
 WDF_DMA_ENABLER_CONFIG config;
 NTSTATUS status; unsigned i; size_t bytes=direction?NNPI_BULK_BYTES:(size_t)chunk*qd; struct nnpi_bulk_state empty={0};
 if (direction>1 || (chunk!=65536 && chunk!=262144 && chunk!=1048576) || (qd!=1 && qd!=4) || d->Enabler || !run || (WdfDeviceGetAlignmentRequirement(device)&4095u)!=4095u)
  return STATUS_INVALID_DEVICE_STATE;
 WDF_DMA_ENABLER_CONFIG_INIT(&config,WdfDmaProfileScatterGather64,bytes);
 status=WdfDmaEnablerCreate(device,&config,WDF_NO_OBJECT_ATTRIBUTES,&d->Enabler);
 if (!NT_SUCCESS(status)) return status;
 status=BulkPool(d->Enabler,&d->Control,4096); if (!NT_SUCCESS(status)) goto failed;
 status=BulkPool(d->Enabler,&d->ControlChain,4096); if (!NT_SUCCESS(status)) goto failed;
 status=BulkPool(d->Enabler,&d->Data,bytes); if (!NT_SUCCESS(status)) goto failed;
 status=BulkPool(d->Enabler,&d->DataChain,4096); if (!NT_SUCCESS(status)) goto failed;
 status=BulkChain(&d->Control,&d->ControlChain); if (!NT_SUCCESS(status)) goto failed;
 status=BulkChain(&d->Data,&d->DataChain); if (!NT_SUCCESS(status)) goto failed;
 if(direction) nnpi_c2h_fill((uint8_t *)d->Data.VirtualAddress,(uint32_t)bytes,run,NNPI_BULK_BINDING_GENERATION,0u,1);
 else for (i=0;i<bytes;++i) ((uint8_t *)d->Data.VirtualAddress)[i]=(uint8_t)(run+1u+17u*(i/chunk)+(i%chunk));
 /* Same x64 coherent WDF commonbuffer contract as NnpiNetDmaMarkPublished.
  * No streaming-map cache operation or user physical address substitution. */
 KeMemoryBarrier(); return STATUS_SUCCESS;
failed:
 (void)NnpiBulkDmaRelease(d,&empty); return status;
}
