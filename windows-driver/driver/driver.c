/* SPDX-License-Identifier: GPL-2.0-only */
/* Windows 11 x64 KMDF: INFO v1 plus cached PCI configuration diagnostics.
 * Prepare caches PCI config and maps status/control windows without reading;
 * only explicit status/snapshot requests read MMIO. Binding/power transitions can still make the PCI bus
 * driver act on hardware. The INF MUST restrict access to SYSTEM/admins.
 * Build and lifecycle/Verifier validation require a real Windows WDK host.
 */
#include <ntddk.h>
#include <ntiologc.h>
#include <wdf.h>
#include <wdmguid.h> /* Declarations; system GUIDs instantiated in nnpi_hw.c. */
#include <initguid.h>
#include <devpkey.h>
#include <bcrypt.h>
#include "nnpi_image.h"
#include "../include/nnpi_boot_abi.h"
#include "../include/nnpi_boot_session.h"
#include "../include/nnpi_runtime_protocol.h"
#include "../include/nnpi_runtime_dispatch.h"
#include "../include/nnpi_runtime_info_abi.h"
#include "../include/nnpi_info_abi.h"
#include "../include/nnpi_interface.h"
#include "nnpi_hw.h"
#include "nnpi_dma.h"
#include "nnpi_reset.h"
#include "../include/nnpi_resetcaps_policy.h"
#include "../include/nnpi_hostready.h"
#include "../include/nnpi_sysinfo_policy.h"
#include "../include/nnpi_net_abi.h"
#include "nnpi_net_dma.h"
#include "nnpi_irq.h"
#include "../include/nnpi_pnp_policy.h"
#include "../include/nnpi_net_control.h"
#include "../include/nnpi_net_data.h"
C_ASSERT(NNPI_NET_DATA_QUERY_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x815,METHOD_BUFFERED,FILE_READ_ACCESS));
C_ASSERT(NNPI_NET_TX_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x816,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS));
C_ASSERT(NNPI_NET_RX_CLAIM_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x817,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS));
C_ASSERT(NNPI_NET_RX_RELEASE_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x818,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS));
C_ASSERT(NNPI_NET_RX_BATCH_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x819,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS));
C_ASSERT(NNPI_NET_GATE_READY == NNPI_RUNTIME_NET_READY);
C_ASSERT(NNPI_NET_GATE_NOT_READY == NNPI_RUNTIME_NET_NOT_READY);
C_ASSERT(NNPI_NET_GATE_CAP_MISSING == NNPI_RUNTIME_NET_CAP_MISSING);
C_ASSERT(NNPI_NET_GATE_SESSION_USED == NNPI_RUNTIME_NET_SESSION_USED);
C_ASSERT(NNPI_NET_GATE_CLOSING == NNPI_RUNTIME_NET_CLOSING);
C_ASSERT(NNPI_NET_GATE_UNEXPECTED_CONFIG == NNPI_RUNTIME_NET_UNEXPECTED_CONFIG);
C_ASSERT(NNPI_RUNTIME_INFO_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x814,METHOD_BUFFERED,FILE_READ_ACCESS));
C_ASSERT(NNPI_NET_BEGIN_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x812,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS));
C_ASSERT(NNPI_NET_QUERY_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x813,METHOD_BUFFERED,FILE_READ_ACCESS));
C_ASSERT(NNPI_SYSINFO_SUBMIT_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x80b,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS));
C_ASSERT(NNPI_SYSINFO_QUERY_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE,0x80c,METHOD_BUFFERED,FILE_READ_ACCESS));
C_ASSERT(NNPI_HOSTREADY_SUBMIT_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x809, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS));
C_ASSERT(NNPI_HOSTREADY_OBSERVE_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x80a, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS));
/* Shared action exclusion across every device and PnP generation in this load.
 * 0 unused, 1 host-ready attempted, 2 reset attempted. Never reset in DeviceAdd. */
static volatile LONG NnpiActionOwner;
static EX_RUNDOWN_REF NnpiSysinfoRundown;
static volatile LONG NnpiBootOwner;

C_ASSERT(NNPI_RESET_SUBMIT_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS));
C_ASSERT(NNPI_RESET_QUERY_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS));
C_ASSERT(NNPI_RESET_REVALIDATE_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x808, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS));
C_ASSERT(NNPI_RESETCAPS_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS));
C_ASSERT(DEVICE_RESET_INTERFACE_VERSION == 1);
C_ASSERT(NNPI_RESETCAPS_FUNCTION_LEVEL == (1UL << FunctionLevelDeviceReset));
C_ASSERT(NNPI_RESETCAPS_PLATFORM_LEVEL == (1UL << PlatformLevelDeviceReset));
/* WDK-defined original v1 prefix, including Reserved; never sizeof the newer
 * structure with QueryBusSpecificResetInfo/DeviceBusSpecificReset/status tail. */
#define NNPI_RESET_INTERFACE_SIZE RTL_SIZEOF_THROUGH_FIELD(DEVICE_RESET_INTERFACE_STANDARD, Reserved)
C_ASSERT(NNPI_RESET_INTERFACE_SIZE <= MAXUSHORT);

C_ASSERT(NNPI_DMACHECK_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS));

C_ASSERT(NNPI_SNAPSHOT_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS));

C_ASSERT(NNPI_STATUS_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS));

C_ASSERT(NNPI_DIAG_IOCTL == CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS));

C_ASSERT(NNPI_INFO_IOCTL ==
         CTL_CODE(NNPI_INFO_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS));

/* Internal-only retained evidence, deliberately absent from public ABI/log
 * schemas. Capture at the existing observation sites; never probe after failure. */
enum NNPI_RUNTIME_STEP {
    NnpiRuntimeIdle=0, NnpiRuntimeCommandGate, NnpiRuntimeQuery,
    NnpiRuntimeCardGate, NnpiRuntimeResponse, NnpiRuntimeParse,
    NnpiRuntimeCompatibility, NnpiRuntimeHostReadyStep,
    NnpiRuntimeKeepaliveStep, NnpiRuntimeWait
};
typedef struct _NNPI_BOOT_DIAGNOSTIC {
    ULONG FirstFailurePhase, FirstFailureReason, FirstFailureStatus;
    ULONG FirstFailureRuntimeStep, RuntimeStep;
    struct nnpi_runtime_query_state RuntimeQuery;
    ULONG RuntimeCommandControl, RuntimeCommandControlValid;
    ULONG RuntimeCard, RuntimeCardValid;
    NNPI_RUNTIME_OBSERVATION RuntimeResponse;
    ULONG RuntimeParserResult, RuntimeParserResultValid;
    ULONG RuntimeDecodedOpcode, RuntimeDecodedOpcodeValid;
    ULONG ResetEntered, ResetReturned, ResetStatus;
} NNPI_BOOT_DIAGNOSTIC;

typedef struct _NNPI_DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFWAITLOCK CacheLock;
    struct nnpi_info_response Cache;
    struct nnpi_diag_response Diagnostic;
    NNPI_STATUS_MAPPING StatusMapping;
    NNPI_SNAPSHOT_MAPPING SnapshotMapping;
    NNPI_DMA_STATE Dma;
    NNPI_SNAPSHOT_MAPPING HostReadyMapping;
    struct nnpi_hostready_response HostReady;
    ULONGLONG HostReadyLastRead;
    BOOLEAN HostReadyInvalid;
    struct nnpi_resetcaps_response ResetCaps;
    DEVICE_RESET_INTERFACE_STANDARD ResetInterface;
    ULONGLONG DeviceId, Epoch;
    WDFQUEUE ResetQueue, LegacyQueue;
    BOOLEAN ResetBlocked;
    BOOLEAN Removed;
    NNPI_COMMAND_MAPPING SysinfoMapping;
    struct nnpi_sysinfo_response Sysinfo;
    PIO_WORKITEM SysinfoWork;
    KEVENT SysinfoWorkerDone, SysinfoStopped;
    BOOLEAN SysinfoClosing, SysinfoStarted;
    NNPI_IMAGE_STATE Image;
    struct nnpi_boot_response Boot;
    struct nnpi_boot_session BootSession;
    PIO_WORKITEM BootWork;
    KEVENT BootDone, BootClose;
    BOOLEAN BootClosing, BootStarted;
    DEVICE_RESET_INTERFACE_STANDARD BootResetInterface;
    NNPI_SNAPSHOT_MAPPING RuntimeMapping;
    struct nnpi_runtime_parser RuntimeParser;
    struct nnpi_runtime_message RuntimeVersion, RuntimeLastMessage;
    uint64_t RuntimeWords[16];
    ULONG RuntimeKeepalive;
    BOOLEAN RuntimeCompatible, RuntimeHealthy;
    BOOLEAN RuntimeEntered, RuntimeFaultRetained; /* Sticky; never DMA-stop evidence. */
    struct nnpi_net_data NetData;
    WDFFILEOBJECT NetConsumer;
    struct nnpi_net_session_key NetConsumerKey;
    uint64_t RuntimeKeepaliveUpdates, UnexpectedNetConfigCount;
    uint64_t LastUnexpectedNetConfig, RuntimeLastEvent;
    NNPI_BOOT_DIAGNOSTIC BootDiagnostic;
    NNPI_IRQ_MAPPING IrqMapping;
    ULONGLONG IrqSerial;
    NNPI_NET_DMA_STATE NetDma;
    struct nnpi_net_response Net;
    struct nnpi_net_session NetSession;
    struct nnpi_net_owner NetOwner;
    PIO_WORKITEM NetWork;
    KEVENT NetDone, NetClose;
    BOOLEAN NetClosing, NetStarted;
    ULONG PnPQueryVetoReasons, PnPQueryVetoCount; /* Internal CPU-only diagnostics. */


} NNPI_DEVICE_CONTEXT, *PNNPI_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NNPI_DEVICE_CONTEXT, NnpiGetContext);
typedef struct _NNPI_FILE_CONTEXT { BOOLEAN Cleaned; } NNPI_FILE_CONTEXT, *PNNPI_FILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NNPI_FILE_CONTEXT, NnpiGetFileContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD NnpiDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE NnpiPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE NnpiReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY NnpiD0Entry;
EVT_WDF_DEVICE_D0_EXIT NnpiD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL NnpiSurpriseRemoval;
EVT_WDF_DEVICE_FILE_CREATE NnpiFileCreate;
EVT_WDF_FILE_CLEANUP NnpiFileCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL NnpiIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP NnpiIoStop;
EVT_WDF_DRIVER_UNLOAD NnpiDriverUnload;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL NnpiRouteRequest;
static volatile LONG64 NnpiDeviceSequence;

static VOID NnpiBootBarrier(PNNPI_DEVICE_CONTEXT Context);
static VOID NnpiBootWorker(PDEVICE_OBJECT DeviceObject,PVOID Parameter);
static VOID NnpiBootRuntime(PNNPI_DEVICE_CONTEXT Context);
static VOID NnpiBootRequest(PNNPI_DEVICE_CONTEXT Context,WDFREQUEST Request,
    size_t OutputBytes,size_t InputBytes,ULONG Code);
static VOID NnpiNetWorker(PDEVICE_OBJECT DeviceObject,PVOID Parameter);
static VOID NnpiNetRequest(PNNPI_DEVICE_CONTEXT Context,WDFREQUEST Request,
    size_t OutputBytes,size_t InputBytes,ULONG Code);
static VOID NnpiNetBarrier(PNNPI_DEVICE_CONTEXT Context);
static VOID NnpiNetDataRequest(PNNPI_DEVICE_CONTEXT c,WDFREQUEST Request,
    size_t OutputBytes,size_t InputBytes,ULONG Code);
static VOID NnpiNetFinalize(PNNPI_DEVICE_CONTEXT Context);
static ULONG NnpiNetGate(PNNPI_DEVICE_CONTEXT c,uint64_t generation,uint64_t epoch,
    ULONG channel,ULONG ip,const uint8_t mac[6],BOOLEAN continuing);
static VOID NnpiSysinfoWorker(PDEVICE_OBJECT DeviceObject,PVOID Parameter);
static VOID NnpiSysinfoRefresh(PNNPI_DEVICE_CONTEXT Context);
EVT_WDF_DEVICE_QUERY_STOP NnpiQueryStop;
EVT_WDF_DEVICE_QUERY_REMOVE NnpiQueryRemove;
static NTSTATUS NnpiSysinfoQueryPnp(WDFDEVICE Device)
{
    PNNPI_DEVICE_CONTEXT context=NnpiGetContext(Device);
    struct nnpi_pnp_snapshot snapshot;
    ULONG reasons;
    WdfWaitLockAcquire(context->CacheLock,NULL);
    /* Read-only snapshot: queued/tail workers and actual DMA ownership must
     * agree with session release evidence before any close notification. */
    snapshot.boot_started=context->BootStarted;
    snapshot.boot_active=context->Boot.worker_active;
    snapshot.boot_done=KeReadStateEvent(&context->BootDone)!=0;
    snapshot.sysinfo_started=context->SysinfoStarted;
    snapshot.sysinfo_done=KeReadStateEvent(&context->SysinfoWorkerDone)!=0;
    snapshot.net_started=context->NetStarted;
    snapshot.net_active=context->Net.worker_active;
    snapshot.net_done=KeReadStateEvent(&context->NetDone)!=0;
    snapshot.boot_owned=context->BootSession.published_dma!=0;
    snapshot.boot_may_release=nnpi_boot_session_may_release(&context->BootSession,NNPI_BS_ALL_DMA);
    snapshot.sysinfo_owned=context->Sysinfo.ownership!=NNPI_SYSINFO_UNPUBLISHED;
    snapshot.sysinfo_may_release=nnpi_sysinfo_may_release(&context->Sysinfo);
    snapshot.net_owned=context->NetSession.published_dma!=0 || context->NetOwner.channel_issued;
    snapshot.net_may_release=nnpi_net_session_may_release(&context->NetSession,NNPI_NS_ALL_DMA) &&
        nnpi_net_owner_may_release(&context->NetOwner);
    snapshot.dma_published=context->Dma.Published;
    snapshot.dma_stop_confirmed=context->Dma.StopConfirmed;
    snapshot.image_published=context->Image.Published;
    snapshot.image_stop_confirmed=context->Image.StopConfirmed;
    snapshot.netdma_published=context->NetDma.Published;
    snapshot.netdma_stop_confirmed=context->NetDma.StopConfirmed;
    reasons=nnpi_pnp_query_veto_reasons(&snapshot);
    if (reasons) {
        context->PnPQueryVetoReasons=reasons;
        if (context->PnPQueryVetoCount!=MAXULONG) ++context->PnPQueryVetoCount;
        WdfWaitLockRelease(context->CacheLock);
        return STATUS_DEVICE_BUSY;
    }
    /* Only an accepted query closes admission. Deliberately one-way on cancel:
     * cancellation does not grant rearm, fresh budgets or publish authority. */
    context->RuntimeHealthy=FALSE; context->BootClosing=TRUE; context->Boot.closing=1;
    KeSetEvent(&context->BootClose,IO_NO_INCREMENT,FALSE);
    context->SysinfoClosing=TRUE;
    context->NetClosing=TRUE; context->Net.closing=1;
    KeSetEvent(&context->NetClose,IO_NO_INCREMENT,FALSE);
    WdfWaitLockRelease(context->CacheLock);
    return STATUS_SUCCESS;
}
NTSTATUS NnpiQueryStop(WDFDEVICE Device) { return NnpiSysinfoQueryPnp(Device); }
NTSTATUS NnpiQueryRemove(WDFDEVICE Device) { return NnpiSysinfoQueryPnp(Device); }
/* CacheLock must NOT be held. ReleaseHardware is the pre-dispose barrier,
 * not an object reference: Prepare/Release are paired even on prepare failure.
 * Surprise only closes admission; it must not call NnpiDmaRelease. Successful
 * reset owns old-publication stop evidence independently of current epoch.
 * No callback/failure => deliberate fail-stop (PnP watchdog remains possible).
 * Sources: WDK EvtDeviceReleaseHardware; WDF A User Unplugs a Device. */
static VOID NnpiSysinfoRefresh(PNNPI_DEVICE_CONTEXT Context)
{
    struct nnpi_reset_response reset;
    if (!Context->Sysinfo.budget_consumed) return;
    NnpiResetQuery(Context->DeviceId,Context->Cache.generation,Context->Epoch,&reset);
    Context->Sysinfo.reset=reset;
    if (nnpi_sysinfo_stopped(&Context->Sysinfo,1,&reset)) {
        Context->Dma.StopConfirmed=TRUE;
        NnpiResetInvalidate(Context->DeviceId);
        NnpiResetQuery(Context->DeviceId,Context->Cache.generation,Context->Epoch,&Context->Sysinfo.reset);
        KeSetEvent(&Context->SysinfoStopped,IO_NO_INCREMENT,FALSE);
    }
    else if (Context->Sysinfo.phase==NNPI_SYSINFO_CLEANUP && reset.state==NNPI_RESET_FAILED)
        Context->Sysinfo.phase=NNPI_SYSINFO_STOP_UNCONFIRMED;
}
static VOID NnpiSysinfoBarrier(PNNPI_DEVICE_CONTEXT Context)
{
    LARGE_INTEGER delay;
    BOOLEAN started, released;
    WdfWaitLockAcquire(Context->CacheLock,NULL);
    Context->SysinfoClosing=TRUE;
    started=Context->SysinfoStarted;
    WdfWaitLockRelease(Context->CacheLock);
    if (!started) return;
    KeWaitForSingleObject(&Context->SysinfoWorkerDone,Executive,KernelMode,FALSE,NULL);
    delay.QuadPart=-5000000;
    for (;;) {
        WdfWaitLockAcquire(Context->CacheLock,NULL);
        NnpiSysinfoRefresh(Context);
        released=(BOOLEAN)nnpi_sysinfo_may_release(&Context->Sysinfo);
        WdfWaitLockRelease(Context->CacheLock);
        if (released) return;
        KeDelayExecutionThread(KernelMode,FALSE,&delay);
    }
}

/* Task006: fixed, best-effort startup/lifecycle records; no strings or addresses.
 * IO_ERR_DRIVER_ERROR is deliberately a diagnostic marker, even when FinalStatus
 * is STATUS_SUCCESS (not a real hardware error). Message rendering may fail;
 * consume raw DumpData only. Missing logs do NOT prove a path did not execute.
 * LoadTag is only the low 32 bits of interrupt time, NOT globally unique.
 */
#define NNPI_LOG_PACKET_SIZE (sizeof(IO_ERROR_LOG_PACKET) + 7 * sizeof(ULONG))
C_ASSERT(NNPI_LOG_PACKET_SIZE <= ERROR_LOG_MAXIMUM_SIZE);
C_ASSERT(NNPI_LOG_PACKET_SIZE <= 255);

static PDRIVER_OBJECT NnpiLogDriverObject;
static ULONG NnpiLogLoadTag;
static volatile LONG NnpiLogSequence;

_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
NnpiLogStage(_In_ ULONG Stage, _In_ NTSTATUS Status, _In_ ULONG Detail)
{
    PIO_ERROR_LOG_PACKET packet;
    ULONG data[8];
    ULONG sequence = (ULONG)InterlockedIncrement(&NnpiLogSequence);

    if (NnpiLogDriverObject == NULL) {
        return;
    }
    packet = (PIO_ERROR_LOG_PACKET)IoAllocateErrorLogEntry(
        NnpiLogDriverObject, (UCHAR)NNPI_LOG_PACKET_SIZE);
    if (packet == NULL) {
        return;
    }
    RtlZeroMemory(packet, NNPI_LOG_PACKET_SIZE);
    packet->ErrorCode = IO_ERR_DRIVER_ERROR;
    packet->FinalStatus = Status;
    packet->UniqueErrorValue = Stage;
    packet->DumpDataSize = (USHORT)sizeof(data);
    data[0] = 0x4e4e5049;
    data[1] = 1; /* schema */
    data[2] = 26; /* TASK026 independent restricted volatile boot */
    data[3] = NnpiLogLoadTag;
    data[4] = sequence;
    data[5] = Stage;
    data[6] = (ULONG)Status;
    data[7] = Detail;
    RtlCopyMemory(packet->DumpData, data, sizeof(data));
    IoWriteErrorLogEntry(packet);
}

