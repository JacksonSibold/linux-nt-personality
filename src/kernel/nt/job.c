// SPDX-License-Identifier: GPL-2.0
/*
 * job.c - NT job objects: the Job object type and its six services.
 *
 * Implemented:
 *   - NtCreateJobObject, NtOpenJobObject (named or unnamed, access masks checked)
 *   - NtAssignProcessToJobObject: a process joins one job. A process already in
 *     a job is refused with STATUS_ACCESS_DENIED (nested jobs are not supported).
 *   - NtTerminateJobObject: terminates every member with the given exit status.
 *   - NtQueryInformationJobObject: BasicAccountingInformation,
 *     BasicLimitInformation, BasicProcessIdList, ExtendedLimitInformation.
 *   - NtSetInformationJobObject: BasicLimitInformation, ExtendedLimitInformation.
 *   - JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: when the last job handle closes, every
 *     member is terminated with exit status 0.
 *
 * Stored but not enforced: every other limit (working set, CPU time, active
 * process count, memory, affinity, priority, scheduling class, UI restrictions
 * are not supported, breakaway and exception flags). The values round-trip
 * through set and query. See docs/modules/job.md.
 *
 * Membership: a job keeps a list of member nodes. Each member holds a
 * reference on the job object, and the process points at its job (proc->job).
 * ObDestroyProcessObject calls nt_job_remove_process first, which drops the
 * node and the reference. Ordinary process exit does not destroy the process
 * object in this tree, so a query, terminate or last-handle close also prunes
 * members that have exited (job_prune_exited). Until then an exited member is
 * excluded from ActiveProcesses and the process-id list.
 *
 * Lock order: job->lock, then proc->lock, then the thread registry lock taken
 * inside ObTerminateProcessThreads. No path takes them in another order.
 */

#include "job.h"
#include "handle.h"
#include <stddef.h>

/* Kernel ABI wrappers copy information through a kernel buffer of at most
 * this many bytes. The largest class (extended limits) is far below it. */
#define JOB_INFO_MAX_BYTES 4096UL

struct job_member {
    struct list_head    link;
    struct nt_eprocess *proc;
};

struct nt_job {
    spinlock_t          lock;
    struct nt_object_header *hdr;
    struct list_head    members;
    ULONG               total_assigned;      /* processes ever assigned */
    ULONG               removed_terminated;  /* terminated members already removed */
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
};

static void job_delete(struct nt_object_header *hdr);
static void job_close(struct nt_object_header *hdr, HANDLE handle);

struct nt_object_type nt_job_type = {
    .name = "Job",
    .ops = { .delete_obj = job_delete, .close = job_close, },
    .pool_tag = 0,
};

/* -------------------------------------------------------------------------- */
/* Process helpers                                                            */
/* -------------------------------------------------------------------------- */

static bool job_proc_terminated(struct nt_eprocess *p)
{
    bool t;
    spin_lock(&p->lock);
    t = p->terminated;
    spin_unlock(&p->lock);
    return t;
}

/* Same effect as the target branch of NtTerminateProcess: set the exit state,
 * wake waiters, then terminate the threads. A process already terminated is
 * left alone. Caller holds the job lock. */
static void job_proc_terminate(struct nt_eprocess *p, NTSTATUS status)
{
    bool was;
    spin_lock(&p->lock);
    was = p->terminated;
    if (!was) {
        p->exit_status = status;
        p->terminated = true;
#ifdef __KERNEL__
        wake_up(&p->exit_wq);
#else
        pthread_cond_broadcast(&p->exit_cond);
#endif
    }
    spin_unlock(&p->lock);
    if (!was) ObTerminateProcessThreads(p, status);
}

/* -------------------------------------------------------------------------- */
/* Membership (caller holds job->lock)                                        */
/* -------------------------------------------------------------------------- */

static struct job_member *job_find_member(struct nt_job *job, struct nt_eprocess *p)
{
    struct list_head *pos;
    list_for_each(pos, &job->members) {
        struct job_member *m = list_entry(pos, struct job_member, link);
        if (m->proc == p) return m;
    }
    return NULL;
}

