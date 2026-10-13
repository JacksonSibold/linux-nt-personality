/* debugobj_test.c - DEBUG object services (src/kernel/nt/debugobj.c).
 *
 * Covers the debug object type through its nt_svc_ entry points:
 *
 *   - NtCreateDebugObject: NULL handle pointer and unknown flags refused,
 *     DEBUG_KILL_ON_CLOSE accepted
 *   - access masks: ADD_REMOVE_PROCESS (attach, remove), WAIT_STATE_CHANGE
 *     (wait, continue), SET_INFORMATION (set information) each enforced
 *   - NtDebugActiveProcess: needs PROCESS_SUSPEND_RESUME on the process handle,
 *     refuses self-debug, and a process has at most one debug object
 *   - attach queues a process-create event; NtWaitForDebugEvent dequeues it
 *     (zero timeout polls, relative timeout expires with STATUS_TIMEOUT,
 *     an alertable wait with a pending APC returns STATUS_USER_APC)
 *   - thread exit (NtTerminateThread) and process exit (NtTerminateProcess)
 *     generate events carrying the exit status
 *   - NtDebugContinue matches the ClientId of the last dequeued event and
 *     refuses an unknown ClientId or a second continue
 *   - NtRemoveProcessDebug detaches, purges that process's queued events, and
 *     refuses a process that is not attached
 *   - closing the last handle detaches every process; with DEBUG_KILL_ON_CLOSE
 *     (or the KillProcessOnExit information) the processes are terminated
 *   - multi-thread: several threads block in NtWaitForDebugEvent and each
 *     receives exactly one of the events posted to the queue
 *
 * Built and run by tests/kernel-host/Makefile like the other module tests
 * (userspace kshim, no kernel boot). Style follows job_test.c.
 */

#include "object.h"
#include "handle.h"
#include "sync.h"
#include "debugobj.h"
#include "proc_thread.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
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
 * kept in g_procs so teardown can destroy any the test left alive. */
static struct nt_eprocess *g_cur;
static struct nt_eprocess *g_procs[32];
static int g_nprocs;
static struct nt_thread *g_main_nt;

static long allocs(void)
{
    return (long)atomic_load(&kshim_alloc_count);
}

static struct nt_eprocess *new_proc(ULONG pid)
{
    struct nt_eprocess *p = NULL;
    NTSTATUS st = ObCreateProcessObject(&p, pid, "debugobj_test");
    if (st != STATUS_SUCCESS || !p) {
        fprintf(stderr, "new_proc: ObCreateProcessObject failed\n");
        g_fail++;
        return NULL;
    }
    if (g_nprocs < (int)(sizeof(g_procs) / sizeof(g_procs[0])))
        g_procs[g_nprocs++] = p;
    return p;
}

/* Inserts a handle to proc into the host's table with the given access. */
static HANDLE proc_handle(struct nt_eprocess *p, ACCESS_MASK access)
{
    HANDLE h = NULL;
    if (ObInsertObject(p->obj, NULL, access, g_cur, &h) != STATUS_SUCCESS) return NULL;
    return h;
}

static HANDLE thread_handle(struct nt_ethread *t, ACCESS_MASK access)
{
    HANDLE h = NULL;
    if (ObInsertObject(t->obj, NULL, access, g_cur, &h) != STATUS_SUCCESS) return NULL;
    return h;
}

static HANDLE new_debug(ULONG flags, ACCESS_MASK access)
{
    HANDLE h = NULL;
    if (nt_svc_NtCreateDebugObject(&h, access, NULL, flags) != STATUS_SUCCESS) return NULL;
    return h;
}

/* Polls (zero timeout) for one event and checks its state and client id. */
static NTSTATUS poll_event(HANDLE dbg, DBGUI_WAIT_STATE_CHANGE *out)
{
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    return nt_svc_NtWaitForDebugEvent(dbg, FALSE, &zero, out);
}

TEST(create_flags_and_null)
{
    HANDLE h = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDebugObject(NULL, DEBUG_OBJECT_ALL_ACCESS, NULL, 0),
                  STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtCreateDebugObject(&h, DEBUG_OBJECT_ALL_ACCESS, NULL, 0x2),
                  STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtCreateDebugObject(&h, DEBUG_OBJECT_ALL_ACCESS, NULL, DEBUG_KILL_ON_CLOSE),
                  STATUS_SUCCESS);
    ASSERT(h != NULL);
    ObCloseHandle(g_cur, h);
    PASS();
}

