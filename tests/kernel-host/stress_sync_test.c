/* stress_sync_test.c - concurrency stress for the dispatcher objects and their
 * syscall layer (src/kernel/nt/sync.c, nt_obj.c, nt_services.c) on the
 * userspace kshim.
 *
 * N workers (8-16) share one process handle table and a pool of events,
 * mutants and semaphores. Each worker runs seeded random operations through
 * the nt_svc_* entry points:
 *   - NtSetEvent / NtResetEvent, NtWaitForSingleObject with 0-2 ms timeouts
 *   - mutant acquire (sometimes recursive) and release by the owning thread
 *   - semaphore acquire and release of what was acquired
 *   - NtDuplicateObject of a pool object, NtClose of owned handles
 *   - named events: NtCreateEvent / NtOpenEvent under \BaseNamedObjects
 *
 * Every OPS_PER_EPOCH operations a race round runs: the coordinator makes one
 * duplicate handle and all workers call NtClose on that same value at once.
 * Exactly one NtClose must succeed.
 *
 * Invariants checked at the end:
 *   - all handles closed, the handle table is empty
 *   - no mutant is left owned, no sync object is left with extra references
 *   - the kshim allocation count returns to its baseline (no leaks)
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Link set matches i2_test (object handle nt_obj nt_services sync proc_thread
 * kshim ntstatus_kernel). Usage: stress_sync_test [seed] [threads] [ops].
 * STRESS_SYNC_SKIP=<mask> disables operation families (bits above SKIP_EVENT_SET).
 */

#include "object.h"
#include "handle.h"
#include "sync.h"
#include "stress_common.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* nt_obj / nt_services syscall entry points (userspace shim). */
NTSTATUS nt_svc_NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes,
                              ULONG EventType, BOOLEAN InitialState);
NTSTATUS nt_svc_NtOpenEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                            POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtCreateMutant(PHANDLE MutantHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN InitialOwner);
NTSTATUS nt_svc_NtCreateSemaphore(PHANDLE SemaphoreHandle, ACCESS_MASK DesiredAccess,
                                  POBJECT_ATTRIBUTES ObjectAttributes,
                                  LONG InitialCount, LONG MaximumCount);
NTSTATUS nt_svc_NtDuplicateObject(HANDLE SourceProcessHandle, HANDLE SourceHandle,
                                  HANDLE TargetProcessHandle, PHANDLE TargetHandle,
                                  ACCESS_MASK DesiredAccess, ULONG HandleAttributes, ULONG Options);
NTSTATUS nt_svc_NtClose(HANDLE Handle);
NTSTATUS nt_svc_NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS nt_svc_NtSetEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS nt_svc_NtResetEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS nt_svc_NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
NTSTATUS nt_svc_NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount);

#define NEVENTS         3
#define NMUTANTS        2
#define NSEMS           2
#define NPOOL           (NEVENTS + NMUTANTS + NSEMS)
#define NAMED           8
#define NAME_CHARS      64
#define LOCAL_MAX       24
#define OPS_PER_EPOCH   64
#define SEM_LIMIT       3

/* Minimisation switch. STRESS_SYNC_SKIP=<mask> (environment, read once at
 * start) turns off operation families so a failure can be attributed to the
 * families that remain. A skipped family draws the same random values but
 * performs no call. Bits: */
#define SKIP_EVENT_SET    0x001
#define SKIP_WAIT         0x002   /* short timed waits on events */
#define SKIP_MUTANT       0x004
#define SKIP_SEM          0x008
#define SKIP_DUP          0x010
#define SKIP_NAMED_CREATE 0x020
#define SKIP_NAMED_OPEN   0x040
#define SKIP_CLOSE        0x080
#define SKIP_OWNED_WAIT   0x100   /* zero-timeout wait on a held handle */
static unsigned g_skip;

extern _Atomic long kshim_alloc_count;

static struct nt_eprocess      *g_proc;
static HANDLE                   g_master[NPOOL];   /* pool handles held by the coordinator */
static WCHAR                    g_names[NAMED][NAME_CHARS];
static UNICODE_STRING           g_us[NAMED];
static stress_watchdog          g_wd;
static stress_barrier           g_bar;
static HANDLE                   g_race_h;
static _Atomic int              g_race_ok, g_race_other;
static _Atomic long             g_waits_ok, g_waits_to, g_mutex_cycles, g_sem_cycles, g_dups, g_named;

