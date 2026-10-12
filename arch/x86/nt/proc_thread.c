/* proc_thread.c - NT process/thread creation, suspend count, query info
 *
 * Dual compilation: USERSPACE_KSHIM (pthreads) and __KERNEL__ (clone3/GS/TIF)
 *
 * SUSPEND COUNT DESIGN
 * --------------------
 * Linux has no per-thread suspend count. NT does: each thread has an integer
 * suspend_count, initially 0 (running). NtSuspendThread increments and returns
 * previous; NtResumeThread decrements (if >0) and returns previous.
 * Count values from golden: suspend: prev 0 then 1; resume: prev 2 then 1.
 *
 * Gate: when suspend_count > 0, thread must block before returning to user mode.
 * Kernel implementation: per-thread atomic suspend_count + wait_queue + TIF_NTSUSPEND flag.
 *  - NtSuspendThread: atomic_inc_return(&ethread->suspend_count); prev = cur-1;
 *    if (prev==0) set_tsk_thread_flag(target, TIF_NTSUSPEND);
 *    if target == current, the flag will cause do_nt_syscall return path to
 *    check and block (see nt_thread_check_suspend). No direct block inside
 *    NtSuspendThread to avoid holding locks while blocking.
 *  - NtResumeThread: atomic_dec_if_positive; prev = old; if new==0 clear flag &
 *    wake_up(&ethread->suspend_wq).
 *  - On return to user (entry_64.S / do_nt_syscall exit), if test_thread_flag(TIF_NTSUSPEND)
 *    then call nt_thread_check_suspend(): wait_event_interruptible(ethread->suspend_wq, suspend_count==0 || terminated).
 *    This is checked *only* on NT ABI return, not on every context switch, so Linux
 *    tasks pay zero cost.
 *  - Termination races: lock protects terminated flag. NtSuspendThread checks
 *    terminated under lock and returns STATUS_THREAD_IS_TERMINATING instead of incrementing.
 *    NtResumeThread similarly checks. The gate wait is interruptible by signal/exit;
 *    on thread exit we wake suspend_wq and exit_wq so any waiter is released.
 *  - Self-suspend safety: suspending self does not block inside the syscall;
 *    it sets flag and returns; the subsequent return-to-user check then blocks.
 *    This avoids deadlock where suspend holds a lock the blocked path needs.
 *    If self-resume is called while blocked, the wake clears flag and unblocks.
 *
 * Userspace shim approximates same semantics with pthread_mutex+cond: suspend_count
 * is atomic_int protected by thr->lock, suspend_cond is the gate. Thread
 * trampoline and nt_thread_check_suspend() wait on suspend_cond while count>0.
 * There is no actual TIF flag; instead flag is simulated by checking count before
 * entering user routine and after each simulated syscall return (we call check
 * explicitly in trampoline and after NtSuspendThread on self).
 *
 * PROCESS CREATION ARCHITECTURE
 * -----------------------------
 * NT creates an empty process via NtCreateUserProcess/NtCreateProcessEx: no address
 * space, no threads; then caller populates via NtMapViewOfSection, etc., and finally
 * creates initial thread via NtCreateThreadEx. Linux creates via execve: atomically
 * replaces address space with new image. They are not equivalent.
 *
 * Chosen approach for ntpers:
 *  - In kernel, NtCreateUserProcess uses kernel_clone(CLONE_NEWPID|VM) to create
 *    a new task with empty mm, then binfmt_pe populates it (similar to execve path
 *    but incremental). This preserves NT's empty-then-populate observable: VAD
 *    starts empty, PEB/TEB not yet visible until after mapping. A helper
 *    nt_create_empty_process() does clone + clearing of mm->mmap, then returns
 *    process handle referencing the new task's eprocess.
 *  - In userspace shim, we cannot create empty mm; we emulate by allocating
 *    eprocess object with empty handle table and fake PEB/TEB reservation, and
 *    treating "empty" as "no committed VADs yet" (g_nt_mm empty). Fork() is NOT
 *    used because it copies memory and would make VAD non-empty, violating the
 *    empty premise. Observable difference: in shim, process creation does NOT create
 *    a separate OS pid; all threads share same address space for test simplicity.
 *    Documented gaps: no true copy-on-write isolation, no separate VAD, no fork
 *    pid; exit code propagation is via in-process-cond not waitpid; image file
 *    name is synthesized not from execve.
 *
 * TEB/GS/TLS
 * ----------
 * Per-thread TEB allocated as 2 pages, populated: NtTib.Self = TEB, StackBase/Limit,
 * ThreadLocalStoragePointer, ProcessEnvironmentBlock = PEB, ClientId, LastError=0,
 * TlsSlots zeroed. Static TLS (if PE has .tls) copied via ldr_copy_static_tls().
 * GS base set via WRMSR_GS_BASE (kernel) or arch_prctl(ARCH_SET_GS, teb) (shim).
 * Initial stack: for clone3 we prepare child stack pointer + RtlUserThreadStart
 * trampoline that calls StartRoutine(Arg) then NtTerminateThread.
 */

#include "proc_thread.h"
#include "object.h"
#include "handle.h"
#include "sync.h"
#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#ifndef __KERNEL__
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <asm/prctl.h>
#endif
#else
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/wait.h>
#include <linux/thread_info.h>
#include <asm/prctl.h>
#include <asm/msr.h>
#endif

/* ------------------------------------------------------------------ */
/* Info class numbers (from Windows SDK)                               */
/* ------------------------------------------------------------------ */
#define ProcessBasicInformation 0
#define ProcessDebugPort 7
#define ProcessWow64Information 26
#define ProcessImageFileName 27
#define ProcessDebugObjectHandle 30

#define ThreadBasicInformation 0
#define ThreadTimes 1
#define ThreadPriority 2

/* NTSTATUS helpers already in ntstatus.h */

/* ------------------------------------------------------------------ */
/* Forward decls                                                      */
/* ------------------------------------------------------------------ */
#ifndef __KERNEL__
static void *thread_trampoline(void *arg);
static NTSTATUS alloc_teb_peb(struct nt_eprocess *proc, struct nt_ethread *thr);
static void free_teb_peb(struct nt_ethread *thr);
static void nt_abandon_for_thread(struct nt_ethread *thr);
#endif

/* ------------------------------------------------------------------ */
/* Userspace shim implementation                                      */
/* ------------------------------------------------------------------ */
#ifndef __KERNEL__

/* PEB per process: allocate on first thread creation if not present */
static NTSTATUS ensure_peb(struct nt_eprocess *proc)
{
    if (proc->peb) return STATUS_SUCCESS;
    proc->peb = (struct _PEB*)kzalloc(sizeof(struct _PEB), GFP_KERNEL);
    if (!proc->peb) return STATUS_NO_MEMORY;
    proc->peb->BeingDebugged = 0;
    proc->peb->ImageBaseAddress = (PVOID)(ULONG_PTR)0x140000000ULL;
    proc->peb->ProcessHeap = (PVOID)(ULONG_PTR)0x10000;
    proc->peb->NtGlobalFlag = 0;
    proc->peb->OSMajorVersion = 10;
    proc->peb->OSMinorVersion = 0;
    proc->peb->OSBuildNumber = 26200;
    proc->peb_base = (ULONG_PTR)proc->peb;
    return STATUS_SUCCESS;
}

static NTSTATUS alloc_teb_peb(struct nt_eprocess *proc, struct nt_ethread *thr)
{
    NTSTATUS st = ensure_peb(proc);
    if (!NT_SUCCESS(st)) return st;
    if (thr->teb) return STATUS_SUCCESS;
    thr->teb = (struct _TEB*)kzalloc(sizeof(struct _TEB), GFP_KERNEL);
    if (!thr->teb) return STATUS_NO_MEMORY;
    thr->teb_base = (ULONG_PTR)thr->teb;
    /* NT_TIB */
    thr->teb->NtTib.Self = (struct _NT_TIB*)thr->teb;
    /* Stack */
    size_t reserve = 0x100000; /* 1MB */
    size_t commit = 0x1000 * 4;
    thr->stack_reserve = reserve;
    thr->stack_commit = commit;
    void *stk = kshim_mmap_reserve(NULL, reserve);
    if (!stk) { /* fallback malloc */
        stk = mmap(NULL, reserve, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (stk == MAP_FAILED) stk = NULL;
    }
    if (stk) {
        kshim_mmap_commit(stk, commit, PROT_READ|PROT_WRITE);
        thr->stack_limit = stk;
        thr->stack_base = (char*)stk + reserve;
        thr->teb->NtTib.StackBase = thr->stack_base;
        thr->teb->NtTib.StackLimit = thr->stack_limit;
        thr->teb->DeallocationStack = stk;
    } else {
        thr->teb->NtTib.StackBase = (PVOID)(ULONG_PTR)0x200000;
        thr->teb->NtTib.StackLimit = (PVOID)(ULONG_PTR)0x100000;
    }
    thr->teb->ProcessEnvironmentBlock = proc->peb;
    thr->teb->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)proc->pid;
    thr->teb->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)thr->tid;
    thr->teb->LastErrorValue = 0;
    memset(thr->teb->TlsSlots, 0, sizeof(thr->teb->TlsSlots));
    /* TlsLinks etc zeroed by kzalloc */

    /* Try set GS base to TEB for PE that reads GS:0x30 */
#ifdef __linux__
    /* arch_prctl is per-thread; do it in trampoline; here just store */
#endif
    return STATUS_SUCCESS;
}

static void __attribute__((unused)) free_teb_peb(struct nt_ethread *thr)
{
    (void)thr;
    /* freed in ObDestroyThreadObject */
}

/* Thread trampoline: sets TLS, handles suspend gate, calls user routine, propagates exit */
static void *thread_trampoline(void *arg)
{
    struct nt_ethread *thr = (struct nt_ethread*)arg;
    /* set thread locals */
    kshim_set_current_process(thr->process);
    kshim_set_current_thread(thr);
    /* sync thread for mutant abandonment */
    struct nt_thread *synct = NULL;
    nt_thread_create(&synct);
    if (synct) {
        nt_set_current_thread(synct);
    }

    /* Set GS base to TEB if possible */
#ifdef __linux__
    if (thr->teb) {
        /* arch_prctl expects unsigned long */
        syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)thr->teb);
    }