static void job_count_members(struct nt_job *job, ULONG *active, ULONG *terminated)
{
    struct list_head *pos;
    *active = 0;
    *terminated = 0;
    list_for_each(pos, &job->members) {
        struct job_member *m = list_entry(pos, struct job_member, link);
        if (job_proc_terminated(m->proc)) (*terminated)++;
        else (*active)++;
    }
}

/* Drops the membership of every member that has exited (caller holds job->lock).
 * Ordinary process exit does not reach ObDestroyProcessObject in this tree, so
 * exited members are pruned here as well. Returns how many membership
 * references the caller must release after it drops the lock. */
static ULONG job_prune_exited(struct nt_job *job)
{
    struct list_head *pos, *next;
    ULONG n = 0;
    for (pos = job->members.next; pos != &job->members; pos = next) {
        struct job_member *m = list_entry(pos, struct job_member, link);
        next = pos->next;
        if (!job_proc_terminated(m->proc)) continue;
        list_del(&m->link);
        m->proc->job = NULL;
        job->removed_terminated++;
        kfree(m);
        n++;
    }
    return n;
}

/* Releases n membership references on hdr. The caller holds its own reference,
 * so the job cannot be deleted here. Call without job->lock held. */
static void job_release_membership_refs(struct nt_object_header *hdr, ULONG n)
{
    while (n--) ObDereferenceObject(hdr);
}

void nt_job_remove_process(struct nt_eprocess *proc)
{
    struct nt_job *job;
    struct job_member *m;
    struct nt_object_header *job_hdr;
    if (!proc) return;
    job = proc->job;
    if (!job) return;

    spin_lock(&job->lock);
    m = job_find_member(job, proc);
    if (m) {
        if (job_proc_terminated(proc)) job->removed_terminated++;
        list_del(&m->link);
    }
    proc->job = NULL;
    job_hdr = job->hdr;
    spin_unlock(&job->lock);

    if (m) kfree(m);
    /* Drop the membership reference taken by the assign service. */
    if (job_hdr) ObDereferenceObject(job_hdr);
}

/* -------------------------------------------------------------------------- */
/* Object type callbacks                                                      */
/* -------------------------------------------------------------------------- */

static void job_delete(struct nt_object_header *hdr)
{
    struct nt_job *job = (struct nt_job *)nt_object_body(hdr);
    /* Members hold references on the job, so none can remain here. */
    (void)job;
#ifndef __KERNEL__
    pthread_mutex_destroy(&job->lock);
#endif
}

/* Runs after every handle close. The last handle (handle_count 0) kills the
 * members when the job has KILL_ON_JOB_CLOSE. */
static void job_close(struct nt_object_header *hdr, HANDLE handle)
{
    struct nt_job *job;
    struct list_head *pos;
    ULONG pruned;
    (void)handle;
    if (atomic64_read(&hdr->handle_count) != 0) return;
    job = (struct nt_job *)nt_object_body(hdr);
    spin_lock(&job->lock);
    pruned = job_prune_exited(job);
    if (job->limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) {
        list_for_each(pos, &job->members) {
            struct job_member *m = list_entry(pos, struct job_member, link);
            job_proc_terminate(m->proc, STATUS_SUCCESS);
        }
    }
    spin_unlock(&job->lock);
    job_release_membership_refs(hdr, pruned);
}

/* -------------------------------------------------------------------------- */
/* Service cores (shared by the userspace entry points and the kernel ABI)    */
/* -------------------------------------------------------------------------- */

static NTSTATUS job_create_core(PHANDLE JobHandle, ACCESS_MASK DesiredAccess,
                                POBJECT_ATTRIBUTES ObjectAttributes)
{
    struct nt_object_header *hdr = NULL;
    struct nt_job *job = NULL;
    struct nt_eprocess *cur = kshim_current_process();
    NTSTATUS st;

    if (!JobHandle) return STATUS_INVALID_PARAMETER;
    ObRegisterObjectType(&nt_job_type);
    st = ObCreateObject(&nt_job_type, ObjectAttributes, sizeof(*job), &hdr, (void **)&job);
    if (!NT_SUCCESS(st)) return st;
    memset(job, 0, sizeof(*job));
    spin_lock_init(&job->lock);
    INIT_LIST_HEAD(&job->members);
    job->hdr = hdr;
    st = ObInsertObject(hdr, ObjectAttributes, DesiredAccess, cur, JobHandle);
    ObDereferenceObject(hdr);
    return st;
}

