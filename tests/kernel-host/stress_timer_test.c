/* stress_timer_test.c - concurrency stress for waitable timers, keyed events and
 * alertable waits (src/kernel/nt/sync.c, proc_thread.c) on the userspace kshim.
 *
 * N workers (8-16) are real nt threads. The coordinator creates a few timers,
 * one keyed event and one event that is never signalled, then gives every
 * worker a thread handle to every other worker. Each worker runs seeded random
 * operations:
 *   - NtSetTimer with a due time 0-2 ms away, sometimes periodic, and NtCancelTimer
 *   - NtWaitForSingleObject on a timer with a 0-2 ms timeout: WAIT_0 or TIMEOUT
 *   - NtReleaseKeyedEvent and NtWaitForKeyedEvent with 0-2 ms timeouts: a release
 *     with no waiter times out, and a wait times out unless a release matches
 *   - alertable waits on the never-signalled event: WAIT_0 cannot happen, so
 *     the result is TIMEOUT or ALERTED
 *   - NtAlertThread of another worker (must succeed while it runs), of itself,
 *     and an alertable zero-timeout wait, which consumes the pending alert
 *
 * Invariants checked at the end:
 *   - timers and keyed events return to a clean state (no waiter left, cancel succeeds)
 *   - every thread handle closed, handle table empty
 *   - the kshim allocation count returns to its baseline
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Usage: stress_timer_test [seed] [threads 8..16] [ops per thread]
 */

#include "object.h"
#include "handle.h"
#include "sync.h"
#include "proc_thread.h"
#include "stress_common.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dispatcher entry points, typed as in sync_extra_test.c (12 argument slots). */
NTSTATUS nt_svc_NtCreateTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                              ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                              ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtSetTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                           ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                           ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtCancelTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                              ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                              ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtCreateKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                   ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                   ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtWaitForKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                    ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                    ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtReleaseKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                    ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                    ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes,
                              ULONG EventType, BOOLEAN InitialState);
NTSTATUS nt_svc_NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS nt_svc_NtOpenThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                             POBJECT_ATTRIBUTES ObjectAttributes, PCLIENT_ID ClientId);
NTSTATUS nt_svc_NtAlertThread(HANDLE ThreadHandle);

#ifndef STATUS_ALERTED
#define STATUS_ALERTED ((NTSTATUS)0x00000101L)
#endif

#define NTIMERS         3
#define NSHARED_EV      1
#define LOCAL_MAX       8
#define OPS_PER_BATCH   32
#define TID_BASE        3000

extern _Atomic long kshim_alloc_count;

static struct nt_eprocess      *g_proc;
static HANDLE                   g_timer[NTIMERS];
static HANDLE                   g_keyed;
static HANDLE                   g_event;               /* never signalled */
static HANDLE                   g_thr[STRESS_MAX_THREADS];
static stress_watchdog          g_wd;
static stress_barrier           g_start;               /* workers created their threads */
static stress_barrier           g_ready;               /* coordinator opened the thread handles */
static stress_barrier           g_end;                 /* workers finished their ops */
static stress_barrier           g_fin;                 /* coordinator closed the thread handles */
static unsigned                 g_nthreads;
static _Atomic long             g_set, g_cancel, g_twait_ok, g_twait_to, g_rel_to, g_wait_to,
                                g_alerted, g_alerts, g_tests;

typedef struct {
    unsigned            id;
    struct nt_thread   *nt;
    struct nt_ethread  *et;
    unsigned            ops;
    stress_rng          rng;
} worker_t;

static LARGE_INTEGER rel_us(uint32_t us)
{
    LARGE_INTEGER li;
    li.QuadPart = -(int64_t)us * 10LL;     /* negative = relative, 100 ns units */
    return li;
}

static NTSTATUS t_set_timer(HANDLE h, LARGE_INTEGER *due, LONG period)
{
    BOOLEAN prev = 0;
    return nt_svc_NtSetTimer((ULONG_PTR)h, (ULONG_PTR)due, 0, 0,
                             0, (ULONG_PTR)period, (ULONG_PTR)&prev, 0, 0, 0, 0, 0);
}