/* Compare a bounded prefix as Unicode, not a substring or ANSI conversion.
 * Accept PCI\\VEN_8086&DEV_45C6 and its &-delimited refinements, never 45C60.
 * Validate the WHOLE MULTI_SZ, including its final empty string, before success.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
NnpiCheckHardwareIds(_In_ PWDFDEVICE_INIT DeviceInit)
{
    UNICODE_STRING target = RTL_CONSTANT_STRING(L"PCI\\VEN_8086&DEV_45C6");
    WDFMEMORY memory;
    NTSTATUS status;
    const WCHAR *ids;
    size_t bytes, count, pos, end, i;
    const size_t prefixChars = target.Length / sizeof(WCHAR);
    BOOLEAN matched = FALSE;

    status = WdfFdoInitAllocAndQueryProperty(DeviceInit, DevicePropertyHardwareID,
                                           NonPagedPoolNx,
                                           WDF_NO_OBJECT_ATTRIBUTES, &memory);
    NnpiLogStage(11, status, 0);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ids = (const WCHAR *)WdfMemoryGetBuffer(memory, &bytes);
    status = STATUS_DEVICE_DATA_ERROR;
    if (bytes < 2 * sizeof(WCHAR) || bytes % sizeof(WCHAR) != 0) {
        goto Exit;
    }
    count = bytes / sizeof(WCHAR);
    if (ids[count - 1] != L'\0' || ids[count - 2] != L'\0') {
        goto Exit;
    }
    pos = 0;
    while (pos < count && ids[pos] != L'\0') {
        UNICODE_STRING prefix;
        end = pos;
        while (end < count && ids[end] != L'\0') {
            ++end;
        }
        if (end == count) {
            goto Exit;
        }
        if (end - pos >= prefixChars &&
            (end - pos == prefixChars || ids[pos + prefixChars] == L'&')) {
            prefix.Buffer = (PWCH)(ids + pos);
            prefix.Length = target.Length;
            prefix.MaximumLength = target.Length;
            if (RtlEqualUnicodeString(&prefix, &target, TRUE)) {
                matched = TRUE;
            }
        }
        pos = end + 1;
    }
    if (pos == count) {
        goto Exit;
    }
    for (i = pos; i < count; ++i) {
        if (ids[i] != L'\0') {
            goto Exit;
        }
    }
    status = matched ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
Exit:
    WdfObjectDelete(memory);
    NnpiLogStage(12, status, 0);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
NnpiInitSnapshot(_Out_ struct nnpi_info_response *Snapshot)
{
    RtlZeroMemory(Snapshot, sizeof(*Snapshot));
    Snapshot->version = NNPI_INFO_VERSION;
    Snapshot->size = (uint32_t)sizeof(*Snapshot);
    Snapshot->driver_version = 1;
    /* DeviceAdd has already verified the PDO's HardwareID property. */
    Snapshot->vendor = 0x8086;
    Snapshot->device = 0x45c6;
    Snapshot->identity_source = NNPI_INFO_ID_PNP_HARDWARE_IDS;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
NnpiInvalidate(_In_ PNNPI_DEVICE_CONTEXT Context, _In_ BOOLEAN Removed)
{
    NnpiBootBarrier(Context);
    NnpiSysinfoBarrier(Context);
    NnpiNetBarrier(Context);
    WdfWaitLockAcquire(Context->CacheLock, NULL);
    Context->Removed = (BOOLEAN)(Context->Removed || Removed);
    if (Context->Epoch != UINT64_MAX) ++Context->Epoch;
    NnpiResetInvalidate(Context->DeviceId);
    if (Context->ResetInterface.InterfaceDereference) {
        Context->ResetInterface.InterfaceDereference(Context->ResetInterface.Context);
        RtlZeroMemory(&Context->ResetInterface, sizeof(Context->ResetInterface));
    }
    Context->HostReadyInvalid=TRUE;
    NnpiIrqUnmap(&Context->IrqMapping);
    NnpiUnmapRuntime(&Context->RuntimeMapping);
    NnpiUnmapHostReady(&Context->HostReadyMapping);
    (void)NnpiImageRelease(&Context->Image);
    NnpiDmaRelease(&Context->Dma);
    (void)NnpiNetDmaRelease(&Context->NetDma);
    Context->Sysinfo.page_retained=0;
    NnpiUnmapSysinfo(&Context->SysinfoMapping);
    RtlZeroMemory(&Context->ResetCaps, sizeof(Context->ResetCaps));
    NnpiUnmapControlSnapshot(&Context->SnapshotMapping);
    NnpiUnmapStatus0(&Context->StatusMapping);
    RtlZeroMemory(&Context->Diagnostic, sizeof(Context->Diagnostic));
    Context->Cache.flags = 0;
    Context->Cache.resource_count = 0;
    RtlZeroMemory(Context->Cache.resources, sizeof(Context->Cache.resources));
    /* Never wrap to an old generation. Exhaustion prevents future publishing. */
    if (Context->Cache.generation != UINT64_MAX) {
        ++Context->Cache.generation;
    }
    WdfWaitLockRelease(Context->CacheLock);
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    NnpiLogDriverObject = DriverObject;
    NnpiLogLoadTag = (ULONG)KeQueryInterruptTime();
    NnpiLogStage(1, STATUS_SUCCESS, 0);
    ExInitializeRundownProtection(&NnpiSysinfoRundown);
    status = NnpiResetInitialize();
    if (!NT_SUCCESS(status)) return status;
    WDF_DRIVER_CONFIG_INIT(&config, NnpiDeviceAdd);
    config.EvtDriverUnload = NnpiDriverUnload;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) NnpiResetUnload();
    NnpiLogStage(2, status, 0);
    return status;
}

_Use_decl_annotations_
NTSTATUS
NnpiDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    PNNPI_DEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    NnpiLogStage(10, STATUS_SUCCESS, 0);
    status = NnpiCheckHardwareIds(DeviceInit);
    if (!NT_SUCCESS(status)) {
        NnpiLogStage(24, status, 0);
        return status;
    }

    WdfDeviceInitSetDeviceType(DeviceInit, NNPI_INFO_DEVICE_TYPE);
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = NnpiPrepareHardware;
    pnp.EvtDeviceReleaseHardware = NnpiReleaseHardware;
    pnp.EvtDeviceD0Entry = NnpiD0Entry;
    pnp.EvtDeviceD0Exit = NnpiD0Exit;
    pnp.EvtDeviceSurpriseRemoval = NnpiSurpriseRemoval;
    pnp.EvtDeviceQueryStop=NnpiQueryStop;
    pnp.EvtDeviceQueryRemove=NnpiQueryRemove;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, NnpiFileCreate,
                              WDF_NO_EVENT_CALLBACK, NnpiFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NNPI_FILE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &attributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NNPI_DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    NnpiLogStage(20, status, 0);
    if (!NT_SUCCESS(status)) {
        NnpiLogStage(24, status, 0);
        return status;
    }
    context = NnpiGetContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->Device=device;
    NnpiInitSnapshot(&context->Cache);
    context->DeviceId = (ULONGLONG)InterlockedIncrement64(&NnpiDeviceSequence);
    context->Epoch = 1;
    nnpi_sysinfo_init(&context->Sysinfo);
    context->Boot.version=NNPI_BOOT_ABI_VERSION;
    context->Boot.size=sizeof(context->Boot);
    context->Boot.image_bytes=NNPI_BOOT_IMAGE_BYTES;
    KeInitializeEvent(&context->BootDone,NotificationEvent,TRUE);
    KeInitializeEvent(&context->BootClose,NotificationEvent,FALSE);
    context->Net.version=NNPI_NET_ABI_VERSION;
    context->Net.size=sizeof(context->Net);
    KeInitializeEvent(&context->NetDone,NotificationEvent,TRUE);
    KeInitializeEvent(&context->NetClose,NotificationEvent,FALSE);
    KeInitializeEvent(&context->SysinfoWorkerDone,NotificationEvent,TRUE);
    KeInitializeEvent(&context->SysinfoStopped,NotificationEvent,FALSE);
    /* Allocate at submit, before publication; Io work item pins the WDM device
     * and driver image until callback RETURN (not just last shared access). */
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &context->CacheLock);
    NnpiLogStage(21, status, 0);
    if (!NT_SUCCESS(status)) {
        NnpiLogStage(24, status, 0);
        return status;
    }

    NnpiDmaInitialize(device, &context->Dma);
    /* Optional enabler failure is cached, never returned as DeviceAdd failure. */

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.PowerManaged = WdfTrue;
    queueConfig.EvtIoDeviceControl = NnpiIoDeviceControl;
    queueConfig.EvtIoStop = NnpiIoStop;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfIoQueueCreate(device, &queueConfig, &attributes, &context->LegacyQueue);
    NnpiLogStage(22, status, 0);
    if (!NT_SUCCESS(status)) {
        NnpiLogStage(24, status, 0);
        return status;
    }
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = NnpiIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, &attributes, &context->ResetQueue);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = NnpiRouteRequest;
    status = WdfIoQueueCreate(device, &queueConfig, &attributes, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    /* The private interface GUID is NOT an ACL. Security comes from the INF. */
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_NNPI_INFO, NULL);
    NnpiLogStage(23, status, 0);
    NnpiLogStage(24, status, 0);
    return status;
}

_Use_decl_annotations_
VOID
NnpiFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject)
{
    PUNICODE_STRING name = WdfFileObjectGetFileName(FileObject);
    UNREFERENCED_PARAMETER(Device);
    /* No namespace under this device, including a trailing backslash. */
    WdfRequestCompleteWithInformation(Request,
        name != NULL && name->Length == 0 ? STATUS_SUCCESS : STATUS_OBJECT_NAME_INVALID, 0);
}

/* File cleanup is deliberately CPU-only. A dispatched request racing cleanup
 * must see Cleaned under the same lock and cannot resurrect the claim. */
static VOID NnpiNetConsumerCleanup(PNNPI_DEVICE_CONTEXT c,WDFFILEOBJECT file)
{
    WdfWaitLockAcquire(c->CacheLock,NULL);
    NnpiGetFileContext(file)->Cleaned=TRUE;
    if (c->NetConsumer==file) {
        c->NetConsumer=NULL;
        RtlZeroMemory(&c->NetConsumerKey,sizeof(c->NetConsumerKey));
    }
    WdfWaitLockRelease(c->CacheLock);
}
_Use_decl_annotations_
VOID NnpiFileCleanup(WDFFILEOBJECT FileObject)
{
    NnpiNetConsumerCleanup(NnpiGetContext(WdfFileObjectGetDevice(FileObject)),FileObject);
}

/* Prepare-only, once per successful resource preparation. Query is not reset.
 * WdfFdoQueryForInterface/IRP_MN_QUERY_INTERFACE returns an already referenced
 * interface on success. Only its matching dereference may be invoked here.
 * No reference, reset/status/bus-specific callback or pointer enters the cache.
 * Sources: WDK wdm.h DEVICE_RESET_INTERFACE_VERSION and original v1 structure;
 * learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_device_reset_interface_standard
 * learn.microsoft.com/windows-hardware/drivers/ddi/wdffdo/nf-wdffdo-wdffdoqueryforinterface
 */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID NnpiCollectResetCaps(WDFDEVICE Device, uint64_t Generation,
                               struct nnpi_resetcaps_response *Cache,
                               DEVICE_RESET_INTERFACE_STANDARD *Retained)
{
    DEVICE_RESET_INTERFACE_STANDARD resetInterface;
    NTSTATUS status;
    uint32_t rawTypes = 0;
    int handlerPresent = 0;
    RtlZeroMemory(&resetInterface, sizeof(resetInterface));
    status = WdfFdoQueryForInterface(Device, &GUID_DEVICE_RESET_INTERFACE_STANDARD,
        (PINTERFACE)&resetInterface, (USHORT)NNPI_RESET_INTERFACE_SIZE,
        DEVICE_RESET_INTERFACE_VERSION, NULL);
    nnpi_resetcaps_init(Cache, Generation, (uint32_t)NNPI_RESET_INTERFACE_SIZE,
        DEVICE_RESET_INTERFACE_VERSION, (uint32_t)status);
    if (nnpi_resetcaps_contract((uint32_t)status, (uint32_t)NNPI_RESET_INTERFACE_SIZE,
            DEVICE_RESET_INTERFACE_VERSION, resetInterface.Size, resetInterface.Version,
            resetInterface.InterfaceDereference != NULL)) {
        rawTypes = resetInterface.SupportedResetTypes;
        handlerPresent = resetInterface.DeviceReset != NULL;
    }
    nnpi_resetcaps_record(Cache, resetInterface.Size, resetInterface.Version,
        resetInterface.InterfaceDereference != NULL, rawTypes, handlerPresent);
    if (Cache->contract_valid && Cache->handler_present &&
        (Cache->supported_reset_types & NNPI_RESETCAPS_FUNCTION_LEVEL) &&
        resetInterface.InterfaceReference != NULL) {
        *Retained = resetInterface;
        RtlZeroMemory(&resetInterface, sizeof(resetInterface));
    }
    /* Even malformed successful size/version results must release their ref.
     * A broken provider with no dereference cannot be released or supported. */
    if (nnpi_resetcaps_needs_dereference((uint32_t)status,
            resetInterface.InterfaceDereference != NULL)) {
        resetInterface.InterfaceDereference(resetInterface.Context);
    }
    RtlZeroMemory(&resetInterface, sizeof(resetInterface));
}

_Use_decl_annotations_
NTSTATUS
NnpiPrepareHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw,
                    WDFCMRESLIST ResourcesTranslated)
{
    PNNPI_DEVICE_CONTEXT context = NnpiGetContext(Device);
    struct nnpi_info_response snapshot;
    struct nnpi_diag_response diagnostic;
    ULONG count, i;
    NTSTATUS status = STATUS_SUCCESS;

    NnpiInitSnapshot(&snapshot);
    count = WdfCmResourceListGetCount(ResourcesTranslated);
    NnpiLogStage(30, STATUS_SUCCESS, count);
    if (count > NNPI_INFO_MAX_RESOURCES) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }
    for (i = 0; i < count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        struct nnpi_info_resource *entry = &snapshot.resources[i];
        if (descriptor == NULL) {
            status = STATUS_DEVICE_DATA_ERROR;
            NnpiLogStage(31, status, i << 8);
            goto Fail;
        }
        /* Detail: bits 31:8 = translated index, bits 7:0 = descriptor type.
         * A NULL descriptor uses type 0 and STATUS_DEVICE_DATA_ERROR.
         * This records inspection, not validation; stage 32 carries the result.
         */
        NnpiLogStage(31, STATUS_SUCCESS, (i << 8) | (ULONG)descriptor->Type);
        /* This is a TRANSLATED DESCRIPTOR INDEX, never a PCI BAR number. */
        entry->translated_resource_index = i;
        entry->flags = descriptor->Flags;
        switch (descriptor->Type) {
        case CmResourceTypeMemory:
            entry->type = NNPI_INFO_RESOURCE_MEMORY;
            entry->start = (uint64_t)descriptor->u.Memory.Start.QuadPart;
            entry->length = descriptor->u.Memory.Length;
            entry->valid = NNPI_INFO_RESOURCE_ADDRESS;
            break;
        case CmResourceTypePort:
            entry->type = NNPI_INFO_RESOURCE_PORT;
            entry->start = (uint64_t)descriptor->u.Port.Start.QuadPart;
            entry->length = descriptor->u.Port.Length;
            entry->valid = NNPI_INFO_RESOURCE_ADDRESS;
            break;
        case CmResourceTypeInterrupt:
            entry->type = NNPI_INFO_RESOURCE_INTERRUPT;
            /* No address/length semantics in this ABI; do not read its union. */
            break;
        case CmResourceTypeDevicePrivate:
            /* Preserve the descriptor and its index, but never interpret or
             * expose system-private union contents as a BAR or address range. */
            entry->type = NNPI_INFO_RESOURCE_DEVICE_PRIVATE;
            break;
        case CmResourceTypeMemoryLarge:
        default:
            /* Do not guess union layout, omit a resource, or publish a prefix. */
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                "nnpi-info: unsupported translated descriptor %lu type %u flags 0x%x\n",
                i, (unsigned int)descriptor->Type, (unsigned int)descriptor->Flags));
            status = STATUS_NOT_SUPPORTED;
            goto Fail;
        }
        if (entry->valid != 0 &&
            (entry->length == 0 || entry->start > UINT64_MAX - (entry->length - 1))) {
            status = STATUS_DEVICE_DATA_ERROR;
            goto Fail;
        }
    }
    /* Each map failure disables only its own capability; INFO/diag survive. */
    NnpiCollectDiagnostic(Device, ResourcesRaw, ResourcesTranslated, &diagnostic);
    snapshot.resource_count = count;
    snapshot.flags = NNPI_INFO_FLAG_PREPARED;
    WdfWaitLockAcquire(context->CacheLock, NULL);
    if (context->Removed) {
        status = STATUS_DEVICE_REMOVED;
    } else if (context->Cache.generation == UINT64_MAX) {
        status = STATUS_INTEGER_OVERFLOW;
    } else {
        snapshot.generation = context->Cache.generation + 1;
        context->Cache = snapshot;
        diagnostic.generation = snapshot.generation;
        diagnostic.flags = NNPI_INFO_FLAG_PREPARED;
        context->Diagnostic = diagnostic;
        NnpiMapStatus0(&diagnostic, &context->StatusMapping);
        NnpiLogStage(33, STATUS_SUCCESS, context->StatusMapping.Reason);
        NnpiMapControlSnapshot(&diagnostic, &context->SnapshotMapping);
        /* 026 has its own independent one-shot host-ready budget. */
        NnpiMapHostReady(&diagnostic,&context->HostReadyMapping);
        NnpiMapRuntime(&diagnostic,&context->RuntimeMapping);
        NnpiMapSysinfo(&diagnostic,&context->SysinfoMapping);
        (void)NnpiIrqMap(&diagnostic,&context->IrqMapping);
        NnpiLogStage(34, STATUS_SUCCESS, context->SnapshotMapping.Reason);
        NnpiDmaPrepare(&context->Dma, snapshot.generation);
        NnpiLogStage(35, (NTSTATUS)context->Dma.Cache.ntstatus, context->Dma.Cache.stage);
        NnpiCollectResetCaps(Device, snapshot.generation, &context->ResetCaps, &context->ResetInterface);
        /* Detail bit0=supported, bit1=handler present, bit2=base contract valid. */
        NnpiLogStage(36, (NTSTATUS)context->ResetCaps.query_ntstatus,
            context->ResetCaps.supported | (context->ResetCaps.handler_present << 1) |
            (context->ResetCaps.contract_valid << 2));
    }
    WdfWaitLockRelease(context->CacheLock);
    if (NT_SUCCESS(status)) {
        NnpiLogStage(32, status, 0);
        return status;
    }
Fail:
    NnpiInvalidate(context, FALSE);
    NnpiLogStage(32, status, 0);
    return status;
}

