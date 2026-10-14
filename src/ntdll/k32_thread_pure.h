/*
 * k32_thread_pure.h - pure rules for the kernel32 thread and process shims.
 *
 * kernel32_thread.c uses these helpers for CreateThread flags, the exit code
 * of a thread or process, and the suspend count result. They have no NT or
 * Win32 dependency, so tests/ntdll-host checks them on the host.
 */
#ifndef NTDLL_K32_THREAD_PURE_H
#define NTDLL_K32_THREAD_PURE_H

#include "nt_types.h"
#include "ntstatus.h"

/* CreateThread dwCreationFlags. */
#define K32T_CREATE_SUSPENDED                    0x00000004u
#define K32T_STACK_SIZE_PARAM_IS_A_RESERVATION   0x00010000u

/* NtCreateThreadEx CreateFlags: THREAD_CREATE_FLAGS_CREATE_SUSPENDED. */
#define K32T_NT_CREATE_SUSPENDED                 0x00000001u

/* THREAD_ALL_ACCESS. */
#define K32T_THREAD_ALL_ACCESS                   0x001FFFFFu

/* Win32 error codes used here. */
#define K32T_ERROR_INVALID_PARAMETER             87u

/* GetExitCodeThread and GetExitCodeProcess report STILL_ACTIVE while the
 * object runs. NT reports STATUS_PENDING in ExitStatus until then. */
#define K32T_STILL_ACTIVE                        259u

/* SuspendThread and ResumeThread return this on failure. */
#define K32T_SUSPEND_FAILED                      0xFFFFFFFFu

/* CreateThread flags to NtCreateThreadEx CreateFlags. Only the two bits above
 * are defined; any other bit fails with ERROR_INVALID_PARAMETER. Returns 1
 * when the flags are valid and stores the NT flags in *nt. */
static inline int k32t_create_flags(DWORD flags, ULONG *nt)
{
    DWORD known = K32T_CREATE_SUSPENDED | K32T_STACK_SIZE_PARAM_IS_A_RESERVATION;
    if (flags & ~known)
        return 0;
    *nt = (flags & K32T_CREATE_SUSPENDED) ? K32T_NT_CREATE_SUSPENDED : 0u;
    return 1;
}

/* The exit code a Win32 call reports for ExitStatus from NT. STATUS_PENDING
 * (0x103) means the object has not exited, so the answer is STILL_ACTIVE. */
static inline DWORD k32t_exit_code(NTSTATUS exit_status)
{
    if (exit_status == STATUS_PENDING)
        return K32T_STILL_ACTIVE;
    return (DWORD)exit_status;
}

/* SuspendThread and ResumeThread: the previous suspend count on success, or
 * (DWORD)-1 on failure. prev is the count NtSuspendThread or NtResumeThread
 * stored. */
static inline DWORD k32t_suspend_result(NTSTATUS s, ULONG prev)
{
    if (!NT_SUCCESS(s))
        return K32T_SUSPEND_FAILED;
    return (DWORD)prev;
}

#endif /* NTDLL_K32_THREAD_PURE_H */
