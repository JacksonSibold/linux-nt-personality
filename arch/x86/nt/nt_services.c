// SPDX-License-Identifier: GPL-2.0
/*
 * nt_services.c - sync-related NT service dispatch wrappers
 *
 * Wires SSNs for events/mutants/semaphores/pulse/perf into the real
 * implementations in sync.c and nt_obj.c, with strict user-pointer hygiene
 * (copy_from_user / copy_to_user, access_ok). All arguments arrive as user
 * pointers from do_nt_syscall and must not be dereferenced directly.
 *
 * Fixed frequency for NtQueryPerformanceCounter: 10 MHz (10,000,000 ticks/sec).
 * Rationale: Windows reports a fixed QPC frequency at boot (often 10MHz = 100ns
 * units, matching FILETIME). Using 10MHz gives simple ns->ticks conversion
 * (ticks = ns/100) and matches the 100ns units used throughout
 * probe_sync.json timing (109ms). Frequency is invariant, monotonic.
 */

#include "nt_internal.h"
#include "sync.h"
#include "object.h"
#include "handle.h"
#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#ifndef __KERNEL__
#include <string.h>
#include <time.h>
#include <unistd.h>
#else
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/ktime.h>
#endif

extern struct nt_object_type nt_event_type;
extern struct nt_object_type nt_mutant_type;
extern struct nt_object_type nt_semaphore_type;

/* Timer resolution (NtSetTimerResolution / NtQueryTimerResolution), 100 ns
 * units. Range 0.5 ms (finest) to 15.625 ms (coarsest, also the default).
 * Model: one global request. Windows' per-process request accounting is not
 * implemented, and the state is not locked. */
#define NT_TIMER_RES_MIN 5000UL
#define NT_TIMER_RES_MAX 156250UL
static ULONG nt_timer_res_current = NT_TIMER_RES_MAX;

/* Applies a request (set) or a release (!set) and returns the resulting
 * current resolution. A requested value is clamped to the legal range. */
static ULONG nt_timer_res_request(ULONG desired, BOOLEAN set)
{
    if (set) {
        if (desired < NT_TIMER_RES_MIN) desired = NT_TIMER_RES_MIN;
        if (desired > NT_TIMER_RES_MAX) desired = NT_TIMER_RES_MAX;
        nt_timer_res_current = desired;
    } else {
        nt_timer_res_current = NT_TIMER_RES_MAX;
    }
    return nt_timer_res_current;
}

/* helper: current eprocess for kernel - weak, object.c provides strong with lazy creation */
#ifdef __KERNEL__
__attribute__((weak)) struct nt_eprocess *nt_get_current_eprocess(void)
{
    return kshim_current_process();
}
#else
struct nt_eprocess *nt_get_current_eprocess(void) { return kshim_current_process(); }
#endif

/* ------------------------------------------------------------------ */
/* NtCreateEvent / NtOpenEvent already in nt_obj.c (kernel wrappers)   */
/* This file provides Set/Reset/Pulse/Release/Query wrappers           */
/* ------------------------------------------------------------------ */