_Use_decl_annotations_
NTSTATUS
NnpiReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    NnpiInvalidate(NnpiGetContext(Device), FALSE);
    NnpiLogStage(50, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NnpiSurpriseRemoval(WDFDEVICE Device)
{
    PNNPI_DEVICE_CONTEXT context=NnpiGetContext(Device);
    /* Concurrent with D0Exit/Release. Never delete/unmap/wait here. */
    WdfWaitLockAcquire(context->CacheLock,NULL);
    context->Removed=TRUE; context->SysinfoClosing=TRUE;
    context->NetClosing=TRUE; context->Net.closing=1;
    KeSetEvent(&context->NetClose,IO_NO_INCREMENT,FALSE);
    NnpiIrqInvalidate(&context->IrqMapping);
    context->RuntimeHealthy=FALSE; context->BootClosing=TRUE; context->Boot.closing=1;
    KeSetEvent(&context->BootClose,IO_NO_INCREMENT,FALSE);
    context->HostReadyInvalid=TRUE;
    nnpi_sysinfo_invalidate(&context->Sysinfo);
    NnpiResetInvalidate(context->DeviceId);
    if (context->Epoch!=UINT64_MAX) ++context->Epoch;
    WdfWaitLockRelease(context->CacheLock);
    NnpiLogStage(51, STATUS_SUCCESS, 0);
}

_Use_decl_annotations_
NTSTATUS
NnpiD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PNNPI_DEVICE_CONTEXT context = NnpiGetContext(Device);
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(PreviousState);
    NnpiLogStage(40, STATUS_SUCCESS, (ULONG)PreviousState);
    WdfWaitLockAcquire(context->CacheLock, NULL);
    if (context->Removed) {
        status = STATUS_DEVICE_REMOVED;
    } else if (!(context->Cache.flags & NNPI_INFO_FLAG_PREPARED)) {
        status = STATUS_DEVICE_NOT_READY;
    } else {
        context->Cache.flags |= NNPI_INFO_FLAG_D0;
        context->Diagnostic.flags |= NNPI_INFO_FLAG_D0;
    }
    WdfWaitLockRelease(context->CacheLock);
    NnpiLogStage(41, status, (ULONG)PreviousState);
    return status;
}

_Use_decl_annotations_
NTSTATUS
NnpiD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PNNPI_DEVICE_CONTEXT context = NnpiGetContext(Device);
    UNREFERENCED_PARAMETER(TargetState);
    NnpiLogStage(42, STATUS_SUCCESS, (ULONG)TargetState);
    /* Close admission and wait before power-down; worker may perform its one
     * reserved cleanup while D0 is still current. Surprise closes MMIO itself. */
    NnpiBootBarrier(context);
    NnpiNetBarrier(context);
    NnpiSysinfoBarrier(context);
    WdfWaitLockAcquire(context->CacheLock, NULL);
    nnpi_sysinfo_invalidate(&context->Sysinfo);
    if (context->Epoch != UINT64_MAX) ++context->Epoch;
    NnpiResetInvalidate(context->DeviceId);
    context->HostReadyInvalid=TRUE;
    NnpiIrqInvalidate(&context->IrqMapping);
    context->Cache.flags &= ~NNPI_INFO_FLAG_D0;
    context->Diagnostic.flags &= ~NNPI_INFO_FLAG_D0;
    WdfWaitLockRelease(context->CacheLock);
    NnpiLogStage(43, STATUS_SUCCESS, (ULONG)TargetState);
    return STATUS_SUCCESS;
}

/* Same bounded CPU-cache ownership as INFO. No PCI bus call in dispatch. */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID NnpiDiagnosticRequest(PNNPI_DEVICE_CONTEXT Context, WDFREQUEST Request,
                                 size_t OutputBytes, size_t InputBytes)
{
    struct nnpi_diag_request input;
    struct nnpi_diag_response snapshot;
    PVOID buffer;
    NTSTATUS status;
    ULONG_PTR information = 0;
    if (InputBytes != sizeof(input)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlCopyMemory(&input, buffer, sizeof(input));
    if (!nnpi_diag_request_valid(&input, InputBytes)) { status = STATUS_INVALID_PARAMETER; goto Complete; }
    if (OutputBytes < sizeof(snapshot)) { status = STATUS_BUFFER_TOO_SMALL; goto Complete; }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(snapshot), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlZeroMemory(&snapshot, sizeof(snapshot));
    WdfWaitLockAcquire(Context->CacheLock, NULL);
    if (Context->Removed) status = STATUS_DEVICE_REMOVED;
    else if (Context->Diagnostic.flags != NNPI_DIAG_PREPARED_D0) status = STATUS_DEVICE_NOT_READY;
    else { snapshot = Context->Diagnostic; status = STATUS_SUCCESS; }
    WdfWaitLockRelease(Context->CacheLock);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(buffer, &snapshot, sizeof(snapshot));
        information = sizeof(snapshot);
    }
Complete:
    WdfRequestCompleteWithInformation(Request, status, information);
}

/* Called with CacheLock: callback only publishes scalar state; MMIO dispatch
 * consults that state as well as its per-device gate. */
static BOOLEAN NnpiResetHardwareBlocked(PNNPI_DEVICE_CONTEXT Context)
{
    struct nnpi_reset_response state;
    NnpiResetQuery(Context->DeviceId, Context->Cache.generation, Context->Epoch, &state);
    return Context->Boot.load_state!=NNPI_BOOT_LOAD_EMPTY || Context->BootClosing || Context->SysinfoStarted || Context->SysinfoClosing || Context->ResetBlocked || state.hardware_blocked;
}

