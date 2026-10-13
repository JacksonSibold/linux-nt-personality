// SPDX-License-Identifier: GPL-2.0
/*
 * debugobj.c - NT DEBUG objects: the DebugObject type and its five services
 * plus NtSetInformationDebugObject.
 *
 * Implemented:
 *   - NtCreateDebugObject: named or unnamed; DEBUG_KILL_ON_CLOSE accepted.
 *   - NtDebugActiveProcess: attaches a process (needs PROCESS_SUSPEND_RESUME on
 *     the process handle and ADD_REMOVE_PROCESS on the debug handle). Refuses
 *     self-debug (STATUS_ACCESS_DENIED), a terminated process, and a process
 *     that already has a debug object (STATUS_PORT_ALREADY_SET). Attaching
 *     queues a DbgCreateProcessStateChange event for the process.
 *   - NtRemoveProcessDebug: detaches, and discards the queued and awaiting
 *     events of that process. A process that is not attached is
 *     STATUS_PORT_NOT_SET.
 *   - NtWaitForDebugEvent: dequeues one event (FIFO). Blocks when the queue is
 *     empty, through the sync wait engine, so Timeout (NULL = infinite, zero =
 *     poll) and Alertable behave as for WaitForSingleObject: STATUS_TIMEOUT,
 *     STATUS_USER_APC. A timeout applies to each wait; a wake that finds the
 *     queue empty again (another waiter took the event) starts a new wait.
 *   - NtDebugContinue: resumes the event last dequeued for the ClientId given.
 *     Only DBG_CONTINUE and DBG_EXCEPTION_NOT_HANDLED are accepted. An
 *     unmatched ClientId is STATUS_INVALID_PARAMETER.
 *   - NtSetInformationDebugObject: DebugObjectKillProcessOnExitInformation
 *     (a BOOLEAN). Same effect as DEBUG_KILL_ON_CLOSE.
 *
 * Events generated: process create (on attach), thread create and thread exit
 * (nt_debug_post_event from proc_thread.c), process exit (NtTerminateProcess).
 * Exception and DLL events are not generated: the tree has no exception
 * dispatch hook that reaches a debug object.
 *
 * Close (last handle): every attached process is detached (its membership
 * reference dropped). With KILL_ON_CLOSE / KillProcessOnExit, each detached
 * process is terminated with exit status STATUS_SUCCESS. Attachments hold a
 * reference on both the process object and the debug object, so neither can
 * be freed while attached.
 *
 * Lock order: debug->lock, then proc->lock. Thread termination
 * (ObTerminateProcessThreads) is called after both are released.
 */

#include "debugobj.h"
#include "handle.h"
#include "sync.h"
#include <stddef.h>
#include <string.h>

/* Kernel ABI wrappers copy the set-information payload through a kernel buffer
 * of at most this many bytes. The only class takes a BOOLEAN. */
#define DEBUG_INFO_MAX_BYTES 64UL

struct debug_proc {
    struct list_head    link;
    struct nt_eprocess *proc;
};

struct debug_event {
    struct list_head        link;
    DBGUI_WAIT_STATE_CHANGE info;
};

struct nt_debug_obj {
    spinlock_t               lock;
    struct nt_object_header *hdr;
    struct nt_event         *ev;        /* signaled while queue is non-empty */
    struct list_head         procs;     /* attached processes (struct debug_proc) */
    struct list_head         queue;     /* posted, not yet dequeued (struct debug_event) */
    struct list_head         waiting;   /* dequeued, awaiting NtDebugContinue */
    bool                     kill_on_close;
};

static void debug_delete(struct nt_object_header *hdr);
static void debug_close(struct nt_object_header *hdr, HANDLE handle);

struct nt_object_type nt_debug_object_type = {
    .name = "DebugObject",
    .ops = { .delete_obj = debug_delete, .close = debug_close, },
    .pool_tag = 0,
};

/* -------------------------------------------------------------------------- */
/* Helpers (caller holds dbg->lock where noted)                               */
/* -------------------------------------------------------------------------- */

