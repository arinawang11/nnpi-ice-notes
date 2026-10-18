/* SPDX-License-Identifier: GPL-2.0-only */
#include "nnpi_reset.h"
#include "../include/nnpi_sysinfo_policy.h"

/* 025 RETURN_ONLY, not the asynchronous 024 validation mode. No callback is
 * registered. Parameters=NULL is documented optional by DEVICE_RESET_HANDLER.
 * Only exact returned STATUS_SUCCESS is stop evidence; PENDING/error retains
 * the operation/interface indefinitely via failure rundown (no retry).
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-device_reset_handler
 */
typedef struct _NNPI_RESET_OPERATION {
    KSPIN_LOCK Lock;
    PIO_WORKITEM Work;
    PDEVICE_OBJECT WorkDevice;
    DEVICE_RESET_INTERFACE_STANDARD Interface;
    struct nnpi_reset_response State;
    ULONGLONG DeviceId;
    BOOLEAN Closing, InterfaceOwned;
} NNPI_RESET_OPERATION;
static EX_RUNDOWN_REF NnpiRundown;
static NNPI_RESET_OPERATION *NnpiOperation;

static VOID NnpiResetWorker(PDEVICE_OBJECT DeviceObject,PVOID Context)
{
    NNPI_RESET_OPERATION *op=(NNPI_RESET_OPERATION *)Context;
    PIO_WORKITEM work=op->Work;
    KIRQL irql;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(DeviceObject);
    PAGED_CODE();
    KeAcquireSpinLock(&op->Lock,&irql);
    if (op->Closing || op->State.epoch_invalidated) {
        nnpi_sysinfo_reset_returned(&op->State,(uint32_t)STATUS_DEVICE_NOT_READY,KeQueryInterruptTime());
        KeReleaseSpinLock(&op->Lock,irql);
        /* No provider call: no unknown provider work to retain. A published
         * page still blocks at ReleaseHardware because stop was not proven. */
        IoFreeWorkItem(work);
        ExReleaseRundownProtection(&NnpiRundown);
        ExReleaseRundownProtection(&NnpiRundown);
        return;
    }
    KeReleaseSpinLock(&op->Lock,irql);
    status=op->Interface.DeviceReset(op->Interface.Context,FunctionLevelDeviceReset,0,NULL);
    KeAcquireSpinLock(&op->Lock,&irql);
    nnpi_sysinfo_reset_returned(&op->State,(uint32_t)status,KeQueryInterruptTime());
    KeReleaseSpinLock(&op->Lock,irql);
    IoFreeWorkItem(work);
    if (status==STATUS_SUCCESS) ExReleaseRundownProtection(&NnpiRundown);
    /* The second ref is retained for any non-SUCCESS return, including PENDING.
     * Worker ref can now end. I/O manager pins device/image through RETURN. */
    ExReleaseRundownProtection(&NnpiRundown);
}
NTSTATUS NnpiResetInitialize(VOID)
{
    NNPI_RESET_OPERATION *op;
    op=(NNPI_RESET_OPERATION *)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*op),'rPNN');
    if (!op) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(op,sizeof(*op));
    ExInitializeRundownProtection(&NnpiRundown);
    KeInitializeSpinLock(&op->Lock);
    nnpi_reset_init(&op->State);
    NnpiOperation=op;
    return STATUS_SUCCESS;
}
VOID NnpiResetUnload(VOID)
{
    NNPI_RESET_OPERATION *op=NnpiOperation;
    KIRQL irql;
    if (!op) return;
    KeAcquireSpinLock(&op->Lock,&irql); op->Closing=TRUE; KeReleaseSpinLock(&op->Lock,irql);
    ExWaitForRundownProtectionRelease(&NnpiRundown);
    if (op->InterfaceOwned) op->Interface.InterfaceDereference(op->Interface.Context);
    if (op->State.state==NNPI_RESET_IDLE && op->Work) IoFreeWorkItem(op->Work);
    NnpiOperation=NULL;
    ExFreePoolWithTag(op,'rPNN');
}
NTSTATUS NnpiResetReserve(PDEVICE_OBJECT DeviceObject)
{
    NNPI_RESET_OPERATION *op=NnpiOperation;
    PIO_WORKITEM work;
    KIRQL irql;
    if (!op || !DeviceObject) return STATUS_INVALID_PARAMETER;
    work=IoAllocateWorkItem(DeviceObject);
    if (!work) return STATUS_INSUFFICIENT_RESOURCES;
    KeAcquireSpinLock(&op->Lock,&irql);
    if (op->Closing || op->State.state!=NNPI_RESET_IDLE || op->Work!=NULL) {
        KeReleaseSpinLock(&op->Lock,irql);
        IoFreeWorkItem(work);
        return STATUS_DEVICE_BUSY;
    }
    op->Work=work;
    op->WorkDevice=DeviceObject;
    KeReleaseSpinLock(&op->Lock,irql);
    return STATUS_SUCCESS;
}
NTSTATUS NnpiResetSubmit(PDEVICE_OBJECT DeviceObject,DEVICE_RESET_INTERFACE_STANDARD *Interface,
                       ULONGLONG DeviceId,ULONGLONG Generation,ULONGLONG Epoch)
{
    NNPI_RESET_OPERATION *op=NnpiOperation;
    KIRQL irql;
    NTSTATUS status=STATUS_DEVICE_BUSY;
    PIO_WORKITEM work;
    KeAcquireSpinLock(&op->Lock,&irql);
    work=op->Work;
    if (!work || op->WorkDevice!=DeviceObject || op->State.state!=NNPI_RESET_IDLE) {
        KeReleaseSpinLock(&op->Lock,irql);
        return STATUS_DEVICE_NOT_READY;
    }
    if (!op->Closing && ExAcquireRundownProtection(&NnpiRundown)) {
        if (ExAcquireRundownProtection(&NnpiRundown)) {
            if (nnpi_reset_begin(&op->State,Generation,Epoch,KeQueryInterruptTime())) {
                op->Work=work; op->DeviceId=DeviceId; op->Interface=*Interface;
                op->InterfaceOwned=TRUE; RtlZeroMemory(Interface,sizeof(*Interface));
                status=STATUS_SUCCESS;
            } else {
                ExReleaseRundownProtection(&NnpiRundown); ExReleaseRundownProtection(&NnpiRundown);
            }
        } else ExReleaseRundownProtection(&NnpiRundown);
    }
    KeReleaseSpinLock(&op->Lock,irql);
    if (NT_SUCCESS(status)) IoQueueWorkItem(work,NnpiResetWorker,DelayedWorkQueue,op);
    /* Failed acceptance leaves the unqueued reservation owned until unload. */
    return status;
}
VOID NnpiResetQuery(ULONGLONG DeviceId,ULONGLONG Generation,ULONGLONG Epoch,
                   struct nnpi_reset_response *Response)
{
    NNPI_RESET_OPERATION *op=NnpiOperation;
    KIRQL irql;
    KeAcquireSpinLock(&op->Lock,&irql); *Response=op->State;
    if (Response->state==NNPI_RESET_IDLE) { Response->generation=Generation; Response->epoch=Epoch; }
    else if (op->DeviceId!=DeviceId) { Response->epoch_invalidated=1; Response->hardware_blocked=1; }
    KeReleaseSpinLock(&op->Lock,irql);
}
VOID NnpiResetInvalidate(ULONGLONG DeviceId)
{
    NNPI_RESET_OPERATION *op=NnpiOperation;
    KIRQL irql;
    KeAcquireSpinLock(&op->Lock,&irql);
    if (op->DeviceId==DeviceId) nnpi_reset_invalidate(&op->State);
    KeReleaseSpinLock(&op->Lock,irql);
}
BOOLEAN NnpiResetRevalidate(ULONGLONG DeviceId,ULONGLONG Generation,ULONGLONG Epoch)
{
    /* 025 never reopens MMIO after cleanup; the legacy IOCTL is rejected. */
    UNREFERENCED_PARAMETER(DeviceId); UNREFERENCED_PARAMETER(Generation); UNREFERENCED_PARAMETER(Epoch);
    return FALSE;
}
