/* SPDX-License-Identifier: GPL-2.0-only */
#include "nnpi_hw.h"
#include "../include/nnpi_runtime_protocol.h"
#include <initguid.h> /* Instantiate GUID_BUS_INTERFACE_STANDARD in this TU. */
#include <wdmguid.h>

/* No fallback definitions: unsupported WDK constants must fail compilation. */
#if !defined(CM_RESOURCE_MEMORY_BAR) || !defined(CM_RESOURCE_MEMORY_PREFETCHABLE)
#error Required WDK memory resource flags are unavailable
#else
C_ASSERT(CM_RESOURCE_MEMORY_BAR == 0x80);
C_ASSERT(CM_RESOURCE_MEMORY_PREFETCHABLE == 0x04);
C_ASSERT(CM_RESOURCE_MEMORY_BAR == NNPI_CM_MEMORY_BAR);
C_ASSERT(CM_RESOURCE_MEMORY_PREFETCHABLE == NNPI_CM_MEMORY_PREFETCHABLE);
#endif

/* CPU_STATUS0 only: Linux nnp_pcie.c:605-614 uses ioread32 without ACK.
 * diag1150-command/post_tx.c:217-222 holds each word ~100ms; post_rx.py:150-165
 * and test_1150_with_receiver.py:20-28 double-read ~5ms apart. The recorded
 * 1150-command-root-received-20260913.log has a complete CRC-valid transfer.
 * Engineering evidence for THIS 45c6/subsys45ce8086/rev0, not a universal
 * no-side-effects guarantee. CPU_STATUS1 is deliberately excluded.
 * PCI configuration is still only GetBusData(offset=0, length=64) at Prepare.
 */
_Use_decl_annotations_
VOID NnpiUnmapStatus0(NNPI_STATUS_MAPPING *Mapping)
{
    PVOID old = nnpi_status_detach(&Mapping->Address);
    Mapping->Reason = NNPI_STATUS_NOT_READY;
    if (old != NULL) MmUnmapIoSpace(old, NNPI_STATUS0_BYTES);
}

_Use_decl_annotations_
VOID NnpiMapStatus0(const struct nnpi_diag_response *Diagnostic, NNPI_STATUS_MAPPING *Mapping)
{
    uint64_t address;
    PHYSICAL_ADDRESS physical;
    NnpiUnmapStatus0(Mapping);
    Mapping->Reason = nnpi_status_window(Diagnostic, &address);
    if (Mapping->Reason != NNPI_STATUS_OK) return;
    physical.QuadPart = (LONGLONG)address;
    /* WDK documents READONLY plus optional NOCACHE, available since Windows 10:
     * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmmapiospaceex
     * No execute permission, write-combining or fallback to writable mappings.
     * Request only the four-byte window (the OS maps backing pages internally).
     * Mapping does NOT read the register; only the explicit status IOCTL does.
     */
    Mapping->Address = MmMapIoSpaceEx(physical, NNPI_STATUS0_BYTES, PAGE_READONLY | PAGE_NOCACHE);
    if (Mapping->Address == NULL) Mapping->Reason = NNPI_STATUS_MAP_FAILED;
}

_Use_decl_annotations_
uint32_t NnpiReadStatus0(void *Context)
{
    NNPI_STATUS_MAPPING *mapping = (NNPI_STATUS_MAPPING *)Context;
    /* Sole status0 access site. Caller owns lifecycle lock and validated gate. */
    return (uint32_t)READ_REGISTER_ULONG((volatile ULONG *)mapping->Address);
}
/* Independent map: failure never detaches or changes the old status mapping. */
_Use_decl_annotations_
VOID NnpiUnmapControlSnapshot(NNPI_SNAPSHOT_MAPPING *Mapping)
{
    PVOID old = nnpi_status_detach(&Mapping->Address);
    Mapping->Reason = NNPI_STATUS_NOT_READY;
    if (old != NULL) MmUnmapIoSpace(old, NNPI_SNAPSHOT_WINDOW_BYTES);
}

