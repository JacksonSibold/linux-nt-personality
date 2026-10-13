/* stress_handle_test.c - concurrency stress for the NT handle table
 * (src/kernel/nt/handle.c) on the userspace kshim.
 *
 * N workers (8-16) share two process handle tables and a pool of test
 * objects. Each worker runs seeded random operations: create, close, duplicate
 * (same and cross process, with and without DUPLICATE_CLOSE_SOURCE), reference
 * and dereference through a handle, and validity checks on handles it owns.
 *
 * Every OPS_PER_EPOCH operations the workers and the coordinator (main thread)
 * meet at a barrier, and one race round runs: the coordinator creates one
 * handle, then all workers call ObCloseHandle on that same value at once.
 * Exactly one close must succeed and the rest must return STATUS_INVALID_HANDLE.
 *
 * Invariants checked at the end:
 *   - every handle is closed and both tables are empty
 *   - every pool object is back to ref_count 1 and handle_count 0
 *   - the kshim allocation count returns to its baseline (no leaks)
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Usage: stress_handle_test [seed] [threads 8..16] [ops per thread]
 * Set KSHIM_POISON=1 in the environment to poison allocations.
 */

#include "object.h"
#include "handle.h"
#include "stress_common.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <stdio.h>
#include <stdlib.h>

#define POOL_OBJS       64
#define NPROC           2
#define LOCAL_MAX       32
#define OPS_PER_EPOCH   64

extern _Atomic long kshim_alloc_count;

struct local_h {
    HANDLE h;
    int    p;           /* index into g_proc[] whose table holds h */
};

typedef struct {
    int            id;
    unsigned       ops;
    stress_rng     rng;
    struct local_h local[LOCAL_MAX];
    int            nlocal;
} worker_t;

static struct nt_eprocess       *g_proc[NPROC];
static struct nt_object_header  *g_pool[POOL_OBJS];
static stress_barrier            g_bar;
static stress_watchdog           g_wd;
static HANDLE                    g_race_h;
static _Atomic int               g_race_ok;
static _Atomic int               g_race_stale;
static _Atomic long              g_creates, g_closes, g_dups, g_refs;

static void local_add(worker_t *w, HANDLE h, int p)
{
    w->local[w->nlocal].h = h;
    w->local[w->nlocal].p = p;
    w->nlocal++;
}

static struct local_h local_take(worker_t *w, unsigned idx)
{
    struct local_h v = w->local[idx];
    w->local[idx] = w->local[--w->nlocal];
    return v;
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    unsigned epochs = w->ops / OPS_PER_EPOCH;
    kshim_set_current_process(g_proc[0]);