TEST(access_masks)
{
    struct nt_eprocess *p = new_proc(4100);
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE wait_only = new_debug(0, DEBUG_OBJECT_WAIT_STATE_CHANGE);
    HANDLE full = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    ASSERT(p && ph && wait_only && full);
    /* Attach and remove need ADD_REMOVE_PROCESS. */
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, wait_only), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, full), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(ph, wait_only), STATUS_ACCESS_DENIED);
    /* Wait needs WAIT_STATE_CHANGE: a debug handle with only ADD_REMOVE is refused. */
    {
        HANDLE add_only = new_debug(0, DEBUG_OBJECT_ADD_REMOVE_PROCESS);
        DBGUI_WAIT_STATE_CHANGE out;
        ASSERT(add_only != NULL);
        ASSERT_STATUS(poll_event(add_only, &out), STATUS_ACCESS_DENIED);
        ObCloseHandle(g_cur, add_only);
    }
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(ph, full), STATUS_SUCCESS);
    ObCloseHandle(g_cur, wait_only);
    ObCloseHandle(g_cur, full);
    PASS();
}

TEST(attach_needs_suspend_resume)
{
    struct nt_eprocess *p = new_proc(4200);
    HANDLE weak = proc_handle(p, PROCESS_TERMINATE);
    HANDLE strong = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE dbg = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    ASSERT(p && weak && strong && dbg);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(weak, dbg), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(strong, dbg), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(strong, dbg), STATUS_SUCCESS);
    ObCloseHandle(g_cur, dbg);
    PASS();
}

TEST(self_debug_refused)
{
    HANDLE self = proc_handle(g_cur, PROCESS_SUSPEND_RESUME);
    HANDLE dbg = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    ASSERT(self && dbg);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(self, dbg), STATUS_ACCESS_DENIED);
    ObCloseHandle(g_cur, dbg);
    PASS();
}

TEST(one_debugger_per_process)
{
    struct nt_eprocess *p = new_proc(4300);
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE a = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    HANDLE b = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    ASSERT(p && ph && a && b);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, a), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, b), STATUS_PORT_ALREADY_SET);
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(ph, b), STATUS_PORT_NOT_SET);
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(ph, a), STATUS_SUCCESS);
    ObCloseHandle(g_cur, a);
    ObCloseHandle(g_cur, b);
    PASS();
}

TEST(attach_event_and_continue)
{
    struct nt_eprocess *p = new_proc(4400);
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE dbg = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    DBGUI_WAIT_STATE_CHANGE out;
    CLIENT_ID cid;
    ASSERT(p && ph && dbg);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, dbg), STATUS_SUCCESS);
    /* Attach queues the create-process event for the target. */
    ASSERT_STATUS(poll_event(dbg, &out), STATUS_SUCCESS);
    ASSERT(out.NewState == DbgCreateProcessStateChange);
    ASSERT((ULONG)(ULONG_PTR)out.AppClientId.UniqueProcess == 4400);
    cid = out.AppClientId;
    /* Queue drained: a zero timeout poll times out. */
    ASSERT_STATUS(poll_event(dbg, &out), STATUS_TIMEOUT);
    /* Continue must match the client id of the dequeued event. */
    cid.UniqueProcess = (HANDLE)(ULONG_PTR)999;
    ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &cid, DBG_CONTINUE), STATUS_INVALID_PARAMETER);
    cid.UniqueProcess = (HANDLE)(ULONG_PTR)4400;
    ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &cid, DBG_CONTINUE), STATUS_SUCCESS);
    /* A second continue has nothing to match. */
    ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &cid, DBG_CONTINUE), STATUS_INVALID_PARAMETER);
    /* An unknown continue status is refused before the lookup. */
    ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &cid, 0xC0000005L), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(ph, dbg), STATUS_SUCCESS);
    ObCloseHandle(g_cur, dbg);
    PASS();
}

TEST(relative_timeout_and_apc)
{
    HANDLE dbg = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    DBGUI_WAIT_STATE_CHANGE out;
    LARGE_INTEGER rel;
    ASSERT(dbg);
    rel.QuadPart = -100000; /* 10 ms relative */
    ASSERT_STATUS(nt_svc_NtWaitForDebugEvent(dbg, FALSE, &rel, &out), STATUS_TIMEOUT);
    /* An alertable wait with an APC already queued returns at once. */
    nt_thread_queue_apc(g_main_nt);
    {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        ASSERT_STATUS(nt_svc_NtWaitForDebugEvent(dbg, TRUE, &zero, &out), STATUS_USER_APC);
    }
    ObCloseHandle(g_cur, dbg);
    PASS();
}