static void debug_free_list(struct list_head *head)
{
    while (!list_empty(head)) {
        struct debug_event *e = list_entry(head->next, struct debug_event, link);
        list_del(&e->link);
        kfree(e);
    }
}

/* Queue an event and wake one waiter. Caller holds dbg->lock. */
static void debug_enqueue_locked(struct nt_debug_obj *dbg, struct debug_event *e)
{
    list_add_tail(&e->link, &dbg->queue);
    nt_set_event(dbg->ev, NULL);
}

/* Drops the events of one process from the queue and the awaiting list.
 * Caller holds dbg->lock. */
static void debug_purge_pid_locked(struct nt_debug_obj *dbg, ULONG pid)
{
    struct list_head *heads[2] = { &dbg->queue, &dbg->waiting };
    int h;
    for (h = 0; h < 2; h++) {
        struct list_head *pos, *next;
        for (pos = heads[h]->next; pos != heads[h]; pos = next) {
            struct debug_event *e = list_entry(pos, struct debug_event, link);
            next = pos->next;
            if ((ULONG)(ULONG_PTR)e->info.AppClientId.UniqueProcess == pid) {
                list_del(&e->link);
                kfree(e);
            }
        }
    }
}

/* Unlinks p from dbg and clears p->debug_obj. Caller holds dbg->lock. Returns
 * the membership node (caller frees it), or NULL when p is not attached. */
static struct debug_proc *debug_unlink_proc_locked(struct nt_debug_obj *dbg, struct nt_eprocess *p)
{
    struct list_head *pos;
    list_for_each(pos, &dbg->procs) {
        struct debug_proc *n = list_entry(pos, struct debug_proc, link);
        if (n->proc != p) continue;
        list_del(&n->link);
        spin_lock(&p->lock);
        p->debug_obj = NULL;
        spin_unlock(&p->lock);
        return n;
    }
    return NULL;
}

/* Terminates p with exit status STATUS_SUCCESS, as the job kill-on-close path
 * does. Caller holds no lock. A process already terminated is left alone. */
static void debug_kill_proc(struct nt_eprocess *p)
{
    bool was;
    spin_lock(&p->lock);
    was = p->terminated;
    if (!was) {
        p->exit_status = STATUS_SUCCESS;
        p->terminated = true;
#ifdef __KERNEL__
        wake_up(&p->exit_wq);
#else
        pthread_cond_broadcast(&p->exit_cond);
#endif
    }
    spin_unlock(&p->lock);
    if (!was) ObTerminateProcessThreads(p, STATUS_SUCCESS);
}

/* -------------------------------------------------------------------------- */
/* Object type callbacks                                                      */
/* -------------------------------------------------------------------------- */

static void debug_delete(struct nt_object_header *hdr)
{
    struct nt_debug_obj *dbg = (struct nt_debug_obj *)nt_object_body(hdr);
    /* Attachments hold references on the debug object, so no process or
     * event can remain attached here. Only queued events are freed. */
    debug_free_list(&dbg->queue);
    debug_free_list(&dbg->waiting);
    if (dbg->ev) nt_put_event(dbg->ev);
    dbg->ev = NULL;
#ifndef __KERNEL__
    pthread_mutex_destroy(&dbg->lock);
#endif
}

/* Runs after every handle close. The last handle detaches every process; with
 * the kill flag the processes are terminated too. */
static void debug_close(struct nt_object_header *hdr, HANDLE handle)
{
    struct nt_debug_obj *dbg;
    bool kill;
    (void)handle;
    if (atomic64_read(&hdr->handle_count) != 0) return;
    dbg = (struct nt_debug_obj *)nt_object_body(hdr);

    spin_lock(&dbg->lock);
    kill = dbg->kill_on_close;
    spin_unlock(&dbg->lock);

    for (;;) {
        struct debug_proc *n;
        struct nt_eprocess *p;
        spin_lock(&dbg->lock);
        if (list_empty(&dbg->procs)) {
            debug_free_list(&dbg->queue);
            debug_free_list(&dbg->waiting);
            spin_unlock(&dbg->lock);
            break;
        }
        n = list_entry(dbg->procs.next, struct debug_proc, link);
        p = n->proc;
        debug_unlink_proc_locked(dbg, p);
        spin_unlock(&dbg->lock);

        /* Detached: the process can no longer post to this object. */
        if (kill) debug_kill_proc(p);
        kfree(n);
        ObDereferenceObject(p->obj);
        ObDereferenceObject(hdr);   /* the attachment's reference on dbg */
    }
}