_Use_decl_annotations_
VOID NnpiMapControlSnapshot(const struct nnpi_diag_response *Diagnostic, NNPI_SNAPSHOT_MAPPING *Mapping)
{
    uint64_t address;
    PHYSICAL_ADDRESS physical;
    NnpiUnmapControlSnapshot(Mapping);
    Mapping->Reason = nnpi_snapshot_window(Diagnostic, &address);
    if (Mapping->Reason != NNPI_STATUS_OK) return;
    physical.QuadPart = (LONGLONG)address;
    /* These windows share backing pages: cache attributes MUST match status0.
     * No writable/executable fallback. Prepare maps but performs no MMIO read. */
    Mapping->Address = MmMapIoSpaceEx(physical, NNPI_SNAPSHOT_WINDOW_BYTES,
                                     PAGE_READONLY | PAGE_NOCACHE);
    if (Mapping->Address == NULL) Mapping->Reason = NNPI_STATUS_MAP_FAILED;
}

_Use_decl_annotations_
uint32_t NnpiReadControlSnapshot(void *Context, uint32_t FixedOffset)
{
    NNPI_SNAPSHOT_MAPPING *mapping = (NNPI_SNAPSHOT_MAPPING *)Context;
    /* Internal policy-only selector, no user offset. Whitelist defensively;
     * only three fixed read32 sites, never FIFO slots/flush/status/ACK. */
    switch (FixedOffset) {
    case NNPI_SNAPSHOT_COMMAND_OFFSET:
        return (uint32_t)READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)mapping->Address + 0x0c));
    case NNPI_SNAPSHOT_RESPONSE_OFFSET:
        return (uint32_t)READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)mapping->Address + 0x28));
    case NNPI_SNAPSHOT_DOORBELL_OFFSET:
        /* DOORBELL_READ may notify the card even though this is a read. */
        return (uint32_t)READ_REGISTER_ULONG((volatile ULONG *)mapping->Address);
    default:
        NT_ASSERT(FALSE);
        return UINT32_MAX;
    }
}

VOID NnpiUnmapHostReady(NNPI_SNAPSHOT_MAPPING *Mapping)
{
    PVOID old=nnpi_status_detach(&Mapping->Address);
    Mapping->Reason=NNPI_STATUS_NOT_READY;
    if (old) MmUnmapIoSpace(old,4);
}
VOID NnpiMapHostReady(const struct nnpi_diag_response *Diagnostic, NNPI_SNAPSHOT_MAPPING *Mapping)
{
    uint64_t address=0;
    PHYSICAL_ADDRESS physical;
    NnpiUnmapHostReady(Mapping);
    /* Reuse exact target/BAR0/range validation. 0x034 lies four bytes before
     * the already validated 0x038..0x063 window within the same BAR. */
    Mapping->Reason=nnpi_snapshot_window(Diagnostic,&address);
    if (Mapping->Reason!=NNPI_STATUS_OK) return;
    physical.QuadPart=(LONGLONG)(address-4);
    Mapping->Address=MmMapIoSpaceEx(physical,4,PAGE_READWRITE | PAGE_NOCACHE);
    if (!Mapping->Address) Mapping->Reason=NNPI_STATUS_MAP_FAILED;
}
uint32_t NnpiReadHostReady(NNPI_SNAPSHOT_MAPPING *Mapping)
{
    return (uint32_t)READ_REGISTER_ULONG((volatile ULONG *)Mapping->Address);
}
VOID NnpiWriteHostReady(NNPI_SNAPSHOT_MAPPING *Mapping)
{
    /* Exact Linux DRV_READY value, no caller-supplied data/offset. */
    WRITE_REGISTER_ULONG((volatile ULONG *)Mapping->Address,9);
}