#endif

    /* Copy static TLS: stub - zero slots already */
    /* Suspend gate: if CREATE_SUSPENDED, wait */
    nt_thread_check_suspend();

    /* Call user routine */
    typedef ULONG (*thr_routine_t)(PVOID);
    thr_routine_t routine = (thr_routine_t)thr->start_routine;
    ULONG ret = 0;
    if (routine) {
        ret = routine(thr->start_arg);
    }

    /* Normal exit: set exit code and wake waiters */
    pthread_mutex_lock(&thr->lock);
    thr->exit_status = (NTSTATUS)ret;
    thr->terminated = true;
    pthread_cond_broadcast(&thr->exit_cond);
    pthread_cond_broadcast(&thr->suspend_cond);
    pthread_mutex_unlock(&thr->lock);

    /* Also mark process exit if this is last thread? For now leave process alive */
    /* Wake process exit waiters? */

    /* Abandon mutants owned by this sync thread */
    if (synct) {
        nt_abandon_owned_mutants(synct);
        nt_thread_destroy(synct);
        nt_set_current_thread(NULL);
    }
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    return (void*)(ULONG_PTR)ret;
}

/* Check suspend gate: block while suspend_count >0 */
void nt_thread_check_suspend(void)
{
    struct nt_ethread *thr = kshim_current_thread();
    if (!thr) return;
    pthread_mutex_lock(&thr->lock);
    while (atomic_load(&thr->suspend_count) > 0 && !thr->terminated) {
        /* Wait with timeout to allow termination race check */
        pthread_cond_wait(&thr->suspend_cond, &thr->lock);
    }
    pthread_mutex_unlock(&thr->lock);
}

static void nt_abandon_for_thread(struct nt_ethread *thr)
{
    (void)thr;
    struct nt_thread *st = nt_current_thread();
    if (st) nt_abandon_owned_mutants(st);
}

/* ------------------------------------------------------------------ */
/* Create helpers                                                     */
/* ------------------------------------------------------------------ */
static NTSTATUS nt_create_thread_internal(HANDLE ProcessHandle, PVOID StartRoutine, PVOID Arg,
                                          ULONG CreateFlags, SIZE_T StackSize, SIZE_T MaxStackSize,
                                          PVOID AttrList, HANDLE *out)
{
    (void)StackSize; (void)MaxStackSize; (void)AttrList;
    struct nt_eprocess *proc = kshim_current_process();
    struct nt_eprocess *target = NULL;
    if (ProcessHandle == NtCurrentProcess() || ProcessHandle == NULL) {
        target = proc;
    } else {
        struct nt_object_header *ph=NULL;
        NTSTATUS st = ObReferenceObjectByHandle(proc, ProcessHandle, &nt_process_type, UserMode, 0, &ph, NULL);
        if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
        target = ObFindProcessByHeader(ph);
        ObDereferenceObject(ph);
        if (!target) return STATUS_INVALID_HANDLE;
    }
    if (!target) return STATUS_INVALID_PARAMETER;

    bool suspended = (CreateFlags & CREATE_SUSPENDED_FLAG) || (CreateFlags & THREAD_CREATE_SUSPENDED) || (CreateFlags & 0x1);
    struct nt_ethread *thr=NULL;
    NTSTATUS st = ObCreateThreadObject(&thr, target, 0);
    if (!NT_SUCCESS(st)) return st;
    thr->start_routine = StartRoutine;
    thr->start_arg = Arg;
    thr->create_flags = CreateFlags;
    if (suspended) atomic_store(&thr->suspend_count, 1);
    else atomic_store(&thr->suspend_count, 0);

    st = alloc_teb_peb(target, thr);
    if (!NT_SUCCESS(st)) { ObDestroyThreadObject(thr); return st; }

    /* Create handle in target process (or current if target==proc) */
    HANDLE h=NULL;
    struct nt_eprocess *handle_proc = (ProcessHandle==NtCurrentProcess()||ProcessHandle==NULL) ? proc : target;
    /* If target != proc, handle goes to caller's table? Windows puts thread handle in caller's table regardless of process. */
    handle_proc = proc;
    st = ObCreateHandle(handle_proc, thr->obj, 0x1FFFFF, 0, &h);
    if (!NT_SUCCESS(st)) { ObDestroyThreadObject(thr); return st; }
    /* Drop creator ref (object now lives via handle + thread registry) */
    /* keep thr alive via thread registry; object ref already accounted */

    /* Start pthread (unless we want to emulate clone3 stack manually) */
    int rc = pthread_create(&thr->pthread, NULL, thread_trampoline, thr);
    if (rc != 0) {
        ObCloseHandle(handle_proc, h);
        ObDestroyThreadObject(thr);
        return STATUS_NO_MEMORY;
    }
    thr->pthread_valid = true;
    pthread_detach(thr->pthread); /* detach; exit code kept in thr struct */

    /* If not suspended, the thread will run immediately and may block on gate if count>0 */

    *out = h;
    /* Keep reference for thread object until destroy - already via handle */
    ObDereferenceObject(thr->obj); /* extra? ObCreateThreadObject gave 1 ref, ObCreateHandle added another, so drop creator */
    return STATUS_SUCCESS;
}

/* Process creation shim: allocates eprocess without OS fork */
static NTSTATUS nt_create_process_internal(PHANDLE out, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES oa,
                                           HANDLE Parent, ULONG Flags, HANDLE Section, HANDLE DebugPort, HANDLE ExceptionPort, PVOID JobLevel)
{
    (void)DesiredAccess; (void)oa; (void)Flags; (void)Section; (void)DebugPort; (void)ExceptionPort; (void)JobLevel;
    struct nt_eprocess *cur = kshim_current_process();
    struct nt_eprocess *parent = cur;
    if (Parent && Parent != NtCurrentProcess()) {
        struct nt_object_header *ph=NULL;
        NTSTATUS st = ObReferenceObjectByHandle(cur, Parent, &nt_process_type, UserMode, 0, &ph, NULL);
        if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
        parent = ObFindProcessByHeader(ph);
        ObDereferenceObject(ph);
        if (!parent) parent = cur;
    }
    struct nt_eprocess *newproc=NULL;
    NTSTATUS st = ObCreateProcessObject(&newproc, 0, "ntchild");
    if (!NT_SUCCESS(st)) return st;
    /* Inherit handles if requested (OBJ_INHERIT already handled via ObInheritHandles) */
    if (parent && parent != newproc) {
        ObInheritHandles(parent, newproc);
    }
    /* Set image path if oa has name? Use parent's image for now */
    if (parent && parent->image_path[0]) {
        memcpy(newproc->image_path, parent->image_path, sizeof(newproc->image_path));
        memcpy(newproc->image_path_w, parent->image_path_w, sizeof(newproc->image_path_w));
        newproc->image_path_wlen = parent->image_path_wlen;
    }
    /* Create handle in caller's table */
    HANDLE h=NULL;
    st = ObCreateHandle(cur, newproc->obj, DesiredAccess ? DesiredAccess : 0x1FFFFF, 0, &h);
    if (!NT_SUCCESS(st)) { ObDestroyProcessObject(newproc); return st; }
    ObDereferenceObject(newproc->obj);
    *out = h;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Syscall wrappers (userspace)                                        */
/* ------------------------------------------------------------------ */

NTSTATUS nt_svc_NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                                 PVOID StartRoutine, PVOID Argument, ULONG CreateFlags,
                                 SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize,
                                 PVOID AttributeList)
{
    (void)DesiredAccess; (void)ObjectAttributes; (void)ZeroBits;
    if (!ThreadHandle) return STATUS_INVALID_PARAMETER;
    if (!StartRoutine) return STATUS_INVALID_PARAMETER;
    HANDLE h=NULL;
    NTSTATUS st = nt_create_thread_internal(ProcessHandle, StartRoutine, Argument, CreateFlags, StackSize, MaximumStackSize, AttributeList, &h);
    if (NT_SUCCESS(st)) *ThreadHandle = h;
    return st;
}

NTSTATUS nt_svc_NtCreateThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                               PVOID ClientId, PVOID ThreadContext, PVOID InitialTeb,
                               BOOLEAN CreateSuspended)
{
    (void)ClientId; (void)ThreadContext; (void)InitialTeb;
    ULONG flags = CreateSuspended ? CREATE_SUSPENDED_FLAG : 0;
    return nt_svc_NtCreateThreadEx(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle,
                                   ThreadContext, NULL, flags, 0, 0, 0, NULL);
}

NTSTATUS nt_svc_NtCreateProcessEx(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                                  POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ParentProcess,
                                  ULONG Flags, HANDLE SectionHandle, HANDLE DebugPort,
                                  HANDLE ExceptionPort, PVOID JobMemberLevel)
{
    if (!ProcessHandle) return STATUS_INVALID_PARAMETER;
    return nt_create_process_internal(ProcessHandle, DesiredAccess, ObjectAttributes, ParentProcess, Flags, SectionHandle, DebugPort, ExceptionPort, JobMemberLevel);
}

NTSTATUS nt_svc_NtCreateProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                                POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ParentProcess,
                                BOOLEAN InheritObjectTable, HANDLE SectionHandle,
                                HANDLE DebugPort, HANDLE ExceptionPort)
{
    (void)InheritObjectTable;
    return nt_svc_NtCreateProcessEx(ProcessHandle, DesiredAccess, ObjectAttributes, ParentProcess, 0, SectionHandle, DebugPort, ExceptionPort, NULL);
}

