/* stress_token_test.c - concurrency stress for access tokens
 * (src/kernel/nt/security.c) on the userspace kshim.
 *
 * The coordinator opens a pool of process-token handles. N workers (8-16)
 * then run seeded random operations on them and on handles they own:
 *   - NtOpenProcessToken with a random access mask
 *   - NtDuplicateToken of a shared pool token, or of one of the worker's own
 *     tokens (a chain of duplicates)
 *   - NtAdjustPrivilegesToken (disable all) on a handle
 *   - NtQueryInformationToken(TokenUser) on a handle
 *   - NtClose of an owned handle
 *
 * The access each handle was granted is tracked, so every result is checked:
 * adjust needs TOKEN_ADJUST_PRIVILEGES and query needs TOKEN_QUERY; without
 * them the call must fail with STATUS_ACCESS_DENIED, with them it must succeed.
 *
 * Every epoch a race round runs: one duplicate of a pool token is made and all
 * workers call NtClose on that same handle at once. Exactly one close succeeds.
 *
 * Invariants checked at the end:
 *   - all handles closed, handle table empty
 *   - the kshim allocation count returns to its baseline (every token freed once)
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Usage: stress_token_test [seed] [threads 8..16] [ops per thread]
 */

#include "object.h"
#include "handle.h"
#include "security.h"
#include "stress_common.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

NTSTATUS nt_svc_NtOpenProcessToken(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                                   PHANDLE TokenHandle);
NTSTATUS nt_svc_NtDuplicateToken(HANDLE ExistingTokenHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN EffectiveOnly,
                                 TOKEN_TYPE TokenType, PHANDLE NewTokenHandle);
NTSTATUS nt_svc_NtAdjustPrivilegesToken(HANDLE TokenHandle, BOOLEAN DisableAllPrivileges,
                                        PTOKEN_PRIVILEGES NewState, ULONG BufferLength,
                                        PTOKEN_PRIVILEGES PreviousState, PULONG ReturnLength);
NTSTATUS nt_svc_NtQueryInformationToken(HANDLE TokenHandle, ULONG TokenInformationClass,
                                        PVOID TokenInformation, ULONG TokenInformationLength,
                                        PULONG ReturnLength);

#ifndef TOKEN_QUERY
#define TOKEN_DUPLICATE          0x0002
#define TOKEN_QUERY              0x0008
#define TOKEN_ADJUST_PRIVILEGES  0x0020
#endif
#ifndef TOKEN_ALL_ACCESS
#define TOKEN_ALL_ACCESS 0x000F01FFUL
#endif
#define TOKEN_USER_CLASS 1UL          /* TokenUser */

#define POOL_TOKENS     4
#define LOCAL_MAX       24
#define OPS_PER_EPOCH   64
#define ACCESS_MIX      5

/* Access masks the workers pick from. Each has a mix of the three rights. */
static const ACCESS_MASK k_access[ACCESS_MIX] = {
    TOKEN_QUERY,
    TOKEN_QUERY | TOKEN_DUPLICATE,
    TOKEN_ADJUST_PRIVILEGES,
    TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
    TOKEN_ALL_ACCESS,
};

extern _Atomic long kshim_alloc_count;

static struct nt_eprocess      *g_proc;
static HANDLE                   g_master[POOL_TOKENS];   /* coordinator-owned pool tokens */
static stress_watchdog          g_wd;
static stress_barrier           g_bar;
static HANDLE                   g_race_h;
static _Atomic int              g_race_ok, g_race_other;
static _Atomic long             g_opens, g_dups, g_adjusts, g_queries, g_denied;

typedef struct {
    HANDLE     h;
    ACCESS_MASK access;
} local_t;

typedef struct {
    local_t    local[LOCAL_MAX];
    int        nlocal;
    unsigned   ops;
    unsigned   epochs;
    stress_rng rng;
} worker_t;

static void local_add(worker_t *w, HANDLE h, ACCESS_MASK acc)
{
    w->local[w->nlocal].h = h;
    w->local[w->nlocal].access = acc;
    w->nlocal++;
}

static void local_remove(worker_t *w, unsigned idx)
{
    w->local[idx] = w->local[--w->nlocal];
}