static NTSTATUS job_open_core(PHANDLE JobHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes)
{
    if (!JobHandle || !ObjectAttributes) return STATUS_INVALID_PARAMETER;
    ObRegisterObjectType(&nt_job_type);
    return ObOpenObjectByName(ObjectAttributes, &nt_job_type, UserMode, DesiredAccess,
                              kshim_current_process(), JobHandle);
}

static NTSTATUS job_assign_core(HANDLE JobHandle, HANDLE ProcessHandle)
{
    struct nt_eprocess *cur = kshim_current_process();
    struct nt_object_header *jh = NULL, *ph = NULL;
    struct nt_job *job;
    struct nt_eprocess *proc;
    struct job_member *m = NULL;
    NTSTATUS st;
    bool terminated;

    if (!JobHandle || !ProcessHandle) return STATUS_INVALID_HANDLE;
    st = ObReferenceObjectByHandle(cur, JobHandle, &nt_job_type, UserMode,
                                   JOB_OBJECT_ASSIGN_PROCESS, &jh, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = ObReferenceObjectByHandle(cur, ProcessHandle, &nt_process_type, UserMode,
                                   JOB_PROCESS_SET_QUOTA | JOB_PROCESS_TERMINATE, &ph, NULL);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(jh);
        return st;
    }

    job = (struct nt_job *)nt_object_body(jh);
    proc = ObFindProcessByHeader(ph);
    if (!proc) {
        st = STATUS_INVALID_HANDLE;
        goto out;
    }
    /* Allocate before the lock: the kernel allocation may sleep. */
    m = (struct job_member *)kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m) {
        st = STATUS_NO_MEMORY;
        goto out;
    }
    m->proc = proc;

    spin_lock(&job->lock);
    terminated = job_proc_terminated(proc);
    if (proc->job) {
        /* Already in a job (this one or another). Nested jobs are not supported. */
        st = STATUS_ACCESS_DENIED;
    } else if (terminated) {
        st = STATUS_PROCESS_IS_TERMINATING;
    } else {
        ObReferenceObject(jh);
        list_add_tail(&m->link, &job->members);
        proc->job = job;
        job->total_assigned++;
        st = STATUS_SUCCESS;
        m = NULL;   /* now owned by the job */
    }
    spin_unlock(&job->lock);

out:
    if (m) kfree(m);
    ObDereferenceObject(ph);
    ObDereferenceObject(jh);
    return st;
}

