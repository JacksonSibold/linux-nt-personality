/*
 * ntstatus_kernel.c - kernel NTSTATUS -> DOS error table
 *
 * This file is the kernel counterpart of src/ntdll/ntstatus_to_dos.c.
 * The table data is auto-generated from the same source of truth via
 * gen_ntstatus_table.py -> ntstatus_table.inc, ensuring they cannot drift.
 * Both __KERNEL__ and USERSPACE_KSHIM paths use the identical binary-search
 * algorithm and fallback logic.
 */

#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "ntstatus_kernel.h"

typedef struct _NTSTATUS_TO_DOS_K {
    NTSTATUS Status;
    ULONG    DosError;
} NTSTATUS_TO_DOS_K;

static const NTSTATUS_TO_DOS_K NtStatusToDosTableKernel[] = {
#include "ntstatus_table.inc"
};

#define NTSTATUS_TO_DOS_K_COUNT (sizeof(NtStatusToDosTableKernel)/sizeof(NtStatusToDosTableKernel[0]))

#define ERROR_MR_MID_NOT_FOUND 317L

static ULONG RtlNtStatusToDosErrorFallbackK(NTSTATUS Status)
{
    ULONG st = (ULONG)Status;
    if (!st) return 0;
    if (st & 0x20000000UL) return st;
    if ((st & 0xF0000000UL) == 0xD0000000UL) {
        st &= ~0x10000000UL;
        Status = (NTSTATUS)st;
    }
    ULONG hi = (st >> 16) & 0xFFFFUL;
    if (hi == 0xC001UL || hi == 0x8007UL || hi == 0xC007UL) {
        return st & 0xFFFFUL;
    }
    return ERROR_MR_MID_NOT_FOUND;
}

ULONG RtlNtStatusToDosError(NTSTATUS Status)
{
    LONG lo = 0;
    LONG hi = (LONG)NTSTATUS_TO_DOS_K_COUNT - 1;
    while (lo <= hi) {
        LONG mid = lo + (hi - lo) / 2;
        if (NtStatusToDosTableKernel[mid].Status == Status)
            return NtStatusToDosTableKernel[mid].DosError;
        if (NtStatusToDosTableKernel[mid].Status < Status)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return RtlNtStatusToDosErrorFallbackK(Status);
}

#ifdef __KERNEL__
ULONG nt_kernel_RtlNtStatusToDosError(NTSTATUS Status)
{
    return RtlNtStatusToDosError(Status);
}

/* syscall wrapper: exercised through real NT syscall */
#include <linux/uaccess.h>
NTSTATUS nt_svc_NtRtlNtStatusToDosError(NTSTATUS Status, ULONG *DosError)
{
    ULONG dos;
    if (!DosError)
        return STATUS_ACCESS_VIOLATION;
    if (!access_ok(DosError, sizeof(*DosError)))
        return STATUS_ACCESS_VIOLATION;
    dos = RtlNtStatusToDosError(Status);
    if (copy_to_user(DosError, &dos, sizeof(dos)))
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}
#else
/* userspace shim syscall wrapper matching kernel prototype for tests */
NTSTATUS nt_svc_NtRtlNtStatusToDosError(NTSTATUS Status, ULONG *DosError)
{
    if (!DosError) return STATUS_ACCESS_VIOLATION;
    *DosError = RtlNtStatusToDosError(Status);
    return STATUS_SUCCESS;
}
#endif