typedef struct {
    unsigned            id;
    struct nt_thread   *nt;
    struct nt_ethread  *et;
    HANDLE              local[LOCAL_MAX];
    int                 nlocal;
    unsigned            ops;
    unsigned            epochs;
    stress_rng          rng;
} worker_t;

static LARGE_INTEGER rel_ms(uint32_t ms)
{
    LARGE_INTEGER li;
    li.QuadPart = -(int64_t)ms * 10000LL;   /* negative = relative, 100 ns units */
    return li;
}

static void set_name(int k)
{
    char tmp[NAME_CHARS];
    snprintf(tmp, sizeof tmp, "\\BaseNamedObjects\\SyncStress%d", k);
    size_t n = strlen(tmp);
    for (size_t i = 0; i < n; i++) g_names[k][i] = (WCHAR)(unsigned char)tmp[i];
    g_names[k][n] = 0;
    g_us[k].Buffer = g_names[k];
    g_us[k].Length = (USHORT)(n * sizeof(WCHAR));
    g_us[k].MaximumLength = g_us[k].Length;
}

static void local_add(worker_t *w, HANDLE h)
{
    w->local[w->nlocal++] = h;
}

static HANDLE pool_handle(worker_t *w)
{
    return g_master[stress_rnd(&w->rng, NPOOL)];
}

/* Wait on a handle with a short random timeout; expect WAIT_0 or TIMEOUT. */
static NTSTATUS short_wait(worker_t *w, HANDLE h)
{
    LARGE_INTEGER to = rel_ms(stress_rnd(&w->rng, 3));
    NTSTATUS st = nt_svc_NtWaitForSingleObject(h, 0, &to);
    if (st == STATUS_WAIT_0) atomic_fetch_add(&g_waits_ok, 1);
    else if (st == STATUS_TIMEOUT) atomic_fetch_add(&g_waits_to, 1);
    return st;
}