#ifndef __KERNEL__
/* Userspace shim: Set/Reset/Release are already in nt_obj.c, keep only Pulse+Query here */
NTSTATUS nt_svc_NtPulseEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE EventHandle = (HANDLE)a0; PLONG PreviousState = (PLONG)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    struct nt_eprocess *proc = kshim_current_process();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, EventHandle, &nt_event_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_event *ev = (struct nt_event*)nt_object_body(hdr);
    LONG prev=0;
    st = nt_pulse_event(ev, PreviousState ? &prev : NULL);
    if (NT_SUCCESS(st) && PreviousState) *PreviousState = prev;
    ObDereferenceObject(hdr);
    return st;
}
NTSTATUS nt_svc_NtQueryEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE EventHandle = (HANDLE)a0; ULONG EventInformationClass = (ULONG)a1; PVOID EventInformation = (PVOID)a2; ULONG EventInformationLength = (ULONG)a3; PULONG ReturnLength = (PULONG)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    struct nt_eprocess *proc = kshim_current_process();
    if (!proc) return STATUS_INVALID_PARAMETER;
    if (EventInformationClass != 0) return STATUS_INVALID_INFO_CLASS;
    if (EventInformationLength != sizeof(ULONG)+sizeof(LONG)) {
        if (ReturnLength) *ReturnLength = 0;
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (ReturnLength) *ReturnLength = sizeof(ULONG)+sizeof(LONG);
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, EventHandle, &nt_event_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_event *ev = (struct nt_event*)nt_object_body(hdr);
    struct { ULONG EventType; LONG EventState; } *info = (void*)EventInformation;
    info->EventType = ev->header.manual_reset ? 0 : 1;
    info->EventState = ev->header.signal_state;
    if (ReturnLength) *ReturnLength = sizeof(*info);
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtQueryPerformanceCounter(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PLARGE_INTEGER PerformanceCounter = (PLARGE_INTEGER)a0; PLARGE_INTEGER PerformanceFrequency = (PLARGE_INTEGER)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    const LONGLONG freq = 10000000LL;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    LONGLONG ns = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    LONGLONG ticks = ns / 100;
    if (PerformanceCounter) PerformanceCounter->QuadPart = ticks;
    if (PerformanceFrequency) PerformanceFrequency->QuadPart = freq;
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtSetTimerResolution(ULONG DesiredTime, BOOLEAN SetResolution, PULONG ActualTime)
{
    if (!ActualTime) return STATUS_ACCESS_VIOLATION;
    *ActualTime = nt_timer_res_request(DesiredTime, SetResolution);
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtQueryTimerResolution(PULONG MaximumTime, PULONG MinimumTime, PULONG CurrentTime)
{
    if (!MaximumTime || !MinimumTime || !CurrentTime) return STATUS_ACCESS_VIOLATION;
    *MaximumTime = NT_TIMER_RES_MAX;
    *MinimumTime = NT_TIMER_RES_MIN;
    *CurrentTime = nt_timer_res_current;
    return STATUS_SUCCESS;
}
/* NtRaiseHardError: refused. A hard error would put up a message box and may
 * bring the system down, and this kernel has neither. The argument count is
 * still validated (at most MAXIMUM_HARDERROR_PARAMETERS). No user memory is
 * read or written, and no response is produced. */
#define NT_MAX_HARDERROR_PARAMETERS 4UL
NTSTATUS nt_svc_NtRaiseHardError(NTSTATUS ErrorStatus, ULONG NumberOfParameters, ULONG UnicodeStringParameterMask, ULONG_PTR *Parameters, ULONG ValidResponseOptions, PULONG Response)
{
    (void)ErrorStatus; (void)UnicodeStringParameterMask; (void)Parameters; (void)ValidResponseOptions; (void)Response;
    if (NumberOfParameters > NT_MAX_HARDERROR_PARAMETERS) return STATUS_INVALID_PARAMETER;
    return STATUS_NOT_SUPPORTED;
}
NTSTATUS nt_svc_NtQueryMutant(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE MutantHandle=(HANDLE)a0; ULONG MutantInformationClass=(ULONG)a1; PVOID MutantInformation=(PVOID)a2; ULONG MutantInformationLength=(ULONG)a3; PULONG ReturnLength=(PULONG)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (MutantInformationClass != 0) return STATUS_INVALID_INFO_CLASS;
    const ULONG needed = sizeof(ULONG)+sizeof(LONG)+sizeof(BOOLEAN)+16;
    if (ReturnLength) *ReturnLength = needed;
    if (MutantInformationLength != needed) return STATUS_INFO_LENGTH_MISMATCH;
    if (!MutantInformation) return STATUS_ACCESS_VIOLATION;
    struct nt_eprocess *proc = kshim_current_process();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, MutantHandle, &nt_mutant_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_mutant *m = (struct nt_mutant*)nt_object_body(hdr);
    /* Fill dummy MutantBasicInformation */
    struct { ULONG CurrentCount; BOOLEAN OwnedByCaller; BOOLEAN Abandoned; UCHAR Reserved[2]; ULONG OwnerThreadId; } *info = MutantInformation;
    info->CurrentCount = m->header.signal_state ? 1 : 0;
    info->OwnedByCaller = (m->owner && m->owner->tid == 0) ? 1 : 0;
    info->Abandoned = m->abandoned ? 1 : 0;
    info->OwnerThreadId = m->owner ? m->owner->tid : 0;
    if (ReturnLength) *ReturnLength = needed;
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtQuerySemaphore(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE SemaphoreHandle=(HANDLE)a0; ULONG SemaphoreInformationClass=(ULONG)a1; PVOID SemaphoreInformation=(PVOID)a2; ULONG SemaphoreInformationLength=(ULONG)a3; PULONG ReturnLength=(PULONG)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (SemaphoreInformationClass != 0) return STATUS_INVALID_INFO_CLASS;
    const ULONG needed = sizeof(ULONG)*2 + sizeof(LONG)*2;
    if (ReturnLength) *ReturnLength = needed;
    if (SemaphoreInformationLength != needed) return STATUS_INFO_LENGTH_MISMATCH;
    if (!SemaphoreInformation) return STATUS_ACCESS_VIOLATION;
    struct nt_eprocess *proc = kshim_current_process();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, SemaphoreHandle, &nt_semaphore_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_semaphore *s = (struct nt_semaphore*)nt_object_body(hdr);
    struct { ULONG CurrentCount; ULONG MaximumCount; } *info = SemaphoreInformation;
    info->CurrentCount = s->header.signal_state;
    info->MaximumCount = s->limit;
    if (ReturnLength) *ReturnLength = needed;
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}

#else /* __KERNEL__ */

/* True if the user LONG at p can be read and written. The value is written
 * back unchanged, so the probe has no visible effect (N-1). */
static bool nt_probe_user_long(PLONG p)
{
    LONG v;
    if (!access_ok(p, sizeof(LONG)))
        return false;
    if (get_user(v, p))
        return false;
    return put_user(v, p) == 0;
}

NTSTATUS nt_svc_NtSetEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE EventHandle = (HANDLE)a0; PLONG PreviousState = (PLONG)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, EventHandle, &nt_event_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_event *ev = (struct nt_event*)nt_object_body(hdr);
    LONG kprev=0;
    if (PreviousState && !nt_probe_user_long(PreviousState)) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    st = nt_set_event(ev, PreviousState ? &kprev : NULL);
    if (NT_SUCCESS(st) && PreviousState) {
        if (!access_ok(PreviousState, sizeof(LONG))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
        if (copy_to_user(PreviousState, &kprev, sizeof(kprev))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    }
    ObDereferenceObject(hdr);
    return st;
}
NTSTATUS nt_svc_NtResetEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE EventHandle = (HANDLE)a0; PLONG PreviousState = (PLONG)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, EventHandle, &nt_event_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_event *ev = (struct nt_event*)nt_object_body(hdr);
    LONG kprev=0;
    if (PreviousState && !nt_probe_user_long(PreviousState)) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    st = nt_reset_event(ev, PreviousState ? &kprev : NULL);
    if (NT_SUCCESS(st) && PreviousState) {
        if (!access_ok(PreviousState, sizeof(LONG))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
        if (copy_to_user(PreviousState, &kprev, sizeof(kprev))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    }
    ObDereferenceObject(hdr);
    return st;
}
NTSTATUS nt_svc_NtPulseEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE EventHandle = (HANDLE)a0; PLONG PreviousState = (PLONG)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, EventHandle, &nt_event_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_event *ev = (struct nt_event*)nt_object_body(hdr);
    LONG kprev=0;
    if (PreviousState && !nt_probe_user_long(PreviousState)) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    st = nt_pulse_event(ev, PreviousState ? &kprev : NULL);
    if (NT_SUCCESS(st) && PreviousState) {
        if (!access_ok(PreviousState, sizeof(LONG))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
        if (copy_to_user(PreviousState, &kprev, sizeof(kprev))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    }
    ObDereferenceObject(hdr);
    return st;
}
NTSTATUS nt_svc_NtReleaseMutant(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE MutantHandle = (HANDLE)a0; PLONG PreviousCount = (PLONG)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, MutantHandle, &nt_mutant_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_mutant *m = (struct nt_mutant*)nt_object_body(hdr);
    LONG kprev=0;
    if (PreviousCount && !nt_probe_user_long(PreviousCount)) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    st = nt_release_mutant(m, PreviousCount ? &kprev : NULL);
    if (NT_SUCCESS(st) && PreviousCount) {
        if (!access_ok(PreviousCount, sizeof(LONG))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
        if (copy_to_user(PreviousCount, &kprev, sizeof(kprev))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    }
    ObDereferenceObject(hdr);
    return st;
}
NTSTATUS nt_svc_NtReleaseSemaphore(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE SemaphoreHandle = (HANDLE)a0; LONG ReleaseCount = (LONG)a1; PLONG PreviousCount = (PLONG)a2;
    (void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (ReleaseCount <= 0) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, SemaphoreHandle, &nt_semaphore_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_semaphore *s = (struct nt_semaphore*)nt_object_body(hdr);
    LONG kprev=0;
    if (PreviousCount && !nt_probe_user_long(PreviousCount)) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    st = nt_release_semaphore(s, ReleaseCount, PreviousCount ? &kprev : NULL);
    if (NT_SUCCESS(st) && PreviousCount) {
        if (!access_ok(PreviousCount, sizeof(LONG))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
        if (copy_to_user(PreviousCount, &kprev, sizeof(kprev))) { ObDereferenceObject(hdr); return STATUS_ACCESS_VIOLATION; }
    }
    ObDereferenceObject(hdr);
    return st;
}
NTSTATUS nt_svc_NtQueryEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE EventHandle = (HANDLE)a0; ULONG EventInformationClass = (ULONG)a1; PVOID EventInformation = (PVOID)a2; ULONG EventInformationLength = (ULONG)a3; PULONG ReturnLength = (PULONG)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    if (EventInformationClass != 0) return STATUS_INVALID_INFO_CLASS;
    const ULONG needed = sizeof(ULONG) + sizeof(LONG);
    if (EventInformationLength != needed) {
        if (ReturnLength) {
            ULONG zero=0;
            if (access_ok(ReturnLength, sizeof(ULONG))) copy_to_user(ReturnLength, &zero, sizeof(zero));
        }
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (ReturnLength) {
        if (!access_ok(ReturnLength, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
        ULONG tmp=needed;
        if (copy_to_user(ReturnLength, &tmp, sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
    }
    if (!access_ok(EventInformation, needed)) return STATUS_ACCESS_VIOLATION;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, EventHandle, &nt_event_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_event *ev = (struct nt_event*)nt_object_body(hdr);
    struct { ULONG EventType; LONG EventState; } kinfo;
    kinfo.EventType = ev->header.manual_reset ? 0 : 1;
    kinfo.EventState = ev->header.signal_state;
    if (copy_to_user(EventInformation, &kinfo, sizeof(kinfo))) st = STATUS_ACCESS_VIOLATION;
    ObDereferenceObject(hdr);
    return st;
}
NTSTATUS nt_svc_NtQueryPerformanceCounter(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PLARGE_INTEGER PerformanceCounter = (PLARGE_INTEGER)a0; PLARGE_INTEGER PerformanceFrequency = (PLARGE_INTEGER)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    const LONGLONG freq = 10000000LL;
    u64 ns = ktime_get_ns();
    LONGLONG ticks = (LONGLONG)(ns / 100);
    if (PerformanceCounter) {
        if (!access_ok(PerformanceCounter, sizeof(LARGE_INTEGER))) return STATUS_ACCESS_VIOLATION;
        LARGE_INTEGER kcnt; kcnt.QuadPart = ticks;
        if (copy_to_user(PerformanceCounter, &kcnt, sizeof(kcnt))) return STATUS_ACCESS_VIOLATION;
    }
    if (PerformanceFrequency) {
        if (!access_ok(PerformanceFrequency, sizeof(LARGE_INTEGER))) return STATUS_ACCESS_VIOLATION;
        LARGE_INTEGER kfreq; kfreq.QuadPart = freq;
        if (copy_to_user(PerformanceFrequency, &kfreq, sizeof(kfreq))) return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtSetTimerResolution(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    ULONG DesiredTime = (ULONG)a0; BOOLEAN SetResolution = a1 ? TRUE : FALSE; PULONG ActualTime = (PULONG)a2;
    ULONG actual;
    (void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (!ActualTime) return STATUS_ACCESS_VIOLATION;
    if (!access_ok(ActualTime, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    actual = nt_timer_res_request(DesiredTime, SetResolution);
    if (copy_to_user(ActualTime, &actual, sizeof(actual))) return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtQueryTimerResolution(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PULONG MaximumTime = (PULONG)a0; PULONG MinimumTime = (PULONG)a1; PULONG CurrentTime = (PULONG)a2;
    ULONG vals[3];
    (void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (!MaximumTime || !MinimumTime || !CurrentTime) return STATUS_ACCESS_VIOLATION;
    if (!access_ok(MaximumTime, sizeof(ULONG)) || !access_ok(MinimumTime, sizeof(ULONG)) ||
        !access_ok(CurrentTime, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    vals[0] = NT_TIMER_RES_MAX;
    vals[1] = NT_TIMER_RES_MIN;
    vals[2] = nt_timer_res_current;
    if (copy_to_user(MaximumTime, &vals[0], sizeof(ULONG)) ||
        copy_to_user(MinimumTime, &vals[1], sizeof(ULONG)) ||
        copy_to_user(CurrentTime, &vals[2], sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtRaiseHardError(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    NTSTATUS ErrorStatus = (NTSTATUS)a0; ULONG NumberOfParameters = (ULONG)a1;
    (void)ErrorStatus;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (NumberOfParameters > NT_MAX_HARDERROR_PARAMETERS) return STATUS_INVALID_PARAMETER;
    return STATUS_NOT_SUPPORTED;
}
NTSTATUS nt_svc_NtQueryMutant(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE MutantHandle=(HANDLE)a0; ULONG MutantInformationClass=(ULONG)a1; PVOID MutantInformation=(PVOID)a2; ULONG MutantInformationLength=(ULONG)a3; PULONG ReturnLength=(PULONG)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (MutantInformationClass != 0) return STATUS_INVALID_INFO_CLASS;
    const ULONG needed = 24;
    if (ReturnLength) {
        if (!access_ok(ReturnLength, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
        ULONG tmp=needed;
        if (copy_to_user(ReturnLength, &tmp, sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
    }
    if (MutantInformationLength != needed) return STATUS_INFO_LENGTH_MISMATCH;
    if (!access_ok(MutantInformation, needed)) return STATUS_ACCESS_VIOLATION;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, MutantHandle, &nt_mutant_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_mutant *m = (struct nt_mutant*)nt_object_body(hdr);
    /* 24 bytes, the length reported above; the whole buffer is copied and
     * zeroed first so no byte is left unset (N-2, N-3). */
    struct { ULONG CurrentCount; BOOLEAN Owned; BOOLEAN Abandoned; UCHAR R[2]; ULONG OwnerTid; ULONG Reserved[3]; } kinfo;
    memset(&kinfo, 0, sizeof(kinfo));
    kinfo.CurrentCount = m->header.signal_state ? 1 : 0;
    kinfo.Owned = m->owner ? 1 : 0;
    kinfo.Abandoned = m->abandoned ? 1 : 0;
    kinfo.OwnerTid = m->owner ? m->owner->tid : 0;
    if (copy_to_user(MutantInformation, &kinfo, sizeof(kinfo))) st = STATUS_ACCESS_VIOLATION;
    ObDereferenceObject(hdr);
    return st;
}
NTSTATUS nt_svc_NtQuerySemaphore(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE SemaphoreHandle=(HANDLE)a0; ULONG SemaphoreInformationClass=(ULONG)a1; PVOID SemaphoreInformation=(PVOID)a2; ULONG SemaphoreInformationLength=(ULONG)a3; PULONG ReturnLength=(PULONG)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (SemaphoreInformationClass != 0) return STATUS_INVALID_INFO_CLASS;
    const ULONG needed = 16;
    if (ReturnLength) {
        if (!access_ok(ReturnLength, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
        ULONG tmp=needed;
        if (copy_to_user(ReturnLength, &tmp, sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
    }
    if (SemaphoreInformationLength != needed) return STATUS_INFO_LENGTH_MISMATCH;
    if (!access_ok(SemaphoreInformation, needed)) return STATUS_ACCESS_VIOLATION;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, SemaphoreHandle, &nt_semaphore_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_semaphore *s = (struct nt_semaphore*)nt_object_body(hdr);
    struct { ULONG CurrentCount; ULONG MaximumCount; } kinfo;
    kinfo.CurrentCount = s->header.signal_state;
    kinfo.MaximumCount = s->limit;
    if (copy_to_user(SemaphoreInformation, &kinfo, sizeof(kinfo))) st = STATUS_ACCESS_VIOLATION;
    ObDereferenceObject(hdr);
    return st;
}

#endif /* __KERNEL__ */