/* Fixed command push ports only, never host-ready/flush or arbitrary offsets. */
VOID NnpiUnmapSysinfo(NNPI_COMMAND_MAPPING *Mapping)
{
    PVOID old=nnpi_status_detach(&Mapping->Address);
    Mapping->Reason=NNPI_STATUS_NOT_READY;
    if (old) MmUnmapIoSpace(old,16);
}
VOID NnpiMapSysinfo(const struct nnpi_diag_response *Diagnostic, NNPI_COMMAND_MAPPING *Mapping)
{
    uint64_t address=0;
    PHYSICAL_ADDRESS physical;
    NnpiUnmapSysinfo(Mapping);
    Mapping->Reason=nnpi_snapshot_window(Diagnostic,&address);
    if (Mapping->Reason!=NNPI_STATUS_OK) return;
    physical.QuadPart=(LONGLONG)(address+0x18); /* BAR0+0x050..0x05f */
    Mapping->Address=MmMapIoSpaceEx(physical,16,PAGE_READWRITE|PAGE_NOCACHE);
    if (!Mapping->Address) Mapping->Reason=NNPI_STATUS_MAP_FAILED;
}
VOID NnpiWriteSysinfoHalf(NNPI_COMMAND_MAPPING *Mapping, ULONG Half, ULONG Value)
{
    static const ULONG offsets[6]={0,4,0,4,8,12};
    NT_ASSERT(Half<6);
    if (Half<6) NnpiWriteCommandHalf(Mapping,offsets[Half],Value);
}
VOID NnpiWriteCommandHalf(NNPI_COMMAND_MAPPING *Mapping, ULONG RelativeOffset, ULONG Value)
{
    /* Typed mapping plus the fixed-offset whitelist. No MMIO on a stale map. */
    if (!Mapping || !Mapping->Address || Mapping->Reason!=NNPI_STATUS_OK) {
        ExRaiseStatus(STATUS_DEVICE_NOT_READY);
        return;
    }
    switch (RelativeOffset) {
    case 0: case 4: case 8: case 12:
        WRITE_REGISTER_ULONG((volatile ULONG *)((PUCHAR)Mapping->Address+RelativeOffset),Value);
        return;
    default:
        NT_ASSERT(FALSE);
        ExRaiseStatus(STATUS_INVALID_PARAMETER);
        return;
    }
}

static BOOLEAN
NnpiNormalize(PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor,
              struct nnpi_bar_resource *Resource)
{
    RtlZeroMemory(Resource, sizeof(*Resource));
    switch (Descriptor->Type) {
    case CmResourceTypeMemory:
        /* Shared, tested whitelist: BAR metadata and optional prefetch only. */
        if (!nnpi_bar_normalize_memory_flags(Descriptor->Flags, &Resource->flags))
            return FALSE;
        Resource->type = NNPI_BAR_MEMORY;
        Resource->start = (uint64_t)Descriptor->u.Memory.Start.QuadPart;
        Resource->length = Descriptor->u.Memory.Length;
        return TRUE;
    case CmResourceTypePort:
        if (!(Descriptor->Flags & CM_RESOURCE_PORT_IO)) return FALSE;
        Resource->type = NNPI_BAR_PORT;
        Resource->start = (uint64_t)Descriptor->u.Port.Start.QuadPart;
        Resource->length = Descriptor->u.Port.Length;
        return TRUE;
    case CmResourceTypeInterrupt:
    case CmResourceTypeDevicePrivate:
        Resource->type = NNPI_BAR_OPAQUE; /* Never read these unions. */
        return TRUE;
    default:
        return FALSE; /* Includes MemoryLarge: no guessed union interpretation. */
    }
}