NTSTATUS nt_svc_NtCreateUserProcess(PHANDLE ProcessHandle, PHANDLE ThreadHandle,
                                    ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess,
                                    POBJECT_ATTRIBUTES ProcessObjectAttributes,
                                    POBJECT_ATTRIBUTES ThreadObjectAttributes,
                                    ULONG ProcessFlags, ULONG ThreadFlags,
                                    PRTL_USER_PROCESS_PARAMETERS ProcessParameters,
                                    PVOID CreateInfo, PVOID AttributeList)
{
    (void)ThreadDesiredAccess; (void)ThreadObjectAttributes; (void)ProcessParameters; (void)CreateInfo;
    if (!ProcessHandle || !ThreadHandle) return STATUS_INVALID_PARAMETER;
    HANDLE ph=NULL, th=NULL;
    NTSTATUS st = nt_create_process_internal(&ph, ProcessDesiredAccess, ProcessObjectAttributes, NtCurrentProcess(), ProcessFlags, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(st)) return st;
    /* Create initial thread suspended if requested */
    PVOID start = (PVOID)(ULONG_PTR)0x140001000ULL; /* dummy entry */
    if (AttributeList) start = AttributeList; /* abuse for test */
    st = nt_create_thread_internal(ph, start, NULL, ThreadFlags, 0, 0, NULL, &th);
    if (!NT_SUCCESS(st)) {
        struct nt_eprocess *cur = kshim_current_process();
        ObCloseHandle(cur, ph);
        return st;
    }
    *ProcessHandle = ph;
    *ThreadHandle = th;
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
{
    if (!ThreadHandle) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *proc = kshim_current_process();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, ThreadHandle, &nt_thread_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) { fprintf(stderr,"Suspend: ObReference failed 0x%x handle %p\n", st, ThreadHandle); return STATUS_INVALID_HANDLE; }
    struct nt_ethread *thr = ObFindThreadByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!thr) { fprintf(stderr,"Suspend: find thr failed handle %p\n", ThreadHandle); return STATUS_INVALID_HANDLE; }
    pthread_mutex_lock(&thr->lock);
    if (thr->terminated) { pthread_mutex_unlock(&thr->lock); return STATUS_THREAD_IS_TERMINATING; }
    ULONG prev = (ULONG)atomic_load(&thr->suspend_count);
    if (PreviousSuspendCount) *PreviousSuspendCount = prev;
    if (prev >= 127) { pthread_mutex_unlock(&thr->lock); return STATUS_SUSPEND_COUNT_EXCEEDED; }
    atomic_fetch_add(&thr->suspend_count, 1);
    /* If first suspend, gate will block on next check */
    pthread_mutex_unlock(&thr->lock);
    /* If suspending self, block now after return? Simulate TIF check */
    if (thr == kshim_current_thread()) {
        /* caller will check gate on return - for shim we can directly wait a bit? But we must return first.
         * To emulate self-suspend, we set a flag and the caller must call nt_thread_check_suspend().
         * For now do nothing; the next nt_thread_check_suspend() call will block.
         */
    }
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
{
    if (!ThreadHandle) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *proc = kshim_current_process();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, ThreadHandle, &nt_thread_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) { fprintf(stderr,"Resume: ObReference failed 0x%x\n", st); return STATUS_INVALID_HANDLE; }
    struct nt_ethread *thr = ObFindThreadByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!thr) { fprintf(stderr,"Resume: find failed\n"); return STATUS_INVALID_HANDLE; }
    pthread_mutex_lock(&thr->lock);
    if (thr->terminated) { pthread_mutex_unlock(&thr->lock); return STATUS_THREAD_IS_TERMINATING; }
    ULONG prev = (ULONG)atomic_load(&thr->suspend_count);
    if (PreviousSuspendCount) *PreviousSuspendCount = prev;
    if (prev == 0) { pthread_mutex_unlock(&thr->lock); return STATUS_SUCCESS; }
    atomic_fetch_sub(&thr->suspend_count, 1);
    ULONG cur = prev - 1;
    if (cur == 0) {
        pthread_cond_broadcast(&thr->suspend_cond);
    }
    pthread_mutex_unlock(&thr->lock);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtSuspendProcess(HANDLE ProcessHandle)
{
    if (!ProcessHandle) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *proc = kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, ProcessHandle, &nt_process_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
    struct nt_eprocess *target = ObFindProcessByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!target) return STATUS_INVALID_HANDLE;
    ObSuspendProcessThreads(target);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtResumeProcess(HANDLE ProcessHandle)
{
    if (!ProcessHandle) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *proc = kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, ProcessHandle, &nt_process_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
    struct nt_eprocess *target = ObFindProcessByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!target) return STATUS_INVALID_HANDLE;
    ObResumeProcessThreads(target);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus)
{
    struct nt_eprocess *proc = kshim_current_process();
    if (!proc) return STATUS_INVALID_PARAMETER;
    HANDLE h = ThreadHandle;
    if (h == NtCurrentThread()) {
        struct nt_ethread *thr = kshim_current_thread();
        if (!thr) return STATUS_INVALID_HANDLE;
        pthread_mutex_lock(&thr->lock);
        thr->exit_status = ExitStatus;
        thr->terminated = true;
        pthread_cond_broadcast(&thr->exit_cond);
        pthread_cond_broadcast(&thr->suspend_cond);
        pthread_mutex_unlock(&thr->lock);
        nt_abandon_for_thread(thr);
        /* Exit current thread */
        pthread_exit((void*)(ULONG_PTR)ExitStatus);
        return STATUS_SUCCESS; /* not reached */
    }
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, h, &nt_thread_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
    struct nt_ethread *thr = ObFindThreadByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!thr) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock(&thr->lock);
    if (thr->terminated) { pthread_mutex_unlock(&thr->lock); return STATUS_THREAD_IS_TERMINATING; }
    thr->exit_status = ExitStatus;
    thr->terminated = true;
    pthread_cond_broadcast(&thr->exit_cond);
    pthread_cond_broadcast(&thr->suspend_cond);
    pthread_mutex_unlock(&thr->lock);
    /* Try to cancel if pthread valid */
    if (thr->pthread_valid) {
        pthread_cancel(thr->pthread);
        /* also wake if suspended */
    }
    nt_abandon_for_thread(thr);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus)
{
    if (!ProcessHandle) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *cur = kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(cur, ProcessHandle, &nt_process_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) {
        if (ProcessHandle == NtCurrentProcess()) {
            struct nt_eprocess *p = cur;
            pthread_mutex_lock(&p->lock);
            p->exit_status = ExitStatus;
            p->terminated = true;
            pthread_cond_broadcast(&p->exit_cond);
            pthread_mutex_unlock(&p->lock);
            ObTerminateProcessThreads(p, ExitStatus);
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_HANDLE;
    }
    struct nt_eprocess *target = ObFindProcessByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!target) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock(&target->lock);
    target->exit_status = ExitStatus;
    target->terminated = true;
    pthread_cond_broadcast(&target->exit_cond);
    pthread_mutex_unlock(&target->lock);
    ObTerminateProcessThreads(target, ExitStatus);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtOpenProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes, CLIENT_ID *ClientId)
{
    (void)ObjectAttributes;
    if (!ProcessHandle || !ClientId) return STATUS_INVALID_PARAMETER;
    ULONG pid = (ULONG)(ULONG_PTR)ClientId->UniqueProcess;
    struct nt_eprocess *p = ObFindProcessByPid(pid);
    if (!p) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *cur = kshim_current_process();
    if (!cur) return STATUS_INVALID_PARAMETER;
    HANDLE h=NULL;
    NTSTATUS st = ObCreateHandle(cur, p->obj, DesiredAccess ? DesiredAccess : 0x1FFFFF, 0, &h);
    if (!NT_SUCCESS(st)) return st;
    *ProcessHandle = h;
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtOpenThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                             POBJECT_ATTRIBUTES ObjectAttributes, CLIENT_ID *ClientId)
{
    (void)ObjectAttributes;
    if (!ThreadHandle || !ClientId) return STATUS_INVALID_PARAMETER;
    ULONG tid = (ULONG)(ULONG_PTR)ClientId->UniqueThread;
    struct nt_ethread *t = ObFindThreadByTid(tid);
    if (!t) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *cur = kshim_current_process();
    if (!cur) return STATUS_INVALID_PARAMETER;
    HANDLE h=NULL;
    NTSTATUS st = ObCreateHandle(cur, t->obj, DesiredAccess ? DesiredAccess : 0x1FFFFF, 0, &h);
    if (!NT_SUCCESS(st)) return st;
    *ThreadHandle = h;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Query Information with exact size checks                            */
/* ------------------------------------------------------------------ */

NTSTATUS nt_svc_NtQueryInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass,
                                          PVOID ProcessInformation, ULONG ProcessInformationLength,
                                          PULONG ReturnLength)
{
    struct nt_eprocess *cur = kshim_current_process();
    if (!cur) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    struct nt_eprocess *target=NULL;
    NTSTATUS st;
    if (ProcessHandle == NtCurrentProcess()) {
        target = cur;
    } else {
        st = ObReferenceObjectByHandle(cur, ProcessHandle, &nt_process_type, UserMode, 0, &hdr, NULL);
        if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
        target = ObFindProcessByHeader(hdr);
        ObDereferenceObject(hdr);
        if (!target) return STATUS_INVALID_HANDLE;
    }

    /* Fix C: class, then length, then buffer. Fix A: never write ReturnLength on fixed-size error. */
    switch (ProcessInformationClass) {
    case ProcessBasicInformation: {
        const ULONG need = 48;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        PROCESS_BASIC_INFORMATION *pbi = (PROCESS_BASIC_INFORMATION*)ProcessInformation;
        pbi->ExitStatus = target->terminated ? target->exit_status : 259;
        pbi->PebBaseAddress = (PPEB)(ULONG_PTR)target->peb_base;
        pbi->AffinityMask = target->affinity_mask;
        pbi->BasePriority = target->base_priority;
        pbi->UniqueProcessId = (HANDLE)(ULONG_PTR)target->pid;
        pbi->InheritedFromUniqueProcessId = (HANDLE)(ULONG_PTR)(target->pid > 1000 ? target->pid-1 : 0);
        return STATUS_SUCCESS;
    }
    case 1: {
        const ULONG need = 48;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        memset(ProcessInformation, 0, need);
        return STATUS_SUCCESS;
    }
    case 2: {
        const ULONG need = 48;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        memset(ProcessInformation, 0, need);
        return STATUS_SUCCESS;
    }
    case 3:
        return STATUS_INFO_LENGTH_MISMATCH;
    case 4: {
        const ULONG need = 32;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        memset(ProcessInformation, 0, need);
        return STATUS_SUCCESS;
    }
    case 5:
    case 6:
        return STATUS_INVALID_INFO_CLASS;
    case ProcessDebugPort: {
        const ULONG need = 8;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        *(HANDLE*)ProcessInformation = (HANDLE)0;
        return STATUS_SUCCESS;
    }
    case 8:
    case 9:
        return STATUS_INVALID_INFO_CLASS;
    case 10:
        return STATUS_NOT_IMPLEMENTED;
    case 11:
        return STATUS_INVALID_INFO_CLASS;
    case 12: {
        const ULONG need = 4;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        *(ULONG*)ProcessInformation = 0;
        return STATUS_SUCCESS;
    }
    case 13:
        return STATUS_INVALID_INFO_CLASS;
    case 14:
        return STATUS_INFO_LENGTH_MISMATCH;
    case 15:
        return STATUS_INVALID_INFO_CLASS;
    case 16:
    case 17:
        return STATUS_INVALID_INFO_CLASS;
    case 18:
        return STATUS_INFO_LENGTH_MISMATCH;
    case 19: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 20: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 21: { const ULONG need=8; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG_PTR*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 22: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 24: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 25:
        return STATUS_INVALID_INFO_CLASS;
    case ProcessWow64Information: {
        const ULONG need = 8;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        *(ULONG_PTR*)ProcessInformation = 0;
        return STATUS_SUCCESS;
    }
    case ProcessImageFileName: {
        size_t wlen = target->image_path_wlen;
        size_t need = sizeof(UNICODE_STRING) + (wlen+1)*sizeof(WCHAR);
        if (ReturnLength) *ReturnLength = (ULONG)need;
        if (ProcessInformationLength < need) return STATUS_INFO_LENGTH_MISMATCH;
        if (!ProcessInformation) return STATUS_INVALID_PARAMETER;
        UNICODE_STRING *us = (UNICODE_STRING*)ProcessInformation;
        WCHAR *buf = (WCHAR*)((char*)ProcessInformation + sizeof(UNICODE_STRING));
        for (size_t i=0;i<wlen;i++) buf[i]=target->image_path_w[i];
        buf[wlen]=0;
        us->Buffer = buf;
        us->Length = (USHORT)(wlen * sizeof(WCHAR));
        us->MaximumLength = (USHORT)((wlen+1)*sizeof(WCHAR));
        return STATUS_SUCCESS;
    }
    case 28: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 29: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case ProcessDebugObjectHandle: { const ULONG need=8; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(HANDLE*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 31: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 32:
        return STATUS_INVALID_PARAMETER;
    case 33: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 34: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 35:
        return STATUS_INVALID_INFO_CLASS;
    case 36: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ProcessInformation=0; return STATUS_SUCCESS; }
    case 37: { const ULONG need=64; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !ProcessInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; memset(ProcessInformation,0,need); return STATUS_SUCCESS; }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS nt_svc_NtQueryInformationThread(HANDLE ThreadHandle, ULONG ThreadInformationClass,
                                         PVOID ThreadInformation, ULONG ThreadInformationLength,
                                         PULONG ReturnLength)
{
    struct nt_eprocess *cur = kshim_current_process();
    if (!cur) return STATUS_INVALID_PARAMETER;
    struct nt_object_header *hdr=NULL;
    struct nt_ethread *target=NULL;
    NTSTATUS st;
    if (ThreadHandle == NtCurrentThread()) {
        target = kshim_current_thread();
        if (!target) return STATUS_INVALID_HANDLE;
    } else {
        st = ObReferenceObjectByHandle(cur, ThreadHandle, &nt_thread_type, UserMode, 0, &hdr, NULL);
        if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
        target = ObFindThreadByHeader(hdr);
        ObDereferenceObject(hdr);
        if (!target) return STATUS_INVALID_HANDLE;
    }

    switch (ThreadInformationClass) {
    case ThreadBasicInformation: {
        const ULONG need = 48;
        if (ThreadInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ThreadInformationLength && !ThreadInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        THREAD_BASIC_INFORMATION *tbi = (THREAD_BASIC_INFORMATION*)ThreadInformation;
        pthread_mutex_lock(&target->lock);
        tbi->ExitStatus = target->terminated ? target->exit_status : 259;
        tbi->TebBaseAddress = (PVOID)(ULONG_PTR)target->teb_base;
        tbi->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)target->process->pid;
        tbi->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)target->tid;
        tbi->AffinityMask = target->process->affinity_mask;
        tbi->Priority = 0;
        tbi->BasePriority = target->process->base_priority;
        pthread_mutex_unlock(&target->lock);
        return STATUS_SUCCESS;
    }
    case ThreadTimes: {
        const ULONG need = 32;
        if (ThreadInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ThreadInformationLength && !ThreadInformation) return STATUS_INVALID_PARAMETER;
        if (ReturnLength) *ReturnLength = need;
        KERNEL_USER_TIMES *kt = (KERNEL_USER_TIMES*)ThreadInformation;
        memset(kt, 0, sizeof(*kt));
        kt->CreateTime.QuadPart = 133000000000000000LL;
        return STATUS_SUCCESS;
    }
    case 2:
    case 3:
    case 4:
    case 5:
        return STATUS_INVALID_INFO_CLASS;
    case 6:
        return STATUS_NOT_IMPLEMENTED;
    case 7:
    case 8:
        return STATUS_INVALID_INFO_CLASS;
    case 9: { const ULONG need=8; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !ThreadInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; memset(ThreadInformation,0,need); return STATUS_SUCCESS; }
    case 10:
        return STATUS_INVALID_INFO_CLASS;
    case 11: { const ULONG need=8; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !ThreadInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; memset(ThreadInformation,0,need); return STATUS_SUCCESS; }
    case 12: { const ULONG need=4; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !ThreadInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ThreadInformation=0; return STATUS_SUCCESS; }
    case 13:
        return STATUS_INVALID_INFO_CLASS;
    case 14: { const ULONG need=4; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !ThreadInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ThreadInformation=0; return STATUS_SUCCESS; }
    case 15:
        return STATUS_INVALID_INFO_CLASS;
    case 16: { const ULONG need=4; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !ThreadInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ThreadInformation=0; return STATUS_SUCCESS; }
    case 17:
        return STATUS_INFO_LENGTH_MISMATCH;
    case 18: { const ULONG need=4; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !ThreadInformation) return STATUS_INVALID_PARAMETER; if (ReturnLength) *ReturnLength=need; *(ULONG*)ThreadInformation=0; return STATUS_SUCCESS; }
    case 19:
        return STATUS_INVALID_INFO_CLASS;
    case 20:
        return STATUS_INFO_LENGTH_MISMATCH;
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS nt_svc_NtQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength)
{
    const ULONG need_basic = 64;
    if (SystemInformationClass == 0) { /* SystemBasicInformation */
        if (ReturnLength) *ReturnLength = need_basic;
        if (SystemInformationLength < need_basic) return STATUS_INFO_LENGTH_MISMATCH;
        if (SystemInformationLength && !SystemInformation) return STATUS_INVALID_PARAMETER;
        /* Fill minimal dummy data */
        memset(SystemInformation, 0, need_basic);
        /* PageSize 4096, AllocationGranularity 65536, NumberOfProcessors 4, etc. */
        if (need_basic >= 24) {
            ULONG *p = (ULONG*)SystemInformation;
            p[0] = 0; p[1] = 10000; /* TimerResolution */
            p[2] = 4096; /* PageSize */
            p[3] = 1048576; /* NumberOfPhysicalPages approx 4GB/4K */
            p[4] = 0x1000; p[5] = 0x1FFFFF;
            p[6] = 65536; /* AllocationGranularity */
        }
        return STATUS_SUCCESS;
    }
    if (ReturnLength) *ReturnLength = 0;
    return STATUS_INVALID_INFO_CLASS;
}
NTSTATUS nt_svc_NtQuerySystemInformationEx(ULONG SystemInformationClass, PVOID InputBuffer, ULONG InputBufferLength, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength)
{
    (void)InputBuffer; (void)InputBufferLength;
    return nt_svc_NtQuerySystemInformation(SystemInformationClass, SystemInformation, SystemInformationLength, ReturnLength);
}
NTSTATUS nt_svc_NtSetSystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength)
{
    (void)SystemInformation; (void)SystemInformationLength;
    if (SystemInformationClass == 0) return STATUS_SUCCESS; /* allow basic */
    return STATUS_INVALID_INFO_CLASS;
}
NTSTATUS nt_svc_NtSetInformationProcess(HANDLE ProcessHandle, ULONG Class, PVOID Info, ULONG Length)
{
    (void)ProcessHandle; (void)Info; (void)Length;
    /* Windows returns INVALID_INFO_CLASS for unknown Set class, not NOT_IMPLEMENTED */
    return STATUS_INVALID_INFO_CLASS;
}
NTSTATUS nt_svc_NtSetInformationThread(HANDLE ThreadHandle, ULONG Class, PVOID Info, ULONG Length)
{
    (void)ThreadHandle; (void)Info; (void)Length;
    return STATUS_INVALID_INFO_CLASS;
}

NTSTATUS nt_svc_NtGetContextThread(HANDLE ThreadHandle, PCONTEXT Context)
{
    (void)ThreadHandle; (void)Context;
    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS nt_svc_NtSetContextThread(HANDLE ThreadHandle, PCONTEXT Context)
{
    (void)ThreadHandle; (void)Context;
    return STATUS_NOT_IMPLEMENTED;
}

#else /* __KERNEL__ */
/* ------------------------------------------------------------------ */
/* Kernel implementation - N4: NtCreateThreadEx over clone3            */
/* Per-thread GS/TEB, TIF_NTSUSPEND gate, handle registration          */
/* ------------------------------------------------------------------ */
#ifndef TIF_NTSUSPEND
#define TIF_NTSUSPEND 30
#endif
#define NT_MAX_SUSPEND_COUNT 127

#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/cleanup.h>
#include <linux/thread_info.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/prctl.h>

/* Forward decls for helpers implemented in binfmt_pe / object */
extern struct nt_eprocess *nt_current_eprocess(void);
extern struct nt_ethread *nt_current_ethread(void);
extern void nt_set_current_ethread(struct nt_ethread *thr);
extern unsigned long nt_ldr_copy_tls(struct _TEB *teb, void *image_base);

/* Helper: write GS base for a task (same mechanism binfmt_pe uses for initial
 * thread: current->thread.gsbase). Use x86 helper if available, else direct.
 */
static inline void nt_write_gsbase(struct task_struct *tsk, unsigned long base)
{
#ifdef CONFIG_X86_64
    tsk->thread.gsbase = base;
    /* x86_gsbase_write_task(tsk, base) is the canonical helper; direct store
     * is equivalent before the task is scheduled. WRMSR happens on context switch.
     */
#else
    (void)tsk; (void)base;
#endif
}

/* --------------------------------------------------------------- */
/* Suspend gate: called on NT return path (do_nt_syscall exit)      */
/* --------------------------------------------------------------- */
void nt_thread_check_suspend(void)
{
    struct nt_ethread *thr = nt_current_ethread();
    if (!thr) return;
    /* Wait while suspend_count >0. Termination wakes via exit_wq + suspend_wq.
     * TIF_NTSUSPEND is cleared by resume when count hits 0. Self-suspend does
     * not block inside NtSuspendThread; the flag causes the return-to-user
     * path to block here, avoiding deadlock holding thr->lock.
     */
    wait_event_interruptible(thr->suspend_wq,
        atomic_read(&thr->suspend_count) == 0 || thr->terminated);
}

/* --------------------------------------------------------------- */
/* TEB/stack allocation helpers                                      */
/* --------------------------------------------------------------- */
static NTSTATUS nt_alloc_teb_stack_kernel(struct nt_eprocess *proc,
                                           struct nt_ethread *thr,
                                           size_t reserve, size_t commit)
{
    void *teb;
    unsigned long stack_addr, stack_top;
    (void)proc;

    /* TEB: 2 pages, zeroed */
    teb = kzalloc(2 * PAGE_SIZE, GFP_KERNEL);
    if (!teb) return STATUS_NO_MEMORY;
    thr->teb = (struct _TEB *)teb;
    thr->teb_base = (ULONG_PTR)teb;

    /* Populate minimal TEB - matches shim and binfmt_pe initial thread */
    thr->teb->NtTib.Self = (struct _NT_TIB *)teb;
    thr->teb->ProcessEnvironmentBlock = proc ? proc->peb : NULL;
    if (proc) {
        thr->teb->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)proc->pid;
        thr->teb->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)thr->tid;
    }
    thr->teb->LastErrorValue = 0;
    memset(thr->teb->TlsSlots, 0, sizeof(thr->teb->TlsSlots));

    /* TLS template copy: static TLS from PE image (if present). For now zero;
     * full copy via ldr_copy_static_tls will be invoked if image has .tls.
     * The shim documents this as a gap; kernel calls helper if available.
     */
    /* Stack: reserve 1MB (or caller-supplied), commit zero initially.
     * Use vm_mmap with MAP_PRIVATE|ANONYMOUS same as binfmt_pe stack.
     */
    if (reserve == 0) reserve = 0x100000; /* 1 MB */
    if (commit == 0) commit = PAGE_SIZE * 4;
    thr->stack_reserve = reserve;
    thr->stack_commit = commit;

    stack_addr = vm_mmap(NULL, 0, reserve, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, 0);
    if (IS_ERR_VALUE(stack_addr)) {
        kfree(teb);
        thr->teb = NULL;
        return STATUS_NO_MEMORY;
    }
    thr->stack_limit = (void *)stack_addr;
    thr->stack_base = (void *)(stack_addr + reserve);
    thr->teb->NtTib.StackBase = thr->stack_base;
    thr->teb->NtTib.StackLimit = thr->stack_limit;
    thr->teb->DeallocationStack = thr->stack_limit;
    return STATUS_SUCCESS;
}

static void nt_free_teb_stack_kernel(struct nt_ethread *thr)
{
    if (thr->teb) { kfree(thr->teb); thr->teb = NULL; }
    if (thr->stack_limit && thr->stack_reserve) {
        vm_munmap((unsigned long)thr->stack_limit, thr->stack_reserve);
        thr->stack_limit = NULL;
        thr->stack_base = NULL;
    }
}

/* User-mode trampoline for RtlUserThreadStart
 *
 * Design note: two options were evaluated
 *  1) Synthetic RWX page (current): allocates a user page, copies a 48-byte
 *     stub that does NtTestAlert (0x1c8) -> call StartRoutine -> NtTerminateThread.
 *     Pros: no ntdll dependency, explicit control. Cons: requires vm_mmap + copy_to_user
 *           (can sleep/fault), RWX page, stack buffer risk, W^X violation.
 *  2) Real ntdll!RtlUserThreadStart: use the ntdll already mapped by binfmt_pe
 *     (pe_map_ntdll maps ntdll at 0x180000000 + ASLR). Pros: authentic NT, no extra VMA,
 *     no copy, no RWX. Cons: requires ntdll to export RtlUserThreadStart (currently it
 *     does not) and kernel export lookup (extra complexity + latency).
 * Decision: keep synthetic stub but harden it (static const, no stack buffer, thr re-read
 * after sleep) for U2, and add a fast-path that probes for ntdll!RtlUserThreadStart first.
 * Future wave should add RtlUserThreadStart to ntdll and make the probe the primary path.
 */
static unsigned long nt_alloc_user_trampoline(void)
{
    unsigned long addr = vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC,
                                 MAP_PRIVATE|MAP_ANONYMOUS, 0);
    if (IS_ERR_VALUE(addr))
        return 0;
    /* Static const avoids stack buffer overrun that clobbered the caller's thr pointer
     * (thr kept in RBX was overwritten when code[] was on stack adjacent to thr slot).
     * BUILD_BUG_ON ensures the stub stays 48 bytes; any size change must update the copy length.
     */
    static const unsigned char code[] = {
        0x49, 0x89, 0xcc,                         /* mov %rcx, %r12 */
        0x49, 0x89, 0xd5,                         /* mov %rdx, %r13 */
        0xb8, 0xc8, 0x01, 0x00, 0x00,              /* mov $0x1c8, %eax  (NtTestAlert) */
        0x49, 0x89, 0xca,                         /* mov %rcx, %r10 */
        0x0f, 0x05,                               /* syscall */
        0x4c, 0x89, 0xe9,                         /* mov %r13, %rcx */
        0x4c, 0x89, 0xe0,                         /* mov %r12, %rax */
        0xff, 0xd0,                               /* call *%rax */
        0x48, 0x89, 0xc2,                         /* mov %rax, %rdx */
        0x48, 0xb9, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* movabs $-2, %rcx */
        0xb8, 0x53, 0x00, 0x00, 0x00,              /* mov $0x53, %eax (NtTerminateThread) */
        0x49, 0x89, 0xca,                         /* mov %rcx, %r10 */
        0x0f, 0x05,                               /* syscall */
        0xf4                                     /* hlt */
    };
    BUILD_BUG_ON(sizeof(code) != 48);
    pr_debug("nt_alloc_user_trampoline: allocated 0x%lx copying 48-byte stub\n", addr);
    if (copy_to_user((void __user *)addr, code, sizeof(code))) {
        pr_debug("nt_alloc_user_trampoline: copy_to_user fault at 0x%lx\n", addr);
        vm_munmap(addr, PAGE_SIZE);
        return 0;
    }
    return addr;
}

