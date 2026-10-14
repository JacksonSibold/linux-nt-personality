/*
 * k32_sync_pure.h - pure logic for the kernel32 synchronisation shims.
 *
 * kernel32.c and kernel32 critical-section code call these helpers. They
 * have no NT or Win32 dependency beyond the basic types, so tests/ntdll-host
 * can check them on the host.
 *
 * Critical sections follow the RTL_CRITICAL_SECTION protocol: LockCount is -1
 * when free and counts the waiters above that; RecursionCount counts the
 * nested enters of the owner; the event in LockSemaphore wakes one waiter.
 */
#ifndef NTDLL_K32_SYNC_PURE_H
#define NTDLL_K32_SYNC_PURE_H

#include "nt_types.h"
#include "ntstatus.h"

/* Win32 error codes used here. */
#define K32S_ERROR_INVALID_PARAMETER 87

/* WaitForMultipleObjects limit: MAXIMUM_WAIT_OBJECTS. */
#define K32S_MAXIMUM_WAIT_OBJECTS 64u

/* Win32 wait results. */
#define K32S_WAIT_OBJECT_0  0u
#define K32S_WAIT_ABANDONED 0x80u
#define K32S_WAIT_TIMEOUT   258u
#define K32S_WAIT_FAILED    0xFFFFFFFFu

/* LockCount when a critical section is free (RTL protocol). */
#define K32S_CS_LOCK_FREE   (-1L)

/* WaitForMultipleObjects argument check. Count must be 1..64 and every
 * handle must be non-NULL. Returns 1 when the call may proceed. */
static inline int k32s_wait_multi_args_ok(DWORD count, const void *const *handles)
{
    if (count == 0 || count > K32S_MAXIMUM_WAIT_OBJECTS || handles == NULL)
        return 0;
    for (DWORD i = 0; i < count; ++i) {
        if (handles[i] == NULL)
            return 0;
    }
    return 1;
}

/* The NT wait type for WaitForMultipleObjects: WaitAll 0, WaitAny 1 (the
 * values of NtWaitForMultipleObjects in src/kernel/nt/sync.h). */
static inline ULONG k32s_wait_type(BOOL wait_all)
{
    return wait_all ? 0u : 1u;
}

/* NTSTATUS from NtWaitForMultipleObjects to the Win32 result. For WaitAny the
 * index is STATUS_WAIT_0 + i; for WaitAll the status is STATUS_WAIT_0 and the
 * result is WAIT_OBJECT_0. Abandoned waits add 0x80 to the index. */
static inline DWORD k32s_wait_multi_result(NTSTATUS s, DWORD count)
{
    if (s >= STATUS_WAIT_0 && s < STATUS_WAIT_0 + (NTSTATUS)count)
        return (DWORD)(s - STATUS_WAIT_0);
    if (s >= STATUS_ABANDONED_WAIT_0 && s < STATUS_ABANDONED_WAIT_0 + (NTSTATUS)count)
        return K32S_WAIT_ABANDONED + (DWORD)(s - STATUS_ABANDONED_WAIT_0);
    if (s == STATUS_TIMEOUT)
        return K32S_WAIT_TIMEOUT;
    return K32S_WAIT_FAILED;
}

/* Critical sections. A thread re-enters a section it owns by counting, not
 * by waiting. owner is OwningThread; self is the caller's thread id. */
static inline int k32s_cs_owned_by(HANDLE owner, HANDLE self)
{
    return owner != NULL && owner == self;
}

/* Result of the lock-count increment on Enter: 0 means the section was free
 * and the caller now owns it; anything else means the caller must wait on the
 * event. */
static inline int k32s_cs_must_wait(LONG lock_count_after_inc)
{
    return lock_count_after_inc != 0;
}

/* Leave: after the decrement, a count of 0 or more means a waiter exists and
 * the event must be set. */
static inline int k32s_cs_must_signal(LONG lock_count_after_dec)
{
    return lock_count_after_dec >= 0;
}

/* GetTickCount: milliseconds from a performance counter reading. The split
 * avoids overflowing count * 1000 for large counters. A zero frequency gives
 * 0. The result is truncated to 32 bits, as GetTickCount returns DWORD. */
static inline DWORD k32s_tick_count_ms(LONGLONG count, LONGLONG freq)
{
    ULONGLONG c, f, ms;
    if (freq <= 0 || count < 0)
        return 0;
    c = (ULONGLONG)count;
    f = (ULONGLONG)freq;
    ms = (c / f) * 1000u + ((c % f) * 1000u) / f;
    return (DWORD)(ms & 0xFFFFFFFFu);
}

#endif /* NTDLL_K32_SYNC_PURE_H */