TEST(thread_and_process_exit_events)
{
    struct nt_eprocess *p = new_proc(4500);
    struct nt_ethread *thr = NULL;
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME | PROCESS_TERMINATE);
    HANDLE dbg = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    HANDLE th = NULL;
    DBGUI_WAIT_STATE_CHANGE out;
    CLIENT_ID cid;
    ASSERT(p && ph && dbg);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, dbg), STATUS_SUCCESS);
    ASSERT_STATUS(poll_event(dbg, &out), STATUS_SUCCESS); /* create-process */
    cid = out.AppClientId;
    ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &cid, DBG_CONTINUE), STATUS_SUCCESS);

    ASSERT_STATUS(ObCreateThreadObject(&thr, p, 4501), STATUS_SUCCESS);
    th = thread_handle(thr, 0x001FFFFFUL);
    ASSERT(th != NULL);
    ASSERT_STATUS(nt_svc_NtTerminateThread(th, 7), STATUS_SUCCESS);
    ASSERT_STATUS(poll_event(dbg, &out), STATUS_SUCCESS);
    ASSERT(out.NewState == DbgExitThreadStateChange);
    ASSERT((ULONG)(ULONG_PTR)out.AppClientId.UniqueThread == 4501);
    ASSERT(out.StateInfo.ExitThread.ExitStatus == 7);
    cid = out.AppClientId;
    ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &cid, DBG_CONTINUE), STATUS_SUCCESS);

    ASSERT_STATUS(nt_svc_NtTerminateProcess(ph, 5), STATUS_SUCCESS);
    ASSERT_STATUS(poll_event(dbg, &out), STATUS_SUCCESS);
    ASSERT(out.NewState == DbgExitProcessStateChange);
    ASSERT(out.StateInfo.ExitProcess.ExitStatus == 5);
    cid = out.AppClientId;
    ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &cid, DBG_CONTINUE), STATUS_SUCCESS);
    ObCloseHandle(g_cur, dbg);
    PASS();
}

TEST(remove_purges_queued_events)
{
    struct nt_eprocess *p = new_proc(4600);
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE dbg = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    DBGUI_WAIT_STATE_CHANGE out;
    ASSERT(p && ph && dbg);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, dbg), STATUS_SUCCESS);
    /* The create event is still queued: detaching discards it. */
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(ph, dbg), STATUS_SUCCESS);
    ASSERT_STATUS(poll_event(dbg, &out), STATUS_TIMEOUT);
    ObCloseHandle(g_cur, dbg);
    PASS();
}

TEST(close_detaches_without_kill)
{
    struct nt_eprocess *p = new_proc(4700);
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE first = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    HANDLE second = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    ASSERT(p && ph && first && second);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, first), STATUS_SUCCESS);
    ObCloseHandle(g_cur, first);
    ASSERT(!p->terminated);
    /* The detach on close freed the process for another debugger. */
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, second), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(ph, second), STATUS_SUCCESS);
    ObCloseHandle(g_cur, second);
    PASS();
}

TEST(kill_on_close_flag)
{
    struct nt_eprocess *p = new_proc(4800);
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE dbg = new_debug(DEBUG_KILL_ON_CLOSE, DEBUG_OBJECT_ALL_ACCESS);
    ASSERT(p && ph && dbg);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, dbg), STATUS_SUCCESS);
    ObCloseHandle(g_cur, dbg);
    ASSERT(p->terminated);
    ASSERT(p->exit_status == STATUS_SUCCESS);
    PASS();
}