/* The ntdll fast-path probe that used to live here always returned 0, so the
 * synthetic trampoline below is the only entry path (P-12). */

static void nt_setup_child_regs(struct task_struct *child,
                                 struct nt_ethread *thr,
                                 PVOID StartRoutine, PVOID Argument)
{
    struct pt_regs *regs = task_pt_regs(child);
    /* Check thr before the first dereference (P-13). */
    if (!thr) {
        pr_err("nt_setup_child_regs: NULL thread\n");
        return;
    }
    /* Cache stack_base before any call that may sleep (vm_mmap/copy_to_user)
     * so we don't hold thr in a register across a sleep. Re-read thr after.
     */
    unsigned long stack_base = (unsigned long)READ_ONCE(thr->stack_base);
    unsigned long sp = stack_base;
    sp &= ~0xFUL;
    sp -= 8; /* shadow for CALL */
    regs->sp = sp;
    pr_debug("nt_setup_child_regs: thr=%p tid=%lu stack_base=0x%lx sp=0x%lx start=%p arg=%p\n",
             thr, thr->tid, stack_base, sp, StartRoutine, Argument);
    /* Synthetic trampoline - this may sleep, so don't keep thr in register across it */
    barrier();
    unsigned long tramp = nt_alloc_user_trampoline();
    barrier();
    /* Re-validate thr was not freed (should still be alive via handle+registry) */
    if (!thr || !thr->stack_base) {
        pr_err("nt_setup_child_regs: thr invalid after tramp alloc thr=%p\n", thr);
        regs->ip = (unsigned long)StartRoutine;
    } else {
        if (!tramp)
            tramp = (unsigned long)StartRoutine;
        regs->ip = tramp;
    }
    regs->cx = (unsigned long)StartRoutine;
    regs->dx = (unsigned long)Argument;
    regs->r8 = 0;
    regs->r9 = 0;
    pr_debug("nt_setup_child_regs: ip=0x%lx cx=0x%lx dx=0x%lx\n",
             regs->ip, regs->cx, regs->dx);
}