/* -------------------------------------------------------------------------- */
/* Post hook (exit and create paths)                                          */
/* -------------------------------------------------------------------------- */

void nt_debug_post_event(struct nt_eprocess *proc, DBG_STATE state, ULONG tid, NTSTATUS status)
{
    struct nt_debug_obj *dbg = NULL;
    struct debug_event *e;
    if (!proc) return;

    spin_lock(&proc->lock);
    dbg = proc->debug_obj;
    if (dbg) ObReferenceObject(dbg->hdr);
    spin_unlock(&proc->lock);
    if (!dbg) return;

    e = (struct debug_event *)kzalloc(sizeof(*e), GFP_KERNEL);
    if (!e) {
        ObDereferenceObject(dbg->hdr);
        return;
    }
    e->info.NewState = state;
    e->info.AppClientId.UniqueProcess = (HANDLE)(ULONG_PTR)proc->pid;
    e->info.AppClientId.UniqueThread = (HANDLE)(ULONG_PTR)tid;
    if (state == DbgExitThreadStateChange)
        e->info.StateInfo.ExitThread.ExitStatus = status;
    else if (state == DbgExitProcessStateChange)
        e->info.StateInfo.ExitProcess.ExitStatus = status;

    spin_lock(&dbg->lock);
    debug_enqueue_locked(dbg, e);
    spin_unlock(&dbg->lock);
    ObDereferenceObject(dbg->hdr);
}

/* -------------------------------------------------------------------------- */
/* Service cores (shared by the userspace entry points and the kernel ABI)    */
/* -------------------------------------------------------------------------- */

static NTSTATUS debug_create_core(PHANDLE DebugObjectHandle, ACCESS_MASK DesiredAccess,
                                  POBJECT_ATTRIBUTES ObjectAttributes, ULONG Flags)
{
    struct nt_object_header *hdr = NULL;
    struct nt_debug_obj *dbg = NULL;
    struct nt_eprocess *cur = kshim_current_process();
    NTSTATUS st;

    if (!DebugObjectHandle) return STATUS_INVALID_PARAMETER;
    if (Flags & ~DEBUG_KILL_ON_CLOSE) return STATUS_INVALID_PARAMETER;
    ObRegisterObjectType(&nt_debug_object_type);
    st = ObCreateObject(&nt_debug_object_type, ObjectAttributes, sizeof(*dbg), &hdr, (void **)&dbg);
    if (!NT_SUCCESS(st)) return st;
    memset(dbg, 0, sizeof(*dbg));
    spin_lock_init(&dbg->lock);
    INIT_LIST_HEAD(&dbg->procs);
    INIT_LIST_HEAD(&dbg->queue);
    INIT_LIST_HEAD(&dbg->waiting);
    dbg->hdr = hdr;
    dbg->kill_on_close = (Flags & DEBUG_KILL_ON_CLOSE) != 0;
    st = nt_create_event(&dbg->ev, SynchronizationEvent, 0);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(hdr);
        return st;
    }
    st = ObInsertObject(hdr, ObjectAttributes, DesiredAccess, cur, DebugObjectHandle);
    ObDereferenceObject(hdr);
    return st;
}

