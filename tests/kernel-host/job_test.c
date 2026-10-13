/* job_test.c - JOB object services (src/kernel/nt/job.c).
 *
 * Covers the six job services through their nt_svc_ entry points:
 *
 *   - create (unnamed and named), open by name, NULL handle pointer refused
 *   - access masks: ASSIGN, SET_ATTRIBUTES, QUERY, TERMINATE each refused when
 *     the handle lacks the right
 *   - assignment: a process joins one job; a second assign (same or other job)
 *     is refused with STATUS_ACCESS_DENIED (nested jobs are not supported)
 *   - query: BasicAccounting, BasicProcessIdList (incl. STATUS_MORE_ENTRIES)
 *   - set/query: BasicLimitInformation and ExtendedLimitInformation round trip;
 *     unknown limit flags, wrong lengths and unsupported classes are refused
 *   - NtTerminateJobObject terminates every member with the given status
 *   - KILL_ON_JOB_CLOSE terminates members on the last job handle close; without
 *     the flag, closing the last handle leaves members running
 *   - process destroy removes its membership (ObDestroyProcessObject hook)
 *
 * Built and run by tests/kernel-host/Makefile like the other module tests
 * (userspace kshim, no kernel boot). Style follows handle_edge_test.c.
 */

#include "object.h"
#include "handle.h"
#include "job.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; return; \
    } \
} while (0)

#define ASSERT_STATUS(expr, expect) do { \
    NTSTATUS _st = (expr); \
    if (_st != (expect)) { \
        fprintf(stderr, "FAIL %s:%d: expected 0x%08lx got 0x%08lx for %s\n", \
                __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_st, #expr); \
        g_fail++; return; \
    } \
} while (0)

#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while (0)

/* The host process owns every handle the tests create. Member processes are
 * kept in g_procs so the teardown can destroy any the test left alive. */
static struct nt_eprocess *g_cur;
static struct nt_eprocess *g_procs[16];
static int g_nprocs;

static long allocs(void)
{
    return (long)atomic_load(&kshim_alloc_count);
}

static struct nt_eprocess *new_proc(ULONG pid)
{
    struct nt_eprocess *p = NULL;
    NTSTATUS st = ObCreateProcessObject(&p, pid, "job_test");
    if (st != STATUS_SUCCESS || !p) {
        fprintf(stderr, "new_proc: ObCreateProcessObject failed\n");
        g_fail++;
        return NULL;
    }
    if (g_nprocs < (int)(sizeof(g_procs) / sizeof(g_procs[0])))
        g_procs[g_nprocs++] = p;
    return p;
}

/* A handle to p in the host process, with full process access. */
static HANDLE proc_handle(struct nt_eprocess *p)
{
    HANDLE h = NULL;
    NTSTATUS st = ObCreateHandle(g_cur, p->obj, 0x1FFFFFUL, 0, &h);
    if (st != STATUS_SUCCESS) { g_fail++; return NULL; }
    return h;
}

static HANDLE job_create(ACCESS_MASK access)
{
    HANDLE h = NULL;
    NTSTATUS st = nt_svc_NtCreateJobObject(&h, access, NULL);
    if (st != STATUS_SUCCESS) { fprintf(stderr, "job_create: 0x%08lx\n", (unsigned long)st); g_fail++; return NULL; }
    return h;
}

static NTSTATUS job_assign(HANDLE job, HANDLE proc)
{
    return nt_svc_NtAssignProcessToJobObject(job, proc);
}

static NTSTATUS job_accounting(HANDLE job, JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *out)
{
    ULONG rl = 0;
    memset(out, 0, sizeof(*out));
    return nt_svc_NtQueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                              out, sizeof(*out), &rl);
}

static void make_us(UNICODE_STRING *us, WCHAR *buf, size_t n, const char *s)
{
    size_t len = strlen(s), i;
    if (len >= n) len = n - 1;
    for (i = 0; i < len; i++) buf[i] = (WCHAR)(unsigned char)s[i];
    buf[len] = 0;
    us->Buffer = buf;
    us->Length = (USHORT)(len * sizeof(WCHAR));
    us->MaximumLength = (USHORT)((len + 1) * sizeof(WCHAR));
}

TEST(create_and_null_pointer)
{
    HANDLE j = NULL;
    ASSERT_STATUS(nt_svc_NtCreateJobObject(NULL, JOB_OBJECT_ALL_ACCESS, NULL), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtCreateJobObject(&j, JOB_OBJECT_ALL_ACCESS, NULL), STATUS_SUCCESS);
    ASSERT(j != NULL);
    ASSERT_STATUS(ObCloseHandle(g_cur, j), STATUS_SUCCESS);
    PASS();
}

