/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_HW_H
#define NNPI_HW_H
#include <ntddk.h>
#include <wdf.h>
#include "../include/nnpi_status_policy.h"
#include "../include/nnpi_snapshot_policy.h"
#include "../include/nnpi_hostwin_policy.h"
#include "../include/nnpi_runtime_transport.h"
/* Intentionally NOT an alias of NNPI_SNAPSHOT_MAPPING. Commands can only use
 * the mapping owned by NnpiMapSysinfo, whose base is BAR0+0x050. */
typedef struct _NNPI_COMMAND_MAPPING {
    PVOID Address;
    uint32_t Reason;
} NNPI_COMMAND_MAPPING;
/* Retain only observations from the existing response drain, never extra MMIO.
 * Valid separates a real zero control from a read that never returned. */
typedef struct _NNPI_RUNTIME_OBSERVATION {
    ULONG Control, ControlValid, Count, ReadHalves, AckWrites;
} NNPI_RUNTIME_OBSERVATION;
/* Separate ownership, same PAGE_READONLY | PAGE_NOCACHE attributes as status. */
typedef struct _NNPI_SNAPSHOT_MAPPING {
    PVOID Address;
    uint32_t Reason;
} NNPI_SNAPSHOT_MAPPING;
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiMapControlSnapshot(const struct nnpi_diag_response *Diagnostic, NNPI_SNAPSHOT_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiUnmapControlSnapshot(NNPI_SNAPSHOT_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
uint32_t NnpiReadControlSnapshot(void *Context, uint32_t FixedOffset);
/* All access to this mapping, including map/unmap, requires the device lock. */
typedef struct _NNPI_STATUS_MAPPING {
    PVOID Address;
    uint32_t Reason;
} NNPI_STATUS_MAPPING;
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiMapStatus0(const struct nnpi_diag_response *Diagnostic, NNPI_STATUS_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiUnmapStatus0(NNPI_STATUS_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
uint32_t NnpiReadStatus0(void *Context);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiCollectDiagnostic(WDFDEVICE Device, WDFCMRESLIST Raw,
                          WDFCMRESLIST Translated, struct nnpi_diag_response *Result);
/* 025A: separate four-byte writable NX mapping; same NOCACHE policy. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiMapHostReady(const struct nnpi_diag_response *Diagnostic, NNPI_SNAPSHOT_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiUnmapHostReady(NNPI_SNAPSHOT_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
uint32_t NnpiReadHostReady(NNPI_SNAPSHOT_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiWriteHostReady(NNPI_SNAPSHOT_MAPPING *Mapping);
VOID NnpiMapSysinfo(const struct nnpi_diag_response *Diagnostic, NNPI_COMMAND_MAPPING *Mapping);
VOID NnpiUnmapSysinfo(NNPI_COMMAND_MAPPING *Mapping);
VOID NnpiWriteSysinfoHalf(NNPI_COMMAND_MAPPING *Mapping, ULONG Half, ULONG Value);
/* Generic command push-port half write. RelativeOffset is 0/4 (no-notify port
 * 0x050) or 8/12 (notify port 0x058), exactly nnpi_fifo_plan_halves output. The
 * mapping already points at the 0x050 push port; any other offset is a fence. */
VOID NnpiWriteCommandHalf(NNPI_COMMAND_MAPPING *Mapping, ULONG RelativeOffset, ULONG Value);
VOID NnpiMapRuntime(const struct nnpi_diag_response *Diagnostic, NNPI_SNAPSHOT_MAPPING *Mapping);
VOID NnpiUnmapRuntime(NNPI_SNAPSHOT_MAPPING *Mapping);
/* Read a single bounded FIFO snapshot; complete reads precede its one ACK. */
NTSTATUS NnpiReadRuntime(NNPI_SNAPSHOT_MAPPING *Mapping, uint64_t Words[16], ULONG *Count,
    NNPI_RUNTIME_OBSERVATION *Observation);
NTSTATUS NnpiQueryVersion(NNPI_COMMAND_MAPPING *Mapping, struct nnpi_runtime_query_state *State);
NTSTATUS NnpiRuntimeHostReady(NNPI_SNAPSHOT_MAPPING *Mapping);
NTSTATUS NnpiRuntimeKeepalive(NNPI_SNAPSHOT_MAPPING *Mapping, ULONG *Counter);
/* Card host-window (CPU-Z inventory) mapped at BAR2+offset. Shared window: the
 * card writes with the C2H magic and the host reads it; the host writes with the
 * H2C magic and the card reads it. One mapping, two directions. */
typedef struct _NNPI_HOSTWIN_MAPPING {
    PVOID Address;
    uint32_t Reason;
} NNPI_HOSTWIN_MAPPING;
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiMapHostwin(const struct nnpi_diag_response *Diagnostic, NNPI_HOSTWIN_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
VOID NnpiUnmapHostwin(NNPI_HOSTWIN_MAPPING *Mapping);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiReadHostwin(NNPI_HOSTWIN_MAPPING *Mapping, struct nnpi_hostwin_response *Response);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS NnpiWriteHostwin(NNPI_HOSTWIN_MAPPING *Mapping, const struct nnpi_hostwin_write_request *Request, uint32_t *WrittenBytes);
#endif