static NTSTATUS job_terminate_core(HANDLE JobHandle, NTSTATUS ExitStatus)
{
    struct nt_object_header *hdr = NULL;
    struct nt_job *job;
    struct list_head *pos;
    NTSTATUS st;

    if (!JobHandle) return STATUS_INVALID_HANDLE;
    st = ObReferenceObjectByHandle(kshim_current_process(), JobHandle, &nt_job_type, UserMode,
                                   JOB_OBJECT_TERMINATE, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    ULONG pruned;
    job = (struct nt_job *)nt_object_body(hdr);
    spin_lock(&job->lock);
    pruned = job_prune_exited(job);
    list_for_each(pos, &job->members) {
        struct job_member *m = list_entry(pos, struct job_member, link);
        job_proc_terminate(m->proc, ExitStatus);
    }
    spin_unlock(&job->lock);
    job_release_membership_refs(hdr, pruned);
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}

static NTSTATUS job_query_core(struct nt_job *job, JOBOBJECTINFOCLASS cls,
                               void *buf, ULONG len, PULONG retlen, ULONG *pruned)
{
    NTSTATUS st = STATUS_SUCCESS;
    ULONG need = 0;

    spin_lock(&job->lock);
    *pruned = job_prune_exited(job);
    switch (cls) {
    case JobObjectBasicAccountingInformation: {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *a = (JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *)buf;
        ULONG active, terminated;
        need = sizeof(*a);
        if (len != need) { st = STATUS_INFO_LENGTH_MISMATCH; break; }
        job_count_members(job, &active, &terminated);
        memset(a, 0, sizeof(*a));
        a->TotalProcesses = job->total_assigned;
        a->ActiveProcesses = active;
        a->TotalTerminatedProcesses = job->removed_terminated + terminated;
        break;
    }
    case JobObjectBasicLimitInformation:
        need = sizeof(JOBOBJECT_BASIC_LIMIT_INFORMATION);
        if (len != need) { st = STATUS_INFO_LENGTH_MISMATCH; break; }
        memcpy(buf, &job->limits.BasicLimitInformation, need);
        break;
    case JobObjectExtendedLimitInformation:
        need = sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION);
        if (len != need) { st = STATUS_INFO_LENGTH_MISMATCH; break; }
        memcpy(buf, &job->limits, need);
        break;
    case JobObjectBasicProcessIdList: {
        JOBOBJECT_BASIC_PROCESS_ID_LIST *list = (JOBOBJECT_BASIC_PROCESS_ID_LIST *)buf;
        size_t off = offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList);
        ULONG capacity, active = 0, filled = 0;
        struct list_head *pos;
        if (len < off) { st = STATUS_INFO_LENGTH_MISMATCH; break; }
        capacity = (ULONG)((len - off) / sizeof(ULONG_PTR));
        list_for_each(pos, &job->members) {
            struct job_member *m = list_entry(pos, struct job_member, link);
            if (job_proc_terminated(m->proc)) continue;
            if (filled < capacity) list->ProcessIdList[filled++] = (ULONG_PTR)m->proc->pid;
            active++;
        }
        list->NumberOfAssignedProcesses = active;
        list->NumberOfProcessIdsInList = filled;
        need = (ULONG)(off + (size_t)active * sizeof(ULONG_PTR));
        if (active > capacity) st = STATUS_MORE_ENTRIES;
        break;
    }
    default:
        st = STATUS_INVALID_INFO_CLASS;
        break;
    }
    spin_unlock(&job->lock);
    if (retlen) *retlen = need;
    return st;
}