TEST(named_create_and_open)
{
    WCHAR buf[64];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL, h2 = NULL;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;

    make_us(&us, buf, 64, "\\BaseNamedObjects\\JobTestNamed");
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;

    ASSERT_STATUS(nt_svc_NtOpenJobObject(&h2, JOB_OBJECT_QUERY, &oa), STATUS_OBJECT_NAME_NOT_FOUND);
    ASSERT_STATUS(nt_svc_NtCreateJobObject(&h, JOB_OBJECT_ALL_ACCESS, &oa), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtOpenJobObject(&h2, JOB_OBJECT_QUERY, &oa), STATUS_SUCCESS);
    ASSERT_STATUS(job_accounting(h2, &acct), STATUS_SUCCESS);
    ASSERT(acct.TotalProcesses == 0);
    ASSERT_STATUS(ObCloseHandle(g_cur, h2), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_cur, h), STATUS_SUCCESS);
    PASS();
}

TEST(access_masks_enforced)
{
    HANDLE jq = job_create(JOB_OBJECT_QUERY);
    HANDLE hp = NULL;
    struct nt_eprocess *p = new_proc(3001);
    JOBOBJECT_BASIC_LIMIT_INFORMATION bl;
    ULONG rl = 0;
    ASSERT(jq && p);
    hp = proc_handle(p);
    ASSERT(hp);
    /* QUERY alone does not allow assign, set, or terminate. */
    ASSERT_STATUS(job_assign(jq, hp), STATUS_ACCESS_DENIED);
    memset(&bl, 0, sizeof(bl));
    ASSERT_STATUS(nt_svc_NtSetInformationJobObject(jq, JobObjectBasicLimitInformation, &bl, sizeof(bl)),
                  STATUS_ACCESS_DENIED);
    ASSERT_STATUS(nt_svc_NtTerminateJobObject(jq, 1), STATUS_ACCESS_DENIED);
    /* Query itself is allowed. */
    ASSERT_STATUS(nt_svc_NtQueryInformationJobObject(jq, JobObjectBasicLimitInformation,
                                                     &bl, sizeof(bl), &rl), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_cur, jq), STATUS_SUCCESS);
    PASS();
}

TEST(assign_is_exclusive)
{
    HANDLE j1 = job_create(JOB_OBJECT_ALL_ACCESS);
    HANDLE j2 = job_create(JOB_OBJECT_ALL_ACCESS);
    struct nt_eprocess *p = new_proc(3002);
    HANDLE hp = proc_handle(p);
    ASSERT(j1 && j2 && p && hp);
    ASSERT_STATUS(job_assign(j1, hp), STATUS_SUCCESS);
    /* Already in a job: same job and a different job are both refused. */
    ASSERT_STATUS(job_assign(j1, hp), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(job_assign(j2, hp), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(ObCloseHandle(g_cur, j1), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_cur, j2), STATUS_SUCCESS);
    PASS();
}

TEST(accounting_and_pid_list)
{
    HANDLE j = job_create(JOB_OBJECT_ALL_ACCESS);
    struct nt_eprocess *p = new_proc(3003);
    HANDLE hp = proc_handle(p);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    unsigned char buf[offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) + 8 * sizeof(ULONG_PTR)];
    JOBOBJECT_BASIC_PROCESS_ID_LIST *list = (JOBOBJECT_BASIC_PROCESS_ID_LIST *)buf;
    ULONG rl = 0;
    ASSERT(j && p && hp);
    ASSERT_STATUS(job_assign(j, hp), STATUS_SUCCESS);

    ASSERT_STATUS(job_accounting(j, &acct), STATUS_SUCCESS);
    ASSERT(acct.TotalProcesses == 1);
    ASSERT(acct.ActiveProcesses == 1);
    ASSERT(acct.TotalTerminatedProcesses == 0);

    ASSERT_STATUS(nt_svc_NtQueryInformationJobObject(j, JobObjectBasicProcessIdList,
                                                     list, sizeof(buf), &rl), STATUS_SUCCESS);
    ASSERT(list->NumberOfAssignedProcesses == 1);
    ASSERT(list->NumberOfProcessIdsInList == 1);
    ASSERT(list->ProcessIdList[0] == (ULONG_PTR)p->pid);

    /* A buffer that holds the header only reports more entries than fit. */
    ASSERT_STATUS(nt_svc_NtQueryInformationJobObject(j, JobObjectBasicProcessIdList,
                                                     list, offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList),
                                                     &rl), STATUS_MORE_ENTRIES);
    ASSERT_STATUS(ObCloseHandle(g_cur, j), STATUS_SUCCESS);
    PASS();
}