static NTSTATUS debug_attach_core(HANDLE ProcessHandle, HANDLE DebugObjectHandle)
{
    struct nt_eprocess *cur = kshim_current_process();
    struct nt_object_header *ph = NULL, *dh = NULL;
    struct nt_eprocess *target;
    struct nt_debug_obj *dbg;
    struct debug_proc *node = NULL;
    struct debug_event *e = NULL;
    NTSTATUS st;

    if (!ProcessHandle || !DebugObjectHandle) return STATUS_INVALID_HANDLE;
    st = ObReferenceObjectByHandle(cur, DebugObjectHandle, &nt_debug_object_type, UserMode,
                                   DEBUG_OBJECT_ADD_REMOVE_PROCESS, &dh, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = ObReferenceObjectByHandle(cur, ProcessHandle, &nt_process_type, UserMode,
                                   PROCESS_SUSPEND_RESUME, &ph, NULL);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(dh);
        return st;
    }

    dbg = (struct nt_debug_obj *)nt_object_body(dh);
    target = ObFindProcessByHeader(ph);
    if (!target) {
        st = STATUS_INVALID_HANDLE;
        goto out;
    }
    if (target == cur) {
        st = STATUS_ACCESS_DENIED;   /* a process cannot debug itself */
        goto out;
    }
    /* Allocate before the locks: the kernel allocation may sleep. */
    node = (struct debug_proc *)kzalloc(sizeof(*node), GFP_KERNEL);
    e = (struct debug_event *)kzalloc(sizeof(*e), GFP_KERNEL);
    if (!node || !e) {
        st = STATUS_NO_MEMORY;
        goto out;
    }

    spin_lock(&dbg->lock);
    spin_lock(&target->lock);
    if (target->terminated) {
        st = STATUS_PROCESS_IS_TERMINATING;
    } else if (target->debug_obj) {
        st = STATUS_PORT_ALREADY_SET;
    } else {
        target->debug_obj = dbg;
        ObReferenceObject(target->obj);
        ObReferenceObject(dbg->hdr);
        node->proc = target;
        list_add_tail(&node->link, &dbg->procs);
        e->info.NewState = DbgCreateProcessStateChange;
        e->info.AppClientId.UniqueProcess = (HANDLE)(ULONG_PTR)target->pid;
        e->info.AppClientId.UniqueThread = NULL;
        debug_enqueue_locked(dbg, e);
        st = STATUS_SUCCESS;
        node = NULL;   /* owned by the debug object */
        e = NULL;
    }
    spin_unlock(&target->lock);
    spin_unlock(&dbg->lock);

out:
    if (node) kfree(node);
    if (e) kfree(e);
    ObDereferenceObject(ph);
    ObDereferenceObject(dh);
    return st;
}

static NTSTATUS debug_remove_core(HANDLE ProcessHandle, HANDLE DebugObjectHandle)
{
    struct nt_eprocess *cur = kshim_current_process();
    struct nt_object_header *ph = NULL, *dh = NULL;
    struct nt_eprocess *target;
    struct nt_debug_obj *dbg;
    struct debug_proc *n;
    NTSTATUS st;

    if (!ProcessHandle || !DebugObjectHandle) return STATUS_INVALID_HANDLE;
    st = ObReferenceObjectByHandle(cur, DebugObjectHandle, &nt_debug_object_type, UserMode,
                                   DEBUG_OBJECT_ADD_REMOVE_PROCESS, &dh, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = ObReferenceObjectByHandle(cur, ProcessHandle, &nt_process_type, UserMode,
                                   PROCESS_SUSPEND_RESUME, &ph, NULL);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(dh);
        return st;
    }

    dbg = (struct nt_debug_obj *)nt_object_body(dh);
    target = ObFindProcessByHeader(ph);
    if (!target) {
        st = STATUS_INVALID_HANDLE;
    } else {
        spin_lock(&dbg->lock);
        n = debug_unlink_proc_locked(dbg, target);
        if (n) {
            debug_purge_pid_locked(dbg, target->pid);
            spin_unlock(&dbg->lock);
            kfree(n);
            ObDereferenceObject(target->obj);
            ObDereferenceObject(dbg->hdr);   /* the attachment's reference on dbg (dh keeps its own) */
            st = STATUS_SUCCESS;
        } else {
            spin_unlock(&dbg->lock);
            st = STATUS_PORT_NOT_SET;
        }
    }
    ObDereferenceObject(ph);
    ObDereferenceObject(dh);
    return st;
}

/* One dequeue attempt per loop turn. The queue is checked and the event reset
 * under dbg->lock, so a post between the check and the wait is not lost. */
