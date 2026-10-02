/* ntstatus_kernel.h - kernel NTSTATUS->DOS table, generated */
#ifndef NTSTATUS_KERNEL_H
#define NTSTATUS_KERNEL_H

#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

ULONG RtlNtStatusToDosError(NTSTATUS Status);

#ifdef __KERNEL__
ULONG nt_kernel_RtlNtStatusToDosError(NTSTATUS Status);
#endif

#endif