TEST(basic_and_extended_limits_round_trip)
{
    HANDLE j = job_create(JOB_OBJECT_ALL_ACCESS);
    JOBOBJECT_BASIC_LIMIT_INFORMATION bl;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
    ULONG rl = 0;
    ASSERT(j);

    memset(&bl, 0, sizeof(bl));
    bl.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    bl.ActiveProcessLimit = 4;
    ASSERT_STATUS(nt_svc_NtSetInformationJobObject(j, JobObjectBasicLimitInformation, &bl, sizeof(bl)),
                  STATUS_SUCCESS);
    memset(&bl, 0, sizeof(bl));
    ASSERT_STATUS(nt_svc_NtQueryInformationJobObject(j, JobObjectBasicLimitInformation,
                                                     &bl, sizeof(bl), &rl), STATUS_SUCCESS);
    ASSERT(rl == sizeof(bl));
    ASSERT(bl.LimitFlags == (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS));
    ASSERT(bl.ActiveProcessLimit == 4);

    memset(&ext, 0, sizeof(ext));
    ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    ext.ProcessMemoryLimit = 1UL << 20;
    ASSERT_STATUS(nt_svc_NtSetInformationJobObject(j, JobObjectExtendedLimitInformation, &ext, sizeof(ext)),
                  STATUS_SUCCESS);
    memset(&ext, 0, sizeof(ext));
    ASSERT_STATUS(nt_svc_NtQueryInformationJobObject(j, JobObjectExtendedLimitInformation,
                                                     &ext, sizeof(ext), &rl), STATUS_SUCCESS);
    ASSERT(ext.BasicLimitInformation.LimitFlags == JOB_OBJECT_LIMIT_PROCESS_MEMORY);
    ASSERT(ext.ProcessMemoryLimit == (1UL << 20));

    /* Unknown flag bit, wrong length, unsupported class. */
    memset(&bl, 0, sizeof(bl));
    bl.LimitFlags = 0x80000000UL;
    ASSERT_STATUS(nt_svc_NtSetInformationJobObject(j, JobObjectBasicLimitInformation, &bl, sizeof(bl)),
                  STATUS_INVALID_PARAMETER);
    bl.LimitFlags = 0;
    ASSERT_STATUS(nt_svc_NtSetInformationJobObject(j, JobObjectBasicLimitInformation, &bl, sizeof(bl) - 1),
                  STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(nt_svc_NtQueryInformationJobObject(j, JobObjectBasicLimitInformation,
                                                     &bl, sizeof(bl) - 1, &rl), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(nt_svc_NtQueryInformationJobObject(j, JobObjectBasicUIRestrictions,
                                                     &bl, sizeof(bl), &rl), STATUS_INVALID_INFO_CLASS);
    ASSERT_STATUS(nt_svc_NtSetInformationJobObject(j, JobObjectBasicProcessIdList, &bl, sizeof(bl)),
                  STATUS_INVALID_INFO_CLASS);
    ASSERT_STATUS(ObCloseHandle(g_cur, j), STATUS_SUCCESS);
    PASS();
}

TEST(terminate_members)
{
    HANDLE j = job_create(JOB_OBJECT_ALL_ACCESS);
    struct nt_eprocess *p = new_proc(3004);
    HANDLE hp = proc_handle(p);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    ASSERT(j && p && hp);
    ASSERT_STATUS(job_assign(j, hp), STATUS_SUCCESS);

    ASSERT_STATUS(nt_svc_NtTerminateJobObject(j, 7), STATUS_SUCCESS);
    ASSERT(p->terminated);
    ASSERT(p->exit_status == 7);
    ASSERT_STATUS(job_accounting(j, &acct), STATUS_SUCCESS);
    ASSERT(acct.ActiveProcesses == 0);
    ASSERT(acct.TotalTerminatedProcesses == 1);
    ASSERT_STATUS(ObCloseHandle(g_cur, j), STATUS_SUCCESS);
    PASS();
}

TEST(kill_on_close_last_handle)
{
    HANDLE jk = job_create(JOB_OBJECT_ALL_ACCESS);
    JOBOBJECT_BASIC_LIMIT_INFORMATION bl;
    struct nt_eprocess *p = new_proc(3005);
    HANDLE hp = proc_handle(p);
    ASSERT(jk && p && hp);
    memset(&bl, 0, sizeof(bl));
    bl.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ASSERT_STATUS(nt_svc_NtSetInformationJobObject(jk, JobObjectBasicLimitInformation, &bl, sizeof(bl)),
                  STATUS_SUCCESS);
    ASSERT_STATUS(job_assign(jk, hp), STATUS_SUCCESS);
    ASSERT(!p->terminated);
    ASSERT_STATUS(ObCloseHandle(g_cur, jk), STATUS_SUCCESS);
    ASSERT(p->terminated);
    PASS();
}

TEST(no_kill_without_flag)
{
    HANDLE jn = job_create(JOB_OBJECT_ALL_ACCESS);
    struct nt_eprocess *p = new_proc(3006);
    HANDLE hp = proc_handle(p);
    ASSERT(jn && p && hp);
    ASSERT_STATUS(job_assign(jn, hp), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_cur, jn), STATUS_SUCCESS);
    ASSERT(!p->terminated);
    PASS();
}

TEST(destroy_removes_membership)
{
    HANDLE j = job_create(JOB_OBJECT_ALL_ACCESS);
    /* Not recorded in g_procs: this test destroys the process itself. */
    struct nt_eprocess *p = NULL;
    HANDLE hp;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    ULONG rl = 0;
    unsigned char buf[offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) + sizeof(ULONG_PTR)];
    JOBOBJECT_BASIC_PROCESS_ID_LIST *list = (JOBOBJECT_BASIC_PROCESS_ID_LIST *)buf;
    ASSERT(ObCreateProcessObject(&p, 3007, "job_test") == STATUS_SUCCESS && p);
    ASSERT(j);
    hp = proc_handle(p);
    ASSERT(hp);
    ASSERT_STATUS(job_assign(j, hp), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtTerminateJobObject(j, 5), STATUS_SUCCESS);

    ObDestroyProcessObject(p);

    ASSERT_STATUS(job_accounting(j, &acct), STATUS_SUCCESS);
    ASSERT(acct.TotalProcesses == 1);
    ASSERT(acct.ActiveProcesses == 0);
    /* The terminated member was removed on destroy and still counts. */
    ASSERT(acct.TotalTerminatedProcesses == 1);
    ASSERT_STATUS(nt_svc_NtQueryInformationJobObject(j, JobObjectBasicProcessIdList,
                                                     list, sizeof(buf), &rl), STATUS_SUCCESS);
    ASSERT(list->NumberOfAssignedProcesses == 0);
    ASSERT_STATUS(ObCloseHandle(g_cur, j), STATUS_SUCCESS);
    PASS();
}