    for (unsigned e = 0; e < epochs; e++) {
        /* Random phase: each worker does OPS_PER_EPOCH operations. */
        for (unsigned i = 0; i < OPS_PER_EPOCH; i++) {
            uint32_t r = stress_rnd(&w->rng, 100);
            if (r < 25 || w->nlocal == 0) {
                /* create */
                if (w->nlocal >= LOCAL_MAX) continue;
                int p = (int)stress_rnd(&w->rng, NPROC);
                struct nt_object_header *o = g_pool[stress_rnd(&w->rng, POOL_OBJS)];
                HANDLE h = NULL;
                NTSTATUS st = ObCreateHandle(g_proc[p], o,
                                             (ACCESS_MASK)(1 + stress_rnd(&w->rng, 15)),
                                             0, &h);
                STRESS_CHECK(st == STATUS_SUCCESS, "create status 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) {
                    local_add(w, h, p);
                    atomic_fetch_add(&g_creates, 1);
                }
            } else if (r < 45) {
                /* close one of ours; it must succeed exactly once */
                struct local_h v = local_take(w, stress_rnd(&w->rng, (uint32_t)w->nlocal));
                NTSTATUS st = ObCloseHandle(g_proc[v.p], v.h);
                STRESS_CHECK(st == STATUS_SUCCESS, "close status 0x%lx", (unsigned long)st);
                atomic_fetch_add(&g_closes, 1);
            } else if (r < 62) {
                /* duplicate to a random table, possibly moving the source */
                if (w->nlocal >= LOCAL_MAX) continue;
                unsigned idx = stress_rnd(&w->rng, (uint32_t)w->nlocal);
                struct local_h src = w->local[idx];
                int q = (int)stress_rnd(&w->rng, NPROC);
                BOOLEAN move = stress_rnd(&w->rng, 8) == 0;
                HANDLE dst = NULL;
                NTSTATUS st = NtDuplicateObject(g_proc[src.p], src.h, g_proc[q], &dst,
                                                0, 0,
                                                move ? DUPLICATE_CLOSE_SOURCE : DUPLICATE_SAME_ACCESS);
                STRESS_CHECK(st == STATUS_SUCCESS, "dup status 0x%lx move=%d", (unsigned long)st, (int)move);
                if (st == STATUS_SUCCESS) {
                    atomic_fetch_add(&g_dups, 1);
                    if (move) {
                        w->local[idx].h = dst;
                        w->local[idx].p = q;
                    } else {
                        local_add(w, dst, q);
                    }
                }
            } else if (r < 80) {
                /* reference and dereference through a handle we own */
                struct local_h v = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                struct nt_object_header *o = NULL;
                ACCESS_MASK ga = 0;
                NTSTATUS st = ObReferenceObjectByHandle(g_proc[v.p], v.h, NULL, KernelMode,
                                                        0, &o, &ga);
                STRESS_CHECK(st == STATUS_SUCCESS && o != NULL, "ref status 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS && o) {
                    ObDereferenceObject(o);
                    atomic_fetch_add(&g_refs, 1);
                }
            } else {
                /* validity of a handle we own (nobody else can close it) */
                struct local_h v = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                STRESS_CHECK(ObHandleIsValid(g_proc[v.p], v.h), "owned handle %p invalid", (void *)v.h);
            }
            stress_progress(&g_wd);
        }

        /* Race round: coordinator creates g_race_h between barriers A and B. */
        stress_barrier_wait(&g_bar);     /* A: random phase done */
        stress_barrier_wait(&g_bar);     /* B: g_race_h published */
        {
            struct nt_object_header *o = NULL;
            ACCESS_MASK ga = 0;
            /* Some losers look the handle up first; a success must be dereferenced. */
            NTSTATUS sref = ObReferenceObjectByHandle(g_proc[0], g_race_h, NULL, KernelMode, 0, &o, &ga);
            if (sref == STATUS_SUCCESS && o) ObDereferenceObject(o);
            NTSTATUS st = ObCloseHandle(g_proc[0], g_race_h);
            if (st == STATUS_SUCCESS) atomic_fetch_add(&g_race_ok, 1);
            else if (st == STATUS_INVALID_HANDLE) atomic_fetch_add(&g_race_stale, 1);
            else STRESS_CHECK(0, "race close unexpected status 0x%lx", (unsigned long)st);
        }
        stress_barrier_wait(&g_bar);     /* C: race round done */
        stress_progress(&g_wd);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    stress_args a;
    stress_parse_args(argc, argv, &a, "stress_handle_test", 65536U);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_handle_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }
    long init_allocs = (long)atomic_load(&kshim_alloc_count);

    for (int p = 0; p < NPROC; p++) {
        char name[32];
        snprintf(name, sizeof name, "stress-handle-%d", p);
        st = ObCreateProcessObject(&g_proc[p], 7000 + (ULONG)p, name);
        if (st != STATUS_SUCCESS || !g_proc[p]) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    }
    kshim_set_current_process(g_proc[0]);