static NTSTATUS job_set_core(struct nt_job *job, JOBOBJECTINFOCLASS cls,
                             const void *buf, ULONG len)
{
    if (cls == JobObjectBasicLimitInformation) {
        const JOBOBJECT_BASIC_LIMIT_INFORMATION *bl = (const JOBOBJECT_BASIC_LIMIT_INFORMATION *)buf;
        if (len != sizeof(*bl)) return STATUS_INFO_LENGTH_MISMATCH;
        if (bl->LimitFlags & ~JOB_OBJECT_LIMIT_VALID_FLAGS) return STATUS_INVALID_PARAMETER;
        spin_lock(&job->lock);
        job->limits.BasicLimitInformation = *bl;
        spin_unlock(&job->lock);
        return STATUS_SUCCESS;
    }
    if (cls == JobObjectExtendedLimitInformation) {
        const JOBOBJECT_EXTENDED_LIMIT_INFORMATION *ext = (const JOBOBJECT_EXTENDED_LIMIT_INFORMATION *)buf;
        if (len != sizeof(*ext)) return STATUS_INFO_LENGTH_MISMATCH;
        if (ext->BasicLimitInformation.LimitFlags & ~JOB_OBJECT_LIMIT_VALID_FLAGS)
            return STATUS_INVALID_PARAMETER;
        spin_lock(&job->lock);
        job->limits = *ext;
        spin_unlock(&job->lock);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_INFO_CLASS;
}

/* Query and set resolve the handle, check the access right for the class, and
 * then run the core. The class is checked first so an unknown class is
 * INVALID_INFO_CLASS whatever the handle. */
static NTSTATUS job_query_service(HANDLE JobHandle, JOBOBJECTINFOCLASS cls,
                                  PVOID buf, ULONG len, PULONG retlen)
{
    struct nt_object_header *hdr = NULL;
    ULONG pruned = 0;
    NTSTATUS st;
    if (!JobHandle) return STATUS_INVALID_HANDLE;
    if (cls != JobObjectBasicAccountingInformation && cls != JobObjectBasicLimitInformation &&
        cls != JobObjectBasicProcessIdList && cls != JobObjectExtendedLimitInformation)
        return STATUS_INVALID_INFO_CLASS;
    if (!buf) return STATUS_INVALID_PARAMETER;
    st = ObReferenceObjectByHandle(kshim_current_process(), JobHandle, &nt_job_type, UserMode,
                                   JOB_OBJECT_QUERY, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = job_query_core((struct nt_job *)nt_object_body(hdr), cls, buf, len, retlen, &pruned);
    job_release_membership_refs(hdr, pruned);
    ObDereferenceObject(hdr);
    return st;
}

static NTSTATUS job_set_service(HANDLE JobHandle, JOBOBJECTINFOCLASS cls, const void *buf, ULONG len)
{
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    if (!JobHandle) return STATUS_INVALID_HANDLE;
    if (cls != JobObjectBasicLimitInformation && cls != JobObjectExtendedLimitInformation)
        return STATUS_INVALID_INFO_CLASS;
    if (!buf) return STATUS_INVALID_PARAMETER;
    st = ObReferenceObjectByHandle(kshim_current_process(), JobHandle, &nt_job_type, UserMode,
                                   JOB_OBJECT_SET_ATTRIBUTES, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = job_set_core((struct nt_job *)nt_object_body(hdr), cls, buf, len);
    ObDereferenceObject(hdr);
    return st;
}

/* -------------------------------------------------------------------------- */
/* Userspace service entry points                                             */
/* -------------------------------------------------------------------------- */

#ifndef __KERNEL__

NTSTATUS nt_svc_NtCreateJobObject(PHANDLE JobHandle, ACCESS_MASK DesiredAccess,
                                  POBJECT_ATTRIBUTES ObjectAttributes)
{
    return job_create_core(JobHandle, DesiredAccess, ObjectAttributes);
}

NTSTATUS nt_svc_NtOpenJobObject(PHANDLE JobHandle, ACCESS_MASK DesiredAccess,
                                POBJECT_ATTRIBUTES ObjectAttributes)
{
    return job_open_core(JobHandle, DesiredAccess, ObjectAttributes);
}

NTSTATUS nt_svc_NtAssignProcessToJobObject(HANDLE JobHandle, HANDLE ProcessHandle)
{
    return job_assign_core(JobHandle, ProcessHandle);
}

NTSTATUS nt_svc_NtTerminateJobObject(HANDLE JobHandle, NTSTATUS ExitStatus)
{
    return job_terminate_core(JobHandle, ExitStatus);
}

NTSTATUS nt_svc_NtQueryInformationJobObject(HANDLE JobHandle,
                                            JOBOBJECTINFOCLASS JobObjectInformationClass,
                                            PVOID JobObjectInformation,
                                            ULONG JobObjectInformationLength,
                                            PULONG ReturnLength)
{
    return job_query_service(JobHandle, JobObjectInformationClass, JobObjectInformation,
                             JobObjectInformationLength, ReturnLength);
}

NTSTATUS nt_svc_NtSetInformationJobObject(HANDLE JobHandle,
                                          JOBOBJECTINFOCLASS JobObjectInformationClass,
                                          PVOID JobObjectInformation,
                                          ULONG JobObjectInformationLength)
{
    return job_set_service(JobHandle, JobObjectInformationClass, JobObjectInformation,
                           JobObjectInformationLength);
}

#else /* __KERNEL__ */

/* -------------------------------------------------------------------------- */
/* Kernel ABI wrappers: ULONG arguments, user pointers copied through the     */
/* kernel (same pattern as security.c and mm.c).                              */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtCreateJobObject(unsigned long a0, unsigned long a1, unsigned long a2,
                                  unsigned long a3, unsigned long a4, unsigned long a5,
                                  unsigned long a6, unsigned long a7, unsigned long a8,
                                  unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE JobHandle = (PHANDLE)a0;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    HANDLE h = NULL;
    NTSTATUS st;
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!JobHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(JobHandle, sizeof(*JobHandle))) return STATUS_ACCESS_VIOLATION;
    st = ObCopyObjectAttributesFromUser((POBJECT_ATTRIBUTES)a2, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    st = job_create_core(&h, (ACCESS_MASK)a1, oa);
    if (!NT_SUCCESS(st)) return st;
    if (copy_to_user(JobHandle, &h, sizeof(h))) {
        ObCloseHandle(kshim_current_process(), h);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtOpenJobObject(unsigned long a0, unsigned long a1, unsigned long a2,
                                unsigned long a3, unsigned long a4, unsigned long a5,
                                unsigned long a6, unsigned long a7, unsigned long a8,
                                unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE JobHandle = (PHANDLE)a0;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    HANDLE h = NULL;
    NTSTATUS st;
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!JobHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(JobHandle, sizeof(*JobHandle))) return STATUS_ACCESS_VIOLATION;
    st = ObCopyObjectAttributesFromUser((POBJECT_ATTRIBUTES)a2, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    st = job_open_core(&h, (ACCESS_MASK)a1, oa);
    if (!NT_SUCCESS(st)) return st;
    if (copy_to_user(JobHandle, &h, sizeof(h))) {
        ObCloseHandle(kshim_current_process(), h);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtAssignProcessToJobObject(unsigned long a0, unsigned long a1, unsigned long a2,
                                           unsigned long a3, unsigned long a4, unsigned long a5,
                                           unsigned long a6, unsigned long a7, unsigned long a8,
                                           unsigned long a9, unsigned long a10, unsigned long a11)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    return job_assign_core((HANDLE)a0, (HANDLE)a1);
}

NTSTATUS nt_svc_NtTerminateJobObject(unsigned long a0, unsigned long a1, unsigned long a2,
                                     unsigned long a3, unsigned long a4, unsigned long a5,
                                     unsigned long a6, unsigned long a7, unsigned long a8,
                                     unsigned long a9, unsigned long a10, unsigned long a11)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    return job_terminate_core((HANDLE)a0, (NTSTATUS)a1);
}

NTSTATUS nt_svc_NtQueryInformationJobObject(unsigned long a0, unsigned long a1, unsigned long a2,
                                            unsigned long a3, unsigned long a4, unsigned long a5,
                                            unsigned long a6, unsigned long a7, unsigned long a8,
                                            unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE JobHandle = (HANDLE)a0;
    PVOID user_buf = (PVOID)a2;
    ULONG len = (ULONG)a3;
    PULONG user_ret = (PULONG)a4;
    char *kbuf;
    ULONG kret = 0;
    NTSTATUS st;
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (len > JOB_INFO_MAX_BYTES) return STATUS_INVALID_PARAMETER;
    if (!user_buf) return STATUS_INVALID_PARAMETER;
    kbuf = (char *)kzalloc(len ? len : 1, GFP_KERNEL);
    if (!kbuf) return STATUS_NO_MEMORY;
    st = job_query_service(JobHandle, (JOBOBJECTINFOCLASS)a1, kbuf, len, &kret);
    if (NT_SUCCESS(st) || st == STATUS_MORE_ENTRIES) {
        if (copy_to_user(user_buf, kbuf, len)) st = STATUS_ACCESS_VIOLATION;
    }
    if (st != STATUS_ACCESS_VIOLATION && user_ret && copy_to_user(user_ret, &kret, sizeof(kret)))
        st = STATUS_ACCESS_VIOLATION;
    kfree(kbuf);
    return st;
}

NTSTATUS nt_svc_NtSetInformationJobObject(unsigned long a0, unsigned long a1, unsigned long a2,
                                          unsigned long a3, unsigned long a4, unsigned long a5,
                                          unsigned long a6, unsigned long a7, unsigned long a8,
                                          unsigned long a9, unsigned long a10, unsigned long a11)
{
    ULONG len = (ULONG)a3;
    char *kbuf;
    NTSTATUS st;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (len > JOB_INFO_MAX_BYTES) return STATUS_INVALID_PARAMETER;
    if (!a2) return STATUS_INVALID_PARAMETER;
    kbuf = (char *)kzalloc(len ? len : 1, GFP_KERNEL);
    if (!kbuf) return STATUS_NO_MEMORY;
    if (!access_ok((void *)a2, len) || copy_from_user(kbuf, (void *)a2, len)) {
        kfree(kbuf);
        return STATUS_ACCESS_VIOLATION;
    }
    st = job_set_service((HANDLE)a0, (JOBOBJECTINFOCLASS)a1, kbuf, len);
    kfree(kbuf);
    return st;
}

#endif /* __KERNEL__ */