/* An exited member that is not destroyed (the normal exit path in this tree)
 * leaves the job on the next query and releases its membership. */
TEST(exited_member_is_pruned)
{
    HANDLE j = job_create(JOB_OBJECT_ALL_ACCESS);
    HANDLE j2 = job_create(JOB_OBJECT_ALL_ACCESS);
    struct nt_eprocess *p = new_proc(3008);
    HANDLE hp = proc_handle(p);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    ASSERT(j && j2 && p && hp);
    ASSERT_STATUS(job_assign(j, hp), STATUS_SUCCESS);

    /* Simulate the exit: state set, object not destroyed. */
    p->terminated = true;
    p->exit_status = 9;

    ASSERT_STATUS(job_accounting(j, &acct), STATUS_SUCCESS);
    ASSERT(acct.TotalProcesses == 1);
    ASSERT(acct.ActiveProcesses == 0);
    ASSERT(acct.TotalTerminatedProcesses == 1);
    ASSERT(p->job == NULL);
    /* Membership released, so the exited process is refused as terminating. */
    ASSERT_STATUS(job_assign(j2, hp), STATUS_PROCESS_IS_TERMINATING);
    ASSERT_STATUS(ObCloseHandle(g_cur, j), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(g_cur, j2), STATUS_SUCCESS);
    PASS();
}

int main(void)
{
    int i;
    printf("job tests starting\n");
    if (ObInitObjectManager() != STATUS_SUCCESS) {
        fprintf(stderr, "ObInitObjectManager failed\n");
        return 1;
    }
    g_cur = new_proc(3000);
    if (!g_cur) return 1;
    kshim_set_current_process(g_cur);

    RUN(create_and_null_pointer);
    RUN(named_create_and_open);
    RUN(access_masks_enforced);
    RUN(assign_is_exclusive);
    RUN(accounting_and_pid_list);
    RUN(basic_and_extended_limits_round_trip);
    RUN(terminate_members);
    RUN(kill_on_close_last_handle);
    RUN(no_kill_without_flag);
    RUN(destroy_removes_membership);
    RUN(exited_member_is_pruned);

    printf("job tests: %d passed, %d failed\n", g_pass, g_fail);

    /* Destroy member processes first (drops their membership), then the host
     * (closes its job handles), then the object manager. */
    for (i = g_nprocs - 1; i >= 0; i--) {
        if (g_procs[i] != g_cur) ObDestroyProcessObject(g_procs[i]);
    }
    kshim_set_current_process(NULL);
    ObDestroyProcessObject(g_cur);
    ObCleanupObjectManager();
    if (g_fail) return 1;
    if (allocs() != 0) {
        fprintf(stderr, "LEAK: %ld allocations after ObCleanupObjectManager\n", allocs());
        return 1;
    }
    return 0;
}