/* Validate BOTH buffers before any hardware operation. METHOD_BUFFERED input
 * must be copied before output; on failure return no data, never stale validity. */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID NnpiStatusRequest(PNNPI_DEVICE_CONTEXT Context, WDFREQUEST Request,
                             size_t OutputBytes, size_t InputBytes)
{
    struct nnpi_status_request input;
    struct nnpi_status_response snapshot;
    PVOID buffer;
    NTSTATUS status;
    uint32_t reason;
    ULONG_PTR information = 0;
    if (InputBytes != sizeof(input)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlCopyMemory(&input, buffer, sizeof(input));
    if (!nnpi_status_request_valid(&input, InputBytes)) { status = STATUS_INVALID_PARAMETER; goto Complete; }
    if (!nnpi_status_buffers_valid(&input, InputBytes, OutputBytes)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(snapshot), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    WdfWaitLockAcquire(Context->CacheLock, NULL);
    reason = nnpi_status_sample(Context->Cache.flags, Context->Removed,
        Context->StatusMapping.Address != NULL && !NnpiResetHardwareBlocked(Context), Context->Cache.generation,
        Context->StatusMapping.Reason, NnpiReadStatus0, &Context->StatusMapping, &snapshot);
    WdfWaitLockRelease(Context->CacheLock);
    switch (reason) {
    case NNPI_STATUS_OK: status = STATUS_SUCCESS; break;
    case NNPI_STATUS_REMOVED: status = STATUS_DEVICE_REMOVED; break;
    case NNPI_STATUS_MAP_FAILED: status = STATUS_INSUFFICIENT_RESOURCES; break;
    case NNPI_STATUS_NOT_READY: status = STATUS_DEVICE_NOT_READY; break;
    default: status = STATUS_NOT_SUPPORTED; break;
    }
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(buffer, &snapshot, sizeof(snapshot));
        information = sizeof(snapshot);
    }
Complete:
    WdfRequestCompleteWithInformation(Request, status, information);
}

/* Validate BOTH buffers before any hardware operation. METHOD_BUFFERED input
 * must be copied before output; on failure return no data, never stale validity. */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID NnpiControlSnapshotRequest(PNNPI_DEVICE_CONTEXT Context, WDFREQUEST Request,
                             size_t OutputBytes, size_t InputBytes)
{
    struct nnpi_snapshot_request input;
    struct nnpi_snapshot_response snapshot;
    PVOID buffer;
    NTSTATUS status;
    uint32_t reason;
    ULONG_PTR information = 0;
    if (InputBytes != sizeof(input)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlCopyMemory(&input, buffer, sizeof(input));
    if (!nnpi_snapshot_request_valid(&input, InputBytes)) { status = STATUS_INVALID_PARAMETER; goto Complete; }
    if (!nnpi_snapshot_buffers_valid(&input, InputBytes, OutputBytes)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(snapshot), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    WdfWaitLockAcquire(Context->CacheLock, NULL);
    reason = nnpi_snapshot_sample(&input, InputBytes, OutputBytes, Context->Cache.flags, Context->Removed,
        Context->SnapshotMapping.Address != NULL && !NnpiResetHardwareBlocked(Context), Context->Cache.generation,
        Context->SnapshotMapping.Reason, NnpiReadControlSnapshot, &Context->SnapshotMapping, &snapshot);
    WdfWaitLockRelease(Context->CacheLock);
    switch (reason) {
    case NNPI_STATUS_OK: status = STATUS_SUCCESS; break;
    case NNPI_STATUS_REMOVED: status = STATUS_DEVICE_REMOVED; break;
    case NNPI_STATUS_MAP_FAILED: status = STATUS_INSUFFICIENT_RESOURCES; break;
    case NNPI_STATUS_NOT_READY: status = STATUS_DEVICE_NOT_READY; break;
    default: status = STATUS_NOT_SUPPORTED; break;
    }
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(buffer, &snapshot, sizeof(snapshot));
        information = sizeof(snapshot);
    }
Complete:
    WdfRequestCompleteWithInformation(Request, status, information);
}

/* Cache-only query: allocation/test failures are data, lifecycle failures are IOCTL errors. */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID NnpiDmaCheckRequest(PNNPI_DEVICE_CONTEXT Context, WDFREQUEST Request,
                              size_t OutputBytes, size_t InputBytes)
{
    struct nnpi_dmacheck_request input;
    struct nnpi_dmacheck_response snapshot;
    PVOID buffer;
    NTSTATUS status;
    uint32_t gate;
    ULONG_PTR information = 0;
    if (InputBytes != sizeof(input)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlCopyMemory(&input, buffer, sizeof(input));
    if (!nnpi_dmacheck_request_valid(&input, InputBytes)) { status = STATUS_INVALID_PARAMETER; goto Complete; }
    if (!nnpi_dmacheck_buffers_valid(&input, InputBytes, OutputBytes)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(snapshot), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    WdfWaitLockAcquire(Context->CacheLock, NULL);
    gate = nnpi_dmacheck_query(&Context->Dma.Cache, Context->Cache.flags,
        Context->Removed, Context->Cache.generation, &snapshot);
    WdfWaitLockRelease(Context->CacheLock);
    status = gate == NNPI_DMA_GATE_OK ? STATUS_SUCCESS :
        gate == NNPI_DMA_GATE_REMOVED ? STATUS_DEVICE_REMOVED : STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(buffer, &snapshot, sizeof(snapshot));
        information = sizeof(snapshot);
    }
Complete:
    WdfRequestCompleteWithInformation(Request, status, information);
}

/* Exact-sized, cache-only query; unsupported interface is successful data. */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID NnpiResetCapsRequest(PNNPI_DEVICE_CONTEXT Context, WDFREQUEST Request,
                               size_t OutputBytes, size_t InputBytes)
{
    struct nnpi_resetcaps_request input;
    struct nnpi_resetcaps_response snapshot;
    PVOID buffer;
    NTSTATUS status;
    uint32_t gate;
    ULONG_PTR information = 0;
    if (InputBytes != sizeof(input)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlCopyMemory(&input, buffer, sizeof(input));
    if (!nnpi_resetcaps_request_valid(&input, InputBytes)) { status = STATUS_INVALID_PARAMETER; goto Complete; }
    if (!nnpi_resetcaps_buffers_valid(&input, InputBytes, OutputBytes)) { status = STATUS_INFO_LENGTH_MISMATCH; goto Complete; }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(snapshot), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    WdfWaitLockAcquire(Context->CacheLock, NULL);
    gate = nnpi_resetcaps_query(&Context->ResetCaps, Context->Cache.flags,
        Context->Removed, Context->Cache.generation, &snapshot);
    WdfWaitLockRelease(Context->CacheLock);
    status = gate == NNPI_RESET_GATE_OK ? STATUS_SUCCESS :
        gate == NNPI_RESET_GATE_REMOVED ? STATUS_DEVICE_REMOVED : STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(buffer, &snapshot, sizeof(snapshot));
        information = sizeof(snapshot);
    }
Complete:
    WdfRequestCompleteWithInformation(Request, status, information);
}

_Use_decl_annotations_
VOID NnpiDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    ExWaitForRundownProtectionRelease(&NnpiSysinfoRundown);
    NnpiResetUnload();
}

/* Nonpower router owns only forwarding; old IOCTLs still execute in their
 * original sequential power-managed queue with unchanged lifecycle gates. */
_Use_decl_annotations_
VOID NnpiRouteRequest(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBytes,
                      size_t InputBytes, ULONG Code)
{
    PNNPI_DEVICE_CONTEXT context=NnpiGetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status;
    UNREFERENCED_PARAMETER(OutputBytes);
    UNREFERENCED_PARAMETER(InputBytes);
    status=WdfRequestForwardToIoQueue(Request,
        (Code==NNPI_RESET_SUBMIT_IOCTL || Code==NNPI_RESET_QUERY_IOCTL ||
         Code==NNPI_RESET_REVALIDATE_IOCTL || Code==NNPI_HOSTREADY_SUBMIT_IOCTL ||
         Code==NNPI_HOSTREADY_OBSERVE_IOCTL || Code==NNPI_SYSINFO_SUBMIT_IOCTL ||
         Code==NNPI_SYSINFO_QUERY_IOCTL || Code==NNPI_BOOT_BEGIN_IOCTL ||
         Code==NNPI_BOOT_CHUNK_IOCTL || Code==NNPI_BOOT_SEAL_IOCTL ||
         Code==NNPI_BOOT_SUBMIT_IOCTL || Code==NNPI_BOOT_QUERY_IOCTL ||
         Code==NNPI_NET_BEGIN_IOCTL || Code==NNPI_NET_QUERY_IOCTL ||
         Code==NNPI_RUNTIME_INFO_IOCTL || Code==NNPI_NET_DATA_QUERY_IOCTL ||
         Code==NNPI_NET_TX_IOCTL || Code==NNPI_NET_RX_CLAIM_IOCTL ||
         Code==NNPI_NET_RX_RELEASE_IOCTL || Code==NNPI_NET_RX_BATCH_IOCTL) ? context->ResetQueue : context->LegacyQueue);
    if (!NT_SUCCESS(status)) WdfRequestComplete(Request,status);
}

static VOID NnpiResetRequest(PNNPI_DEVICE_CONTEXT Context, WDFREQUEST Request,
                            size_t OutputBytes, size_t InputBytes, ULONG Code)
{
    struct nnpi_reset_request input;
    struct nnpi_reset_response output;
    PVOID buffer;
    /* Minimal 024 compatibility fix (see logs/fixes-driver.diff): zero the
     * response before any pointer use; C4701 under /W4 /WX otherwise treats
     * the by-pointer fill in NnpiResetQuery as potentially uninitialized. */
    RtlZeroMemory(&output, sizeof(output));
    NTSTATUS status;
    ULONG_PTR information = 0;
    if (InputBytes != sizeof(input) || OutputBytes != sizeof(output)) {
        status=STATUS_INFO_LENGTH_MISMATCH; goto Complete;
    }
    status=WdfRequestRetrieveInputBuffer(Request, sizeof(input), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlCopyMemory(&input, buffer, sizeof(input));
    if (!nnpi_reset_request_valid(&input, InputBytes, Code != NNPI_RESET_QUERY_IOCTL)) {
        status=STATUS_INVALID_PARAMETER; goto Complete;
    }
    status=WdfRequestRetrieveOutputBuffer(Request, sizeof(output), &buffer, NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    WdfWaitLockAcquire(Context->CacheLock, NULL);
    if (Code == NNPI_RESET_QUERY_IOCTL) {
        NnpiResetQuery(Context->DeviceId, Context->Cache.generation, Context->Epoch, &output);
        status=STATUS_SUCCESS;
    } else if (InterlockedCompareExchange(&NnpiActionOwner,0,0)==1) {
        status=STATUS_DEVICE_BUSY;
    } else if (Context->Removed || Context->Cache.flags != NNPI_DIAG_PREPARED_D0 ||
               input.generation != Context->Cache.generation || input.epoch != Context->Epoch ||
               Context->Epoch == UINT64_MAX || Context->Cache.generation == UINT64_MAX ||
               Context->Diagnostic.reason != NNPI_BAR_OK ||
               Context->StatusMapping.Address == NULL || Context->SnapshotMapping.Address == NULL) {
        status=STATUS_DEVICE_NOT_READY;
    } else if (Code == NNPI_RESET_SUBMIT_IOCTL) {
        if (Context->ResetBlocked || !Context->ResetInterface.DeviceReset ||
            !Context->Dma.Cache.passed || !Context->Dma.Cache.retained) status=STATUS_DEVICE_NOT_READY;
        else {
            /* All DMA remains UNPUBLISHED in 024. Close MMIO before worker can run. */
            if (InterlockedCompareExchange(&NnpiActionOwner,2,0)!=0) {
                status=STATUS_DEVICE_BUSY;
                goto ResetUnlock;
            }
            Context->ResetBlocked=TRUE;
            status=NnpiResetSubmit(WdfDeviceWdmGetDeviceObject(Context->Device), &Context->ResetInterface, Context->DeviceId,
                                  Context->Cache.generation, Context->Epoch);
            if (!NT_SUCCESS(status)) Context->ResetBlocked=FALSE;
        }
    } else {
        NnpiResetQuery(Context->DeviceId, Context->Cache.generation, Context->Epoch, &output);
        if (!nnpi_reset_can_revalidate(&output, Context->Cache.generation, Context->Epoch))
            status=STATUS_DEVICE_NOT_READY;
        else {
            /* A NEW CPU test; commit eligibility only after it actually passes. */
            NnpiDmaRelease(&Context->Dma);
            NnpiDmaPrepare(&Context->Dma, Context->Cache.generation);
            if (!Context->Dma.Cache.passed || !Context->Dma.Cache.retained ||
                !NnpiResetRevalidate(Context->DeviceId, Context->Cache.generation, Context->Epoch))
                status=STATUS_DEVICE_NOT_READY;
            else { Context->ResetBlocked=FALSE; status=STATUS_SUCCESS; }
        }
    }
    if (NT_SUCCESS(status))
        NnpiResetQuery(Context->DeviceId, Context->Cache.generation, Context->Epoch, &output);
ResetUnlock:
    WdfWaitLockRelease(Context->CacheLock);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(buffer, &output, sizeof(output)); information=sizeof(output);
    }
Complete:
    WdfRequestCompleteWithInformation(Request, status, information);
}

/* 025A synchronous action/observation. CacheLock covers each complete MMIO
 * sequence, never sleeps while holding it. No asynchronous object ownership. */
static VOID NnpiHostReadyRequest(PNNPI_DEVICE_CONTEXT Context, WDFREQUEST Request,
                               size_t OutputBytes, size_t InputBytes, ULONG Code)
{
    struct nnpi_hostready_request input;
    struct nnpi_hostready_response output;
    struct nnpi_reset_response reset;
    PVOID buffer;
    NTSTATUS status;
    ULONG_PTR information=0;
    ULONGLONG now;
    if (InputBytes!=sizeof(input) || OutputBytes!=sizeof(output)) {
        status=STATUS_INFO_LENGTH_MISMATCH; goto Complete;
    }
    status=WdfRequestRetrieveInputBuffer(Request,sizeof(input),&buffer,NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlCopyMemory(&input,buffer,sizeof(input));
    if (!nnpi_hostready_request_valid(&input,InputBytes,OutputBytes)) {
        status=STATUS_INVALID_PARAMETER; goto Complete;
    }
    status=WdfRequestRetrieveOutputBuffer(Request,sizeof(output),&buffer,NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlZeroMemory(&output,sizeof(output));
    WdfWaitLockAcquire(Context->CacheLock,NULL);
    NnpiResetQuery(Context->DeviceId,Context->Cache.generation,Context->Epoch,&reset);
    if (Context->Removed || Context->HostReadyInvalid || Context->ResetBlocked ||
        reset.state!=NNPI_RESET_IDLE || reset.hardware_blocked ||
        Context->Cache.flags!=NNPI_DIAG_PREPARED_D0 ||
        input.generation!=Context->Cache.generation || input.epoch!=Context->Epoch ||
        !Context->HostReadyMapping.Address || !Context->SnapshotMapping.Address) {
        status=STATUS_DEVICE_NOT_READY; goto Unlock;
    }
    if (Code==NNPI_HOSTREADY_SUBMIT_IOCTL) {
        /* Consume attempt BEFORE preflight MMIO, not just before the write.
         * Failure/hung read cannot be repeated through another device/epoch. */
        if (InterlockedCompareExchange(&NnpiActionOwner,1,0)!=0) {
            status=STATUS_DEVICE_BUSY; goto Unlock;
        }
        output.version=1; output.size=sizeof(output);
        output.generation=Context->Cache.generation; output.epoch=Context->Epoch;
        output.host_before=NnpiReadHostReady(&Context->HostReadyMapping);
        output.command_before=NnpiReadControlSnapshot(&Context->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        output.response_before=NnpiReadControlSnapshot(&Context->SnapshotMapping,NNPI_SNAPSHOT_RESPONSE_OFFSET);
        output.card_before=NnpiReadControlSnapshot(&Context->SnapshotMapping,NNPI_SNAPSHOT_DOORBELL_OFFSET);
        if (!nnpi_hostready_baseline(output.host_before,output.card_before,
                                    output.command_before,output.response_before)) {
            status=STATUS_DEVICE_NOT_READY; goto Unlock;
        }
        output.phase=NNPI_HR_WRITTEN; output.writes=1; output.reads=4;
        Context->HostReady=output;
        NnpiWriteHostReady(&Context->HostReadyMapping);
        Context->HostReadyLastRead=KeQueryInterruptTime();
        status=STATUS_SUCCESS;
    } else {
        now=KeQueryInterruptTime();
        if (InterlockedCompareExchange(&NnpiActionOwner,0,0)!=1 ||
            !nnpi_hostready_can_observe(&Context->HostReady,input.generation,input.epoch,
                                        now,Context->HostReadyLastRead)) {
            status=STATUS_DEVICE_NOT_READY; goto Unlock;
        }
        Context->HostReadyLastRead=now;
        (void)nnpi_hostready_observed(&Context->HostReady,
            NnpiReadControlSnapshot(&Context->SnapshotMapping,NNPI_SNAPSHOT_DOORBELL_OFFSET));
        output=Context->HostReady;
        status=STATUS_SUCCESS;
    }
Unlock:
    WdfWaitLockRelease(Context->CacheLock);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(buffer,&output,sizeof(output)); information=sizeof(output);
    }
Complete:
    WdfRequestCompleteWithInformation(Request,status,information);
}

/* Independent of client lifetime: request is completed only after this work
 * item is queued. Pre-dispose barrier keeps context/page/mapping alive until
 * the worker exits AND reset evidence stops DMA. No WDF queue is required. */
static VOID NnpiSysinfoWorker(PDEVICE_OBJECT DeviceObject,PVOID Parameter)
{
    PNNPI_DEVICE_CONTEXT c=(PNNPI_DEVICE_CONTEXT)Parameter;
    WDFDEVICE device=c->Device;
    LARGE_INTEGER delay;
    ULONG i;
    NTSTATUS status;
    PIO_WORKITEM work=c->SysinfoWork;
    UNREFERENCED_PARAMETER(DeviceObject);
    delay.QuadPart=-5000000;
    for (i=0;i<20;++i) {
        KeDelayExecutionThread(KernelMode,FALSE,&delay);
        WdfWaitLockAcquire(c->CacheLock,NULL);
        if (c->Sysinfo.phase!=NNPI_SYSINFO_OBSERVING || c->Removed || c->Sysinfo.epoch_invalidated || c->SysinfoClosing ||
            c->Cache.flags!=NNPI_DIAG_PREPARED_D0) {
            WdfWaitLockRelease(c->CacheLock); break;
        }
        if (nnpi_sysinfo_observe(&c->Sysinfo,
            NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_DOORBELL_OFFSET))) {
            KeMemoryBarrier();
            nnpi_sysinfo_payload(&c->Sysinfo,(const uint8_t *)c->Dma.VirtualAddress);
            WdfWaitLockRelease(c->CacheLock); break;
        }
        WdfWaitLockRelease(c->CacheLock);
    }
    WdfWaitLockAcquire(c->CacheLock,NULL);
    c->Sysinfo.timed_out=(i==20);
    c->Sysinfo.phase=NNPI_SYSINFO_CLEANUP;
    c->ResetBlocked=TRUE;
    /* D0Exit closes observations but waits before invalidating/powering off.
     * Surprise cannot initiate a new reset; old DMA stays owned indefinitely. */
    if (!c->Removed && !c->Sysinfo.epoch_invalidated && c->Cache.flags==NNPI_DIAG_PREPARED_D0) {
        status=NnpiResetSubmit(WdfDeviceWdmGetDeviceObject(c->Device),&c->ResetInterface,c->DeviceId,c->Sysinfo.generation,c->Sysinfo.epoch);
        if (!NT_SUCCESS(status)) c->Sysinfo.phase=NNPI_SYSINFO_STOP_UNCONFIRMED;
    } else c->Sysinfo.phase=NNPI_SYSINFO_STOP_UNCONFIRMED;
    /* Closing hardware access is separate from reset's original G/E. The
     * reset worker must not be invalidated here before it invokes provider. */
    nnpi_sysinfo_invalidate(&c->Sysinfo);
    WdfWaitLockRelease(c->CacheLock);
    KeSetEvent(&c->SysinfoWorkerDone,IO_NO_INCREMENT,FALSE);
    /* Event waiter's return can race this epilogue. Reference retains ONLY
     * context backing, never mapping; Release barrier owns mapping safety. */
    WdfObjectDereference(device);
    IoFreeWorkItem(work);
    ExReleaseRundownProtection(&NnpiSysinfoRundown);
    /* I/O manager holds the DEVICE_OBJECT/image through actual return. */
}

/* Durable driver-service budget, separate from the host-ready ledger. Never
 * clear on unload, DeviceAdd, failure, PnP cancel or reprepare. Flush BEFORE
 * first possibly publishing write. CLI independently uses CreateNew ledger. */
static NTSTATUS NnpiSysinfoConsumeBudget(VOID)
{
    WDFKEY key;
    UNICODE_STRING name=RTL_CONSTANT_STRING(L"Sysinfo025Consumed");
    ULONG consumed;
    NTSTATUS status;
    status=WdfDriverOpenParametersRegistryKey(WdfGetDriver(),KEY_QUERY_VALUE|KEY_SET_VALUE,
                                               WDF_NO_OBJECT_ATTRIBUTES,&key);
    if (!NT_SUCCESS(status)) return status;
    status=WdfRegistryQueryULong(key,&name,&consumed);
    if (status==STATUS_OBJECT_NAME_NOT_FOUND) {
        status=WdfRegistryAssignULong(key,&name,1);
        if (NT_SUCCESS(status)) status=ZwFlushKey(WdfRegistryWdmGetHandle(key));
    } else if (NT_SUCCESS(status)) status=STATUS_DEVICE_BUSY;
    WdfRegistryClose(key);
    return status;
}
static int NnpiSysinfoWriteHalf(void *Context,uint32_t Half,uint32_t Value)
{
    __try { NnpiWriteSysinfoHalf((NNPI_COMMAND_MAPPING *)Context,Half,Value); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static VOID NnpiSysinfoRequest(PNNPI_DEVICE_CONTEXT c,WDFREQUEST Request,
                              size_t OutputBytes,size_t InputBytes,ULONG Code)
{
    struct nnpi_sysinfo_request input;
    struct nnpi_sysinfo_response output;
    /* Initialize every response path; WDK /W4 /WX diagnoses conditional fills. */
    RtlZeroMemory(&output, sizeof(output));
    struct nnpi_fifo_command_plan plan={0};
    struct nnpi_reset_response reset;
    PVOID buffer;
    NTSTATUS status;
    ULONG_PTR information=0;
    if (InputBytes!=sizeof(input) || OutputBytes!=sizeof(output)) {
        status=STATUS_INFO_LENGTH_MISMATCH; goto Complete;
    }
    status=WdfRequestRetrieveInputBuffer(Request,sizeof(input),&buffer,NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    RtlCopyMemory(&input,buffer,sizeof(input));
    if (!nnpi_sysinfo_request_valid(&input,InputBytes,Code==NNPI_SYSINFO_SUBMIT_IOCTL)) {
        status=STATUS_INVALID_PARAMETER; goto Complete;
    }
    status=WdfRequestRetrieveOutputBuffer(Request,sizeof(output),&buffer,NULL);
    if (!NT_SUCCESS(status)) goto Complete;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (Code==NNPI_SYSINFO_QUERY_IOCTL) {
        NnpiSysinfoRefresh(c); output=c->Sysinfo;
        if (!output.budget_consumed) { output.generation=c->Cache.generation; output.epoch=c->Epoch; }
        status=STATUS_SUCCESS; goto Unlock;
    }
    NnpiResetQuery(c->DeviceId,c->Cache.generation,c->Epoch,&reset);
    if (c->Removed || c->SysinfoClosing || c->SysinfoStarted || c->ResetBlocked ||
        c->Cache.flags!=NNPI_DIAG_PREPARED_D0 || input.generation!=c->Cache.generation ||
        input.epoch!=c->Epoch || input.generation==UINT64_MAX || input.epoch==UINT64_MAX ||
        reset.state!=NNPI_RESET_IDLE || reset.hardware_blocked || !c->ResetInterface.DeviceReset ||
        !c->Dma.Cache.passed || !c->Dma.Cache.retained || !c->Dma.VirtualAddress ||
        !c->SysinfoMapping.Address || !c->SnapshotMapping.Address) {
        status=STATUS_DEVICE_NOT_READY; goto Unlock;
    }
    if (InterlockedCompareExchange(&NnpiActionOwner,3,0)!=0) {
        status=STATUS_DEVICE_BUSY; goto Unlock;
    }
    status=NnpiSysinfoConsumeBudget();
    if (!NT_SUCCESS(status)) goto Unlock;
    c->Sysinfo.card_before=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_DOORBELL_OFFSET);
    c->Sysinfo.command_before=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
    if (!nnpi_sysinfo_plan(c->Sysinfo.card_before,c->Sysinfo.command_before,
                           (uint64_t)c->Dma.LogicalAddress.QuadPart,&plan)) {
        status=STATUS_DEVICE_NOT_READY; goto Unlock;
    }
    status=NnpiResetReserve(WdfDeviceWdmGetDeviceObject(c->Device));
    if (!NT_SUCCESS(status)) goto Unlock;
    c->Sysinfo.cleanup_reserved=1; c->Sysinfo.page_retained=1;
    if (!nnpi_sysinfo_publish(&c->Sysinfo,c->Cache.generation,c->Epoch)) {
        status=STATUS_DEVICE_NOT_READY; goto Unlock;
    }
    /* First possible half-write is already in-flight, even if MMIO faults or
     * only a prefix reaches hardware. No rollback, re-zero or retry. */
    RtlZeroMemory(c->Dma.VirtualAddress,NNPI_SYSINFO_BYTES);
    KeMemoryBarrier();
    c->SysinfoWork=IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(c->Device));
    if (!c->SysinfoWork) {
        c->Sysinfo.ownership=NNPI_SYSINFO_UNPUBLISHED;
        c->Sysinfo.phase=NNPI_SYSINFO_STOP_UNCONFIRMED;
        status=STATUS_INSUFFICIENT_RESOURCES; goto Unlock;
    }
    if (!ExAcquireRundownProtection(&NnpiSysinfoRundown)) {
        IoFreeWorkItem(c->SysinfoWork); c->SysinfoWork=NULL;
        /* No hardware write yet. Roll back ownership only, never budget. */
        c->Sysinfo.ownership=NNPI_SYSINFO_UNPUBLISHED;
        c->Sysinfo.phase=NNPI_SYSINFO_STOP_UNCONFIRMED;
        status=STATUS_DELETE_PENDING; goto Unlock;
    }
    WdfObjectReference(c->Device);
    c->Dma.Published=TRUE;
    c->SysinfoStarted=TRUE;
    KeClearEvent(&c->SysinfoWorkerDone);
    (void)nnpi_sysinfo_emit(&c->Sysinfo,&plan,&c->SysinfoMapping,NnpiSysinfoWriteHalf);
    IoQueueWorkItem(c->SysinfoWork,NnpiSysinfoWorker,DelayedWorkQueue,c);
    output=c->Sysinfo; status=STATUS_SUCCESS;
Unlock:
    WdfWaitLockRelease(c->CacheLock);
    if (NT_SUCCESS(status)) { RtlCopyMemory(buffer,&output,sizeof(output)); information=sizeof(output); }
Complete:
    WdfRequestCompleteWithInformation(Request,status,information);
}

/* Non-power-managed query: copy cached CPU evidence under CacheLock only.
 * No MMIO, status clearing, lifecycle gate, session mutation or allocation. */
static VOID NnpiRuntimeInfoRequest(PNNPI_DEVICE_CONTEXT c,WDFREQUEST Request,
    size_t OutputBytes,size_t InputBytes)
{
    struct nnpi_runtime_info_request input;
    struct nnpi_runtime_info_response output={0};
    PVOID inBuffer,outBuffer; NTSTATUS status; ULONG_PTR information=0;
    if (InputBytes!=sizeof(input) || OutputBytes!=sizeof(output)) {
        status=STATUS_INFO_LENGTH_MISMATCH; goto End;
    }
    status=WdfRequestRetrieveInputBuffer(Request,sizeof(input),&inBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    RtlCopyMemory(&input,inBuffer,sizeof(input));
    if (!nnpi_runtime_info_buffers_valid(&input,InputBytes,OutputBytes)) {
        status=STATUS_INVALID_PARAMETER; goto End;
    }
    status=WdfRequestRetrieveOutputBuffer(Request,sizeof(output),&outBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    output.version=NNPI_RUNTIME_INFO_VERSION; output.size=sizeof(output);
    output.generation=c->Cache.generation; output.epoch=c->Epoch;
    output.compatible=c->RuntimeCompatible; output.healthy=c->RuntimeHealthy;
    output.protocol_version=c->RuntimeVersion.protocol_version;
    output.firmware_version=c->RuntimeVersion.firmware_version;
    output.channel_version=c->RuntimeVersion.channel_version;
    output.last_stage=c->BootDiagnostic.RuntimeStep;
    output.last_failure_reason=c->BootDiagnostic.FirstFailureReason;
    output.last_failure_status=c->BootDiagnostic.FirstFailureStatus;
    output.keepalive_updates=c->RuntimeKeepaliveUpdates;
    output.irq_mask=c->IrqMapping.Policy.AfterMask;
    output.irq_status=c->IrqMapping.Policy.Status;
    output.irq_reason=c->IrqMapping.Policy.Failed ? c->IrqMapping.Policy.Reason : c->IrqMapping.Reason;
    /* Conservative validity: partial/failed checks can retain stale raw values. */
    output.irq_valid=c->IrqMapping.Policy.Verified ? 3u : 0u;
    output.partial_count=c->RuntimeParser.count;
    {
        static const uint8_t probeMac[6]={2,0,0,0,0,1};
        output.net_disabled_reason=NnpiNetGate(c,c->Cache.generation,c->Epoch,
            NNPI_CHAN_MIN_NONCTX,1,probeMac,FALSE);
        output.net_enabled=output.net_disabled_reason==NNPI_RUNTIME_NET_READY;
    }
    output.unexpected_net_config_count=c->UnexpectedNetConfigCount;
    output.last_unexpected_net_config=c->LastUnexpectedNetConfig;
    output.last_event=c->RuntimeLastEvent;
    WdfWaitLockRelease(c->CacheLock);
    RtlCopyMemory(outBuffer,&output,sizeof(output)); information=sizeof(output);
End:
    WdfRequestCompleteWithInformation(Request,status,information);
}

_Use_decl_annotations_
VOID
NnpiIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength,
                    size_t InputBufferLength, ULONG IoControlCode)
{
    PNNPI_DEVICE_CONTEXT context = NnpiGetContext(WdfIoQueueGetDevice(Queue));
    struct nnpi_info_request input;
    struct nnpi_info_response snapshot;
    PVOID buffer;
    NTSTATUS status;
    ULONG_PTR information = 0;

    if (IoControlCode==NNPI_BOOT_BEGIN_IOCTL || IoControlCode==NNPI_BOOT_CHUNK_IOCTL ||
        IoControlCode==NNPI_BOOT_SEAL_IOCTL || IoControlCode==NNPI_BOOT_SUBMIT_IOCTL ||
        IoControlCode==NNPI_BOOT_QUERY_IOCTL) {
        NnpiBootRequest(context,Request,OutputBufferLength,InputBufferLength,IoControlCode); return;
    }
    if (IoControlCode==NNPI_NET_BEGIN_IOCTL) {
        NnpiNetRequest(context,Request,OutputBufferLength,InputBufferLength,IoControlCode); return;
    }
    if (IoControlCode==NNPI_NET_DATA_QUERY_IOCTL || IoControlCode==NNPI_NET_TX_IOCTL ||
        IoControlCode==NNPI_NET_RX_CLAIM_IOCTL || IoControlCode==NNPI_NET_RX_RELEASE_IOCTL ||
        IoControlCode==NNPI_NET_RX_BATCH_IOCTL) {
        NnpiNetDataRequest(context,Request,OutputBufferLength,InputBufferLength,IoControlCode); return;
    }
    if (IoControlCode==NNPI_RUNTIME_INFO_IOCTL) {
        NnpiRuntimeInfoRequest(context,Request,OutputBufferLength,InputBufferLength); return;
    }
    if (IoControlCode==NNPI_NET_QUERY_IOCTL) {
        NnpiNetRequest(context,Request,OutputBufferLength,InputBufferLength,IoControlCode); return;
    }
    if (IoControlCode==NNPI_SYSINFO_SUBMIT_IOCTL) {
        WdfRequestComplete(Request,STATUS_NOT_SUPPORTED); return;
    }
    if (IoControlCode==NNPI_SYSINFO_QUERY_IOCTL) {
        NnpiSysinfoRequest(context,Request,OutputBufferLength,InputBufferLength,IoControlCode);
        return;
    }
    /* This build is 026-only: spent 025/025A and separate reset/revalidate cannot
     * be invoked, including before BOOT. No reload grants their budgets. */
    if (IoControlCode==NNPI_HOSTREADY_SUBMIT_IOCTL || IoControlCode==NNPI_HOSTREADY_OBSERVE_IOCTL ||
        IoControlCode==NNPI_RESET_SUBMIT_IOCTL || IoControlCode==NNPI_RESET_REVALIDATE_IOCTL) {
        WdfRequestComplete(Request,STATUS_NOT_SUPPORTED); return;
    }
    if (IoControlCode==NNPI_HOSTREADY_SUBMIT_IOCTL || IoControlCode==NNPI_HOSTREADY_OBSERVE_IOCTL) {
        NnpiHostReadyRequest(context,Request,OutputBufferLength,InputBufferLength,IoControlCode);
        return;
    }
    if (IoControlCode == NNPI_RESET_SUBMIT_IOCTL || IoControlCode == NNPI_RESET_QUERY_IOCTL ||
        IoControlCode == NNPI_RESET_REVALIDATE_IOCTL) {
        NnpiResetRequest(context, Request, OutputBufferLength, InputBufferLength, IoControlCode);
        return;
    }
    if (IoControlCode == NNPI_RESETCAPS_IOCTL) {
        NnpiResetCapsRequest(context, Request, OutputBufferLength, InputBufferLength);
        return;
    }
    if (IoControlCode == NNPI_DMACHECK_IOCTL) {
        NnpiDmaCheckRequest(context, Request, OutputBufferLength, InputBufferLength);
        return;
    }
    if (IoControlCode == NNPI_SNAPSHOT_IOCTL) {
        NnpiControlSnapshotRequest(context, Request, OutputBufferLength, InputBufferLength);
        return;
    }
    if (IoControlCode == NNPI_STATUS_IOCTL) {
        NnpiStatusRequest(context, Request, OutputBufferLength, InputBufferLength);
        return;
    }
    if (IoControlCode == NNPI_DIAG_IOCTL) {
        NnpiDiagnosticRequest(context, Request, OutputBufferLength, InputBufferLength);
        return;
    }
    if (IoControlCode != NNPI_INFO_IOCTL) {
        status = STATUS_INVALID_DEVICE_REQUEST;
        goto Complete;
    }
    if (InputBufferLength != sizeof(input)) {
        status = STATUS_INFO_LENGTH_MISMATCH;
        goto Complete;
    }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), &buffer, NULL);
    if (!NT_SUCCESS(status)) {
        goto Complete;
    }
    /* METHOD_BUFFERED aliases input/output: copy all 16 bytes before writing. */
    RtlCopyMemory(&input, buffer, sizeof(input));
    if (!nnpi_info_request_valid(&input, InputBufferLength)) {
        status = STATUS_INVALID_PARAMETER;
        goto Complete;
    }
    if (OutputBufferLength < sizeof(snapshot)) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto Complete;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(snapshot), &buffer, NULL);
    if (!NT_SUCCESS(status)) {
        goto Complete;
    }
    RtlZeroMemory(&snapshot, sizeof(snapshot));
    WdfWaitLockAcquire(context->CacheLock, NULL);
    if (context->Removed) {
        status = STATUS_DEVICE_REMOVED;
    } else if (!(context->Cache.flags & NNPI_INFO_FLAG_PREPARED) ||
               !(context->Cache.flags & NNPI_INFO_FLAG_D0)) {
        status = STATUS_DEVICE_NOT_READY;
    } else {
        snapshot = context->Cache;
        status = STATUS_SUCCESS;
    }
    WdfWaitLockRelease(context->CacheLock);
    if (NT_SUCCESS(status)) {
        /* Point-in-time snapshot; removal can happen after releasing the lock. */
        RtlCopyMemory(buffer, &snapshot, sizeof(snapshot));
        information = sizeof(snapshot);
    }
Complete:
    /* Single synchronous owner. No pend, forwarding, or cancel registration. */
    WdfRequestCompleteWithInformation(Request, status, information);
}

_Use_decl_annotations_
VOID
NnpiIoStop(WDFQUEUE Queue, WDFREQUEST Request, ULONG ActionFlags)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(ActionFlags);
    /* SynchronizationScopeNone allows dispatch/stop to race. Dispatch remains
     * the sole completion owner: no stop-side completion, acknowledgment, requeue
     * or cancel registration for an in-flight synchronous register read. WDF
     * waits for dispatch completion. There is no asynchronous work to abort.
     * Unlike INFO/diag, status/snapshot are NOT CPU-only: any PCIe read can stall. Neither
     * this callback nor CancelIoEx can impose a hardware-read timeout. A hung
     * read can delay queue stop/power/removal indefinitely; do not claim bounded
     * completion. D0Exit/removal use the same lock as the actual read, preventing
     * subsequent accesses after their state transition, but cannot eliminate a
     * physical unplug racing an already issued read. No retry or automatic read.
     */
}


/* TASK026: CacheLock serializes admission, staged bytes, MMIO and lifecycle.
 * A single Io work item owns every boot/reset hardware action. The successful
 * worker deliberately remains resident until PnP closes it; it consumes one
 * system worker thread. No timers, request cancellation callbacks or retries. */
static VOID NnpiBootFail(PNNPI_DEVICE_CONTEXT c,ULONG Reason,NTSTATUS Status)
{
    c->RuntimeHealthy=FALSE;
    if (c->RuntimeEntered) {
        /* Runtime parse/fatal/MMIO/partial publication must never trigger an
         * automatic FLR. Preserve all publication + parser/first-error evidence.
         * Closing/worker completion is NOT device-DMA cessation. */
        c->RuntimeFaultRetained=TRUE; c->NetData.stats.fault_retained=1;
        c->ResetBlocked=TRUE; c->BootClosing=TRUE; c->Boot.closing=1;
        c->NetClosing=TRUE; c->Net.closing=1;
        KeSetEvent(&c->BootClose,IO_NO_INCREMENT,FALSE);
        KeSetEvent(&c->NetClose,IO_NO_INCREMENT,FALSE);
    }
    if (!c->BootDiagnostic.FirstFailureReason) {
        c->BootDiagnostic.FirstFailurePhase=c->BootSession.phase;
        c->BootDiagnostic.FirstFailureReason=Reason;
        c->BootDiagnostic.FirstFailureStatus=(ULONG)Status;
        c->BootDiagnostic.FirstFailureRuntimeStep=c->BootDiagnostic.RuntimeStep;
    }
    if (!c->Boot.failed_stage) {
        c->Boot.failed_stage=Reason; c->Boot.ntstatus=(ULONG)Status;
    }
    c->Boot.load_state=NNPI_BOOT_LOAD_FAILED;
    nnpi_boot_session_abort(&c->BootSession);
}
static VOID NnpiBootSnapshot(PNNPI_DEVICE_CONTEXT c)
{
    c->Boot.phase=c->BootSession.phase;
    c->Boot.hostready_writes=c->BootSession.hostready_used;
    c->Boot.payload_valid=c->BootSession.payload_valid;
    c->Boot.boot_started=c->BootSession.boot_started;
    c->Boot.driver_ready=c->BootSession.driver_ready;
    c->Boot.published_dma=c->BootSession.published_dma;
    c->Boot.stopped_dma=c->BootSession.stopped_dma;
    c->Boot.reset_used=c->BootSession.reset_used;
    c->Boot.reset_returned=c->BootSession.reset_returned;
    c->Boot.closing=c->BootClosing;
}
static NTSTATUS NnpiBootBudget(VOID)
{
    /* r11 compatibility shim: the user removed persistent boot attempt limits.
     * No registry, ledger, experiment grant or other I/O is performed here.
     * This is NOT an authorization grant or a rearm: BootOwner singleflight,
     * session/phase admission and DMA stop/retention rules remain unchanged. */
    return STATUS_SUCCESS;
}
static NTSTATUS NnpiBootSeal(PNNPI_DEVICE_CONTEXT c)
{
    static const UCHAR expected[32]={0x94,0xce,0x7a,0xf6,0xa1,0x0a,0x4f,0xa7,
        0x58,0x0b,0xfe,0xd2,0xd7,0x43,0x7b,0xdb,0x23,0x34,0x81,0x06,
        0x37,0x8a,0x63,0x04,0x96,0x12,0x9a,0xe3,0x9e,0xc2,0xf2,0xe1};
    /* Linux bootimage.c:83,257 EFI capsule GUID, little-endian wire fields.
     * Reject matching GUID even if a malicious capsule lies about its size. */
    static const UCHAR capsule[16]={0xed,0xd5,0xcb,0x6d,0x2d,0xe8,0x44,0x4c,
        0xbd,0xa1,0x71,0x94,0x19,0x9a,0xd9,0x2a};
    BCRYPT_ALG_HANDLE algorithm=NULL; BCRYPT_HASH_HANDLE hash=NULL;
    PUCHAR object=NULL; ULONG objectBytes=0,returned=0,i;
    NTSTATUS status;
    if (c->Image.WrittenBytes!=NNPI_IMAGE_BYTES) return STATUS_INVALID_BUFFER_SIZE;
    if (RtlCompareMemory(c->Image.Chunks[0].VirtualAddress,capsule,sizeof(capsule))==sizeof(capsule)) {
        c->Boot.capsule_rejected=1;
        NnpiBootFail(c,NNPI_BOOT_FAIL_CAPSULE,STATUS_ACCESS_DENIED); return STATUS_ACCESS_DENIED;
    }
    status=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,
        MS_PRIMITIVE_PROVIDER,0);
    if (!NT_SUCCESS(status)) goto End;
    status=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,(PUCHAR)&objectBytes,
        sizeof(objectBytes),&returned,0);
    if (!NT_SUCCESS(status)) goto End;
    if (returned!=sizeof(objectBytes) || !objectBytes || objectBytes>1048576) {
        status=STATUS_INVALID_BUFFER_SIZE; goto End;
    }
    object=(PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,objectBytes,'hBnN');
    if (!object) { status=STATUS_INSUFFICIENT_RESOURCES; goto End; }
    status=BCryptCreateHash(algorithm,&hash,object,objectBytes,NULL,0,0);
    if (!NT_SUCCESS(status)) goto End;
    for (i=0;i<NNPI_IMAGE_CHUNK_COUNT;++i) {
        status=BCryptHashData(hash,(PUCHAR)c->Image.Chunks[i].VirtualAddress,
            (ULONG)c->Image.Chunks[i].ImageBytes,0);
        if (!NT_SUCCESS(status)) goto End;
    }
    status=BCryptFinishHash(hash,c->Boot.sha256,sizeof(c->Boot.sha256),0);
    if (!NT_SUCCESS(status)) goto End;
    if (RtlCompareMemory(expected,c->Boot.sha256,sizeof(expected))!=sizeof(expected)) {
        status=STATUS_DATA_ERROR; goto End;
    }
    c->Boot.sha256_verified=1;
    status=NnpiImageSeal(&c->Image,TRUE);
End:
    if (hash) BCryptDestroyHash(hash);
    if (object) { RtlSecureZeroMemory(object,objectBytes); ExFreePoolWithTag(object,'hBnN'); }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    return status;
}
static BOOLEAN NnpiBootCurrent(PNNPI_DEVICE_CONTEXT c)
{
    return !c->Removed && c->Cache.flags==NNPI_DIAG_PREPARED_D0 &&
        c->Boot.generation==c->Cache.generation && c->Boot.epoch==c->Epoch;
}
/* Caller holds CacheLock across its fresh baseline/card read and this gate.
 * The serial is independent of boot observation counts; no implicit re-arm. */