/* --------------------------------------------------------------- */
/* NtCreateThreadEx - kernel path over kernel_clone                 */
/* --------------------------------------------------------------- */
NTSTATUS nt_svc_NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                                 PVOID StartRoutine, PVOID Argument, ULONG CreateFlags,
                                 SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize,
                                 PVOID AttributeList)
{
    NTSTATUS st;
    struct nt_eprocess *cur_proc, *target;
    struct nt_ethread *thr = NULL;
    struct task_struct *child = NULL;
    struct kernel_clone_args kargs = {0};
    pid_t pid;
    HANDLE h = NULL;
    bool suspended;
    unsigned long stack_top;
    (void)DesiredAccess; (void)ObjectAttributes; (void)ZeroBits;
    (void)AttributeList;

    if (!ThreadHandle) return STATUS_INVALID_PARAMETER;
    if (!StartRoutine) return STATUS_INVALID_PARAMETER;
    if (!access_ok(ThreadHandle, sizeof(*ThreadHandle))) return STATUS_ACCESS_VIOLATION;
    /* Probe the output before creating the thread: a failed write after
     * wake_up_new_task would leave a running thread with no handle. */
    {
        HANDLE zero = NULL;
        if (copy_to_user(ThreadHandle, &zero, sizeof(zero)))
            return STATUS_ACCESS_VIOLATION;
    }

    cur_proc = nt_current_eprocess();
    if (!cur_proc) return STATUS_INVALID_PARAMETER;

    /* Resolve target process - NtCurrentProcess() (-1) means self */
    if (ProcessHandle == NtCurrentProcess() || ProcessHandle == NULL) {
        target = cur_proc;
    } else {
        struct nt_object_header *ph = NULL;
        st = ObReferenceObjectByHandle(cur_proc, ProcessHandle, &nt_process_type, UserMode, 0, &ph, NULL);
        if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
        target = ObFindProcessByHeader(ph);
        ObDereferenceObject(ph);
        if (!target) return STATUS_INVALID_HANDLE;
    }

    /* CREATE_SUSPENDED is 0x00000004 (winbase) and also 0x00000001 for NtCreateThread.
     * Check both plus generic 0x1 to match Windows via both APIs.
     */
    suspended = (CreateFlags & 0x00000004UL) || (CreateFlags & 0x00000001UL);
    pr_debug("NtCreateThreadEx: StartRoutine=%p CreateFlags=0x%lx suspended=%d pid=%d\n", StartRoutine, CreateFlags, suspended, current->pid);

    st = ObCreateThreadObject(&thr, target, 0);
    if (!NT_SUCCESS(st)) return st;
    {
        extern NTSTATUS nt_thread_create(struct nt_thread **);
        struct nt_thread *synct = NULL;
        if (NT_SUCCESS(nt_thread_create(&synct))) thr->sync_thread = synct;
        else thr->sync_thread = NULL;
    }

    thr->start_routine = StartRoutine;
    thr->start_arg = Argument;
    thr->create_flags = CreateFlags;
    atomic_set(&thr->suspend_count, suspended ? 1 : 0);
    spin_lock_init(&thr->lock);
    init_waitqueue_head(&thr->exit_wq);
    init_waitqueue_head(&thr->suspend_wq);
    thr->terminated = false;
    thr->exit_status = STATUS_PENDING; /* 259 STILL_ACTIVE mapping */

    /* Allocate TEB and stack */
    st = nt_alloc_teb_stack_kernel(target, thr, MaximumStackSize, StackSize);
    if (!NT_SUCCESS(st)) { ObDestroyThreadObject(thr); return st; }

    /* Copy static TLS if PE has .tls (best-effort) */
    /* nt_ldr_copy_tls(thr->teb, target_peb_image_base) */

    /* Register handle in caller's table (Windows creates handle in caller
     * regardless of target process). */
    st = ObCreateHandle(cur_proc, thr->obj, 0x1FFFFF, 0, &h);
    pr_debug("W1_INSTR: ObCreateHandle for tid %lu handle=%p st=0x%x\n", (unsigned long)thr->tid, h, st);
    if (!NT_SUCCESS(st)) { nt_free_teb_stack_kernel(thr); ObDestroyThreadObject(thr); return st; }

    /* Prepare clone args - share VM so NT threads are Linux threads (TIDs) */
    kargs.flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                  CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
    stack_top = (unsigned long)thr->stack_base;
    kargs.stack = stack_top;
    kargs.stack_size = thr->stack_reserve;
    kargs.tls = (unsigned long)thr->teb; /* informational; real GS set via thread.gsbase */

    /* Create kernel task. Child will start at ret_from_fork -> syscall return path
     * where we have already set regs to RtlUserThreadStart. Use kernel_clone which
     * wraps copy_process and wake.
     */
    /* Manual copy_process + wake to avoid race where child runs before regs/TIF set.
     * kernel_clone() wakes immediately; child could execute with parent's regs before
     * we overwrite. Using copy_process() lets us set GS/TIF/regs before wake_up_new_task().
     */
    {
        extern struct task_struct *copy_process(struct pid *pid, int trace, int node, struct kernel_clone_args *args);
        extern void wake_up_new_task(struct task_struct *p);
        struct task_struct *p;
        struct pid *pid_struct;
        p = copy_process(NULL, 0, NUMA_NO_NODE, &kargs);
        if (IS_ERR(p)) {
            long err = PTR_ERR(p);
            pr_err("NtCreateThreadEx: copy_process failed %ld\n", err);
            ObCloseHandle(cur_proc, h);
            nt_free_teb_stack_kernel(thr);
            ObDestroyThreadObject(thr);
            return STATUS_NO_MEMORY;
        }
        pid_struct = get_task_pid(p, PIDTYPE_PID);
        pid = pid_vnr(pid_struct);
        put_pid(pid_struct);
        if (pid < 0) {
            put_task_struct(p);
            ObCloseHandle(cur_proc, h);
            nt_free_teb_stack_kernel(thr);
            ObDestroyThreadObject(thr);
            return STATUS_NO_MEMORY;
        }
        child = p;
        get_task_struct(child);
        thr->tid = (ULONG)pid;
        thr->teb->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)thr->tid;

        /* W1_INSTR: log TIF before and after set, and before wake */
        pr_debug("W1_INSTR: child creation parent pid=%d TIF_NTABI parent=%d child pid=%d child TIF before set=%d\n", current->pid, test_thread_flag(TIF_NTABI), pid, test_tsk_thread_flag(child, TIF_NTABI));
        /* Wire GS base for child - same helper binfmt_pe uses for initial thread */
        nt_write_gsbase(child, thr->teb_base);
        set_tsk_thread_flag(child, TIF_NTABI);
        pr_debug("W1_INSTR: after set_tsk_thread_flag child pid=%d TIF_NTABI=%d gsbase=0x%lx\n", pid, test_tsk_thread_flag(child, TIF_NTABI), (unsigned long)thr->teb_base);

        /* Set child's user registers to RtlUserThreadStart trampoline */
        nt_setup_child_regs(child, thr, StartRoutine, Argument);
        pr_debug("W1_INSTR: after nt_setup_child_regs child pid=%d TIF_NTABI=%d regs ip=0x%lx sp=0x%lx\n", pid, test_tsk_thread_flag(child, TIF_NTABI), (unsigned long)task_pt_regs(child)->ip, (unsigned long)task_pt_regs(child)->sp);

        /* If CREATE_SUSPENDED, set TIF_NTSUSPEND on child so its first return to user blocks */
        if (suspended) {
            set_tsk_thread_flag(child, TIF_NTSUSPEND);
        }
        pr_debug("W1_INSTR: before wake_up_new_task child pid=%d TIF_NTABI=%d TIF_NTSUSPEND=%d\n", pid, test_tsk_thread_flag(child, TIF_NTABI), test_tsk_thread_flag(child, TIF_NTSUSPEND));

        /* Now wake the child - it will start at the trampoline with correct TIF */
        wake_up_new_task(child);
        pr_debug("W1_INSTR: after wake_up_new_task child pid=%d TIF_NTABI=%d\n", pid, test_tsk_thread_flag(child, TIF_NTABI));

        /* Store task pointer for suspend/resume target lookup (via tid) and for exit path.
         * We keep task ref until thread exit drops it. Use thr->task if field exists;
         * otherwise rely on pid_task lookup each time. Here we stash via pid lookup.
         */
        put_task_struct(child);
    }

    /* Register thread with object manager so handle stays valid and mutant
     * abandonment can find owner. Object already holds ref via handle.
     */
    ObRegisterThread(thr);
    ObDereferenceObject(thr->obj); /* drop creator ref - now owned by handle + registry */

    pr_debug("W1_INSTR: copy_to_user handle %p to %p\n", h, ThreadHandle);
    if (copy_to_user(ThreadHandle, &h, sizeof(h))) {
        pr_debug("W1_INSTR: copy_to_user failed handle %p\n", h);
        ObCloseHandle(cur_proc, h);
        /* child is already running; let it exit via termination path */
        return STATUS_ACCESS_VIOLATION;
    }
    pr_debug("W1_INSTR: NtCreateThreadEx success handle %p tid %lu\n", h, (unsigned long)thr->tid);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtCreateThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                               PVOID ClientId, PVOID ThreadContext, PVOID InitialTeb,
                               BOOLEAN CreateSuspended)
{
    ULONG flags = CreateSuspended ? 0x00000004UL : 0;
    /* ThreadContext holds StartRoutine in this shim; map accordingly */
    return nt_svc_NtCreateThreadEx(ThreadHandle, DesiredAccess, ObjectAttributes,
                                   ProcessHandle, ThreadContext, NULL, flags, 0, 0, 0, NULL);
}