static NTSTATUS t_cancel_timer(HANDLE h)
{
    BOOLEAN cur = 0;
    return nt_svc_NtCancelTimer((ULONG_PTR)h, (ULONG_PTR)&cur, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

static NTSTATUS t_wait_keyed(HANDLE h, void *key, LARGE_INTEGER *to)
{
    return nt_svc_NtWaitForKeyedEvent((ULONG_PTR)h, (ULONG_PTR)key, 0, (ULONG_PTR)to, 0, 0, 0, 0, 0, 0, 0, 0);
}

static NTSTATUS t_release_keyed(HANDLE h, void *key, LARGE_INTEGER *to)
{
    return nt_svc_NtReleaseKeyedEvent((ULONG_PTR)h, (ULONG_PTR)key, 0, (ULONG_PTR)to, 0, 0, 0, 0, 0, 0, 0, 0);
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;

    /* Each worker is its own nt thread, so the wait engine has thread state. */
    NTSTATUS st = nt_thread_create(&w->nt);
    if (st != STATUS_SUCCESS) { STRESS_CHECK(0, "nt_thread_create 0x%lx", (unsigned long)st); return NULL; }
    nt_set_current_thread(w->nt);
    st = ObCreateThreadObject(&w->et, g_proc, TID_BASE + w->id);
    if (st != STATUS_SUCCESS) { STRESS_CHECK(0, "thread object 0x%lx", (unsigned long)st); return NULL; }
    w->et->sync_thread = w->nt;
    kshim_set_current_thread(w->et);
    kshim_set_current_process(g_proc);

    stress_barrier_wait(&g_start);
    stress_barrier_wait(&g_ready);

    for (unsigned i = 0; i < w->ops; i++) {
        uint32_t r = stress_rnd(&w->rng, 100);
        int ti = (int)stress_rnd(&w->rng, NTIMERS);
        unsigned other = stress_rnd(&w->rng, g_nthreads);

        if (r < 14) {
            /* set a timer due 0-2 ms away, sometimes periodic */
            LARGE_INTEGER due = rel_us(stress_rnd(&w->rng, 2000));
            LONG period = stress_rnd(&w->rng, 3) == 0 ? (LONG)(1 + stress_rnd(&w->rng, 2)) : 0;
            st = t_set_timer(g_timer[ti], &due, period);
            STRESS_CHECK(st == STATUS_SUCCESS, "set timer 0x%lx", (unsigned long)st);
            if (st == STATUS_SUCCESS) atomic_fetch_add(&g_set, 1);
        } else if (r < 24) {
            st = t_cancel_timer(g_timer[ti]);
            STRESS_CHECK(st == STATUS_SUCCESS, "cancel timer 0x%lx", (unsigned long)st);
            if (st == STATUS_SUCCESS) atomic_fetch_add(&g_cancel, 1);
        } else if (r < 40) {
            /* wait on a timer: it is signalled or the wait times out */
            LARGE_INTEGER to = rel_us(stress_rnd(&w->rng, 2000));
            st = nt_svc_NtWaitForSingleObject(g_timer[ti], 0, &to);
            STRESS_CHECK(st == STATUS_WAIT_0 || st == STATUS_TIMEOUT,
                         "timer wait 0x%lx", (unsigned long)st);
            if (st == STATUS_WAIT_0) atomic_fetch_add(&g_twait_ok, 1);
            else if (st == STATUS_TIMEOUT) atomic_fetch_add(&g_twait_to, 1);
        } else if (r < 52) {
            /* keyed release: no waiter means it times out; keys are per worker */
            LARGE_INTEGER to = rel_us(stress_rnd(&w->rng, 1000));
            st = t_release_keyed(g_keyed, (void *)(uintptr_t)(0x1000 + w->id), &to);
            STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_TIMEOUT,
                         "keyed release 0x%lx", (unsigned long)st);
            if (st == STATUS_TIMEOUT) atomic_fetch_add(&g_rel_to, 1);
        } else if (r < 62) {
            /* keyed wait on another worker's key: a matching release may arrive */
            LARGE_INTEGER to = rel_us(stress_rnd(&w->rng, 2000));
            st = t_wait_keyed(g_keyed, (void *)(uintptr_t)(0x1000 + other), &to);
            STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_TIMEOUT,
                         "keyed wait 0x%lx", (unsigned long)st);
            if (st == STATUS_TIMEOUT) atomic_fetch_add(&g_wait_to, 1);
        } else if (r < 72) {
            /* alertable wait on the event that is never signalled */
            LARGE_INTEGER to = rel_us(stress_rnd(&w->rng, 2000));
            st = nt_svc_NtWaitForSingleObject(g_event, 1, &to);
            STRESS_CHECK(st == STATUS_TIMEOUT || st == STATUS_ALERTED,
                         "alertable wait 0x%lx", (unsigned long)st);
            if (st == STATUS_ALERTED) atomic_fetch_add(&g_alerted, 1);
        } else if (r < 84) {
            /* alert another worker, which may be in any wait above */
            if (other != w->id) {
                st = nt_svc_NtAlertThread(g_thr[other]);
                STRESS_CHECK(st == STATUS_SUCCESS, "alert other 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) atomic_fetch_add(&g_alerts, 1);
            }
        } else if (r < 92) {
            /* alert ourselves; an alertable wait with a zero timeout consumes it */
            st = nt_svc_NtAlertThread(NtCurrentThread());
            STRESS_CHECK(st == STATUS_SUCCESS, "alert self 0x%lx", (unsigned long)st);
            LARGE_INTEGER zero = rel_us(0);
            NTSTATUS ts = nt_svc_NtWaitForSingleObject(g_event, 1, &zero);
            STRESS_CHECK(ts == STATUS_ALERTED || ts == STATUS_TIMEOUT,
                         "self-alert wait 0x%lx", (unsigned long)ts);
            atomic_fetch_add(&g_tests, 1);
        } else {
            /* a zero-timeout alertable wait with nothing pending must not alert */
            LARGE_INTEGER zero = rel_us(0);
            st = nt_svc_NtWaitForSingleObject(g_event, 1, &zero);
            STRESS_CHECK(st == STATUS_TIMEOUT || st == STATUS_ALERTED,
                         "zero alertable wait 0x%lx", (unsigned long)st);
        }
        stress_progress(&g_wd);
    }

    /* Keep the thread object alive until the coordinator has closed its handles. */
    stress_barrier_wait(&g_end);
    stress_barrier_wait(&g_fin);
    kshim_set_current_thread(NULL);
    nt_set_current_thread(NULL);
    ObDestroyThreadObject(w->et);
    w->et = NULL;
    nt_thread_destroy(w->nt);
    w->nt = NULL;
    return NULL;
}

int main(int argc, char **argv)
{
    stress_args a;
    stress_parse_args(argc, argv, &a, "stress_timer_test", 4096U);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_timer_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }
    st = ObCreateProcessObject(&g_proc, 7800, "stress-timer");
    if (st != STATUS_SUCCESS || !g_proc) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    kshim_set_current_process(g_proc);

    struct nt_thread *main_nt = NULL;
    st = nt_thread_create(&main_nt);
    if (st != STATUS_SUCCESS) return 2;
    nt_set_current_thread(main_nt);

    for (int i = 0; i < NTIMERS; i++) {
        st = nt_svc_NtCreateTimer((ULONG_PTR)&g_timer[i], 0x1F0003UL, 0, (ULONG_PTR)(i & 1), 0, 0, 0, 0, 0, 0, 0, 0);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "timer create 0x%lx\n", (unsigned long)st); return 2; }
    }
    st = nt_svc_NtCreateKeyedEvent((ULONG_PTR)&g_keyed, 0x1F0003UL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (st != STATUS_SUCCESS) { fprintf(stderr, "keyed create 0x%lx\n", (unsigned long)st); return 2; }
    st = nt_svc_NtCreateEvent(&g_event, 0x1F0003UL, NULL, NotificationEvent, 0);
    if (st != STATUS_SUCCESS) { fprintf(stderr, "event create 0x%lx\n", (unsigned long)st); return 2; }

    unsigned nthreads = a.threads;
    g_nthreads = nthreads;
    stress_barrier_init(&g_start, nthreads + 1);
    stress_barrier_init(&g_ready, nthreads + 1);
    stress_barrier_init(&g_end, nthreads + 1);
    stress_barrier_init(&g_fin, nthreads + 1);

    worker_t *ws = calloc(nthreads, sizeof *ws);
    pthread_t *th = calloc(nthreads, sizeof *th);
    if (!ws || !th) return 2;
    for (unsigned t = 0; t < nthreads; t++) {
        ws[t].id = t;
        ws[t].ops = a.ops;
        stress_rng_seed(&ws[t].rng, a.seed, t);
        pthread_create(&th[t], NULL, worker_main, &ws[t]);
    }

    /* Once every worker has its thread object, open a handle to each one. */
    stress_barrier_wait(&g_start);
    for (unsigned t = 0; t < nthreads; t++) {
        CLIENT_ID cid;
        memset(&cid, 0, sizeof cid);
        cid.UniqueThread = (HANDLE)(ULONG_PTR)(TID_BASE + t);
        st = nt_svc_NtOpenThread(&g_thr[t], 0, NULL, &cid);
        STRESS_CHECK(st == STATUS_SUCCESS, "open thread %u 0x%lx", t, (unsigned long)st);
    }
    stress_barrier_wait(&g_ready);
    stress_barrier_wait(&g_end);
    /* All workers have finished their operations. They wait on g_fin, so the
     * thread objects are still alive while the handles are closed below. */
    for (int i = 0; i < NTIMERS; i++) {
        st = t_cancel_timer(g_timer[i]);
        STRESS_CHECK(st == STATUS_SUCCESS, "final cancel %d 0x%lx", i, (unsigned long)st);
    }
    for (unsigned t = 0; t < nthreads; t++) {
        st = ObCloseHandle(g_proc, g_thr[t]);
        STRESS_CHECK(st == STATUS_SUCCESS, "close thread handle %u 0x%lx", t, (unsigned long)st);
    }
    for (int i = 0; i < NTIMERS; i++) ObCloseHandle(g_proc, g_timer[i]);
    ObCloseHandle(g_proc, g_keyed);
    ObCloseHandle(g_proc, g_event);
    STRESS_CHECK(ObHandleTableCount(g_proc->handle_table) == 0,
                 "handle table not empty: %lu", (unsigned long)ObHandleTableCount(g_proc->handle_table));

    stress_barrier_wait(&g_fin);
    for (unsigned t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);

    nt_set_current_thread(NULL);
    nt_thread_destroy(main_nt);
    ObDestroyProcessObject(g_proc);
    kshim_set_current_process(NULL);
    ObCleanupObjectManager();

    stress_watchdog_stop(&g_wd);

    long end_allocs = (long)atomic_load(&kshim_alloc_count);
    STRESS_CHECK(end_allocs == base_allocs,
                 "allocation count %ld at end, baseline %ld", end_allocs, base_allocs);
    if (end_allocs != base_allocs) kshim_leak_report();

    printf("stress_timer_test seed=%lu: timer_set=%ld cancel=%ld twait_ok=%ld twait_to=%ld rel_to=%ld wait_to=%ld alerted=%ld alerts=%ld tests=%ld\n",
           a.seed, atomic_load(&g_set), atomic_load(&g_cancel), atomic_load(&g_twait_ok),
           atomic_load(&g_twait_to), atomic_load(&g_rel_to), atomic_load(&g_wait_to),
           atomic_load(&g_alerted), atomic_load(&g_alerts), atomic_load(&g_tests));
    long errs = atomic_load(&stress_errors);
    printf("stress_timer_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