_Use_decl_annotations_
VOID NnpiCollectDiagnostic(WDFDEVICE Device, WDFCMRESLIST Raw,
                          WDFCMRESLIST Translated, struct nnpi_diag_response *Result)
{
    BUS_INTERFACE_STANDARD bus;
    struct nnpi_bar_resource raw[NNPI_BAR_MAX_RESOURCES];
    struct nnpi_bar_resource translated[NNPI_BAR_MAX_RESOURCES];
    NTSTATUS status;
    ULONG i, bytes;
    RtlZeroMemory(Result, sizeof(*Result));
    Result->version = NNPI_DIAG_VERSION;
    Result->size = (uint32_t)sizeof(*Result);
    Result->reason = NNPI_BAR_CONFIG_UNAVAILABLE;
    Result->raw_count = WdfCmResourceListGetCount(Raw);
    Result->translated_count = WdfCmResourceListGetCount(Translated);
    RtlZeroMemory(&bus, sizeof(bus));
    /* The queried interface is referenced by the provider. Dereference once on
     * every successful query; never retain it beyond this synchronous call.
     * No SetBusData, IRP_MN_WRITE_CONFIG, HAL access, or config-space mapping.
     */
    status = WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&bus, (USHORT)sizeof(bus), 1, NULL);
    Result->pci_status = (uint32_t)status;
    if (!NT_SUCCESS(status)) return;
    if (bus.Size < sizeof(bus) || bus.Version != 1 || !bus.GetBusData ||
        !bus.InterfaceDereference) {
        if (bus.InterfaceDereference) bus.InterfaceDereference(bus.Context);
        Result->pci_status = (uint32_t)STATUS_NOT_SUPPORTED;
        return;
    }
    bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, Result->config, 0, 64);
    bus.InterfaceDereference(bus.Context);
    if (bytes > 64) {
        RtlZeroMemory(Result->config, sizeof(Result->config));
        Result->pci_status = (uint32_t)STATUS_DEVICE_DATA_ERROR;
        return;
    }
    Result->config_bytes = bytes;
    if (bytes != 64) {
        RtlZeroMemory(Result->config + bytes, sizeof(Result->config) - bytes);
        Result->pci_status = (uint32_t)STATUS_DEVICE_DATA_ERROR;
        return;
    }
    Result->pci_status = 0;
    if (nnpi_bar_le32(Result->config) != 0x45c68086U) {
        Result->reason = NNPI_BAR_IDENTITY; return;
    }
    if ((Result->config[14] & 0x7fU) != 0) {
        Result->reason = NNPI_BAR_HEADER; return;
    }
    Result->reason = NNPI_BAR_RESOURCE_PAIR;
    if (Result->raw_count != Result->translated_count ||
        Result->raw_count > NNPI_BAR_MAX_RESOURCES) return;
    for (i = 0; i < Result->raw_count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Raw, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR t = WdfCmResourceListGetDescriptor(Translated, i);
        /* Windows raw/translated descriptors correspond by position, but that
         * position never identifies a BAR. Require a conservative same-type
         * pair; BAR matching subsequently uses only the raw PCI bus address.
         * https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/raw-and-translated-resources
         */
        if (!r || !t || r->Type != t->Type ||
            !NnpiNormalize(r, &raw[i]) || !NnpiNormalize(t, &translated[i])) return;
    }
    Result->reason = nnpi_bar_match(Result->config, sizeof(Result->config), raw,
        translated, Result->raw_count, Result->bars, &Result->bar_count);
}