/* The SKIP_* bit of the operation family that draw r selects (see worker_main). */
static unsigned family_bit(uint32_t r)
{
    if (r < 12) return SKIP_EVENT_SET;
    if (r < 22) return SKIP_EVENT_SET;
    if (r < 36) return SKIP_WAIT;
    if (r < 48) return SKIP_MUTANT;
    if (r < 56) return SKIP_SEM;
    if (r < 66) return SKIP_DUP;
    if (r < 74) return SKIP_NAMED_CREATE;
    if (r < 80) return SKIP_NAMED_OPEN;
    if (r < 90) return SKIP_CLOSE;
    return SKIP_OWNED_WAIT;
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;

    /* Each worker is its own nt thread: the wait engine needs thread-local state. */
    NTSTATUS st = nt_thread_create(&w->nt);
    if (st != STATUS_SUCCESS) { STRESS_CHECK(0, "nt_thread_create 0x%lx", (unsigned long)st); return NULL; }
    nt_set_current_thread(w->nt);
    st = ObCreateThreadObject(&w->et, g_proc, 3000 + (ULONG)w->id);
    if (st == STATUS_SUCCESS) kshim_set_current_thread(w->et);
    kshim_set_current_process(g_proc);

    for (unsigned e = 0; e < w->epochs; e++) {
        for (unsigned i = 0; i < OPS_PER_EPOCH; i++) {
            uint32_t r = stress_rnd(&w->rng, 100);
            int idx = (int)stress_rnd(&w->rng, NEVENTS);

            if (g_skip & family_bit(r)) { stress_progress(&g_wd); continue; }
            if (r < 12) {
                st = nt_svc_NtSetEvent(g_master[idx], NULL);
                STRESS_CHECK(st == STATUS_SUCCESS, "set event 0x%lx", (unsigned long)st);
            } else if (r < 22) {
                st = nt_svc_NtResetEvent(g_master[idx], NULL);
                STRESS_CHECK(st == STATUS_SUCCESS, "reset event 0x%lx", (unsigned long)st);
            } else if (r < 36) {
                st = short_wait(w, g_master[stress_rnd(&w->rng, NEVENTS)]);
                STRESS_CHECK(st == STATUS_WAIT_0 || st == STATUS_TIMEOUT,
                             "event wait 0x%lx", (unsigned long)st);
            } else if (r < 48) {
                /* mutant: acquire (maybe recursively) and release by this thread */
                HANDLE m = g_master[NEVENTS + stress_rnd(&w->rng, NMUTANTS)];
                st = short_wait(w, m);
                if (st == STATUS_WAIT_0) {
                    int depth = stress_rnd(&w->rng, 4) == 0 ? 2 : 1;
                    if (depth == 2) {
                        NTSTATUS st2 = short_wait(w, m);
                        STRESS_CHECK(st2 == STATUS_WAIT_0, "recursive mutant wait 0x%lx", (unsigned long)st2);
                        st2 = nt_svc_NtReleaseMutant(m, NULL);
                        STRESS_CHECK(st2 == STATUS_SUCCESS, "inner mutant release 0x%lx", (unsigned long)st2);
                    }
                    st = nt_svc_NtReleaseMutant(m, NULL);
                    STRESS_CHECK(st == STATUS_SUCCESS, "mutant release 0x%lx", (unsigned long)st);
                    atomic_fetch_add(&g_mutex_cycles, 1);
                }
            } else if (r < 56) {
                /* semaphore: take one and give it back */
                HANDLE s = g_master[NEVENTS + NMUTANTS + stress_rnd(&w->rng, NSEMS)];
                st = short_wait(w, s);
                if (st == STATUS_WAIT_0) {
                    st = nt_svc_NtReleaseSemaphore(s, 1, NULL);
                    STRESS_CHECK(st == STATUS_SUCCESS, "sem release 0x%lx", (unsigned long)st);
                    atomic_fetch_add(&g_sem_cycles, 1);
                }
            } else if (r < 66) {
                /* duplicate a pool object into this process */
                if (w->nlocal < LOCAL_MAX) {
                    HANDLE dst = NULL;
                    st = nt_svc_NtDuplicateObject(NtCurrentProcess(), pool_handle(w),
                                                  NtCurrentProcess(), &dst, 0, 0,
                                                  DUPLICATE_SAME_ACCESS);
                    STRESS_CHECK(st == STATUS_SUCCESS, "dup 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) { local_add(w, dst); atomic_fetch_add(&g_dups, 1); }
                }
            } else if (r < 74) {
                /* named event: create (or open existing) and keep the handle */
                if (w->nlocal < LOCAL_MAX) {
                    int k = (int)stress_rnd(&w->rng, NAMED);
                    OBJECT_ATTRIBUTES oa;
                    memset(&oa, 0, sizeof oa);
                    oa.Length = sizeof oa;
                    oa.ObjectName = &g_us[k];
                    HANDLE h = NULL;
                    st = nt_svc_NtCreateEvent(&h, 0x1FFFFF, &oa, NotificationEvent, 0);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_COLLISION,
                                 "named create 0x%lx", (unsigned long)st);
                    /* On NAME_COLLISION the call still returns a handle to the existing event. */
                    if (st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_COLLISION) {
                        local_add(w, h);
                        atomic_fetch_add(&g_named, 1);
                    }
                }
            } else if (r < 80) {
                /* open an existing named event, may not exist yet */
                if (w->nlocal < LOCAL_MAX) {
                    int k = (int)stress_rnd(&w->rng, NAMED);
                    OBJECT_ATTRIBUTES oa;
                    memset(&oa, 0, sizeof oa);
                    oa.Length = sizeof oa;
                    oa.ObjectName = &g_us[k];
                    HANDLE h = NULL;
                    st = nt_svc_NtOpenEvent(&h, 0x1FFFFF, &oa);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND,
                                 "named open 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) local_add(w, h);
                }
            } else if (r < 90) {
                if (w->nlocal > 0) {
                    unsigned j = stress_rnd(&w->rng, (uint32_t)w->nlocal);
                    HANDLE h = w->local[j];
                    w->local[j] = w->local[--w->nlocal];
                    st = nt_svc_NtClose(h);
                    STRESS_CHECK(st == STATUS_SUCCESS, "close 0x%lx", (unsigned long)st);
                }
            } else {
                /* validity: a wait on one of our own handles, zero timeout */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    LARGE_INTEGER zero = rel_ms(0);
                    st = nt_svc_NtWaitForSingleObject(h, 0, &zero);
                    STRESS_CHECK(st == STATUS_WAIT_0 || st == STATUS_TIMEOUT,
                                 "owned wait 0x%lx", (unsigned long)st);
                }
            }
            stress_progress(&g_wd);
        }

        /* Race round: NtClose of the same handle from every worker at once. */
        stress_barrier_wait(&g_bar);     /* A: random phase done */
        stress_barrier_wait(&g_bar);     /* B: race handle published */
        st = nt_svc_NtClose(g_race_h);
        if (st == STATUS_SUCCESS) atomic_fetch_add(&g_race_ok, 1);
        else if (st == STATUS_INVALID_HANDLE) atomic_fetch_add(&g_race_other, 1);
        else STRESS_CHECK(0, "race close unexpected 0x%lx", (unsigned long)st);
        stress_barrier_wait(&g_bar);     /* C: race round done */
        stress_progress(&g_wd);
    }

    /* Drain this worker's handles. */
    for (int i = 0; i < w->nlocal; i++) {
        st = nt_svc_NtClose(w->local[i]);
        STRESS_CHECK(st == STATUS_SUCCESS, "worker drain close 0x%lx", (unsigned long)st);
    }
    w->nlocal = 0;
    if (w->et) { ObDestroyThreadObject(w->et); w->et = NULL; }
    kshim_set_current_thread(NULL);
    nt_set_current_thread(NULL);
    nt_thread_destroy(w->nt);
    w->nt = NULL;
    return NULL;
}