static NTSTATUS debug_wait_core(struct nt_debug_obj *dbg, BOOLEAN Alertable,
                                LARGE_INTEGER *Timeout, DBGUI_WAIT_STATE_CHANGE *out)
{
    for (;;) {
        NTSTATUS st;
        spin_lock(&dbg->lock);
        if (!list_empty(&dbg->queue)) {
            struct debug_event *e = list_entry(dbg->queue.next, struct debug_event, link);
            list_del(&e->link);
            list_add_tail(&e->link, &dbg->waiting);
            if (!list_empty(&dbg->queue)) nt_set_event(dbg->ev, NULL);
            *out = e->info;
            spin_unlock(&dbg->lock);
            return STATUS_SUCCESS;
        }
        nt_reset_event(dbg->ev, NULL);
        spin_unlock(&dbg->lock);

        st = nt_wait_for_single_object(&dbg->ev->header, Alertable, Timeout);
        if (st != STATUS_SUCCESS) return st;   /* STATUS_TIMEOUT, STATUS_USER_APC, ... */
    }
}

static NTSTATUS debug_wait_service(HANDLE DebugObjectHandle, BOOLEAN Alertable,
                                   LARGE_INTEGER *Timeout, DBGUI_WAIT_STATE_CHANGE *out)
{
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    if (!DebugObjectHandle) return STATUS_INVALID_HANDLE;
    if (!out) return STATUS_INVALID_PARAMETER;
    st = ObReferenceObjectByHandle(kshim_current_process(), DebugObjectHandle,
                                   &nt_debug_object_type, UserMode,
                                   DEBUG_OBJECT_WAIT_STATE_CHANGE, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = debug_wait_core((struct nt_debug_obj *)nt_object_body(hdr), Alertable, Timeout, out);
    ObDereferenceObject(hdr);
    return st;
}

static NTSTATUS debug_continue_core(struct nt_debug_obj *dbg, PCLIENT_ID ClientId,
                                    NTSTATUS ContinueStatus)
{
    struct list_head *pos;
    ULONG pid, tid;
    if (ContinueStatus != DBG_CONTINUE && ContinueStatus != DBG_EXCEPTION_NOT_HANDLED)
        return STATUS_INVALID_PARAMETER;
    if (!ClientId) return STATUS_INVALID_PARAMETER;
    pid = (ULONG)(ULONG_PTR)ClientId->UniqueProcess;
    tid = (ULONG)(ULONG_PTR)ClientId->UniqueThread;

    spin_lock(&dbg->lock);
    list_for_each(pos, &dbg->waiting) {
        struct debug_event *e = list_entry(pos, struct debug_event, link);
        if ((ULONG)(ULONG_PTR)e->info.AppClientId.UniqueProcess != pid) continue;
        if ((ULONG)(ULONG_PTR)e->info.AppClientId.UniqueThread != tid) continue;
        list_del(&e->link);
        spin_unlock(&dbg->lock);
        kfree(e);
        return STATUS_SUCCESS;
    }
    spin_unlock(&dbg->lock);
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS debug_set_info_core(struct nt_debug_obj *dbg, DEBUGOBJECTINFOCLASS cls,
                                    const void *buf, ULONG len)
{
    BOOLEAN on;
    if (cls != DebugObjectKillProcessOnExitInformation) return STATUS_INVALID_INFO_CLASS;
    if (len != sizeof(BOOLEAN)) return STATUS_INFO_LENGTH_MISMATCH;
    if (!buf) return STATUS_INVALID_PARAMETER;
    memcpy(&on, buf, sizeof(on));
    spin_lock(&dbg->lock);
    dbg->kill_on_close = (on != 0);
    spin_unlock(&dbg->lock);
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Userspace service entry points                                             */
/* -------------------------------------------------------------------------- */

#ifndef __KERNEL__

NTSTATUS nt_svc_NtCreateDebugObject(PHANDLE DebugObjectHandle, ACCESS_MASK DesiredAccess,
                                    POBJECT_ATTRIBUTES ObjectAttributes, ULONG Flags)
{
    return debug_create_core(DebugObjectHandle, DesiredAccess, ObjectAttributes, Flags);
}

NTSTATUS nt_svc_NtDebugActiveProcess(HANDLE ProcessHandle, HANDLE DebugObjectHandle)
{
    return debug_attach_core(ProcessHandle, DebugObjectHandle);
}

NTSTATUS nt_svc_NtRemoveProcessDebug(HANDLE ProcessHandle, HANDLE DebugObjectHandle)
{
    return debug_remove_core(ProcessHandle, DebugObjectHandle);
}

NTSTATUS nt_svc_NtWaitForDebugEvent(HANDLE DebugObjectHandle, BOOLEAN Alertable,
                                    PLARGE_INTEGER Timeout,
                                    PDBGUI_WAIT_STATE_CHANGE WaitStateChange)
{
    return debug_wait_service(DebugObjectHandle, Alertable, Timeout, WaitStateChange);
}

NTSTATUS nt_svc_NtDebugContinue(HANDLE DebugObjectHandle, PCLIENT_ID ClientId,
                                NTSTATUS ContinueStatus)
{
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    if (!DebugObjectHandle) return STATUS_INVALID_HANDLE;
    st = ObReferenceObjectByHandle(kshim_current_process(), DebugObjectHandle,
                                   &nt_debug_object_type, UserMode,
                                   DEBUG_OBJECT_WAIT_STATE_CHANGE, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = debug_continue_core((struct nt_debug_obj *)nt_object_body(hdr), ClientId, ContinueStatus);
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS nt_svc_NtSetInformationDebugObject(HANDLE DebugObjectHandle,
                                            DEBUGOBJECTINFOCLASS DebugObjectInformationClass,
                                            PVOID DebugInformation,
                                            ULONG DebugInformationLength,
                                            PULONG ReturnLength)
{
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    (void)ReturnLength;
    if (!DebugObjectHandle) return STATUS_INVALID_HANDLE;
    if (DebugObjectInformationClass != DebugObjectKillProcessOnExitInformation)
        return STATUS_INVALID_INFO_CLASS;
    st = ObReferenceObjectByHandle(kshim_current_process(), DebugObjectHandle,
                                   &nt_debug_object_type, UserMode,
                                   DEBUG_OBJECT_SET_INFORMATION, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = debug_set_info_core((struct nt_debug_obj *)nt_object_body(hdr),
                             DebugObjectInformationClass, DebugInformation, DebugInformationLength);
    ObDereferenceObject(hdr);
    return st;
}

#else /* __KERNEL__ */

/* -------------------------------------------------------------------------- */
/* Kernel ABI wrappers: ULONG arguments, user pointers copied through the     */
/* kernel (same pattern as job.c). Built but not exercised by the host tests. */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtCreateDebugObject(unsigned long a0, unsigned long a1, unsigned long a2,
                                    unsigned long a3, unsigned long a4, unsigned long a5,
                                    unsigned long a6, unsigned long a7, unsigned long a8,
                                    unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE DebugObjectHandle = (PHANDLE)a0;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    HANDLE h = NULL;
    NTSTATUS st;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!DebugObjectHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(DebugObjectHandle, sizeof(*DebugObjectHandle))) return STATUS_ACCESS_VIOLATION;
    st = ObCopyObjectAttributesFromUser((POBJECT_ATTRIBUTES)a2, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    st = debug_create_core(&h, (ACCESS_MASK)a1, oa, (ULONG)a3);
    if (!NT_SUCCESS(st)) return st;
    if (copy_to_user(DebugObjectHandle, &h, sizeof(h))) {
        ObCloseHandle(kshim_current_process(), h);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtDebugActiveProcess(unsigned long a0, unsigned long a1, unsigned long a2,
                                     unsigned long a3, unsigned long a4, unsigned long a5,
                                     unsigned long a6, unsigned long a7, unsigned long a8,
                                     unsigned long a9, unsigned long a10, unsigned long a11)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    return debug_attach_core((HANDLE)a0, (HANDLE)a1);
}

NTSTATUS nt_svc_NtRemoveProcessDebug(unsigned long a0, unsigned long a1, unsigned long a2,
                                     unsigned long a3, unsigned long a4, unsigned long a5,
                                     unsigned long a6, unsigned long a7, unsigned long a8,
                                     unsigned long a9, unsigned long a10, unsigned long a11)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    return debug_remove_core((HANDLE)a0, (HANDLE)a1);
}

NTSTATUS nt_svc_NtWaitForDebugEvent(unsigned long a0, unsigned long a1, unsigned long a2,
                                    unsigned long a3, unsigned long a4, unsigned long a5,
                                    unsigned long a6, unsigned long a7, unsigned long a8,
                                    unsigned long a9, unsigned long a10, unsigned long a11)
{
    LARGE_INTEGER k_timeout;
    LARGE_INTEGER *timeout = NULL;
    DBGUI_WAIT_STATE_CHANGE k_out;
    NTSTATUS st;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!a3) return STATUS_INVALID_PARAMETER;
    if (a2) {
        if (!access_ok((void *)a2, sizeof(k_timeout)) || copy_from_user(&k_timeout, (void *)a2, sizeof(k_timeout)))
            return STATUS_ACCESS_VIOLATION;
        timeout = &k_timeout;
    }
    st = debug_wait_service((HANDLE)a0, (BOOLEAN)(a1 != 0), timeout, &k_out);
    if (NT_SUCCESS(st) && st == STATUS_SUCCESS) {
        if (copy_to_user((void *)a3, &k_out, sizeof(k_out))) return STATUS_ACCESS_VIOLATION;
    }
    return st;
}

NTSTATUS nt_svc_NtDebugContinue(unsigned long a0, unsigned long a1, unsigned long a2,
                                unsigned long a3, unsigned long a4, unsigned long a5,
                                unsigned long a6, unsigned long a7, unsigned long a8,
                                unsigned long a9, unsigned long a10, unsigned long a11)
{
    CLIENT_ID k_cid;
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!a1) return STATUS_INVALID_PARAMETER;
    if (!access_ok((void *)a1, sizeof(k_cid)) || copy_from_user(&k_cid, (void *)a1, sizeof(k_cid)))
        return STATUS_ACCESS_VIOLATION;
    st = ObReferenceObjectByHandle(kshim_current_process(), (HANDLE)a0, &nt_debug_object_type,
                                   UserMode, DEBUG_OBJECT_WAIT_STATE_CHANGE, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = debug_continue_core((struct nt_debug_obj *)nt_object_body(hdr), &k_cid, (NTSTATUS)a2);
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS nt_svc_NtSetInformationDebugObject(unsigned long a0, unsigned long a1, unsigned long a2,
                                            unsigned long a3, unsigned long a4, unsigned long a5,
                                            unsigned long a6, unsigned long a7, unsigned long a8,
                                            unsigned long a9, unsigned long a10, unsigned long a11)
{
    ULONG len = (ULONG)a3;
    char kbuf[DEBUG_INFO_MAX_BYTES];
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!a0) return STATUS_INVALID_HANDLE;
    if ((DEBUGOBJECTINFOCLASS)a1 != DebugObjectKillProcessOnExitInformation) return STATUS_INVALID_INFO_CLASS;
    if (len > DEBUG_INFO_MAX_BYTES) return STATUS_INFO_LENGTH_MISMATCH;
    if (!a2) return STATUS_INVALID_PARAMETER;
    if (!access_ok((void *)a2, len) || copy_from_user(kbuf, (void *)a2, len)) return STATUS_ACCESS_VIOLATION;
    st = ObReferenceObjectByHandle(kshim_current_process(), (HANDLE)a0, &nt_debug_object_type,
                                   UserMode, DEBUG_OBJECT_SET_INFORMATION, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = debug_set_info_core((struct nt_debug_obj *)nt_object_body(hdr),
                             (DEBUGOBJECTINFOCLASS)a1, kbuf, len);
    ObDereferenceObject(hdr);
    return st;
}

#endif /* __KERNEL__ */
