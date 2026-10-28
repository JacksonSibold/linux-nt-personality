/*
 * ntdll_sync.c - synchronisation exports of ntdll: critical sections (RtlEnterCriticalSection and the
 * rest), slim reader/writer locks, condition variables, RtlRunOnce, RtlWaitOnAddress and interlocked
 * singly linked lists.
 *
 * The lock-free parts are rtl_sync_pure.h, run here on NtWaitForAlertByThreadId and
 * NtAlertThreadByThreadId (host tests: tests/ntdll-host/test_rtl_sync_pure.c). The critical section is the
 * RTL protocol of k32_sync_pure.h with an auto-reset NT event for the waiters. kernel32 forwards its
 * EnterCriticalSection, AcquireSRWLockExclusive and the like to these.
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
#include "nt_syscalls.h"
#include "k32_sync_pure.h"
#include "rtl_sync_pure.h"
#include <stddef.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME

#ifndef VOID
#define VOID void
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS)0x00000103)
#endif

/* ---- The wait primitive: a per-thread alert token ---------------------------------------- */

static uint32_t sync_self(void)
{
    uint64_t tid;
    __asm__ volatile("movq %%gs:0x48, %0" : "=r"(tid));
    return (uint32_t)tid;
}

static int sync_wait(uint32_t timeout_ms)
{
    LARGE_INTEGER t;
    NTSTATUS st;
    if (timeout_ms == RS_INFINITE) {
        st = NtWaitForAlertByThreadId(NULL, NULL);
    } else {
        t.QuadPart = -(LONGLONG)timeout_ms * 10000;
        st = NtWaitForAlertByThreadId(NULL, &t);
    }
    return st == STATUS_ALERTED;
}

static void sync_alert(uint32_t tid)
{
    NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)tid);
}

static void sync_yield(void)
{
    NtYieldExecution();
}

static uint32_t sync_now_ms(void)
{
    LARGE_INTEGER t;
    t.QuadPart = 0;
    NtQuerySystemTime(&t);
    return (uint32_t)((uint64_t)t.QuadPart / 10000u);
}

static const rs_os g_sync_os = {sync_self, sync_wait, sync_alert, sync_yield};
static rs_lot g_lot;
static const rs_ctx g_ctx = {&g_lot, &g_sync_os, sync_now_ms};

/* An NT timeout (NULL: wait for ever; negative: relative, 100 ns; positive: absolute system time) as
 * milliseconds, rounding a partial millisecond up. */
static uint32_t timeout_ms(const LARGE_INTEGER *t)
{
    int64_t ticks;
    if (!t)
        return RS_INFINITE;
    ticks = t->QuadPart;
    if (ticks == 0)
        return 0;
    if (ticks > 0) { /* absolute: the time left until then */
        LARGE_INTEGER now;
        now.QuadPart = 0;
        NtQuerySystemTime(&now);
        ticks = now.QuadPart >= ticks ? 0 : -(ticks - now.QuadPart);
        if (ticks == 0)
            return 0;
    }
    {
        uint64_t ms = ((uint64_t)(-ticks) + 9999u) / 10000u;
        return ms >= RS_INFINITE ? RS_INFINITE - 1 : (uint32_t)ms;
    }
}

/* ---- Slim reader/writer locks ------------------------------------------------------------------ */

VOID __stdcall RtlInitializeSRWLock(PVOID *lock)
{
    *lock = NULL;
}

VOID __stdcall RtlAcquireSRWLockExclusive(PVOID *lock)
{
    rs_srw_acquire_exclusive(&g_ctx, (volatile uint32_t *)lock);
}

VOID __stdcall RtlAcquireSRWLockShared(PVOID *lock)
{
    rs_srw_acquire_shared(&g_ctx, (volatile uint32_t *)lock);
}

BOOLEAN __stdcall RtlTryAcquireSRWLockExclusive(PVOID *lock)
{
    return (BOOLEAN)rs_srw_try_exclusive((volatile uint32_t *)lock);
}

BOOLEAN __stdcall RtlTryAcquireSRWLockShared(PVOID *lock)
{
    return (BOOLEAN)rs_srw_try_shared((volatile uint32_t *)lock);
}

VOID __stdcall RtlReleaseSRWLockExclusive(PVOID *lock)
{
    rs_srw_release_exclusive(&g_ctx, (volatile uint32_t *)lock);
}

VOID __stdcall RtlReleaseSRWLockShared(PVOID *lock)
{
    rs_srw_release_shared(&g_ctx, (volatile uint32_t *)lock);
}