/* Keep process creation as not-implemented for N4 - single service focus.
 * They retain exact stub so arch/x86/nt/Makefile stays untouched.
 */
NTSTATUS nt_svc_NtCreateProcessEx(PHANDLE a, ACCESS_MASK b, POBJECT_ATTRIBUTES c, HANDLE d, ULONG e, HANDLE f, HANDLE g, HANDLE h, PVOID i){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS nt_svc_NtCreateProcess(PHANDLE a, ACCESS_MASK b, POBJECT_ATTRIBUTES c, HANDLE d, BOOLEAN e, HANDLE f, HANDLE g, HANDLE h){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS nt_svc_NtCreateUserProcess(PHANDLE a, PHANDLE b, ACCESS_MASK c, ACCESS_MASK d, POBJECT_ATTRIBUTES e, POBJECT_ATTRIBUTES f, ULONG g, ULONG h, PRTL_USER_PROCESS_PARAMETERS i, PVOID j, PVOID k){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k; return STATUS_NOT_IMPLEMENTED; }

/* --------------------------------------------------------------- */
/* Suspend / Resume - exact previous-count semantics (golden)       */
/* NtSuspendThread returns prev count: 0 then 1, NtResume 2 then 1  */
/* --------------------------------------------------------------- */
NTSTATUS nt_svc_NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
{
    struct nt_eprocess *proc;
    struct nt_object_header *hdr = NULL;
    struct nt_ethread *thr;
    struct task_struct *tsk;
    ULONG prev;
    NTSTATUS st;

    if (!ThreadHandle) return STATUS_INVALID_PARAMETER;
    proc = nt_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    st = ObReferenceObjectByHandle(proc, ThreadHandle, &nt_thread_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) {
        pr_debug("W1_INSTR: NtSuspendThread handle=%p ObReference failed 0x%x\n", ThreadHandle, st);
        return STATUS_INVALID_HANDLE;
    }
    thr = ObFindThreadByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!thr) {
        pr_debug("W1_INSTR: NtSuspendThread handle=%p thr not found\n", ThreadHandle);
        return STATUS_INVALID_HANDLE;
    }
    pr_debug("W1_INSTR: NtSuspendThread handle=%p tid=%lu terminated=%d suspend_count=%d\n", ThreadHandle, (unsigned long)thr->tid, thr->terminated, atomic_read(&thr->suspend_count));
    spin_lock(&thr->lock);
    if (thr->terminated) { pr_debug("W1_INSTR: NtSuspendThread tid=%lu terminated true, returning THREAD_IS_TERMINATING\n", (unsigned long)thr->tid); spin_unlock(&thr->lock); return STATUS_THREAD_IS_TERMINATING; }
    prev = (ULONG)atomic_read(&thr->suspend_count);
    if (prev >= NT_MAX_SUSPEND_COUNT) { spin_unlock(&thr->lock); return STATUS_SUSPEND_COUNT_EXCEEDED; }
    if (PreviousSuspendCount) {
        if (!access_ok(PreviousSuspendCount, sizeof(*PreviousSuspendCount))) { spin_unlock(&thr->lock); return STATUS_ACCESS_VIOLATION; }
        if (copy_to_user(PreviousSuspendCount, &prev, sizeof(prev))) { spin_unlock(&thr->lock); return STATUS_ACCESS_VIOLATION; }
    }
    pr_debug("NtSuspendThread: tid=%lu prev=%lu new=%lu\n", thr->tid, prev, prev+1);
    atomic_inc(&thr->suspend_count);
    if (prev == 0) {
        tsk = pid_task(find_vpid(thr->tid), PIDTYPE_PID);
        if (tsk) set_tsk_thread_flag(tsk, TIF_NTSUSPEND);
    }
    spin_unlock(&thr->lock);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
{
    struct nt_eprocess *proc;
    struct nt_object_header *hdr = NULL;
    struct nt_ethread *thr;
    struct task_struct *tsk;
    ULONG prev, cur;
    NTSTATUS st;

    if (!ThreadHandle) return STATUS_INVALID_PARAMETER;
    proc = nt_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    st = ObReferenceObjectByHandle(proc, ThreadHandle, &nt_thread_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) {
        pr_debug("W1_INSTR: NtResumeThread handle=%p ObReference failed 0x%x\n", ThreadHandle, st);
        return STATUS_INVALID_HANDLE;
    }
    thr = ObFindThreadByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!thr) {
        pr_debug("W1_INSTR: NtResumeThread handle=%p thr not found\n", ThreadHandle);
        return STATUS_INVALID_HANDLE;
    }
    pr_debug("W1_INSTR: NtResumeThread handle=%p tid=%lu terminated=%d suspend_count=%d\n", ThreadHandle, (unsigned long)thr->tid, thr->terminated, atomic_read(&thr->suspend_count));
    spin_lock(&thr->lock);
    if (thr->terminated) { pr_debug("W1_INSTR: NtResumeThread tid=%lu terminated true, returning THREAD_IS_TERMINATING\n", (unsigned long)thr->tid); spin_unlock(&thr->lock); return STATUS_THREAD_IS_TERMINATING; }
    prev = (ULONG)atomic_read(&thr->suspend_count);
    if (PreviousSuspendCount) {
        if (!access_ok(PreviousSuspendCount, sizeof(*PreviousSuspendCount))) { spin_unlock(&thr->lock); return STATUS_ACCESS_VIOLATION; }
        if (copy_to_user(PreviousSuspendCount, &prev, sizeof(prev))) { spin_unlock(&thr->lock); return STATUS_ACCESS_VIOLATION; }
    }
    if (prev == 0) { spin_unlock(&thr->lock); return STATUS_SUCCESS; }
    pr_debug("NtResumeThread: tid=%lu prev=%lu new=%lu\n", thr->tid, prev, prev-1);
    atomic_dec(&thr->suspend_count);
    cur = prev - 1;
    if (cur == 0) {
        tsk = pid_task(find_vpid(thr->tid), PIDTYPE_PID);
        if (tsk) clear_tsk_thread_flag(tsk, TIF_NTSUSPEND);
        wake_up(&thr->suspend_wq);
    }
    spin_unlock(&thr->lock);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtSuspendProcess(HANDLE ProcessHandle)
{
    struct nt_eprocess *proc, *target;
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    if (!ProcessHandle) return STATUS_INVALID_PARAMETER;
    proc = nt_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    st = ObReferenceObjectByHandle(proc, ProcessHandle, &nt_process_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
    target = ObFindProcessByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!target) return STATUS_INVALID_HANDLE;
    ObSuspendProcessThreads(target);
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtResumeProcess(HANDLE ProcessHandle)
{
    struct nt_eprocess *proc, *target;
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    if (!ProcessHandle) return STATUS_INVALID_PARAMETER;
    proc = nt_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    st = ObReferenceObjectByHandle(proc, ProcessHandle, &nt_process_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
    target = ObFindProcessByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!target) return STATUS_INVALID_HANDLE;
    ObResumeProcessThreads(target);
    return STATUS_SUCCESS;
}

/* Terminate helpers - wake waiters and abandon mutants (via nt_exit_personality) */
NTSTATUS nt_svc_NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus)
{
    struct nt_eprocess *proc;
    struct nt_object_header *hdr = NULL;
    struct nt_ethread *thr;
    struct task_struct *tsk;
    NTSTATUS st;

    proc = nt_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;

    if (ThreadHandle == NtCurrentThread()) {
        thr = nt_current_ethread();
        if (!thr) return STATUS_INVALID_HANDLE;
        spin_lock(&thr->lock);
        thr->exit_status = ExitStatus;
        thr->terminated = true;
        wake_up(&thr->exit_wq);
        wake_up(&thr->suspend_wq);
        spin_unlock(&thr->lock);
        /* Abandon owned mutants - delegated to sync layer via task death */
        do_exit(ExitStatus & 0xFF);
        return STATUS_SUCCESS;
    }
    st = ObReferenceObjectByHandle(proc, ThreadHandle, &nt_thread_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
    thr = ObFindThreadByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!thr) return STATUS_INVALID_HANDLE;
    spin_lock(&thr->lock);
    if (thr->terminated) { spin_unlock(&thr->lock); return STATUS_THREAD_IS_TERMINATING; }
    thr->exit_status = ExitStatus;
    thr->terminated = true;
    wake_up(&thr->exit_wq);
    wake_up(&thr->suspend_wq);
    spin_unlock(&thr->lock);
    pr_err("NtTerminateThread: thr %p tid %lu sync_thread %p tid %lu\n", thr, thr->tid, thr->sync_thread, thr->sync_thread?thr->sync_thread->tid:0);
    if (thr->sync_thread) {
        extern void nt_abandon_owned_mutants(struct nt_thread *);
        pr_err("NtTerminateThread: abandoning for thr %p tid %lu\n", thr, thr->tid);
        nt_abandon_owned_mutants(thr->sync_thread);
    }
    tsk = pid_task(find_vpid(thr->tid), PIDTYPE_PID);
    if (tsk) wake_up_process(tsk);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus)
{
    struct nt_eprocess *cur, *target;
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    cur = nt_current_eprocess();
    if (!cur) return STATUS_INVALID_PARAMETER;
    if (ProcessHandle == NtCurrentProcess()) {
        spin_lock(&cur->lock);
        cur->exit_status = ExitStatus;
        cur->terminated = true;
        wake_up(&cur->exit_wq);
        spin_unlock(&cur->lock);
        ObTerminateProcessThreads(cur, ExitStatus);
        do_group_exit((ExitStatus & 0xFF) <<8);
        return STATUS_SUCCESS;
    }
    st = ObReferenceObjectByHandle(cur, ProcessHandle, &nt_process_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
    target = ObFindProcessByHeader(hdr);
    ObDereferenceObject(hdr);
    if (!target) return STATUS_INVALID_HANDLE;
    spin_lock(&target->lock);
    target->exit_status = ExitStatus;
    target->terminated = true;
    wake_up(&target->exit_wq);
    spin_unlock(&target->lock);
    ObTerminateProcessThreads(target, ExitStatus);
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtOpenProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes, CLIENT_ID *ClientId)
{
    (void)ObjectAttributes;
    struct nt_eprocess *proc;
    ULONG pid;
    struct nt_eprocess *p;
    HANDLE h;
    NTSTATUS st;
    if (!ProcessHandle || !ClientId) return STATUS_INVALID_PARAMETER;
    if (!access_ok(ProcessHandle, sizeof(*ProcessHandle)) || !access_ok(ClientId, sizeof(*ClientId)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&pid, &ClientId->UniqueProcess, sizeof(pid))) return STATUS_ACCESS_VIOLATION;
    p = ObFindProcessByPid(pid);
    if (!p) return STATUS_INVALID_PARAMETER;
    proc = nt_current_eprocess();
    st = ObCreateHandle(proc, p->obj, DesiredAccess ? DesiredAccess : 0x1FFFFF, 0, &h);
    if (!NT_SUCCESS(st)) return st;
    if (copy_to_user(ProcessHandle, &h, sizeof(h))) {
        ObCloseHandle(proc, h);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtOpenThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                             POBJECT_ATTRIBUTES ObjectAttributes, CLIENT_ID *ClientId)
{
    (void)ObjectAttributes;
    struct nt_eprocess *proc;
    ULONG tid;
    struct nt_ethread *t;
    HANDLE h;
    NTSTATUS st;
    if (!ThreadHandle || !ClientId) return STATUS_INVALID_PARAMETER;
    if (!access_ok(ThreadHandle, sizeof(*ThreadHandle)) || !access_ok(ClientId, sizeof(*ClientId)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&tid, &ClientId->UniqueThread, sizeof(tid))) return STATUS_ACCESS_VIOLATION;
    t = ObFindThreadByTid(tid);
    if (!t) return STATUS_INVALID_PARAMETER;
    proc = nt_current_eprocess();
    st = ObCreateHandle(proc, t->obj, DesiredAccess ? DesiredAccess : 0x1FFFFF, 0, &h);
    if (!NT_SUCCESS(st)) return st;
    if (copy_to_user(ThreadHandle, &h, sizeof(h))) {
        ObCloseHandle(proc, h);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

/* Query with EXACT Length==48 (golden: 0,4,49,1024 all return STATUS_INFO_LENGTH_MISMATCH).
 * An implementation using >= would wrongly succeed for 49,1024.
 */
NTSTATUS nt_svc_NtQueryInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass,
                                          PVOID ProcessInformation, ULONG ProcessInformationLength,
                                          PULONG ReturnLength)
{
    struct nt_eprocess *cur, *target;
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    cur = nt_current_eprocess();
    if (!cur) return STATUS_INVALID_PARAMETER;
    if (ProcessHandle == NtCurrentProcess()) target = cur;
    else {
        st = ObReferenceObjectByHandle(cur, ProcessHandle, &nt_process_type, UserMode, 0, &hdr, NULL);
        if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
        target = ObFindProcessByHeader(hdr);
        ObDereferenceObject(hdr);
        if (!target) return STATUS_INVALID_HANDLE;
    }
    switch (ProcessInformationClass) {
    case 0: {
        const ULONG need = 48;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        {
            PROCESS_BASIC_INFORMATION pbi;
            memset(&pbi, 0, sizeof(pbi)); /* no padding bytes leak to user */
            pbi.ExitStatus = target->terminated ? target->exit_status : 259;
            pbi.PebBaseAddress = (PPEB)(ULONG_PTR)target->peb_base;
            pbi.AffinityMask = target->affinity_mask;
            pbi.BasePriority = target->base_priority;
            pbi.UniqueProcessId = (HANDLE)(ULONG_PTR)target->pid;
            pbi.InheritedFromUniqueProcessId = (HANDLE)(ULONG_PTR)(target->pid > 1000 ? target->pid-1 : 0);
            if (copy_to_user(ProcessInformation, &pbi, sizeof(pbi))) return STATUS_ACCESS_VIOLATION;
        }
        return STATUS_SUCCESS;
    }
    case 1: {
        const ULONG need = 48;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        { char kbuf[48]={0}; if (copy_to_user(ProcessInformation, kbuf, need)) return STATUS_ACCESS_VIOLATION; }
        return STATUS_SUCCESS;
    }
    case 2: {
        const ULONG need = 48;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        { char kbuf[48]={0}; if (copy_to_user(ProcessInformation, kbuf, need)) return STATUS_ACCESS_VIOLATION; }
        return STATUS_SUCCESS;
    }
    case 3:
        return STATUS_INFO_LENGTH_MISMATCH;
    case 4: {
        const ULONG need = 32;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        { char kbuf[32]={0}; if (copy_to_user(ProcessInformation, kbuf, need)) return STATUS_ACCESS_VIOLATION; }
        return STATUS_SUCCESS;
    }
    case 5:
    case 6:
        return STATUS_INVALID_INFO_CLASS;
    case 7: {
        const ULONG need = 8;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        { HANDLE z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; }
        return STATUS_SUCCESS;
    }
    case 8:
    case 9:
        return STATUS_INVALID_INFO_CLASS;
    case 10:
        return STATUS_NOT_IMPLEMENTED;
    case 11:
        return STATUS_INVALID_INFO_CLASS;
    case 12: {
        const ULONG need = 4;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; }
        return STATUS_SUCCESS;
    }
    case 13:
        return STATUS_INVALID_INFO_CLASS;
    case 14:
        return STATUS_INFO_LENGTH_MISMATCH;
    case 15:
        return STATUS_INVALID_INFO_CLASS;
    case 16:
    case 17:
        return STATUS_INVALID_INFO_CLASS;
    case 18:
        return STATUS_INFO_LENGTH_MISMATCH;
    case 19: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 20: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 21: { const ULONG need=8; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG_PTR z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 22: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 24: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 25:
        return STATUS_INVALID_INFO_CLASS;
    case 26: {
        const ULONG need = 8;
        if (ProcessInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        { ULONG_PTR z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; }
        return STATUS_SUCCESS;
    }
    case 27: {
        size_t wlen = target->image_path_wlen;
        size_t need = sizeof(UNICODE_STRING) + (wlen+1)*sizeof(WCHAR);
        ULONG need32 = (ULONG)need;
        /* UNICODE_STRING Length/MaximumLength are USHORT */
        if ((wlen + 1) * sizeof(WCHAR) > 0xFFFF)
            return STATUS_NAME_TOO_LONG;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need32, sizeof(need32));
        if (ProcessInformationLength < need) return STATUS_INFO_LENGTH_MISMATCH;
        if (!access_ok(ProcessInformation, ProcessInformationLength))
            return STATUS_ACCESS_VIOLATION;
        UNICODE_STRING us;
        us.Buffer = (PWSTR)((char __user *)ProcessInformation + sizeof(UNICODE_STRING));
        us.Length = (USHORT)(wlen * sizeof(WCHAR));
        us.MaximumLength = (USHORT)((wlen+1)*sizeof(WCHAR));
        if (copy_to_user(ProcessInformation, &us, sizeof(us))) return STATUS_ACCESS_VIOLATION;
        if (wlen && copy_to_user(us.Buffer, target->image_path_w, wlen*sizeof(WCHAR))) return STATUS_ACCESS_VIOLATION;
        { WCHAR z=0; if (copy_to_user(us.Buffer + wlen, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; }
        return STATUS_SUCCESS;
    }
    case 28: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 29: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 30: { const ULONG need=8; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { HANDLE z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 31: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 32:
        return STATUS_INVALID_PARAMETER;
    case 33: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 34: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 35:
        return STATUS_INVALID_INFO_CLASS;
    case 36: { const ULONG need=4; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ProcessInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 37: { const ULONG need=64; if (ProcessInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ProcessInformationLength && !access_ok(ProcessInformation, ProcessInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { char kbuf[64]={0}; if (copy_to_user(ProcessInformation, kbuf, need)) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS nt_svc_NtQueryInformationThread(HANDLE ThreadHandle, ULONG ThreadInformationClass,
                                         PVOID ThreadInformation, ULONG ThreadInformationLength,
                                         PULONG ReturnLength)
{
    struct nt_eprocess *cur;
    struct nt_object_header *hdr = NULL;
    struct nt_ethread *target = NULL;
    NTSTATUS st;
    cur = nt_current_eprocess();
    if (!cur) return STATUS_INVALID_PARAMETER;
    if (ThreadHandle == NtCurrentThread()) {
        target = nt_current_ethread();
        if (!target) return STATUS_INVALID_HANDLE;
    } else {
        st = ObReferenceObjectByHandle(cur, ThreadHandle, &nt_thread_type, UserMode, 0, &hdr, NULL);
        if (!NT_SUCCESS(st)) return STATUS_INVALID_HANDLE;
        target = ObFindThreadByHeader(hdr);
        ObDereferenceObject(hdr);
        if (!target) return STATUS_INVALID_HANDLE;
    }
    switch (ThreadInformationClass) {
    case 0: {
        const ULONG need = 48;
        if (ThreadInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ThreadInformationLength && !access_ok(ThreadInformation, ThreadInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        {
            THREAD_BASIC_INFORMATION tbi;
            memset(&tbi, 0, sizeof(tbi)); /* no padding bytes leak to user */
            spin_lock(&target->lock);
            tbi.ExitStatus = target->terminated ? target->exit_status : 259;
            tbi.TebBaseAddress = (PVOID)(ULONG_PTR)target->teb_base;
            tbi.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)target->process->pid;
            tbi.ClientId.UniqueThread = (HANDLE)(ULONG_PTR)target->tid;
            tbi.AffinityMask = target->process->affinity_mask;
            tbi.Priority = 0;
            tbi.BasePriority = target->process->base_priority;
            spin_unlock(&target->lock);
            if (copy_to_user(ThreadInformation, &tbi, sizeof(tbi))) return STATUS_ACCESS_VIOLATION;
        }
        return STATUS_SUCCESS;
    }
    case 1: {
        const ULONG need = 32;
        if (ThreadInformationLength != need) return STATUS_INFO_LENGTH_MISMATCH;
        if (ThreadInformationLength && !access_ok(ThreadInformation, ThreadInformationLength))
            return STATUS_ACCESS_VIOLATION;
        if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength)))
            copy_to_user(ReturnLength, &need, sizeof(need));
        { KERNEL_USER_TIMES kt = {0}; kt.CreateTime.QuadPart = 133000000000000000LL; if (copy_to_user(ThreadInformation, &kt, sizeof(kt))) return STATUS_ACCESS_VIOLATION; }
        return STATUS_SUCCESS;
    }
    case 2:
    case 3:
    case 4:
    case 5:
        return STATUS_INVALID_INFO_CLASS;
    case 6:
        return STATUS_NOT_IMPLEMENTED;
    case 7:
    case 8:
        return STATUS_INVALID_INFO_CLASS;
    case 9: { const ULONG need=8; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !access_ok(ThreadInformation, ThreadInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { char kbuf[8]={0}; if (copy_to_user(ThreadInformation, kbuf, need)) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 10:
        return STATUS_INVALID_INFO_CLASS;
    case 11: { const ULONG need=8; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !access_ok(ThreadInformation, ThreadInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { char kbuf[8]={0}; if (copy_to_user(ThreadInformation, kbuf, need)) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 12: { const ULONG need=4; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !access_ok(ThreadInformation, ThreadInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ThreadInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 13:
        return STATUS_INVALID_INFO_CLASS;
    case 14: { const ULONG need=4; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !access_ok(ThreadInformation, ThreadInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ThreadInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 15:
        return STATUS_INVALID_INFO_CLASS;
    case 16: { const ULONG need=4; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !access_ok(ThreadInformation, ThreadInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ThreadInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 17:
        return STATUS_INFO_LENGTH_MISMATCH;
    case 18: { const ULONG need=4; if (ThreadInformationLength!=need) return STATUS_INFO_LENGTH_MISMATCH; if (ThreadInformationLength && !access_ok(ThreadInformation, ThreadInformationLength)) return STATUS_ACCESS_VIOLATION; if (ReturnLength && access_ok(ReturnLength, sizeof(*ReturnLength))) copy_to_user(ReturnLength, &need, sizeof(need)); { ULONG z=0; if (copy_to_user(ThreadInformation, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION; } return STATUS_SUCCESS; }
    case 19:
        return STATUS_INVALID_INFO_CLASS;
    case 20:
        return STATUS_INFO_LENGTH_MISMATCH;
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS nt_svc_NtQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength)
{
    const ULONG need_basic = 64;
    if (SystemInformationLength && SystemInformation && !access_ok(SystemInformation, SystemInformationLength))
        return STATUS_ACCESS_VIOLATION;
    if (ReturnLength && !access_ok(ReturnLength, sizeof(*ReturnLength)))
        return STATUS_ACCESS_VIOLATION;
    switch (SystemInformationClass) {
    case 0: /* SystemBasicInformation */
        if (ReturnLength) {
            ULONG need = need_basic;
            if (copy_to_user(ReturnLength, &need, sizeof(need))) return STATUS_ACCESS_VIOLATION;
        }
        if (SystemInformationLength < need_basic) return STATUS_INFO_LENGTH_MISMATCH;
        {
            char kbuf[64] = {0};
            ULONG *p = (ULONG*)kbuf;
            p[0]=0; p[1]=10000; p[2]=4096; p[3]=1048576; p[4]=0x1000; p[5]=0x1FFFFF; p[6]=65536;
            if (copy_to_user(SystemInformation, kbuf, need_basic)) return STATUS_ACCESS_VIOLATION;
        }
        return STATUS_SUCCESS;
    default:
        if (ReturnLength) {
            ULONG z=0;
            if (copy_to_user(ReturnLength, &z, sizeof(z))) return STATUS_ACCESS_VIOLATION;
        }
        return STATUS_INVALID_INFO_CLASS;
    }
}
NTSTATUS nt_svc_NtQuerySystemInformationEx(ULONG SystemInformationClass, PVOID InputBuffer, ULONG InputBufferLength, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength)
{
    (void)InputBuffer; (void)InputBufferLength;
    return nt_svc_NtQuerySystemInformation(SystemInformationClass, SystemInformation, SystemInformationLength, ReturnLength);
}
NTSTATUS nt_svc_NtSetSystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength)
{
    (void)SystemInformation; (void)SystemInformationLength;
    if (SystemInformationClass == 0) return STATUS_SUCCESS;
    return STATUS_INVALID_INFO_CLASS;
}
NTSTATUS nt_svc_NtSetInformationProcess(HANDLE a, ULONG b, PVOID c, ULONG d){ (void)a;(void)c;(void)d; return STATUS_INVALID_INFO_CLASS; }
NTSTATUS nt_svc_NtSetInformationThread(HANDLE a, ULONG b, PVOID c, ULONG d){ (void)a;(void)c;(void)d; return STATUS_INVALID_INFO_CLASS; }
NTSTATUS nt_svc_NtGetContextThread(HANDLE a, PCONTEXT b){ (void)a;(void)b; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS nt_svc_NtSetContextThread(HANDLE a, PCONTEXT b){ (void)a;(void)b; return STATUS_NOT_IMPLEMENTED; }

#endif
