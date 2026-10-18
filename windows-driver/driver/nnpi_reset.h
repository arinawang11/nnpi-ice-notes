/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NNPI_RESET_H
#define NNPI_RESET_H
#include <ntddk.h>
#include <wdf.h>
#include "../include/nnpi_reset_policy.h"
NTSTATUS NnpiResetInitialize(VOID);
VOID NnpiResetUnload(VOID);
NTSTATUS NnpiResetReserve(PDEVICE_OBJECT DeviceObject);
/* Prepare-time interface transferred on successful acceptance, never a WDF child. */
NTSTATUS NnpiResetSubmit(PDEVICE_OBJECT DeviceObject, DEVICE_RESET_INTERFACE_STANDARD *Interface, ULONGLONG DeviceId,
                       ULONGLONG Generation, ULONGLONG Epoch);
VOID NnpiResetQuery(ULONGLONG DeviceId, ULONGLONG Generation, ULONGLONG Epoch,
                    struct nnpi_reset_response *Response);
VOID NnpiResetInvalidate(ULONGLONG DeviceId);
BOOLEAN NnpiResetRevalidate(ULONGLONG DeviceId, ULONGLONG Generation, ULONGLONG Epoch);
#endif