/* Fixed runtime window 0x060..0x17f; BAR length separately verified. */
VOID NnpiMapRuntime(const struct nnpi_diag_response *Diagnostic, NNPI_SNAPSHOT_MAPPING *Mapping)
{
    uint64_t address=0; PHYSICAL_ADDRESS physical; ULONG i;
    NnpiUnmapRuntime(Mapping);
    Mapping->Reason=nnpi_snapshot_window(Diagnostic,&address);
    if (Mapping->Reason!=NNPI_STATUS_OK) return;
    for (i=0;i<Diagnostic->bar_count;++i) {
        if (Diagnostic->bars[i].bar==0 && Diagnostic->bars[i].length>=0x180) break;
    }
    if (i==Diagnostic->bar_count) { Mapping->Reason=NNPI_STATUS_NOT_READY; return; }
    physical.QuadPart=(LONGLONG)(address+0x28);
    Mapping->Address=MmMapIoSpaceEx(physical,0x120,PAGE_READWRITE|PAGE_NOCACHE);
    if (!Mapping->Address) Mapping->Reason=NNPI_STATUS_MAP_FAILED;
}
VOID NnpiUnmapRuntime(NNPI_SNAPSHOT_MAPPING *Mapping)
{
    PVOID old=nnpi_status_detach(&Mapping->Address);
    Mapping->Reason=NNPI_STATUS_NOT_READY;
    if (old) MmUnmapIoSpace(old,0x120);
}
NTSTATUS NnpiReadRuntime(NNPI_SNAPSHOT_MAPPING *Mapping,uint64_t Words[16],ULONG *Count,
    NNPI_RUNTIME_OBSERVATION *Observation)
{
    ULONG control,index,i,low,high;
    struct nnpi_fifo_snapshot fifo;
    volatile ULONG *base=(volatile ULONG *)Mapping->Address;
    *Count=0;
    if (Observation) RtlZeroMemory(Observation,sizeof(*Observation));
    control=READ_REGISTER_ULONG(base);
    if (Observation) { Observation->Control=control; Observation->ControlValid=1; }
    if (control==MAXULONG || nnpi_fifo_decode(control,&fifo)!=NNPI_FIFO_OK)
        return STATUS_DEVICE_DATA_ERROR;
    if (Observation) Observation->Count=fifo.used_qwords;
    if (!fifo.used_qwords) return STATUS_SUCCESS;
    index=fifo.read_pointer;
    for (i=0;i<fifo.used_qwords;++i) {
        index=(index+1)%16;
        /* ULONG indexing: (0x100-0x060)/4 == 0x28. */
        low=READ_REGISTER_ULONG(base+0x28+index*2);
        if (Observation) ++Observation->ReadHalves;
        high=READ_REGISTER_ULONG(base+0x29+index*2);
        if (Observation) ++Observation->ReadHalves;
        Words[i]=(uint64_t)low|((uint64_t)high<<32);
        if (Words[i]==UINT64_MAX) return STATUS_DEVICE_DATA_ERROR;
    }
    if (fifo.used_qwords==16) {
        WRITE_REGISTER_ULONG(base,(control&~0xfU)|((index+1)%16));
        if (Observation) ++Observation->AckWrites;
    }
    WRITE_REGISTER_ULONG(base,(control&~0xfU)|index);
    if (Observation) ++Observation->AckWrites;
    *Count=fifo.used_qwords; return STATUS_SUCCESS;
}
static int NnpiQueryWrite32(void *Context,uint32_t RelativeOffset,uint32_t Value)
{
    /* Exceptions unwind to the worker; the shared state already claimed the
     * attempted half, and never permits replay even if a write reached hardware. */
    NnpiWriteCommandHalf((NNPI_COMMAND_MAPPING *)Context,RelativeOffset,Value);
    return 1;
}
NTSTATUS NnpiQueryVersion(NNPI_COMMAND_MAPPING *Mapping,struct nnpi_runtime_query_state *State)
{
    int result=nnpi_runtime_query_send(State,NNPI_RUNTIME_COMMAND_BASE,
        Mapping && Mapping->Address && Mapping->Reason==NNPI_STATUS_OK,
        Mapping,NnpiQueryWrite32);
    if (result==NNPI_RUNTIME_QUERY_OK) return STATUS_SUCCESS;
    if (result==NNPI_RUNTIME_QUERY_USED) return STATUS_INVALID_DEVICE_STATE;
    return STATUS_DEVICE_NOT_READY;
}

NTSTATUS NnpiRuntimeHostReady(NNPI_SNAPSHOT_MAPPING *Mapping)
{
    uint32_t value=NnpiReadHostReady(Mapping),out;
    if (value==MAXULONG || nnpi_runtime_host_ready_value(value,&out)!=NNPI_RUNTIME_MESSAGE)
        return STATUS_DEVICE_DATA_ERROR;
    WRITE_REGISTER_ULONG((volatile ULONG *)Mapping->Address,out);
    return STATUS_SUCCESS;
}
NTSTATUS NnpiRuntimeKeepalive(NNPI_SNAPSHOT_MAPPING *Mapping,ULONG *Counter)
{
    uint32_t value=NnpiReadHostReady(Mapping),next,out;
    if (value==MAXULONG || nnpi_runtime_keepalive_value(value,*Counter,&next,&out)!=NNPI_RUNTIME_MESSAGE)
        return STATUS_DEVICE_DATA_ERROR;
    *Counter=next; /* claim before possibly partial MMIO, never retry */
    WRITE_REGISTER_ULONG((volatile ULONG *)Mapping->Address,out);
    return STATUS_SUCCESS;
}