/* Expect SUCCESS when the granted access has the right, ACCESS_DENIED otherwise. */
static void check_right(NTSTATUS st, ACCESS_MASK have, ACCESS_MASK need, const char *what)
{
    if ((have & need) == need)
        STRESS_CHECK(st == STATUS_SUCCESS, "%s with right: 0x%lx (want success)", what, (unsigned long)st);
    else
        STRESS_CHECK(st == STATUS_ACCESS_DENIED, "%s without right: 0x%lx (want access denied)",
                     what, (unsigned long)st);
    if (st == STATUS_ACCESS_DENIED) atomic_fetch_add(&g_denied, 1);
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    kshim_set_current_process(g_proc);

    for (unsigned e = 0; e < w->epochs; e++) {
        for (unsigned i = 0; i < OPS_PER_EPOCH; i++) {
            uint32_t r = stress_rnd(&w->rng, 100);
            NTSTATUS st;

            if (r < 15) {
                /* open the process token */
                if (w->nlocal < LOCAL_MAX) {
                    ACCESS_MASK acc = k_access[stress_rnd(&w->rng, ACCESS_MIX)];
                    HANDLE h = NULL;
                    st = nt_svc_NtOpenProcessToken(NtCurrentProcess(), acc, &h);
                    STRESS_CHECK(st == STATUS_SUCCESS, "open process token 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) {
                        local_add(w, h, acc);
                        atomic_fetch_add(&g_opens, 1);
                    }
                }
            } else if (r < 30) {
                /* duplicate a shared pool token into this worker */
                if (w->nlocal < LOCAL_MAX) {
                    ACCESS_MASK acc = k_access[stress_rnd(&w->rng, ACCESS_MIX)];
                    HANDLE h = NULL;
                    st = nt_svc_NtDuplicateToken(g_master[stress_rnd(&w->rng, POOL_TOKENS)], acc,
                                                 NULL, 0, TokenPrimary, &h);
                    STRESS_CHECK(st == STATUS_SUCCESS, "duplicate pool token 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) {
                        local_add(w, h, acc);
                        atomic_fetch_add(&g_dups, 1);
                    }
                }
            } else if (r < 42) {
                /* chained duplicate of one of our own tokens */
                if (w->nlocal > 0 && w->nlocal < LOCAL_MAX) {
                    unsigned j = stress_rnd(&w->rng, (uint32_t)w->nlocal);
                    ACCESS_MASK acc = k_access[stress_rnd(&w->rng, ACCESS_MIX)];
                    HANDLE h = NULL;
                    /* Duplicating needs TOKEN_DUPLICATE on the source. */
                    st = nt_svc_NtDuplicateToken(w->local[j].h, acc, NULL, 0, TokenImpersonation, &h);
                    if (w->local[j].access & TOKEN_DUPLICATE) {
                        STRESS_CHECK(st == STATUS_SUCCESS, "chain duplicate 0x%lx", (unsigned long)st);
                        if (st == STATUS_SUCCESS) {
                            local_add(w, h, acc);
                            atomic_fetch_add(&g_dups, 1);
                        }
                    } else {
                        STRESS_CHECK(st == STATUS_ACCESS_DENIED, "chain duplicate without right 0x%lx",
                                     (unsigned long)st);
                        if (st == STATUS_SUCCESS) ObCloseHandle(g_proc, h);
                    }
                }
            } else if (r < 58) {
                /* disable all privileges: needs TOKEN_ADJUST_PRIVILEGES */
                if (w->nlocal > 0) {
                    local_t *l = &w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    ULONG ret = 0;
                    st = nt_svc_NtAdjustPrivilegesToken(l->h, TRUE, NULL, 0, NULL, &ret);
                    check_right(st, l->access, TOKEN_ADJUST_PRIVILEGES, "adjust");
                    if (st == STATUS_SUCCESS) atomic_fetch_add(&g_adjusts, 1);
                }
            } else if (r < 74) {
                /* query TokenUser: needs TOKEN_QUERY */
                if (w->nlocal > 0) {
                    local_t *l = &w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    unsigned char buf[512];
                    ULONG ret = 0;
                    st = nt_svc_NtQueryInformationToken(l->h, TOKEN_USER_CLASS, buf, sizeof buf, &ret);
                    check_right(st, l->access, TOKEN_QUERY, "query");
                    if (st == STATUS_SUCCESS) {
                        STRESS_CHECK(ret > 0 && ret <= sizeof buf, "query returned length %lu", (unsigned long)ret);
                        atomic_fetch_add(&g_queries, 1);
                    }
                }
            } else if (r < 90) {
                /* close one of ours */
                if (w->nlocal > 0) {
                    unsigned j = stress_rnd(&w->rng, (uint32_t)w->nlocal);
                    HANDLE h = w->local[j].h;
                    local_remove(w, j);
                    st = ObCloseHandle(g_proc, h);
                    STRESS_CHECK(st == STATUS_SUCCESS, "close 0x%lx", (unsigned long)st);
                }
            } else {
                /* a pool token must stay valid while the coordinator holds it */
                HANDLE h = g_master[stress_rnd(&w->rng, POOL_TOKENS)];
                STRESS_CHECK(ObHandleIsValid(g_proc, h), "pool token %p invalid", (void *)h);
            }
            stress_progress(&g_wd);
        }

        /* Race round: one duplicate, NtClose from every worker at once. */
        stress_barrier_wait(&g_bar);     /* A: random phase done */
        stress_barrier_wait(&g_bar);     /* B: race handle published */
        NTSTATUS rs = ObCloseHandle(g_proc, g_race_h);
        if (rs == STATUS_SUCCESS) atomic_fetch_add(&g_race_ok, 1);
        else if (rs == STATUS_INVALID_HANDLE) atomic_fetch_add(&g_race_other, 1);
        else STRESS_CHECK(0, "race close unexpected 0x%lx", (unsigned long)rs);
        stress_barrier_wait(&g_bar);     /* C: race round done */
        stress_progress(&g_wd);
    }

    for (int i = 0; i < w->nlocal; i++) {
        NTSTATUS cs = ObCloseHandle(g_proc, w->local[i].h);
        STRESS_CHECK(cs == STATUS_SUCCESS, "worker drain close 0x%lx", (unsigned long)cs);
    }
    w->nlocal = 0;
    return NULL;
}

