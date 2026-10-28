/*
 * kernel32_sync.c - synchronisation exports whose Win32 signature differs from the Rtl function in
 * ntdll_sync.c: InitializeCriticalSectionAndSpinCount, InitializeCriticalSectionEx,
 * SleepConditionVariableCS, SleepConditionVariableSRW, InitOnceBeginInitialize, InitOnceComplete,
 * InitOnceExecuteOnce and WaitOnAddress. The ones with the same signature are forwarded in
 * kernel32.def (EnterCriticalSection = ntdll.RtlEnterCriticalSection and so on).
 *
 * A timed-out sleep or wait fails with ERROR_TIMEOUT (1460).
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include <stddef.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif

typedef PVOID LPVOID;

#define K32Q_ERROR_TIMEOUT 1460u
#define K32Q_INFINITE 0xFFFFFFFFu

__declspec(dllimport) ULONG __stdcall RtlNtStatusToDosError(NTSTATUS st);
__declspec(dllimport) NTSTATUS __stdcall RtlInitializeCriticalSectionAndSpinCount(PVOID cs, ULONG spin);
__declspec(dllimport) NTSTATUS __stdcall RtlInitializeCriticalSectionEx(PVOID cs, ULONG spin, ULONG flags);
__declspec(dllimport) NTSTATUS __stdcall RtlSleepConditionVariableCS(PVOID *cv, PVOID cs, PLARGE_INTEGER timeout);
__declspec(dllimport) NTSTATUS __stdcall RtlSleepConditionVariableSRW(PVOID *cv, PVOID *srw, PLARGE_INTEGER timeout, ULONG flags);
__declspec(dllimport) NTSTATUS __stdcall RtlRunOnceBeginInitialize(PVOID *once, ULONG flags, PVOID *context);
__declspec(dllimport) NTSTATUS __stdcall RtlRunOnceComplete(PVOID *once, ULONG flags, PVOID context);
__declspec(dllimport) NTSTATUS __stdcall RtlRunOnceExecuteOnce(PVOID *once, PVOID fn, PVOID parameter, PVOID *context);
__declspec(dllimport) NTSTATUS __stdcall RtlWaitOnAddress(volatile void *addr, PVOID compare, SIZE_T size, PLARGE_INTEGER timeout);

void SetLastError(DWORD e);

/* A millisecond timeout as an NT timeout: NULL for INFINITE, otherwise relative (negative, 100 ns). */
static PLARGE_INTEGER ms_to_nt(DWORD ms, LARGE_INTEGER *storage)
{
    if (ms == K32Q_INFINITE)
        return NULL;
    storage->QuadPart = -(LONGLONG)ms * 10000;
    return storage;
}

/* Maps a wait-style status to the Win32 BOOL, setting the last error on failure. */
static BOOL wait_result(NTSTATUS st)
{
    if (st == STATUS_SUCCESS)
        return 1;
    SetLastError(st == STATUS_TIMEOUT ? K32Q_ERROR_TIMEOUT : RtlNtStatusToDosError(st));
    return 0;
}

BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPVOID cs, DWORD spin)
{
    NTSTATUS st = RtlInitializeCriticalSectionAndSpinCount(cs, spin);
    if (!NT_SUCCESS(st)) {
        SetLastError(RtlNtStatusToDosError(st));
        return 0;
    }
    return 1;
}

BOOL WINAPI InitializeCriticalSectionEx(LPVOID cs, DWORD spin, DWORD flags)
{
    NTSTATUS st = RtlInitializeCriticalSectionEx(cs, spin, flags);
    if (!NT_SUCCESS(st)) {
        SetLastError(RtlNtStatusToDosError(st));
        return 0;
    }
    return 1;
}

BOOL WINAPI SleepConditionVariableCS(PVOID *cv, LPVOID cs, DWORD ms)
{
    LARGE_INTEGER t;
    return wait_result(RtlSleepConditionVariableCS(cv, cs, ms_to_nt(ms, &t)));
}

BOOL WINAPI SleepConditionVariableSRW(PVOID *cv, PVOID *srw, DWORD ms, ULONG flags)
{
    LARGE_INTEGER t;
    return wait_result(RtlSleepConditionVariableSRW(cv, srw, ms_to_nt(ms, &t), flags));
}

/* InitOnceBeginInitialize: *pending says whether the caller must run the initialiser. With
 * INIT_ONCE_CHECK_ONLY on a once that has not finished it fails with ERROR_GEN_FAILURE and *pending set. */
BOOL WINAPI InitOnceBeginInitialize(PVOID *once, DWORD flags, BOOL *pending, LPVOID *context)
{
    NTSTATUS st;
    if (!pending) {
        SetLastError(87);
        return 0;
    }
    st = RtlRunOnceBeginInitialize(once, flags, context);
    if (st == STATUS_PENDING) {
        *pending = 1;
        return 1;
    }
    if (NT_SUCCESS(st)) {
        *pending = 0;
        return 1;
    }
    if (flags & 1u) /* INIT_ONCE_CHECK_ONLY: the once has not run */
        *pending = 1;
    SetLastError(RtlNtStatusToDosError(st));
    return 0;
}

BOOL WINAPI InitOnceComplete(PVOID *once, DWORD flags, LPVOID context)
{
    NTSTATUS st = RtlRunOnceComplete(once, flags, context);
    if (!NT_SUCCESS(st)) {
        SetLastError(RtlNtStatusToDosError(st));
        return 0;
    }
    return 1;
}

BOOL WINAPI InitOnceExecuteOnce(PVOID *once, PVOID fn, PVOID parameter, LPVOID *context)
{
    NTSTATUS st = RtlRunOnceExecuteOnce(once, fn, parameter, context);
    if (!NT_SUCCESS(st)) {
        SetLastError(RtlNtStatusToDosError(st));
        return 0;
    }
    return 1;
}

BOOL WINAPI WaitOnAddress(volatile void *addr, PVOID compare, SIZE_T size, DWORD ms)
{
    LARGE_INTEGER t;
    return wait_result(RtlWaitOnAddress(addr, compare, size, ms_to_nt(ms, &t)));
}
#endif