/* On a failed drain, list the surviving handles by object type so the leak
 * can be attributed. Walks the table through ObHandleIsValid and the public
 * reference call only. */
static void dump_leftover_handles(void)
{
    ULONG high = g_proc->handle_table->high_water;
    int shown = 0;
    for (ULONG idx = 1; idx < high; idx++) {
        HANDLE h = (HANDLE)(ULONG_PTR)((ULONG_PTR)idx << 2);
        if (!ObHandleIsValid(g_proc, h)) continue;
        struct nt_object_header *o = NULL;
        ACCESS_MASK ga = 0;
        NTSTATUS st = ObReferenceObjectByHandle(g_proc, h, NULL, KernelMode, 0, &o, &ga);
        if (st == STATUS_SUCCESS && o) {
            if (shown < 10)
                fprintf(stderr, "LEFTOVER handle=%p type=%s handle_count=%lld ref_count=%lld\n",
                        (void *)h, o->type ? o->type->name : "?",
                        (long long)atomic64_read(&o->handle_count),
                        (long long)atomic64_read(&o->ref_count));
            ObDereferenceObject(o);
            shown++;
        }
    }
    fprintf(stderr, "LEFTOVER total=%d\n", shown);
}

int main(int argc, char **argv)
{
    stress_args a;
    stress_parse_args(argc, argv, &a, "stress_sync_test", 4096U);
    const char *skip = getenv("STRESS_SYNC_SKIP");
    g_skip = skip ? (unsigned)strtoul(skip, NULL, 0) : 0U;
    if (g_skip) printf("stress_sync_test: STRESS_SYNC_SKIP=0x%x (families disabled)\n", g_skip);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_sync_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }
    st = ObCreateProcessObject(&g_proc, 7200, "stress-sync");
    if (st != STATUS_SUCCESS || !g_proc) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    kshim_set_current_process(g_proc);

    /* Coordinator needs a thread context for the object-type call paths. */
    struct nt_thread *main_nt = NULL;
    st = nt_thread_create(&main_nt);
    if (st != STATUS_SUCCESS) return 2;
    nt_set_current_thread(main_nt);

    for (int k = 0; k < NAMED; k++) set_name(k);

    for (int i = 0; i < NEVENTS; i++) {
        st = nt_svc_NtCreateEvent(&g_master[i], 0x1FFFFF, NULL,
                                  (i & 1) ? SynchronizationEvent : NotificationEvent, 0);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "event create 0x%lx\n", (unsigned long)st); return 2; }
    }
    for (int i = 0; i < NMUTANTS; i++) {
        st = nt_svc_NtCreateMutant(&g_master[NEVENTS + i], 0x1FFFFF, NULL, 0);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "mutant create 0x%lx\n", (unsigned long)st); return 2; }
    }
    for (int i = 0; i < NSEMS; i++) {
        st = nt_svc_NtCreateSemaphore(&g_master[NEVENTS + NMUTANTS + i], 0x1FFFFF, NULL,
                                      SEM_LIMIT, SEM_LIMIT);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "semaphore create 0x%lx\n", (unsigned long)st); return 2; }
    }

    unsigned nthreads = a.threads;
    unsigned epochs = a.ops / OPS_PER_EPOCH;
    stress_barrier_init(&g_bar, nthreads + 1);

    worker_t *ws = calloc(nthreads, sizeof *ws);
    pthread_t *th = calloc(nthreads, sizeof *th);
    if (!ws || !th) return 2;
    for (unsigned t = 0; t < nthreads; t++) {
        ws[t].id = t;
        ws[t].ops = epochs * OPS_PER_EPOCH;
        ws[t].epochs = epochs;
        stress_rng_seed(&ws[t].rng, a.seed, t);
        pthread_create(&th[t], NULL, worker_main, &ws[t]);
    }

    for (unsigned e = 0; e < epochs; e++) {
        stress_barrier_wait(&g_bar);     /* A */
        st = nt_svc_NtDuplicateObject(NtCurrentProcess(), g_master[e % NPOOL],
                                      NtCurrentProcess(), &g_race_h, 0, 0,
                                      DUPLICATE_SAME_ACCESS);
        STRESS_CHECK(st == STATUS_SUCCESS, "race dup 0x%lx", (unsigned long)st);
        g_race_ok = 0;
        g_race_other = 0;
        stress_barrier_wait(&g_bar);     /* B */
        stress_barrier_wait(&g_bar);     /* C */
        STRESS_CHECK(atomic_load(&g_race_ok) == 1,
                     "epoch %u: %d NtClose calls succeeded (want exactly 1)",
                     e, atomic_load(&g_race_ok));
        STRESS_CHECK(atomic_load(&g_race_ok) + atomic_load(&g_race_other) == (int)nthreads,
                     "epoch %u: race results do not add up", e);
    }

    for (unsigned t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);

    /* Drain: close the pool handles, then remove the named events. */
    for (int i = 0; i < NPOOL; i++) {
        st = nt_svc_NtClose(g_master[i]);
        STRESS_CHECK(st == STATUS_SUCCESS, "pool close 0x%lx", (unsigned long)st);
    }
    for (int k = 0; k < NAMED; k++) {
        struct nt_object_header *o = NULL;
        st = ObReferenceObjectByName(&g_us[k], NULL, KernelMode, 0, &o);
        if (st == STATUS_SUCCESS && o) {
            ObMakeTemporaryObject(o);
            ObDereferenceObject(o);
        }
    }
    for (int k = 0; k < NAMED; k++) {
        struct nt_object_header *o = NULL;
        st = ObReferenceObjectByName(&g_us[k], NULL, KernelMode, 0, &o);
        STRESS_CHECK(st == STATUS_OBJECT_NAME_NOT_FOUND,
                     "named %d still present after drain (0x%lx)", k, (unsigned long)st);
        if (st == STATUS_SUCCESS && o) ObDereferenceObject(o);
    }
    if (ObHandleTableCount(g_proc->handle_table) != 0) dump_leftover_handles();
    STRESS_CHECK(ObHandleTableCount(g_proc->handle_table) == 0,
                 "handle table not empty: %lu", (unsigned long)ObHandleTableCount(g_proc->handle_table));

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

    printf("stress_sync_test seed=%lu: waits_ok=%ld waits_timeout=%ld mutex_cycles=%ld sem_cycles=%ld dups=%ld named=%ld\n",
           a.seed, atomic_load(&g_waits_ok), atomic_load(&g_waits_to),
           atomic_load(&g_mutex_cycles), atomic_load(&g_sem_cycles),
           atomic_load(&g_dups), atomic_load(&g_named));
    long errs = atomic_load(&stress_errors);
    printf("stress_sync_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