static BOOLEAN NnpiBootIrqGate(PNNPI_DEVICE_CONTEXT c,BOOLEAN Arm,ULONG Card)
{
    struct nnpi_irq_scope scope={0};
    struct nnpi_irq_evidence evidence={0};
    uint32_t reason;
    scope.Flags=c->Cache.flags; scope.Removed=c->Removed;
    scope.Generation=c->Cache.generation; scope.Epoch=c->Epoch;
    evidence.Generation=c->Boot.generation; evidence.Epoch=c->Boot.epoch;
    if (c->IrqSerial!=UINT64_MAX) ++c->IrqSerial;
    evidence.Serial=c->IrqSerial; evidence.Card=Card;
    if (Arm) {
        evidence.Host=c->Boot.host_before;
        evidence.Command=c->Boot.command_before;
        evidence.Response=c->Boot.response_before;
    }
    reason=Arm ? NnpiIrqArm(&c->IrqMapping,&scope,&evidence) :
        NnpiIrqCheck(&c->IrqMapping,&scope,&evidence);
    if (reason!=NNPI_IRQ_OK) {
        NnpiBootFail(c,NNPI_BOOT_FAIL_BASELINE,STATUS_DEVICE_NOT_READY);
        return FALSE;
    }
    return TRUE;
}
/* Caller holds lock. All reads counted; first all-ones evidence is retained. */
static ULONG NnpiBootCard(PNNPI_DEVICE_CONTEXT c)
{
    ULONG card=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_DOORBELL_OFFSET);
    c->Boot.card_last=card; ++c->Boot.observations; return card;
}
static VOID NnpiBootEmit(PNNPI_DEVICE_CONTEXT c,const struct nnpi_fifo_command_plan *plan,
    uint32_t *completed)
{
    ULONG half;
    *completed=0;
    KeMemoryBarrier();
    for (half=0;half<6;++half) {
        ULONGLONG value=plan->writes[half/2].value;
        NnpiWriteSysinfoHalf(&c->SysinfoMapping,half,
            (ULONG)((half&1)?value>>32:value));
        ++*completed;
    }
}
/* Bounds and poll interval are shared with the session policy and supervisor.
 * PCIe MMIO can stall independently. No timeout is proof of DMA stop.
 * The last permitted fresh observation can still establish the ready target. */
static BOOLEAN NnpiBootWait(PNNPI_DEVICE_CONTEXT c,ULONG Desired)
{
    LARGE_INTEGER delay; ULONG i,card; BOOLEAN done=FALSE;
    ULONG limit=nnpi_boot_session_wait_limit(Desired);
    if (!limit) {
        WdfWaitLockAcquire(c->CacheLock,NULL);
        NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
        NnpiBootSnapshot(c); WdfWaitLockRelease(c->CacheLock); return FALSE;
    }
    delay.QuadPart=-(LONGLONG)NNPI_BS_POLL_MS*10000;
    for (i=0;i<limit;++i) {
        KeDelayExecutionThread(KernelMode,FALSE,&delay);
        WdfWaitLockAcquire(c->CacheLock,NULL);
        if (!NnpiBootCurrent(c) || c->BootClosing) {
            NnpiBootFail(c,NNPI_BOOT_FAIL_CLOSING,STATUS_DELETE_PENDING);
        } else {
            __try {
                card=NnpiBootCard(c);
                KeMemoryBarrier();
                if (NnpiBootIrqGate(c,FALSE,card))
                    (void)nnpi_boot_session_observe(&c->BootSession,&c->BootSession.key,
                        c->BootSession.last_read+1,card,
                        (const uint8_t *)c->Dma.VirtualAddress,NNPI_SYSINFO_PAYLOAD_BYTES);
                if (c->BootSession.phase==NNPI_BS_CLEANUP) {
                    if (c->BootSession.timed_out)
                        NnpiBootFail(c,NNPI_BOOT_FAIL_TIMEOUT,STATUS_IO_TIMEOUT);
                    else NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode());
            }
        }
        done=c->BootSession.phase==Desired;
        NnpiBootSnapshot(c);
        if (done || c->Boot.failed_stage) { WdfWaitLockRelease(c->CacheLock); return done; }
        WdfWaitLockRelease(c->CacheLock);
    }
    WdfWaitLockAcquire(c->CacheLock,NULL);
    NnpiBootFail(c,NNPI_BOOT_FAIL_TIMEOUT,STATUS_IO_TIMEOUT);
    NnpiBootSnapshot(c); WdfWaitLockRelease(c->CacheLock); return FALSE;
}
/* 026-r3: bounded MMIO readiness wait after a host reboot. The card's BAR MMIO
 * can read 0xFFFFFFFF until its own logic comes up (observed in 025 and 026-r2,
 * both run immediately after a host reboot; 026-r1 on a long-settled device read
 * normal values). Poll the four baselines with a bound, delaying outside
 * CacheLock. All-ones is still a hard gate after the bound, never repaired.
 * Reads the card doorbell directly (not NnpiBootCard) so these probes do not
 * consume the boot observation counter. Returns TRUE when all four are non-ones. */