int main(int argc, char **argv)
{
    stress_args a;
    stress_parse_args(argc, argv, &a, "stress_token_test", 16384U);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_token_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }
    st = ObCreateProcessObject(&g_proc, 7300, "stress-token");
    if (st != STATUS_SUCCESS || !g_proc) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    kshim_set_current_process(g_proc);

    for (int i = 0; i < POOL_TOKENS; i++) {
        st = nt_svc_NtOpenProcessToken(NtCurrentProcess(), TOKEN_ALL_ACCESS, &g_master[i]);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "pool token open 0x%lx\n", (unsigned long)st); return 2; }
    }

    unsigned nthreads = a.threads;
    unsigned epochs = a.ops / OPS_PER_EPOCH;
    stress_barrier_init(&g_bar, nthreads + 1);

    worker_t *ws = calloc(nthreads, sizeof *ws);
    pthread_t *th = calloc(nthreads, sizeof *th);
    if (!ws || !th) return 2;
    for (unsigned t = 0; t < nthreads; t++) {
        ws[t].ops = epochs * OPS_PER_EPOCH;
        ws[t].epochs = epochs;
        stress_rng_seed(&ws[t].rng, a.seed, t);
        pthread_create(&th[t], NULL, worker_main, &ws[t]);
    }

    for (unsigned e = 0; e < epochs; e++) {
        stress_barrier_wait(&g_bar);     /* A */
        st = nt_svc_NtDuplicateToken(g_master[e % POOL_TOKENS], TOKEN_QUERY | TOKEN_DUPLICATE,
                                     NULL, 0, TokenPrimary, &g_race_h);
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

    for (int i = 0; i < POOL_TOKENS; i++) {
        st = ObCloseHandle(g_proc, g_master[i]);
        STRESS_CHECK(st == STATUS_SUCCESS, "pool close 0x%lx", (unsigned long)st);
    }
    STRESS_CHECK(ObHandleTableCount(g_proc->handle_table) == 0,
                 "handle table not empty: %lu", (unsigned long)ObHandleTableCount(g_proc->handle_table));

    ObDestroyProcessObject(g_proc);
    kshim_set_current_process(NULL);
    ObCleanupObjectManager();

    stress_watchdog_stop(&g_wd);

    long end_allocs = (long)atomic_load(&kshim_alloc_count);
    STRESS_CHECK(end_allocs == base_allocs,
                 "allocation count %ld at end, baseline %ld", end_allocs, base_allocs);
    if (end_allocs != base_allocs) kshim_leak_report();

    printf("stress_token_test seed=%lu: opens=%ld dups=%ld adjusts=%ld queries=%ld denied=%ld\n",
           a.seed, atomic_load(&g_opens), atomic_load(&g_dups), atomic_load(&g_adjusts),
           atomic_load(&g_queries), atomic_load(&g_denied));
    long errs = atomic_load(&stress_errors);
    printf("stress_token_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