TEST(kill_on_exit_information)
{
    struct nt_eprocess *p = new_proc(4900);
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE dbg = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    HANDLE ro = new_debug(0, DEBUG_OBJECT_WAIT_STATE_CHANGE);
    BOOLEAN on = TRUE;
    ULONG wide = 1;
    ASSERT(p && ph && dbg && ro);
    /* Set information needs SET_INFORMATION and an exact payload size. */
    ASSERT_STATUS(nt_svc_NtSetInformationDebugObject(ro, DebugObjectKillProcessOnExitInformation,
                                                     &on, sizeof(on), NULL), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(nt_svc_NtSetInformationDebugObject(dbg, DebugObjectKillProcessOnExitInformation,
                                                     &wide, sizeof(wide) + 1, NULL),
                  STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(nt_svc_NtSetInformationDebugObject(dbg, (DEBUGOBJECTINFOCLASS)99,
                                                     &on, sizeof(on), NULL), STATUS_INVALID_INFO_CLASS);
    ASSERT_STATUS(nt_svc_NtSetInformationDebugObject(dbg, DebugObjectKillProcessOnExitInformation,
                                                     &on, sizeof(on), NULL), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, dbg), STATUS_SUCCESS);
    ObCloseHandle(g_cur, dbg);
    ASSERT(p->terminated);
    ObCloseHandle(g_cur, ro);
    PASS();
}

/* Multi-thread wait: workers block in NtWaitForDebugEvent; the host posts one
 * event per worker. Each event is dequeued exactly once. */
#define MT_WORKERS 3

struct worker {
    HANDLE dbg;
    struct nt_thread *nt;
    NTSTATUS st;
    DBGUI_WAIT_STATE_CHANGE out;
};

static void *worker_main(void *arg)
{
    struct worker *w = (struct worker *)arg;
    if (NT_SUCCESS(nt_thread_create(&w->nt))) {
        nt_set_current_thread(w->nt);
        kshim_set_current_process(g_cur);
        w->st = nt_svc_NtWaitForDebugEvent(w->dbg, FALSE, NULL, &w->out);
        nt_set_current_thread(NULL);
        nt_thread_destroy(w->nt);
    } else {
        w->st = STATUS_NO_MEMORY;
    }
    return NULL;
}

TEST(multithread_wait_dequeue)
{
    struct nt_eprocess *p = new_proc(5000);
    HANDLE ph = proc_handle(p, PROCESS_SUSPEND_RESUME);
    HANDLE dbg = new_debug(0, DEBUG_OBJECT_ALL_ACCESS);
    DBGUI_WAIT_STATE_CHANGE drain;
    struct worker ws[MT_WORKERS];
    pthread_t th[MT_WORKERS];
    ULONG seen[MT_WORKERS];
    int i, j;
    ASSERT(p && ph && dbg);
    ASSERT_STATUS(nt_svc_NtDebugActiveProcess(ph, dbg), STATUS_SUCCESS);
    ASSERT_STATUS(poll_event(dbg, &drain), STATUS_SUCCESS); /* create-process */
    ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &drain.AppClientId, DBG_CONTINUE), STATUS_SUCCESS);

    for (i = 0; i < MT_WORKERS; i++) {
        ws[i].dbg = dbg;
        ws[i].nt = NULL;
        ws[i].st = STATUS_PENDING;
        pthread_create(&th[i], NULL, worker_main, &ws[i]);
    }
    /* Post one exit-thread event per worker, tids 6001..6003. */
    for (i = 0; i < MT_WORKERS; i++) {
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        nt_debug_post_event(p, DbgExitThreadStateChange, 6001 + i, (NTSTATUS)i);
    }
    for (i = 0; i < MT_WORKERS; i++) pthread_join(th[i], NULL);

    for (i = 0; i < MT_WORKERS; i++) {
        ASSERT_STATUS(ws[i].st, STATUS_SUCCESS);
        ASSERT(ws[i].out.NewState == DbgExitThreadStateChange);
        seen[i] = (ULONG)(ULONG_PTR)ws[i].out.AppClientId.UniqueThread;
        ASSERT(seen[i] >= 6001 && seen[i] <= 6003);
    }
    /* Each tid dequeued exactly once. */
    for (i = 0; i < MT_WORKERS; i++)
        for (j = i + 1; j < MT_WORKERS; j++)
            ASSERT(seen[i] != seen[j]);
    /* Every dequeued event needs its own continue. */
    for (i = 0; i < MT_WORKERS; i++) {
        CLIENT_ID cid = ws[i].out.AppClientId;
        ASSERT_STATUS(nt_svc_NtDebugContinue(dbg, &cid, DBG_CONTINUE), STATUS_SUCCESS);
    }
    ASSERT_STATUS(poll_event(dbg, &drain), STATUS_TIMEOUT);
    ASSERT_STATUS(nt_svc_NtRemoveProcessDebug(ph, dbg), STATUS_SUCCESS);
    ObCloseHandle(g_cur, dbg);
    PASS();
}

int main(void)
{
    int i;
    printf("debugobj tests starting\n");
    if (ObInitObjectManager() != STATUS_SUCCESS) {
        fprintf(stderr, "ObInitObjectManager failed\n");
        return 1;
    }
    g_cur = new_proc(3000);
    if (!g_cur) return 1;
    kshim_set_current_process(g_cur);
    if (nt_thread_create(&g_main_nt) != STATUS_SUCCESS) return 1;
    nt_set_current_thread(g_main_nt);

    RUN(create_flags_and_null);
    RUN(access_masks);
    RUN(attach_needs_suspend_resume);
    RUN(self_debug_refused);
    RUN(one_debugger_per_process);
    RUN(attach_event_and_continue);
    RUN(relative_timeout_and_apc);
    RUN(thread_and_process_exit_events);
    RUN(remove_purges_queued_events);
    RUN(close_detaches_without_kill);
    RUN(kill_on_close_flag);
    RUN(kill_on_exit_information);
    RUN(multithread_wait_dequeue);

    printf("debugobj tests: %d passed, %d failed\n", g_pass, g_fail);

    /* Destroy member processes first, then the host (closes its handles),
     * then the object manager. */
    nt_set_current_thread(NULL);
    nt_thread_destroy(g_main_nt);
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