static BOOLEAN NnpiBootReadyWait(PNNPI_DEVICE_CONTEXT c)
{
    LARGE_INTEGER delay; ULONG i; BOOLEAN ready=FALSE;
    delay.QuadPart=-(LONGLONG)NNPI_BS_POLL_MS*10000;
    for (i=0;i<NNPI_BS_READY_LIMIT;++i) {
        KeDelayExecutionThread(KernelMode,FALSE,&delay);
        WdfWaitLockAcquire(c->CacheLock,NULL);
        if (!NnpiBootCurrent(c) || c->BootClosing) {
            NnpiBootFail(c,NNPI_BOOT_FAIL_CLOSING,STATUS_DELETE_PENDING);
        } else {
            __try {
                c->Boot.host_before=NnpiReadHostReady(&c->HostReadyMapping);
                c->Boot.command_before=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
                c->Boot.response_before=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_RESPONSE_OFFSET);
                c->Boot.card_before=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_DOORBELL_OFFSET);
                c->Boot.card_last=c->Boot.card_before;
                ready=c->Boot.host_before!=MAXULONG && c->Boot.command_before!=MAXULONG &&
                    c->Boot.response_before!=MAXULONG && c->Boot.card_before!=MAXULONG;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode());
            }
        }
        NnpiBootSnapshot(c);
        if (ready || c->Boot.failed_stage) { WdfWaitLockRelease(c->CacheLock); return ready; }
        WdfWaitLockRelease(c->CacheLock);
    }
    /* Bound exhausted while still all-ones: latch the hard baseline gate. */
    WdfWaitLockAcquire(c->CacheLock,NULL);
    NnpiBootFail(c,NNPI_BOOT_FAIL_BASELINE,STATUS_DEVICE_NOT_READY);
    NnpiBootSnapshot(c);
    WdfWaitLockRelease(c->CacheLock);
    return FALSE;
}
static VOID NnpiBootWorker(PDEVICE_OBJECT DeviceObject,PVOID Parameter)
{
    PNNPI_DEVICE_CONTEXT c=(PNNPI_DEVICE_CONTEXT)Parameter;
    PIO_WORKITEM work=c->BootWork; WDFDEVICE device=c->Device;
    struct nnpi_fifo_command_plan plan={0};
    struct nnpi_boot_session_key key;
    struct nnpi_reset_response reset;
    NTSTATUS status; ULONG card,command; BOOLEAN resetCall=FALSE;
    UNREFERENCED_PARAMETER(DeviceObject);
    WdfWaitLockAcquire(c->CacheLock,NULL);
    key.session_id=c->Boot.session_id; key.generation=c->Boot.generation; key.epoch=c->Boot.epoch;
    if (!NnpiBootCurrent(c) || c->BootClosing) {
        NnpiBootFail(c,NNPI_BOOT_FAIL_CLOSING,STATUS_DELETE_PENDING); goto CleanupLocked;
    }
    status=NnpiBootBudget();
    if (!NT_SUCCESS(status)) { NnpiBootFail(c,NNPI_BOOT_FAIL_BUDGET,status); goto CleanupLocked; }
    /* ABI compatibility: session admitted, NOT a persistent budget marker. */
    c->Boot.budget_consumed=1;
    /* 026-r3: bounded MMIO readiness wait (delays outside CacheLock) before the
     * hard baseline gate and host-ready. A host reboot leaves the card's BAR
     * MMIO reading 0xFFFFFFFF until its own logic comes up; failing fast here
     * would reject a merely-not-yet-ready card. The all-ones gate below holds. */
    WdfWaitLockRelease(c->CacheLock);
    if (!NnpiBootReadyWait(c)) goto Cleanup;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (!NnpiBootCurrent(c) || c->BootClosing) {
        NnpiBootFail(c,NNPI_BOOT_FAIL_CLOSING,STATUS_DELETE_PENDING); goto CleanupLocked;
    }
    __try {
        c->Boot.host_before=NnpiReadHostReady(&c->HostReadyMapping);
        c->Boot.command_before=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        c->Boot.response_before=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_RESPONSE_OFFSET);
        c->Boot.card_before=NnpiBootCard(c);
        /* No repair of 0xffffffff: fail before host-ready or any DMA. */
        if (c->Boot.host_before==MAXULONG || c->Boot.card_before==MAXULONG ||
            c->Boot.command_before==MAXULONG || c->Boot.response_before==MAXULONG ||
            !nnpi_boot_session_begin(&c->BootSession,&key,1,c->Boot.host_before,
                c->Boot.card_before,c->Boot.command_before,c->Boot.response_before) ||
            !nnpi_boot_session_reserve_cleanup(&c->BootSession,&key) ||
            !NnpiBootIrqGate(c,TRUE,c->Boot.card_before) ||
            !nnpi_boot_session_hostready(&c->BootSession,&key)) {
            NnpiBootFail(c,NNPI_BOOT_FAIL_BASELINE,STATUS_DEVICE_NOT_READY);
        } else NnpiWriteHostReady(&c->HostReadyMapping);
    } __except(EXCEPTION_EXECUTE_HANDLER) { NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode()); }
    NnpiBootSnapshot(c);
    if (c->Boot.failed_stage) goto CleanupLocked;
    WdfWaitLockRelease(c->CacheLock);
    if (!NnpiBootWait(c,NNPI_BS_BIOS_READY)) goto Cleanup;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (!NnpiBootCurrent(c) || c->BootClosing) {
        NnpiBootFail(c,NNPI_BOOT_FAIL_CLOSING,STATUS_DELETE_PENDING); goto CleanupLocked;
    }
    __try {
        card=NnpiBootCard(c);
        command=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        if (command==MAXULONG || !NnpiBootIrqGate(c,FALSE,card) ||
            !nnpi_boot_session_sysinfo(&c->BootSession,&key,
            c->BootSession.last_read+1,card,command,(uint64_t)c->Dma.LogicalAddress.QuadPart,&plan))
            NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
        else {
            c->Dma.Published=TRUE; c->Dma.StopConfirmed=FALSE;
            RtlZeroMemory(c->Dma.VirtualAddress,c->Dma.Length);
            NnpiBootEmit(c,&plan,&c->Boot.sysinfo_halves);
            if (!nnpi_boot_session_sent(&c->BootSession,&key,c->Boot.sysinfo_halves))
                NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode()); }
    NnpiBootSnapshot(c);
    if (c->Boot.failed_stage) goto CleanupLocked;
    WdfWaitLockRelease(c->CacheLock);
    if (!NnpiBootWait(c,NNPI_BS_SYSINFO_READY)) goto Cleanup;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (!NnpiBootCurrent(c) || c->BootClosing) {
        NnpiBootFail(c,NNPI_BOOT_FAIL_CLOSING,STATUS_DELETE_PENDING); goto CleanupLocked;
    }
    __try {
        card=NnpiBootCard(c);
        command=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        if (command==MAXULONG || !NnpiBootIrqGate(c,FALSE,card) ||
            !nnpi_boot_session_image(&c->BootSession,&key,
            c->BootSession.last_read+1,card,command,(uint64_t)c->Image.Table.LogicalAddress.QuadPart,
            c->Image.Table.MappedBytes,NNPI_IMAGE_CHUNK_COUNT,NNPI_IMAGE_BYTES,&plan))
            NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
        else {
            status=NnpiImageMarkPublished(&c->Image);
            if (!NT_SUCCESS(status)) NnpiBootFail(c,NNPI_BOOT_FAIL_IMAGE,status);
            else {
                NnpiBootEmit(c,&plan,&c->Boot.image_halves);
                if (!nnpi_boot_session_sent(&c->BootSession,&key,c->Boot.image_halves))
                    NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode()); }
    NnpiBootSnapshot(c);
    if (c->Boot.failed_stage) goto CleanupLocked;
    WdfWaitLockRelease(c->CacheLock);
    if (NnpiBootWait(c,NNPI_BS_DRIVER_READY)) NnpiBootRuntime(c);
Cleanup:
    WdfWaitLockAcquire(c->CacheLock,NULL);
CleanupLocked:
    /* Stop all net command producers BEFORE claiming the one boot-owner FLR.
     * Never join under CacheLock; provider reentrancy risk is unchanged. */
    c->RuntimeHealthy=FALSE; c->NetClosing=TRUE; c->Net.closing=1;
    KeSetEvent(&c->NetClose,IO_NO_INCREMENT,FALSE);
    WdfWaitLockRelease(c->CacheLock);
    KeWaitForSingleObject(&c->NetDone,Executive,KernelMode,FALSE,NULL);
    WdfWaitLockAcquire(c->CacheLock,NULL);
    c->ResetBlocked=TRUE; /* reset claim closes every legacy MMIO path too */
    NnpiIrqInvalidate(&c->IrqMapping); /* CPU-only: no cleanup MMIO. */
    if (!c->RuntimeFaultRetained && c->BootSession.hostready_used && !c->BootSession.reset_used &&
        NnpiBootCurrent(c) && c->BootResetInterface.DeviceReset)
        resetCall=(BOOLEAN)nnpi_boot_session_reset_begin(&c->BootSession,&key);
    NnpiBootSnapshot(c);
    WdfWaitLockRelease(c->CacheLock);
    if (resetCall) {
        RtlZeroMemory(&reset,sizeof(reset));
        reset.version=NNPI_RESET_VERSION; reset.size=sizeof(reset);
        reset.operation_id=1; reset.generation=key.generation; reset.epoch=key.epoch;
        reset.completion_status=NNPI_RESET_PENDING; reset.hardware_blocked=1;
        /* Reset was linearized under CacheLock before releasing it. A later
         * Surprise is an already-in-flight operation, never a new reset claim.
         * Do not hold CacheLock across provider (which may call PnP itself).
         * RETURN_ONLY: no callback, flags 0, NULL parameters, exact zero only.
         * Provider and session stay retained across a possibly unbounded call. */
        c->BootDiagnostic.ResetEntered=1;
        __try { status=c->BootResetInterface.DeviceReset(c->BootResetInterface.Context,
            FunctionLevelDeviceReset,0,NULL);
            c->BootDiagnostic.ResetReturned=1; }
        __except(EXCEPTION_EXECUTE_HANDLER) { status=GetExceptionCode(); }
        reset.call_returned=c->BootDiagnostic.ResetReturned; reset.call_status=(ULONG)status;
        reset.state=reset.call_returned && status==STATUS_SUCCESS ? NNPI_RESET_COMPLETE : NNPI_RESET_FAILED;
        WdfWaitLockAcquire(c->CacheLock,NULL);
        c->BootDiagnostic.ResetStatus=(ULONG)status;
        c->Boot.reset_status=(ULONG)status;
        if (nnpi_boot_session_stopped(&c->BootSession,&key,NNPI_BS_RESET_RETURN_ONLY,0,&reset)) {
            c->Dma.StopConfirmed=TRUE; c->Image.StopConfirmed=TRUE;
            nnpi_net_owner_record_stop(&c->NetOwner,&key,reset.call_returned,(ULONG)status,1);
        } else {
            nnpi_net_owner_record_stop(&c->NetOwner,&key,reset.call_returned,(ULONG)status,0);
            NnpiBootFail(c,NNPI_BOOT_FAIL_RESET,status);
        }
        WdfWaitLockRelease(c->CacheLock);
    }
    WdfWaitLockAcquire(c->CacheLock,NULL);
    NnpiBootSnapshot(c); c->Boot.worker_active=0;
    if (c->BootResetInterface.InterfaceDereference) {
        c->BootResetInterface.InterfaceDereference(c->BootResetInterface.Context);
        RtlZeroMemory(&c->BootResetInterface,sizeof(c->BootResetInterface));
    }
    c->BootWork=NULL;
    KeSetEvent(&c->BootDone,IO_NO_INCREMENT,FALSE);
    NnpiNetFinalize(c);
    WdfWaitLockRelease(c->CacheLock);
    WdfObjectDereference(device); IoFreeWorkItem(work);
    ExReleaseRundownProtection(&NnpiSysinfoRundown);
    /* I/O manager retains device/image until actual callback return. */
}
static VOID NnpiBootBarrier(PNNPI_DEVICE_CONTEXT c)
{
    BOOLEAN started,safe; LARGE_INTEGER delay;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    c->RuntimeHealthy=FALSE; c->BootClosing=TRUE; c->Boot.closing=1; started=c->BootStarted;
    c->NetClosing=TRUE; c->Net.closing=1;
    KeSetEvent(&c->NetClose,IO_NO_INCREMENT,FALSE);
    KeSetEvent(&c->BootClose,IO_NO_INCREMENT,FALSE);
    WdfWaitLockRelease(c->CacheLock);
    if (!started) return;
    KeWaitForSingleObject(&c->BootDone,Executive,KernelMode,FALSE,NULL);
    delay.QuadPart=-5000000;
    for (;;) {
        WdfWaitLockAcquire(c->CacheLock,NULL);
        safe=(BOOLEAN)(nnpi_boot_session_may_release(&c->BootSession,NNPI_BS_ALL_DMA) &&
            (!c->Dma.Published || c->Dma.StopConfirmed) &&
            (!c->Image.Published || c->Image.StopConfirmed));
        WdfWaitLockRelease(c->CacheLock);
        if (safe) return;
        /* Fail-stop BEFORE WDF parent Dispose, not merely a child reference. */
        KeDelayExecutionThread(KernelMode,FALSE,&delay);
    }
}
static VOID NnpiBootRequest(PNNPI_DEVICE_CONTEXT c,WDFREQUEST Request,
    size_t OutputBytes,size_t InputBytes,ULONG Code)
{
    struct nnpi_boot_request input; struct nnpi_boot_response output;
    PVOID inBuffer,outBuffer; NTSTATUS status; ULONG_PTR information=0;
    ULONG i; BOOLEAN query=Code==NNPI_BOOT_QUERY_IOCTL;
    RtlZeroMemory(&output,sizeof(output));
    if (InputBytes<sizeof(input) || OutputBytes!=sizeof(output)) {
        status=STATUS_INFO_LENGTH_MISMATCH; goto End;
    }
    status=WdfRequestRetrieveInputBuffer(Request,InputBytes,&inBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    RtlCopyMemory(&input,inBuffer,sizeof(input));
    if (input.version!=NNPI_BOOT_ABI_VERSION || input.size!=sizeof(input)) {
        status=STATUS_INVALID_PARAMETER; goto End;
    }
    for (i=0;i<3;++i) if (input.reserved[i]) { status=STATUS_INVALID_PARAMETER; goto End; }
    if (InputBytes!=sizeof(input)+(size_t)input.chunk_bytes ||
        input.chunk_bytes>NNPI_BOOT_CHUNK_MAX ||
        (Code!=NNPI_BOOT_CHUNK_IOCTL && (input.chunk_bytes || input.offset))) {
        status=STATUS_INVALID_PARAMETER; goto End;
    }
    if (query && (input.session_id || input.generation || input.epoch || input.image_bytes)) {
        status=STATUS_INVALID_PARAMETER; goto End;
    }
    status=WdfRequestRetrieveOutputBuffer(Request,sizeof(output),&outBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (query) { status=STATUS_SUCCESS; goto Copy; }
    if (c->Removed || c->BootClosing ||
        c->Cache.flags!=NNPI_DIAG_PREPARED_D0 || input.image_bytes!=NNPI_BOOT_IMAGE_BYTES ||
        !input.generation || !input.epoch || input.generation==UINT64_MAX || input.epoch==UINT64_MAX ||
        input.generation!=c->Cache.generation || input.epoch!=c->Epoch) {
        status=STATUS_DEVICE_NOT_READY; goto UnlockBoot;
    }
    if (Code==NNPI_BOOT_BEGIN_IOCTL) {
        if (input.session_id || c->Boot.load_state!=NNPI_BOOT_LOAD_EMPTY ||
            InterlockedCompareExchange(&NnpiBootOwner,1,0)!=0) {
            status=STATUS_DEVICE_BUSY; goto UnlockBoot;
        }
        /* Random per-load session identity, never an old reset operation/cache. */
        status=BCryptGenRandom(NULL,(PUCHAR)&c->Boot.session_id,sizeof(c->Boot.session_id),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        c->Boot.generation=input.generation; c->Boot.epoch=input.epoch;
        c->Boot.load_state=NNPI_BOOT_LOAD_LOADING;
        if (!NT_SUCCESS(status) || !c->Boot.session_id || c->Boot.session_id==UINT64_MAX) {
            NnpiBootFail(c,NNPI_BOOT_FAIL_IMAGE,STATUS_UNSUCCESSFUL); goto CopySuccess;
        }
        status=NnpiImagePrepare(c->Device,&c->Image);
        if (!NT_SUCCESS(status)) NnpiBootFail(c,NNPI_BOOT_FAIL_IMAGE,status);
    } else {
        if (input.session_id!=c->Boot.session_id || !input.session_id ||
            input.generation!=c->Boot.generation || input.epoch!=c->Boot.epoch) {
            status=STATUS_INVALID_PARAMETER; goto UnlockBoot;
        }
        if (Code==NNPI_BOOT_CHUNK_IOCTL) {
            if (c->Boot.load_state!=NNPI_BOOT_LOAD_LOADING) { status=STATUS_INVALID_DEVICE_STATE; goto UnlockBoot; }
            status=NnpiImageWriteChunk(&c->Image,(size_t)input.offset,
                (PUCHAR)inBuffer+sizeof(input),input.chunk_bytes);
            c->Boot.loaded_bytes=c->Image.WrittenBytes;
            if (!NT_SUCCESS(status)) NnpiBootFail(c,NNPI_BOOT_FAIL_IMAGE,status);
        } else if (Code==NNPI_BOOT_SEAL_IOCTL) {
            if (c->Boot.load_state!=NNPI_BOOT_LOAD_LOADING) { status=STATUS_INVALID_DEVICE_STATE; goto UnlockBoot; }
            status=NnpiBootSeal(c);
            if (!NT_SUCCESS(status)) NnpiBootFail(c,NNPI_BOOT_FAIL_HASH,status);
            else c->Boot.load_state=NNPI_BOOT_LOAD_SEALED;
        } else if (Code==NNPI_BOOT_SUBMIT_IOCTL) {
            if (c->Boot.load_state!=NNPI_BOOT_LOAD_SEALED || c->BootStarted ||
                !c->Dma.Cache.passed || !c->Dma.Cache.retained || !c->Dma.VirtualAddress ||
                c->Dma.Length<NNPI_SYSINFO_PAYLOAD_BYTES || c->Dma.Published || !c->HostReadyMapping.Address || !c->SnapshotMapping.Address ||
                !c->SysinfoMapping.Address || !c->RuntimeMapping.Address || !c->ResetInterface.DeviceReset ||
                !c->ResetInterface.InterfaceReference || !c->ResetInterface.InterfaceDereference ||
                !(c->ResetInterface.SupportedResetTypes&NNPI_RESETCAPS_FUNCTION_LEVEL)) {
                status=STATUS_DEVICE_NOT_READY; goto UnlockBoot;
            }
            c->BootWork=IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(c->Device));
            if (!c->BootWork) { NnpiBootFail(c,NNPI_BOOT_FAIL_IMAGE,STATUS_INSUFFICIENT_RESOURCES); goto CopySuccess; }
            if (!ExAcquireRundownProtection(&NnpiSysinfoRundown)) {
                IoFreeWorkItem(c->BootWork); c->BootWork=NULL;
                NnpiBootFail(c,NNPI_BOOT_FAIL_CLOSING,STATUS_DELETE_PENDING); goto CopySuccess;
            }
            c->BootResetInterface=c->ResetInterface;
            c->BootResetInterface.InterfaceReference(c->BootResetInterface.Context);
            WdfObjectReference(c->Device);
            c->BootStarted=TRUE; c->Boot.worker_active=1;
            c->Boot.load_state=NNPI_BOOT_LOAD_SUBMITTED;
            KeClearEvent(&c->BootDone);
            IoQueueWorkItem(c->BootWork,NnpiBootWorker,DelayedWorkQueue,c);
        } else { status=STATUS_INVALID_DEVICE_REQUEST; goto UnlockBoot; }
    }
CopySuccess:
    status=STATUS_SUCCESS;
Copy:
    NnpiBootSnapshot(c); output=c->Boot;
    if (!output.session_id) { output.generation=c->Cache.generation; output.epoch=c->Epoch; }
    RtlCopyMemory(outBuffer,&output,sizeof(output)); information=sizeof(output);
UnlockBoot:
    WdfWaitLockRelease(c->CacheLock);
End:
    WdfRequestCompleteWithInformation(Request,status,information);
}

/* 027 minimal network channel lifecycle. One non-context channel, one H2C and
 * one C2H page pool (16 x 4096 each), ETH_CONFIG, then READY. Frame TX/RX are a
 * separate data plane, not this lifecycle. r14 admits only the negotiated gate.
 * BootRuntime alone consumes FIFO responses; boot owns the single cleanup FLR. */
static VOID NnpiNetFail(PNNPI_DEVICE_CONTEXT c, ULONG Reason, NTSTATUS Status)
{
    if (!c->Net.failed_stage) {
        c->Net.failed_stage=Reason; c->Net.ntstatus=(ULONG)Status;
    }
    nnpi_net_session_abort(&c->NetSession);
}
static VOID NnpiNetSnapshot(PNNPI_DEVICE_CONTEXT c)
{
    /* phase/confirmation/published-DMA advance with the state machine. chan_id,
     * card_ip and mac are CONFIGURATION: set once by NnpiNetRequest and never
     * overwritten here, otherwise the synchronous BEGIN reply would read the
     * still-zero NetSession and lose the kernel-assigned id and IP echo. */
    c->Net.phase=c->NetSession.phase;
    c->Net.chan_created=c->NetSession.chan_created;
    c->Net.h2c_rb_published=c->NetSession.h2c_rb_published;
    c->Net.c2h_rb_published=c->NetSession.c2h_rb_published;
    c->Net.eth_configured=c->NetSession.eth_configured;
    c->Net.published_dma=c->NetSession.published_dma;
    c->Net.stopped_dma=c->NetSession.stopped_dma;
    c->Net.observations=c->NetSession.observations;
    c->Net.closing=c->NetClosing;
}
static BOOLEAN NnpiNetCurrent(PNNPI_DEVICE_CONTEXT c)
{
    return !c->Removed && c->Cache.flags==NNPI_DIAG_PREPARED_D0 &&
        c->Net.generation==c->Cache.generation && c->Net.epoch==c->Epoch;
}
/* CacheLock held. The same gate protects admission and every command. */
static ULONG NnpiNetGate(PNNPI_DEVICE_CONTEXT c,uint64_t generation,uint64_t epoch,
    ULONG channel,ULONG ip,const uint8_t mac[6],BOOLEAN continuing)
{
    struct nnpi_net_gate g={0};
    g.compatible=c->RuntimeCompatible; g.healthy=c->RuntimeHealthy;
    g.boot_worker=c->BootStarted && c->Boot.worker_active &&
        !KeReadStateEvent(&c->BootDone);
    g.boot_phase=c->BootSession.phase; g.driver_ready=c->BootSession.driver_ready;
    g.closing=c->BootClosing || c->NetClosing;
    g.reset=c->ResetBlocked || c->BootSession.reset_used;
    g.failed=c->Boot.failed_stage || c->Net.failed_stage || c->BootSession.invalidated;
    g.started=c->NetStarted;
    g.available=!c->Removed && c->Cache.flags==NNPI_DIAG_PREPARED_D0 &&
        c->SysinfoMapping.Address && c->SnapshotMapping.Address && c->RuntimeMapping.Address;
    g.generation=c->Cache.generation; g.epoch=c->Epoch;
    g.boot_generation=c->BootSession.key.generation; g.boot_epoch=c->BootSession.key.epoch;
    g.version=&c->RuntimeVersion;
    if (continuing && !nnpi_net_owner_matches(&c->NetOwner,&c->BootSession,
                                             &c->NetOwner.net))
        return NNPI_NET_GATE_NOT_READY;
    return nnpi_net_control_gate(&g,generation,epoch,channel,ip,mac,continuing);
}
static BOOLEAN NnpiNetCommandGate(PNNPI_DEVICE_CONTEXT c)
{
    return NnpiNetGate(c,c->Net.generation,c->Net.epoch,c->Net.chan_id,
        c->Net.card_ip,c->Net.mac,TRUE)==NNPI_NET_GATE_READY;
}
static NTSTATUS NnpiNetBudget(VOID)
{
    /* Compatibility field means session acceptance only. No persistent I/O;
     * old Net027Consumed records are neither consulted nor deleted. */
    return STATUS_SUCCESS;
}
/* CPU-only propagation of the boot owner's exact associated stop evidence.
 * Called with CacheLock, only after both producers have stopped hardware use. */
static VOID NnpiNetFinalize(PNNPI_DEVICE_CONTEXT c)
{
    if (!nnpi_net_session_matches(&c->NetSession,&c->NetOwner.net) ||
        !nnpi_net_owner_finalize(&c->NetOwner,&c->BootSession,&c->NetOwner.net,
            KeReadStateEvent(&c->BootDone)!=0,KeReadStateEvent(&c->NetDone)!=0,
            c->NetSession.published_dma,c->NetDma.Published)) return;
    if (c->NetSession.published_dma)
        (void)nnpi_net_session_stopped(&c->NetSession,c->NetSession.published_dma);
    if (c->NetDma.Published) c->NetDma.StopConfirmed=TRUE;
    NnpiNetSnapshot(c);
}
/* Generic command emit: expand the plan to halves and write each with the
 * whitelisted push-port helper. completed is the exact half count on success. */
static VOID NnpiNetEmit(PNNPI_DEVICE_CONTEXT c, const struct nnpi_fifo_command_plan *plan,
    uint32_t *completed)
{
    struct nnpi_fifo_half_write halves[NNPI_FIFO_HALF_MAX];
    uint32_t count, i;
    *completed=0;
    if (nnpi_fifo_plan_halves(plan, halves, NNPI_FIFO_HALF_MAX, &count)!=NNPI_FIFO_OK)
        return;
    KeMemoryBarrier();
    for (i=0;i<count;++i) {
        NnpiWriteCommandHalf(&c->SysinfoMapping, halves[i].relative_offset, halves[i].value);
        ++*completed;
    }
}
/* Data gate is stronger than the control handshake gate. CacheLock held. */
static BOOLEAN NnpiNetDataCurrent(PNNPI_DEVICE_CONTEXT c)
{
    struct nnpi_net_session_key key;
    key.session_id=c->Net.session_id; key.generation=c->Net.generation; key.epoch=c->Net.epoch;
    return !c->RuntimeFaultRetained && NnpiNetCurrent(c) && NnpiNetCommandGate(c) &&
        nnpi_net_session_matches(&c->NetSession,&key) &&
        nnpi_net_owner_matches(&c->NetOwner,&c->BootSession,&key) &&
        c->NetSession.phase==NNPI_NS_READY && c->NetSession.eth_configured &&
        c->NetSession.chan_id==c->Net.chan_id && c->NetDma.Prepared &&
        c->NetDma.Published && !c->NetDma.StopConfirmed;
}
static NTSTATUS NnpiNetReceiveDescriptor(PNNPI_DEVICE_CONTEXT c,uint64_t qw)
{
    uint16_t length=0; uint8_t slot=0; PVOID page; int kind;
    kind=nnpi_net_data_classify(&c->NetData,qw,(uint16_t)c->Net.chan_id,
        NnpiNetDataCurrent(c),&length,&slot);
    if (kind<0) return STATUS_DEVICE_PROTOCOL_ERROR;
    if (kind!=1) return STATUS_SUCCESS; /* Includes ACK: never ACK an ACK. */
    page=NnpiNetDmaSlotAddress(&c->NetDma,FALSE,slot);
    if (!page) return STATUS_INVALID_ADDRESS;
    /* x64 WDF common buffers are DMA-coherent. The card queues C2H40 from
     * its DMA-completion callback. Acquire before CPU copy; publish ACK only
     * after copy/drop. A barrier is NOT substituted for noncoherent DMA sync. */
    KeMemoryBarrier();
    if (!nnpi_net_data_receive(&c->NetData,slot,length,page)) return STATUS_DEVICE_PROTOCOL_ERROR;
    return STATUS_SUCCESS;
}
/* Sole data command producer: called by BootRuntime under CacheLock, never by
 * RX parsing or IOCTL. At most 16 descriptors per poll, no delay/spin on full.
 * Re-read FIFO for EACH plan. Pending is retained on full; ambiguous writes
 * retain ownership and fault, not replay. Control producers use the same lock. */
static NTSTATUS NnpiNetDataPump(PNNPI_DEVICE_CONTEXT c)
{
    unsigned n,j; uint8_t slot,bytes[8]; int ack,rc; uint64_t qw;
    ULONG command; struct nnpi_fifo_command_plan plan;
    if (!NnpiNetDataCurrent(c)) return STATUS_SUCCESS;
    for (n=0;n<NNPI_ETH_SLOTS;++n) {
        if (!nnpi_net_data_next(&c->NetData,(uint16_t)c->Net.chan_id,&qw,&slot,&ack)) break;
        command=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        if (command==MAXULONG) return STATUS_DEVICE_DATA_ERROR;
        for (j=0;j<8;++j) bytes[j]=(uint8_t)(qw>>(j*8));
        rc=nnpi_fifo_plan_command(command,bytes,sizeof(bytes),&plan);
        if (rc==NNPI_FIFO_NO_SPACE) {
            nnpi_net_data_inc(&c->NetData.stats.fifo_full); break;
        }
        if (rc!=NNPI_FIFO_OK) return STATUS_DEVICE_DATA_ERROR;
        c->NetData.stats.last_emit_halves=0;
        NnpiNetEmit(c,&plan,&c->NetData.stats.last_emit_halves);
        if (c->NetData.stats.last_emit_halves!=2) return STATUS_DEVICE_PROTOCOL_ERROR;
        nnpi_net_data_sent(&c->NetData,slot,ack);
    }
    return STATUS_SUCCESS;
}
static VOID NnpiNetRxRequest(PNNPI_DEVICE_CONTEXT c,WDFREQUEST Request,
    size_t OutputBytes,size_t InputBytes,ULONG Code)
{
    PVOID inBuffer,outBuffer; NTSTATUS status; ULONG_PTR information=0;
    struct nnpi_net_rx_batch_request input;
    struct nnpi_net_rx_claim_response response={0};
    struct nnpi_net_rx_batch_response *batch;
    WDFFILEOBJECT file=WdfRequestGetFileObject(Request);
    BOOLEAN receive=Code==NNPI_NET_RX_BATCH_IOCTL;
    if (InputBytes!=sizeof(input) || OutputBytes!=(receive?
        sizeof(struct nnpi_net_rx_batch_response):sizeof(response))) {
        status=STATUS_INFO_LENGTH_MISMATCH; goto End;
    }
    status=WdfRequestRetrieveInputBuffer(Request,InputBytes,&inBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    status=WdfRequestRetrieveOutputBuffer(Request,OutputBytes,&outBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    /* Claim reserved[0] occupies max_frames. Copy BEFORE zeroing aliased output. */
    RtlCopyMemory(&input,inBuffer,sizeof(input));
    if (input.version!=NNPI_NET_RX_VERSION || input.size!=sizeof(input) ||
        !input.session_id || input.session_id==UINT64_MAX ||
        !input.generation || input.generation==UINT64_MAX ||
        !input.epoch || input.epoch==UINT64_MAX ||
        input.reserved[0] || input.reserved[1] || input.reserved[2] ||
        (receive?(input.max_frames<1 || input.max_frames>NNPI_NET_RX_QUEUE_DEPTH):input.max_frames!=0)) {
        status=STATUS_INVALID_PARAMETER; goto End;
    }
    if (!file) { status=STATUS_ACCESS_DENIED; goto End; }
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (NnpiGetFileContext(file)->Cleaned || WdfRequestIsCanceled(Request)) {
        status=STATUS_CANCELLED; goto Unlock;
    }
    if (!NnpiNetDataCurrent(c) || input.session_id!=c->Net.session_id ||
        input.generation!=c->Net.generation || input.epoch!=c->Net.epoch) {
        status=STATUS_DEVICE_NOT_READY; goto Unlock;
    }
    if (Code==NNPI_NET_RX_CLAIM_IOCTL) {
        if (c->NetConsumer && c->NetConsumer!=file) {
            status=STATUS_SHARING_VIOLATION; goto Unlock;
        }
        if (!c->NetConsumer) {
            /* Never let pre-claim legacy TX appear after bridge acquisition. */
            if (c->NetData.tx.busy) { status=STATUS_DEVICE_BUSY; goto Unlock; }
            c->NetConsumer=file;
            c->NetConsumerKey.session_id=input.session_id;
            c->NetConsumerKey.generation=input.generation;
            c->NetConsumerKey.epoch=input.epoch;
        }
    } else if (c->NetConsumer!=file) {
        status=STATUS_ACCESS_DENIED; goto Unlock;
    }
    if (c->NetConsumerKey.session_id!=input.session_id ||
        c->NetConsumerKey.generation!=input.generation || c->NetConsumerKey.epoch!=input.epoch) {
        status=STATUS_DEVICE_NOT_READY; goto Unlock;
    }
    if (receive) {
        batch=(struct nnpi_net_rx_batch_response *)outBuffer;
        RtlZeroMemory(batch,sizeof(*batch));
        batch->version=NNPI_NET_RX_VERSION; batch->size=sizeof(*batch);
        batch->session_id=input.session_id; batch->generation=input.generation; batch->epoch=input.epoch;
        nnpi_net_data_dequeue(&c->NetData,input.max_frames,batch);
        information=sizeof(*batch);
    } else {
        if (Code==NNPI_NET_RX_RELEASE_IOCTL) {
            c->NetConsumer=NULL;
            RtlZeroMemory(&c->NetConsumerKey,sizeof(c->NetConsumerKey));
        }
        response.version=NNPI_NET_RX_VERSION; response.size=sizeof(response);
        response.session_id=input.session_id; response.generation=input.generation; response.epoch=input.epoch;
        response.claimed=Code==NNPI_NET_RX_CLAIM_IOCTL;
        response.queue_depth=NNPI_NET_RX_QUEUE_DEPTH; response.max_frame_bytes=NNPI_NET_RX_FRAME_BYTES;
        RtlCopyMemory(outBuffer,&response,sizeof(response)); information=sizeof(response);
    }
    status=STATUS_SUCCESS;
Unlock:
    WdfWaitLockRelease(c->CacheLock);
End:
    WdfRequestCompleteWithInformation(Request,status,information);
}
static VOID NnpiNetDataRequest(PNNPI_DEVICE_CONTEXT c,WDFREQUEST Request,
    size_t OutputBytes,size_t InputBytes,ULONG Code)
{
    PVOID inBuffer,outBuffer,page; NTSTATUS status; ULONG_PTR information=0;
    struct nnpi_net_data_request query;
    const struct nnpi_net_tx_request *tx;
    struct nnpi_net_tx_response response={0}; int slot; unsigned i;
    BOOLEAN observe=Code==NNPI_NET_DATA_QUERY_IOCTL;
    WDFFILEOBJECT file=WdfRequestGetFileObject(Request);
    if (Code==NNPI_NET_RX_CLAIM_IOCTL || Code==NNPI_NET_RX_RELEASE_IOCTL || Code==NNPI_NET_RX_BATCH_IOCTL) {
        NnpiNetRxRequest(c,Request,OutputBytes,InputBytes,Code); return;
    }
    if (InputBytes!=(observe?sizeof(query):sizeof(*tx)) ||
        OutputBytes!=(observe?sizeof(c->NetData.stats):sizeof(response))) {
        status=STATUS_INFO_LENGTH_MISMATCH; goto End;
    }
    status=WdfRequestRetrieveInputBuffer(Request,InputBytes,&inBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    status=WdfRequestRetrieveOutputBuffer(Request,OutputBytes,&outBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    /* METHOD_BUFFERED gives one stable kernel-owned buffer until completion.
     * Do not overwrite it until every input byte has been consumed. */
    tx=(const struct nnpi_net_tx_request *)inBuffer;
    if (observe) {
        RtlCopyMemory(&query,inBuffer,sizeof(query));
        if (query.version!=NNPI_NET_DATA_VERSION || query.size!=sizeof(query) ||
            query.reserved[0] || query.reserved[1]) { status=STATUS_INVALID_PARAMETER; goto End; }
    } else {
        if (tx->version!=NNPI_NET_DATA_VERSION || tx->size!=sizeof(*tx) ||
            tx->length<NNPI_ETH_MIN_FRAME || tx->length>NNPI_ETH_PAGE_BYTES ||
            tx->reserved[0] || tx->reserved[1] || tx->reserved[2]) {
            status=STATUS_INVALID_PARAMETER; goto End;
        }
        for (i=tx->length;i<sizeof(tx->frame);++i) if (tx->frame[i]) {
            status=STATUS_INVALID_PARAMETER; goto End;
        }
    }
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (observe) {
        nnpi_net_data_snapshot(&c->NetData);
        c->NetData.stats.version=NNPI_NET_DATA_VERSION; c->NetData.stats.size=sizeof(c->NetData.stats);
        c->NetData.stats.session_id=c->Net.session_id;
        c->NetData.stats.generation=c->Net.session_id?c->Net.generation:c->Cache.generation;
        c->NetData.stats.epoch=c->Net.session_id?c->Net.epoch:c->Epoch;
        RtlCopyMemory(outBuffer,&c->NetData.stats,sizeof(c->NetData.stats));
        information=sizeof(c->NetData.stats); status=STATUS_SUCCESS;
    } else {
        if (!NnpiNetDataCurrent(c) || tx->session_id!=c->Net.session_id ||
            tx->generation!=c->Net.generation || tx->epoch!=c->Net.epoch) {
            status=STATUS_DEVICE_NOT_READY; goto Unlock;
        }
        if (!file || (c->NetConsumer && c->NetConsumer!=file)) {
            status=STATUS_ACCESS_DENIED; goto Unlock;
        }
        if (NnpiGetFileContext(file)->Cleaned || WdfRequestIsCanceled(Request)) {
            status=STATUS_CANCELLED; goto Unlock;
        }
        if (c->NetConsumer && (c->NetConsumerKey.session_id!=tx->session_id ||
            c->NetConsumerKey.generation!=tx->generation || c->NetConsumerKey.epoch!=tx->epoch)) {
            status=STATUS_DEVICE_NOT_READY; goto Unlock;
        }
        slot=nnpi_eth_slots_reserve(&c->NetData.tx);
        if (slot<0) { status=STATUS_DEVICE_BUSY; goto Unlock; }
        page=NnpiNetDmaSlotAddress(&c->NetDma,TRUE,(ULONG)slot);
        if (!page) {
            (void)nnpi_eth_slots_release(&c->NetData.tx,(unsigned)slot);
            status=STATUS_INVALID_ADDRESS; goto Unlock;
        }
        RtlCopyMemory(page,tx->frame,tx->length);
        KeMemoryBarrier(); /* release CPU copy before later descriptor push */
        c->NetData.tx_lengths[slot]=(uint16_t)tx->length;
        c->NetData.tx_pending=(uint16_t)(c->NetData.tx_pending|(1u<<slot));
        response.version=NNPI_NET_DATA_VERSION; response.size=sizeof(response);
        response.session_id=c->Net.session_id; response.generation=c->Net.generation;
        response.epoch=c->Net.epoch; response.accepted=1; response.slot=(uint32_t)slot;
        RtlCopyMemory(outBuffer,&response,sizeof(response)); information=sizeof(response);
        status=STATUS_SUCCESS;
    }
Unlock:
    WdfWaitLockRelease(c->CacheLock);
End:
    WdfRequestCompleteWithInformation(Request,status,information);
}
/* Cached, bounded observation only. BootRuntime is the sole FIFO consumer.
 * Runtime/error gates precede desired phase, including simultaneous failure. */
static BOOLEAN NnpiNetWait(PNNPI_DEVICE_CONTEXT c, ULONG Desired)
{
    LARGE_INTEGER delay; ULONG i; BOOLEAN done;
    delay.QuadPart=-5000000;
    for (i=0;i<120;++i) {
        KeDelayExecutionThread(KernelMode,FALSE,&delay);
        WdfWaitLockAcquire(c->CacheLock,NULL);
        if (!NnpiNetCurrent(c) || c->NetClosing)
            NnpiNetFail(c,NNPI_NET_FAIL_CLOSING,STATUS_DELETE_PENDING);
        else if (!c->RuntimeHealthy || c->Boot.failed_stage ||
                 nnpi_net_session_failed(&c->NetSession))
            NnpiNetFail(c,NNPI_NET_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
        done=(BOOLEAN)nnpi_runtime_net_wait_ready(c->RuntimeHealthy,
            c->Net.failed_stage,c->NetSession.phase,Desired);
        NnpiNetSnapshot(c);
        if (done || c->Net.failed_stage) { WdfWaitLockRelease(c->CacheLock); return done; }
        WdfWaitLockRelease(c->CacheLock);
    }
    WdfWaitLockAcquire(c->CacheLock,NULL);
    NnpiNetFail(c,NNPI_NET_FAIL_TIMEOUT,STATUS_IO_TIMEOUT);
    NnpiNetSnapshot(c); WdfWaitLockRelease(c->CacheLock); return FALSE;
}
static VOID NnpiNetWorker(PDEVICE_OBJECT DeviceObject, PVOID Parameter)
{
    PNNPI_DEVICE_CONTEXT c=(PNNPI_DEVICE_CONTEXT)Parameter;
    PIO_WORKITEM work=c->NetWork; WDFDEVICE device=c->Device;
    struct nnpi_fifo_command_plan plan={0};
    struct nnpi_net_session_key key;
    uint32_t halves; ULONG command;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(DeviceObject);
    WdfWaitLockAcquire(c->CacheLock,NULL);
    key.session_id=c->Net.session_id; key.generation=c->Net.generation; key.epoch=c->Net.epoch;
    if (!NnpiNetCommandGate(c)) {
        NnpiNetFail(c,NNPI_NET_FAIL_CLOSING,STATUS_DEVICE_NOT_READY); goto CleanupLocked;
    }
    status=NnpiNetBudget();
    if (!NT_SUCCESS(status)) { NnpiNetFail(c,NNPI_NET_FAIL_BUDGET,status); goto CleanupLocked; }
    c->Net.budget_consumed=1;
    if (!nnpi_net_session_begin(&c->NetSession,&key,1,(uint16_t)c->Net.chan_id)) {
        NnpiNetFail(c,NNPI_NET_FAIL_IDENTITY,STATUS_DEVICE_NOT_READY); goto CleanupLocked;
    }
    NnpiNetSnapshot(c);
    WdfWaitLockRelease(c->CacheLock);

    /* CHANNEL_OP create. uid/privileged mirror Linux credentials; 0/0 is the
     * conservative default pending hardware confirmation. */
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (!NnpiNetCommandGate(c)) { NnpiNetFail(c,NNPI_NET_FAIL_CLOSING,STATUS_DEVICE_NOT_READY); goto CleanupLocked; }
    __try {
        command=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        if (command==MAXULONG) NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,STATUS_DEVICE_DATA_ERROR);
        if (command==MAXULONG || !nnpi_net_session_chan_op(&c->NetSession,&key,
            c->NetSession.last_read+1,command,0,0,&plan))
            NnpiNetFail(c,NNPI_NET_FAIL_CHANNEL,STATUS_DEVICE_DATA_ERROR);
        else {
            /* Even a partial create owns device lifecycle, without claiming DMA. */
            c->NetOwner.channel_issued=1;
            NnpiNetEmit(c,&plan,&halves);
            if (!nnpi_net_session_sent(&c->NetSession,&key,halves)) {
                NnpiNetFail(c,NNPI_NET_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
                NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        NnpiNetFail(c,NNPI_NET_FAIL_MMIO,GetExceptionCode());
        NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode());
        c->BootClosing=TRUE; c->Boot.closing=1;
        KeSetEvent(&c->BootClose,IO_NO_INCREMENT,FALSE);
    }
    NnpiNetSnapshot(c);
    if (c->Net.failed_stage) goto CleanupLocked;
    WdfWaitLockRelease(c->CacheLock);
    if (!NnpiNetWait(c,NNPI_NS_CHAN_READY)) goto Cleanup;

    /* H2C ring buffer publication. */
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (!NnpiNetCommandGate(c)) { NnpiNetFail(c,NNPI_NET_FAIL_CLOSING,STATUS_DEVICE_NOT_READY); goto CleanupLocked; }
    __try {
        command=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        if (command==MAXULONG) NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,STATUS_DEVICE_DATA_ERROR);
        if (command==MAXULONG || !nnpi_net_session_rb_op(&c->NetSession,&key,
            c->NetSession.last_read+1,command,1,NnpiNetDmaHostPfn(&c->NetDma,TRUE),&plan))
            NnpiNetFail(c,NNPI_NET_FAIL_RB,STATUS_DEVICE_DATA_ERROR);
        else {
            /* Plan claimed session publication. Validate/claim actual objects
             * under this same lock before the very first RB half-write. */
            status=NnpiNetDmaMarkPublished(&c->NetDma);
            if (!NT_SUCCESS(status)) {
                NnpiNetFail(c,NNPI_NET_FAIL_DMA,status); goto CleanupLocked;
            }
            NnpiNetEmit(c,&plan,&halves);
            if (!nnpi_net_session_sent(&c->NetSession,&key,halves)) {
                NnpiNetFail(c,NNPI_NET_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
                NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        NnpiNetFail(c,NNPI_NET_FAIL_MMIO,GetExceptionCode());
        NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode());
        c->BootClosing=TRUE; c->Boot.closing=1;
        KeSetEvent(&c->BootClose,IO_NO_INCREMENT,FALSE);
    }
    NnpiNetSnapshot(c);
    if (c->Net.failed_stage) goto CleanupLocked;
    WdfWaitLockRelease(c->CacheLock);
    if (!NnpiNetWait(c,NNPI_NS_H2C_RB_READY)) goto Cleanup;

    /* C2H ring buffer publication. */
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (!NnpiNetCommandGate(c)) { NnpiNetFail(c,NNPI_NET_FAIL_CLOSING,STATUS_DEVICE_NOT_READY); goto CleanupLocked; }
    __try {
        command=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        if (command==MAXULONG) NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,STATUS_DEVICE_DATA_ERROR);
        if (command==MAXULONG || !nnpi_net_session_rb_op(&c->NetSession,&key,
            c->NetSession.last_read+1,command,0,NnpiNetDmaHostPfn(&c->NetDma,FALSE),&plan))
            NnpiNetFail(c,NNPI_NET_FAIL_RB,STATUS_DEVICE_DATA_ERROR);
        else {
            NnpiNetEmit(c,&plan,&halves);
            if (!nnpi_net_session_sent(&c->NetSession,&key,halves)) {
                NnpiNetFail(c,NNPI_NET_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
                NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        NnpiNetFail(c,NNPI_NET_FAIL_MMIO,GetExceptionCode());
        NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode());
        c->BootClosing=TRUE; c->Boot.closing=1;
        KeSetEvent(&c->BootClose,IO_NO_INCREMENT,FALSE);
    }
    NnpiNetSnapshot(c);
    if (c->Net.failed_stage) goto CleanupLocked;
    WdfWaitLockRelease(c->CacheLock);
    if (!NnpiNetWait(c,NNPI_NS_C2H_RB_READY)) goto Cleanup;

    /* ETH_CONFIG (2 QW) with the card IP and MAC. */
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (!NnpiNetCommandGate(c)) { NnpiNetFail(c,NNPI_NET_FAIL_CLOSING,STATUS_DEVICE_NOT_READY); goto CleanupLocked; }
    __try {
        command=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        if (command==MAXULONG) NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,STATUS_DEVICE_DATA_ERROR);
        if (command==MAXULONG || !nnpi_net_session_eth_config(&c->NetSession,&key,
            c->NetSession.last_read+1,command,c->Net.card_ip,c->Net.mac,&plan))
            NnpiNetFail(c,NNPI_NET_FAIL_ETH,STATUS_DEVICE_DATA_ERROR);
        else {
            NnpiNetEmit(c,&plan,&halves);
            if (!nnpi_net_session_sent(&c->NetSession,&key,halves)) {
                NnpiNetFail(c,NNPI_NET_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
                NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,STATUS_DEVICE_DATA_ERROR);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        NnpiNetFail(c,NNPI_NET_FAIL_MMIO,GetExceptionCode());
        NnpiBootFail(c,NNPI_BOOT_FAIL_MMIO,GetExceptionCode());
        c->BootClosing=TRUE; c->Boot.closing=1;
        KeSetEvent(&c->BootClose,IO_NO_INCREMENT,FALSE);
    }
    NnpiNetSnapshot(c);
    if (c->Net.failed_stage) goto CleanupLocked;
    WdfWaitLockRelease(c->CacheLock);
    if (NnpiNetWait(c,NNPI_NS_READY)) goto Success;

Cleanup:
    WdfWaitLockAcquire(c->CacheLock,NULL);
CleanupLocked:
    /* Local failure never resets the shared card or frees pools. Boot keeps
     * receiving/keepalive; its eventual teardown owns stop evidence. */
    goto WorkerDoneLocked;
Success:
    WdfWaitLockAcquire(c->CacheLock,NULL);
WorkerDoneLocked:
    NnpiNetSnapshot(c); c->Net.worker_active=0; c->NetWork=NULL;
    KeSetEvent(&c->NetDone,IO_NO_INCREMENT,FALSE);
    WdfWaitLockRelease(c->CacheLock);
    WdfObjectDereference(device); IoFreeWorkItem(work);
    ExReleaseRundownProtection(&NnpiSysinfoRundown);
}
/* PnP-only barrier, after BootBarrier. Never called by either worker, never
 * resets or joins Boot: Boot -> NetDone is the only worker dependency. */
static VOID NnpiNetBarrier(PNNPI_DEVICE_CONTEXT c)
{
    BOOLEAN started,safe; LARGE_INTEGER delay;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    c->NetClosing=TRUE; c->Net.closing=1;
    KeSetEvent(&c->NetClose,IO_NO_INCREMENT,FALSE);
    started=c->NetStarted;
    WdfWaitLockRelease(c->CacheLock);
    if (!started) return;
    KeWaitForSingleObject(&c->NetDone,Executive,KernelMode,FALSE,NULL);
    delay.QuadPart=-5000000;
    for (;;) {
        WdfWaitLockAcquire(c->CacheLock,NULL);
        NnpiNetFinalize(c);
        safe=(BOOLEAN)(nnpi_net_session_may_release(&c->NetSession,NNPI_NS_ALL_DMA) &&
            nnpi_net_owner_may_release(&c->NetOwner) &&
            (!c->NetDma.Published || c->NetDma.StopConfirmed));
        WdfWaitLockRelease(c->CacheLock);
        if (safe) return;
        KeDelayExecutionThread(KernelMode,FALSE,&delay);
    }
}
static VOID NnpiNetRequest(PNNPI_DEVICE_CONTEXT c, WDFREQUEST Request,
    size_t OutputBytes, size_t InputBytes, ULONG Code)
{
    struct nnpi_net_request input; struct nnpi_net_response output;
    PVOID inBuffer,outBuffer; NTSTATUS status; ULONG_PTR information=0;
    BOOLEAN query=Code==NNPI_NET_QUERY_IOCTL;
    RtlZeroMemory(&output,sizeof(output));
    if (InputBytes!=sizeof(input) || OutputBytes!=sizeof(output)) {
        status=STATUS_INFO_LENGTH_MISMATCH; goto End;
    }
    status=WdfRequestRetrieveInputBuffer(Request,sizeof(input),&inBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    RtlCopyMemory(&input,inBuffer,sizeof(input));
    if (input.version!=NNPI_NET_ABI_VERSION || input.size!=sizeof(input)) {
        status=STATUS_INVALID_PARAMETER; goto End;
    }
    if (input.reserved || input.reserved2[0] || input.reserved2[1] ||
        input.reserved2[2] || input.reserved2[3]) {
        status=STATUS_INVALID_PARAMETER; goto End;
    }
    if (query && (input.session_id || input.generation || input.epoch ||
        input.chan_id || input.card_ip || input.mac[0] || input.mac[1] ||
        input.mac[2] || input.mac[3] || input.mac[4] || input.mac[5])) {
        status=STATUS_INVALID_PARAMETER; goto End;
    }
    status=WdfRequestRetrieveOutputBuffer(Request,sizeof(output),&outBuffer,NULL);
    if (!NT_SUCCESS(status)) goto End;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    if (query) { status=STATUS_SUCCESS; goto Copy; }
    if (input.session_id) { status=STATUS_INVALID_PARAMETER; goto Unlock; }
    if (NnpiNetGate(c,input.generation,input.epoch,input.chan_id,input.card_ip,
                    input.mac,FALSE)!=NNPI_NET_GATE_READY) {
        status=STATUS_DEVICE_NOT_READY; goto Unlock;
    }
    /* Consume this compatible session BEFORE RNG/allocation. Preparation or
     * failure is not permission to BEGIN again; no persistent budget exists. */
    c->NetStarted=TRUE;
    c->NetOwner.boot=c->BootSession.key;
    status=BCryptGenRandom(NULL,(PUCHAR)&c->Net.session_id,sizeof(c->Net.session_id),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    c->Net.generation=input.generation; c->Net.epoch=input.epoch;
    c->Net.chan_id=NNPI_CHAN_MIN_NONCTX; c->Net.card_ip=input.card_ip;
    RtlCopyMemory(c->Net.mac,input.mac,6);
    if (!NT_SUCCESS(status) || !c->Net.session_id || c->Net.session_id==UINT64_MAX) {
        NnpiNetFail(c,NNPI_NET_FAIL_IDENTITY,STATUS_UNSUCCESSFUL); goto CopySuccess;
    }
    c->NetOwner.net.session_id=c->Net.session_id;
    c->NetOwner.net.generation=c->Net.generation; c->NetOwner.net.epoch=c->Net.epoch;
    status=NnpiNetDmaPrepare(c->Device,&c->NetDma);
    if (!NT_SUCCESS(status)) { NnpiNetFail(c,NNPI_NET_FAIL_DMA,status); goto CopySuccess; }
    c->NetWork=IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(c->Device));
    if (!c->NetWork) {
        (void)NnpiNetDmaRelease(&c->NetDma);
        NnpiNetFail(c,NNPI_NET_FAIL_DMA,STATUS_INSUFFICIENT_RESOURCES); goto CopySuccess;
    }
    if (!ExAcquireRundownProtection(&NnpiSysinfoRundown)) {
        IoFreeWorkItem(c->NetWork); c->NetWork=NULL;
        (void)NnpiNetDmaRelease(&c->NetDma);
        NnpiNetFail(c,NNPI_NET_FAIL_CLOSING,STATUS_DELETE_PENDING); goto CopySuccess;
    }
    WdfObjectReference(c->Device);
    c->NetStarted=TRUE; c->Net.worker_active=1;
    KeClearEvent(&c->NetDone);
    IoQueueWorkItem(c->NetWork,NnpiNetWorker,DelayedWorkQueue,c);
CopySuccess:
    status=STATUS_SUCCESS;
Copy:
    NnpiNetSnapshot(c); output=c->Net;
    output.version=NNPI_NET_ABI_VERSION; output.size=sizeof(output);
    /* QUERY carries no request identity; report current device generation/epoch.
     * BEGIN echoes the request G/E even on a synchronous failure (session_id may
     * still be zero), so the CLI can validate identity without guessing. */
    if (query && !output.session_id) { output.generation=c->Cache.generation; output.epoch=c->Epoch; }
    RtlCopyMemory(outBuffer,&output,sizeof(output)); information=sizeof(output);
Unlock:
    WdfWaitLockRelease(c->CacheLock);
End:
    WdfRequestCompleteWithInformation(Request,status,information);
}

/* Runtime transport remains on the SAME hardware worker. Each poll reads at
 * most 16 QWs, ACKs once (two writes only for full depth), then parses retained
 * data. A lost/partial reply or unknown opcode terminates, never reissues query.
 * Ready keeps all DMA, device and code lifetime until the close/reset barrier. */
static VOID NnpiBootRuntime(PNNPI_DEVICE_CONTEXT c)
{
    LARGE_INTEGER delay; ULONG rounds=0,partialRounds=0,count=0,i,command,card;
    struct nnpi_fifo_snapshot fifo={0};
    struct nnpi_runtime_message message={0};
    NTSTATUS status=STATUS_SUCCESS; int parsed,route;
    delay.QuadPart=-5000000;
    WdfWaitLockAcquire(c->CacheLock,NULL);
    c->RuntimeEntered=TRUE;
    if (!NnpiBootCurrent(c) || c->BootClosing) { c->RuntimeHealthy=FALSE; WdfWaitLockRelease(c->CacheLock); return; }
    __try {
        c->BootDiagnostic.RuntimeStep=NnpiRuntimeCommandGate;
        command=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_COMMAND_OFFSET);
        c->BootDiagnostic.RuntimeCommandControl=command;
        c->BootDiagnostic.RuntimeCommandControlValid=1;
        card=NnpiBootCard(c);
        c->BootDiagnostic.RuntimeCard=card; c->BootDiagnostic.RuntimeCardValid=1;
        if (command==MAXULONG || nnpi_fifo_decode(command,&fifo)!=NNPI_FIFO_OK || fifo.used_qwords ||
            ((card&0xffU)!=4 && (card&0xffU)!=8) || !NnpiBootIrqGate(c,FALSE,card))
            status=STATUS_DEVICE_DATA_ERROR;
        else {
            c->BootDiagnostic.RuntimeStep=NnpiRuntimeQuery;
            status=NnpiQueryVersion(&c->SysinfoMapping,&c->BootDiagnostic.RuntimeQuery);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { status=GetExceptionCode(); }
    if (!NT_SUCCESS(status)) NnpiBootFail(c,NNPI_BOOT_FAIL_PROTOCOL,status);
    WdfWaitLockRelease(c->CacheLock);
    if (!NT_SUCCESS(status)) return;
    for (;;) {
        status=KeWaitForSingleObject(&c->BootClose,Executive,KernelMode,FALSE,&delay);
        if (status==STATUS_SUCCESS) {
            WdfWaitLockAcquire(c->CacheLock,NULL); c->RuntimeHealthy=FALSE;
            WdfWaitLockRelease(c->CacheLock); return;
        }
        WdfWaitLockAcquire(c->CacheLock,NULL);
        if (!NnpiBootCurrent(c) || c->BootClosing) { c->RuntimeHealthy=FALSE; WdfWaitLockRelease(c->CacheLock); return; }
        status=STATUS_SUCCESS;
        __try {
            c->BootDiagnostic.RuntimeStep=NnpiRuntimeCardGate;
            card=NnpiReadControlSnapshot(&c->SnapshotMapping,NNPI_SNAPSHOT_DOORBELL_OFFSET);
            c->BootDiagnostic.RuntimeCard=card; c->BootDiagnostic.RuntimeCardValid=1;
            if (card==MAXULONG || (card&0xf000U) || ((card&0xffU)!=4 && (card&0xffU)!=8) ||
                !NnpiBootIrqGate(c,FALSE,card))
                status=STATUS_DEVICE_DATA_ERROR;
            if (NT_SUCCESS(status)) {
                c->BootDiagnostic.RuntimeStep=NnpiRuntimeResponse;
                status=NnpiReadRuntime(&c->RuntimeMapping,c->RuntimeWords,&count,
                    &c->BootDiagnostic.RuntimeResponse);
            }
            if (NT_SUCCESS(status)) for (i=0;i<count;++i) {
                c->BootDiagnostic.RuntimeStep=NnpiRuntimeParse;
                parsed=nnpi_runtime_push(&c->RuntimeParser,c->RuntimeWords[i],&message);
                c->BootDiagnostic.RuntimeParserResult=(ULONG)parsed;
                c->BootDiagnostic.RuntimeParserResultValid=1;
                if (parsed==NNPI_RUNTIME_MESSAGE) {
                    c->BootDiagnostic.RuntimeDecodedOpcode=message.opcode;
                    c->BootDiagnostic.RuntimeDecodedOpcodeValid=1;
                    c->RuntimeLastMessage=message;
                    route=nnpi_runtime_route_frame(&message);
                    if (route==NNPI_RUNTIME_ROUTE_VERSION) {
                        c->BootDiagnostic.RuntimeStep=NnpiRuntimeCompatibility;
                        if (c->RuntimeCompatible || !nnpi_runtime_compatible_1150(&message)) {
                            status=STATUS_DEVICE_PROTOCOL_ERROR; break;
                        }
                        c->RuntimeVersion=message;
                        c->BootDiagnostic.RuntimeStep=NnpiRuntimeHostReadyStep;
                        status=NnpiRuntimeHostReady(&c->HostReadyMapping);
                        if (!NT_SUCCESS(status)) break;
                        c->RuntimeCompatible=TRUE;
                    } else if (route==NNPI_RUNTIME_ROUTE_DESCRIPTOR) {
                        status=NnpiNetReceiveDescriptor(c,message.qwords[0]);
                        if (!NT_SUCCESS(status)) break;
                    } else if (route==NNPI_RUNTIME_ROUTE_UNEXPECTED_NET ||
                               route==NNPI_RUNTIME_ROUTE_EVENT) {
                        int netResult;
                        if (route==NNPI_RUNTIME_ROUTE_EVENT) c->RuntimeLastEvent=message.qwords[0];
                        netResult=nnpi_runtime_dispatch_net(&message,&c->NetSession,
                            &c->NetOwner.net,c->NetStarted && !c->NetClosing &&
                            nnpi_net_owner_matches(&c->NetOwner,&c->BootSession,&c->NetOwner.net));
                        if (netResult==NNPI_RUNTIME_NET_FATAL) {
                            status=STATUS_DEVICE_PROTOCOL_ERROR; break;
                        }
                        if (netResult==NNPI_RUNTIME_NET_UNEXPECTED) {
                            c->UnexpectedNetConfigCount=nnpi_runtime_saturating_increment(c->UnexpectedNetConfigCount);
                            c->LastUnexpectedNetConfig=message.qwords[0];
                        } else if (netResult==NNPI_RUNTIME_NET_FAILED) {
                            /* Network-local failure cannot take down healthy boot. */
                            NnpiNetFail(c,NNPI_NET_FAIL_PROTOCOL,STATUS_DEVICE_PROTOCOL_ERROR);
                        }
                        NnpiNetSnapshot(c);
                    } else if (route!=NNPI_RUNTIME_ROUTE_SYSINFO) {
                        status=STATUS_DEVICE_PROTOCOL_ERROR; break;
                    }
                } else if (parsed!=NNPI_RUNTIME_NEED_MORE) {
                    status=STATUS_DEVICE_PROTOCOL_ERROR; break;
                }
            }
            if (NT_SUCCESS(status) && c->RuntimeCompatible)
                status=NnpiNetDataPump(c);
            if (NT_SUCCESS(status) && c->RuntimeCompatible) {
                c->BootDiagnostic.RuntimeStep=NnpiRuntimeKeepaliveStep;
                status=NnpiRuntimeKeepalive(&c->HostReadyMapping,&c->RuntimeKeepalive);
                if (NT_SUCCESS(status)) c->RuntimeKeepaliveUpdates=
                    nnpi_runtime_saturating_increment(c->RuntimeKeepaliveUpdates);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) { status=GetExceptionCode(); }
        if (NT_SUCCESS(status)) c->BootDiagnostic.RuntimeStep=NnpiRuntimeWait;
        if (!c->RuntimeCompatible && ++rounds>=20 && NT_SUCCESS(status)) status=STATUS_IO_TIMEOUT;
        if (c->RuntimeParser.count) {
            if (++partialRounds>=20 && NT_SUCCESS(status)) status=STATUS_IO_TIMEOUT;
        } else partialRounds=0;
        if (!NT_SUCCESS(status)) NnpiBootFail(c,
            status==STATUS_IO_TIMEOUT?NNPI_BOOT_FAIL_TIMEOUT:NNPI_BOOT_FAIL_PROTOCOL,status);
        c->RuntimeHealthy=(BOOLEAN)(NT_SUCCESS(status) && c->RuntimeCompatible);
        NnpiBootSnapshot(c);
        WdfWaitLockRelease(c->CacheLock);
        if (!NT_SUCCESS(status)) return;
    }
}