/* ---- Critical sections --------------------------------------------------------------------------- */

typedef struct RTLS_CRITICAL_SECTION {
    void *DebugInfo;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} RTLS_CRITICAL_SECTION;

#define RTLS_EVENT_ALL_ACCESS 0x1F0003u
#define RTLS_SYNCHRONIZATION_EVENT 1u

static HANDLE cs_self(void)
{
    return (HANDLE)(ULONG_PTR)sync_self();
}

/* The section's event, created if no one has yet; NULL only when it cannot be created. */
static HANDLE cs_event(RTLS_CRITICAL_SECTION *cs)
{
    HANDLE ev = __atomic_load_n(&cs->LockSemaphore, __ATOMIC_ACQUIRE);
    HANDLE created = NULL, expected = NULL;
    if (ev)
        return ev;
    if (!NT_SUCCESS(NtCreateEvent(&created, RTLS_EVENT_ALL_ACCESS, NULL, RTLS_SYNCHRONIZATION_EVENT, 0)))
        return NULL;
    if (__atomic_compare_exchange_n(&cs->LockSemaphore, &expected, created, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return created;
    NtClose(created); /* another thread created it first */
    return expected;
}

static HANDLE cs_event_retry(RTLS_CRITICAL_SECTION *cs)
{
    HANDLE ev = cs_event(cs);
    while (!ev) { /* creation failed: back off a millisecond and try again */
        LARGE_INTEGER d;
        d.QuadPart = -10000;
        NtDelayExecution(0, &d);
        ev = cs_event(cs);
    }
    return ev;
}

NTSTATUS __stdcall RtlInitializeCriticalSectionAndSpinCount(RTLS_CRITICAL_SECTION *cs, ULONG spin)
{
    if (!cs)
        return STATUS_INVALID_PARAMETER;
    cs->DebugInfo = NULL;
    cs->LockCount = K32S_CS_LOCK_FREE;
    cs->RecursionCount = 0;
    cs->OwningThread = NULL;
    cs->LockSemaphore = NULL;
    cs->SpinCount = spin & 0x00FFFFFFu; /* kept for GetSpinCount-style reads; the section does not spin */
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall RtlInitializeCriticalSection(RTLS_CRITICAL_SECTION *cs)
{
    return RtlInitializeCriticalSectionAndSpinCount(cs, 0);
}

NTSTATUS __stdcall RtlInitializeCriticalSectionEx(RTLS_CRITICAL_SECTION *cs, ULONG spin, ULONG flags)
{
    (void)flags; /* RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO and friends change only debug records */
    return RtlInitializeCriticalSectionAndSpinCount(cs, spin);
}

ULONG __stdcall RtlSetCriticalSectionSpinCount(RTLS_CRITICAL_SECTION *cs, ULONG spin)
{
    ULONG old = (ULONG)cs->SpinCount;
    cs->SpinCount = spin & 0x00FFFFFFu;
    return old;
}

NTSTATUS __stdcall RtlEnterCriticalSection(RTLS_CRITICAL_SECTION *cs)
{
    HANDLE self;
    LONG after;
    if (!cs)
        return STATUS_INVALID_PARAMETER;
    self = cs_self();
    if (k32s_cs_owned_by(cs->OwningThread, self)) {
        cs->RecursionCount++;
        __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
        return STATUS_SUCCESS;
    }
    after = __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
    if (k32s_cs_must_wait(after))
        NtWaitForSingleObject(cs_event_retry(cs), 0, NULL);
    cs->OwningThread = self;
    cs->RecursionCount = 1;
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall RtlLeaveCriticalSection(RTLS_CRITICAL_SECTION *cs)
{
    if (!cs)
        return STATUS_INVALID_PARAMETER;
    if (--cs->RecursionCount == 0) {
        LONG after;
        /* Clear the owner before the count changes, so a woken waiter never sees a stale owner. */
        cs->OwningThread = NULL;
        after = __atomic_sub_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
        if (k32s_cs_must_signal(after))
            NtSetEvent(cs_event_retry(cs), NULL);
        return STATUS_SUCCESS;
    }
    __atomic_sub_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
    return STATUS_SUCCESS;
}

ULONG __stdcall RtlTryEnterCriticalSection(RTLS_CRITICAL_SECTION *cs)
{
    HANDLE self;
    LONG expected = K32S_CS_LOCK_FREE;
    if (!cs)
        return 0;
    self = cs_self();
    if (k32s_cs_owned_by(cs->OwningThread, self)) {
        cs->RecursionCount++;
        __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
        return 1;
    }
    if (!__atomic_compare_exchange_n(&cs->LockCount, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    cs->OwningThread = self;
    cs->RecursionCount = 1;
    return 1;
}

NTSTATUS __stdcall RtlDeleteCriticalSection(RTLS_CRITICAL_SECTION *cs)
{
    HANDLE ev;
    if (!cs)
        return STATUS_INVALID_PARAMETER;
    ev = __atomic_exchange_n(&cs->LockSemaphore, NULL, __ATOMIC_ACQ_REL);
    if (ev)
        NtClose(ev);
    cs->OwningThread = NULL;
    cs->RecursionCount = 0;
    cs->LockCount = K32S_CS_LOCK_FREE;
    cs->SpinCount = 0;
    return STATUS_SUCCESS;
}

BOOLEAN __stdcall RtlIsCriticalSectionLocked(RTLS_CRITICAL_SECTION *cs)
{
    return cs->RecursionCount != 0;
}

BOOLEAN __stdcall RtlIsCriticalSectionLockedByThread(RTLS_CRITICAL_SECTION *cs)
{
    return k32s_cs_owned_by(cs->OwningThread, cs_self()) != 0;
}

/* ---- Condition variables ------------------------------------------------------------------------ */

typedef struct cv_cs_arg {
    RTLS_CRITICAL_SECTION *cs;
    LONG saved_recursion;
} cv_cs_arg;

static void cv_cs_release(void *p)
{
    cv_cs_arg *a = (cv_cs_arg *)p;
    /* A section entered recursively is released all the way and re-entered to the same depth. */
    a->saved_recursion = a->cs->RecursionCount;
    for (LONG i = 0; i < a->saved_recursion; ++i)
        RtlLeaveCriticalSection(a->cs);
}

static void cv_cs_acquire(void *p)
{
    cv_cs_arg *a = (cv_cs_arg *)p;
    for (LONG i = 0; i < a->saved_recursion; ++i)
        RtlEnterCriticalSection(a->cs);
}

typedef struct cv_srw_arg {
    PVOID *srw;
    int shared;
} cv_srw_arg;

static void cv_srw_release(void *p)
{
    cv_srw_arg *a = (cv_srw_arg *)p;
    if (a->shared)
        RtlReleaseSRWLockShared(a->srw);
    else
        RtlReleaseSRWLockExclusive(a->srw);
}

static void cv_srw_acquire(void *p)
{
    cv_srw_arg *a = (cv_srw_arg *)p;
    if (a->shared)
        RtlAcquireSRWLockShared(a->srw);
    else
        RtlAcquireSRWLockExclusive(a->srw);
}

VOID __stdcall RtlInitializeConditionVariable(PVOID *cv)
{
    *cv = NULL;
}

VOID __stdcall RtlWakeConditionVariable(PVOID *cv)
{
    rs_cv_wake(&g_ctx, (volatile uint32_t *)cv, 1);
}

VOID __stdcall RtlWakeAllConditionVariable(PVOID *cv)
{
    rs_cv_wake(&g_ctx, (volatile uint32_t *)cv, 0x7FFFFFFFu);
}

/* STATUS_SUCCESS when woken (the caller must still re-test its condition), STATUS_TIMEOUT otherwise. The
 * lock is held again on return either way. */
NTSTATUS __stdcall RtlSleepConditionVariableCS(PVOID *cv, RTLS_CRITICAL_SECTION *cs, PLARGE_INTEGER timeout)
{
    cv_cs_arg a;
    a.cs = cs;
    a.saved_recursion = 0;
    return rs_cv_sleep(&g_ctx, (volatile uint32_t *)cv, cv_cs_release, cv_cs_acquire, &a, timeout_ms(timeout))
               ? STATUS_SUCCESS
               : STATUS_TIMEOUT;
}

NTSTATUS __stdcall RtlSleepConditionVariableSRW(PVOID *cv, PVOID *srw, PLARGE_INTEGER timeout, ULONG flags)
{
    cv_srw_arg a;
    a.srw = srw;
    a.shared = (flags & 1u) != 0; /* RTL_CONDITION_VARIABLE_LOCKMODE_SHARED */
    return rs_cv_sleep(&g_ctx, (volatile uint32_t *)cv, cv_srw_release, cv_srw_acquire, &a, timeout_ms(timeout))
               ? STATUS_SUCCESS
               : STATUS_TIMEOUT;
}

/* ---- Wait on address ------------------------------------------------------------------------------- */

NTSTATUS __stdcall RtlWaitOnAddress(volatile VOID *addr, PVOID compare, SIZE_T size, PLARGE_INTEGER timeout)
{
    int r = rs_wait_on_address(&g_ctx, addr, compare, size, timeout_ms(timeout));
    if (r < 0)
        return STATUS_INVALID_PARAMETER;
    return r ? STATUS_SUCCESS : STATUS_TIMEOUT;
}

VOID __stdcall RtlWakeAddressSingle(PVOID addr)
{
    rs_wake_address(&g_ctx, addr, 0);
}

VOID __stdcall RtlWakeAddressAll(PVOID addr)
{
    rs_wake_address(&g_ctx, addr, 1);
}

/* ---- Run once --------------------------------------------------------------------------------------- */

typedef ULONG (__stdcall *RTLS_RUN_ONCE_FN)(PVOID once, PVOID parameter, PVOID *context);

VOID __stdcall RtlRunOnceInitialize(PVOID *once)
{
    *once = NULL;
}

static NTSTATUS once_status(int r)
{
    return r == RS_ONCE_ERR_INVALID ? STATUS_INVALID_PARAMETER : STATUS_UNSUCCESSFUL;
}

/* STATUS_PENDING when the caller must run the initialiser, STATUS_SUCCESS when it already ran (the
 * context comes back in *context). */
NTSTATUS __stdcall RtlRunOnceBeginInitialize(PVOID *once, ULONG flags, PVOID *context)
{
    int pending = 0;
    int r = rs_once_begin(&g_ctx, (volatile uintptr_t *)once, flags, &pending, context);
    if (r != RS_ONCE_OK)
        return once_status(r);
    return pending ? STATUS_PENDING : STATUS_SUCCESS;
}

NTSTATUS __stdcall RtlRunOnceComplete(PVOID *once, ULONG flags, PVOID context)
{
    int r = rs_once_complete(&g_ctx, (volatile uintptr_t *)once, flags, context);
    return r == RS_ONCE_OK ? STATUS_SUCCESS : once_status(r);
}

NTSTATUS __stdcall RtlRunOnceExecuteOnce(PVOID *once, RTLS_RUN_ONCE_FN fn, PVOID parameter, PVOID *context)
{
    PVOID ctx = NULL;
    NTSTATUS st = RtlRunOnceBeginInitialize(once, 0, &ctx);
    if (st == STATUS_PENDING) {
        ctx = NULL;
        if (fn((PVOID)once, parameter, &ctx)) {
            st = RtlRunOnceComplete(once, 0, ctx);
            if (!NT_SUCCESS(st))
                return st;
        } else {
            RtlRunOnceComplete(once, 4u /* RTL_RUN_ONCE_INIT_FAILED */, NULL);
            return STATUS_UNSUCCESSFUL;
        }
    } else if (!NT_SUCCESS(st)) {
        return st;
    }
    if (context)
        *context = ctx;
    return STATUS_SUCCESS;
}

/* ---- Interlocked lists ----------------------------------------------------------------------------- */

VOID __stdcall RtlInitializeSListHead(PVOID header)
{
    rs_slist_init((rs_slist_header *)header);
}

PVOID __stdcall RtlInterlockedPushEntrySList(PVOID header, PVOID entry)
{
    return rs_slist_push((rs_slist_header *)header, &g_sync_os, (rs_slist_entry *)entry);
}

PVOID __stdcall RtlInterlockedPushListSListEx(PVOID header, PVOID first, PVOID last, ULONG count)
{
    return rs_slist_push_list((rs_slist_header *)header, &g_sync_os, (rs_slist_entry *)first, (rs_slist_entry *)last, count);
}

PVOID __stdcall RtlInterlockedPopEntrySList(PVOID header)
{
    return rs_slist_pop((rs_slist_header *)header, &g_sync_os);
}

PVOID __stdcall RtlInterlockedFlushSList(PVOID header)
{
    return rs_slist_flush((rs_slist_header *)header, &g_sync_os);
}

USHORT __stdcall RtlQueryDepthSList(PVOID header)
{
    return rs_slist_depth((const rs_slist_header *)header);
}

PVOID __stdcall RtlFirstEntrySList(const VOID *header)
{
    return (PVOID)((const rs_slist_header *)header)->first;
}
#endif