    for (int i = 0; i < POOL_OBJS; i++) {
        void *body = NULL;
        st = ObCreateObject(&nt_test_type, NULL, sizeof(struct nt_test_body), &g_pool[i], &body);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "pool ObCreateObject failed\n"); return 2; }
    }

    unsigned nthreads = a.threads;
    unsigned epochs = a.ops / OPS_PER_EPOCH;
    stress_barrier_init(&g_bar, nthreads + 1);   /* workers + coordinator */

    worker_t *ws = calloc(nthreads, sizeof *ws);
    pthread_t *th = calloc(nthreads, sizeof *th);
    if (!ws || !th) return 2;
    for (unsigned t = 0; t < nthreads; t++) {
        ws[t].id = (int)t;
        ws[t].ops = epochs * OPS_PER_EPOCH;
        stress_rng_seed(&ws[t].rng, a.seed, t);
        pthread_create(&th[t], NULL, worker_main, &ws[t]);
    }

    /* Coordinator: one race round per epoch. */
    for (unsigned e = 0; e < epochs; e++) {
        stress_barrier_wait(&g_bar);     /* A: workers finished random phase */
        struct nt_object_header *o = g_pool[e % POOL_OBJS];
        HANDLE h = NULL;
        st = ObCreateHandle(g_proc[0], o, 0x1, 0, &h);
        STRESS_CHECK(st == STATUS_SUCCESS, "race create status 0x%lx", (unsigned long)st);
        g_race_h = h;
        g_race_ok = 0;
        g_race_stale = 0;
        stress_barrier_wait(&g_bar);     /* B: release the race */
        stress_barrier_wait(&g_bar);     /* C: race finished */
        STRESS_CHECK(atomic_load(&g_race_ok) == 1,
                     "epoch %u: %d concurrent closes succeeded (want exactly 1)",
                     e, atomic_load(&g_race_ok));
        STRESS_CHECK(atomic_load(&g_race_ok) + atomic_load(&g_race_stale) == (int)nthreads,
                     "epoch %u: race results do not add up", e);
    }

    for (unsigned t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);

    /* Drain: close whatever each worker still owns. */
    for (unsigned t = 0; t < nthreads; t++) {
        for (int i = 0; i < ws[t].nlocal; i++) {
            st = ObCloseHandle(g_proc[ws[t].local[i].p], ws[t].local[i].h);
            STRESS_CHECK(st == STATUS_SUCCESS, "drain close status 0x%lx", (unsigned long)st);
        }
        ws[t].nlocal = 0;
    }

    STRESS_CHECK(ObHandleTableCount(g_proc[0]->handle_table) == 0,
                 "proc0 handle table not empty: %lu", (unsigned long)ObHandleTableCount(g_proc[0]->handle_table));
    STRESS_CHECK(ObHandleTableCount(g_proc[1]->handle_table) == 0,
                 "proc1 handle table not empty: %lu", (unsigned long)ObHandleTableCount(g_proc[1]->handle_table));

    for (int i = 0; i < POOL_OBJS; i++) {
        long long rc = atomic64_read(&g_pool[i]->ref_count);
        long long hc = atomic64_read(&g_pool[i]->handle_count);
        STRESS_CHECK(rc == 1, "pool[%d] ref_count %lld (want 1)", i, rc);
        STRESS_CHECK(hc == 0, "pool[%d] handle_count %lld (want 0)", i, hc);
        ObDereferenceObject(g_pool[i]);
    }

    ObDestroyProcessObject(g_proc[1]);
    ObDestroyProcessObject(g_proc[0]);
    kshim_set_current_process(NULL);
    ObCleanupObjectManager();

    stress_watchdog_stop(&g_wd);

    long end_allocs = (long)atomic_load(&kshim_alloc_count);
    STRESS_CHECK(end_allocs == base_allocs,
                 "allocation count %ld at end, baseline %ld (init used %ld)",
                 end_allocs, base_allocs, init_allocs - base_allocs);
    if (end_allocs != base_allocs) kshim_leak_report();

    printf("stress_handle_test seed=%lu: creates=%ld closes=%ld dups=%ld refs=%ld races=%u epochs\n",
           a.seed, atomic_load(&g_creates), atomic_load(&g_closes),
           atomic_load(&g_dups), atomic_load(&g_refs), epochs);
    long errs = atomic_load(&stress_errors);
    printf("stress_handle_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
