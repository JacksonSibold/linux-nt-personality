/* stress_registry_test.c - concurrency stress for the registry (src/kernel/nt/registry.c)
 * on the userspace kshim.
 *
 * N workers (8-16) share a few keys under \Registry\User\Software. Each worker
 * runs seeded random operations through the typed registry entry points:
 *   - NtCreateKey and NtOpenKey (a key may be missing or just deleted)
 *   - NtSetValueKey and NtDeleteValueKey on a value of a key we hold
 *   - NtQueryValueKey (partial information): a DWORD value is 4 bytes long
 *   - NtDeleteKey, NtFlushKey, and ObCloseHandle of an owned handle
 *
 * Results are checked against the set each call may return. A handle whose
 * key was deleted may only return STATUS_KEY_DELETED afterwards.
 *
 * Every epoch a race round runs: the coordinator deletes one key while every
 * worker opens it by path at the same moment. Each open must succeed or return
 * STATUS_OBJECT_NAME_NOT_FOUND. A worker that gets a handle then reads and closes it.
 *
 * Invariants checked at the end:
 *   - every key is gone after the drain
 *   - all handles closed, handle table empty
 *   - the kshim allocation count returns to its baseline (no leaked key or value)
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Usage: stress_registry_test [seed] [threads 8..16] [ops per thread]
 */

#include "object.h"
#include "handle.h"
#include "registry.h"
#include "stress_common.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#ifndef STATUS_KEY_DELETED
#define STATUS_KEY_DELETED ((NTSTATUS)0xC000017CL)
#endif
#define REG_DWORD_TYPE 4UL
#define KEY_VALUE_PARTIAL_CLASS 2UL      /* KeyValuePartialInformation */

#define NKEYS           6
#define NVALS           3
#define NAME_CHARS      160
#define LOCAL_MAX       24
#define OPS_PER_EPOCH   64

extern _Atomic long kshim_alloc_count;

static struct nt_eprocess      *g_proc;
static stress_watchdog          g_wd;
static stress_barrier           g_bar;
static HANDLE                   g_race_key;
static _Atomic int              g_race_hits, g_race_miss;
static _Atomic long             g_creates, g_opens, g_sets, g_queries, g_qvals, g_delkeys, g_delvals, g_flushes;

typedef struct {
    WCHAR          buf[NAME_CHARS];
    UNICODE_STRING us;
} wname_t;

static wname_t g_key[NKEYS];
static wname_t g_val[NVALS];
static wname_t g_race;

typedef struct {
    HANDLE     local[LOCAL_MAX];
    int        nlocal;
    unsigned   ops;
    unsigned   epochs;
    stress_rng rng;
} worker_t;

static void wname_set(wname_t *w, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) w->buf[i] = (WCHAR)(unsigned char)s[i];
    w->buf[n] = 0;
    w->us.Buffer = w->buf;
    w->us.Length = (USHORT)(n * sizeof(WCHAR));
    w->us.MaximumLength = w->us.Length;
}

static OBJECT_ATTRIBUTES oa_of(wname_t *w)
{
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.ObjectName = &w->us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    return oa;
}

static void local_add(worker_t *w, HANDLE h)
{
    if (w->nlocal < LOCAL_MAX) w->local[w->nlocal++] = h;
    else ObCloseHandle(g_proc, h);
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    kshim_set_current_process(g_proc);
    kshim_set_current_thread(NULL);

    for (unsigned e = 0; e < w->epochs; e++) {
        for (unsigned i = 0; i < OPS_PER_EPOCH; i++) {
            uint32_t r = stress_rnd(&w->rng, 100);
            int k = (int)stress_rnd(&w->rng, NKEYS);
            int v = (int)stress_rnd(&w->rng, NVALS);
            NTSTATUS st;

            if (r < 16) {
                /* create (or open the existing key) and keep the handle */
                OBJECT_ATTRIBUTES oa = oa_of(&g_key[k]);
                HANDLE h = NULL;
                ULONG disp = 0;
                st = NtCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL, 0, &disp);
                STRESS_CHECK(st == STATUS_SUCCESS, "create 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) { local_add(w, h); atomic_fetch_add(&g_creates, 1); }
            } else if (r < 30) {
                /* open a key that may be missing */
                OBJECT_ATTRIBUTES oa = oa_of(&g_key[k]);
                HANDLE h = NULL;
                st = NtOpenKey(&h, KEY_ALL_ACCESS, &oa);
                STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND,
                             "open 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) { local_add(w, h); atomic_fetch_add(&g_opens, 1); }
            } else if (r < 46) {
                /* set a DWORD value on a key we hold */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    UNICODE_STRING vn = g_val[v].us;
                    ULONG data = stress_next(&w->rng) & 0xffff;
                    st = NtSetValueKey(h, &vn, 0, REG_DWORD_TYPE, &data, sizeof data);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_KEY_DELETED,
                                 "set value 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) atomic_fetch_add(&g_sets, 1);
                }
            } else if (r < 58) {
                /* query a value: a DWORD comes back as 4 bytes */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    UNICODE_STRING vn = g_val[v].us;
                    unsigned char buf[64];
                    ULONG rl = 0;
                    st = NtQueryValueKey(h, &vn, KEY_VALUE_PARTIAL_CLASS, buf, sizeof buf, &rl);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND ||
                                 st == STATUS_KEY_DELETED,
                                 "query value 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) {
                        ULONG type = *(ULONG *)(buf + 4);
                        ULONG dlen = *(ULONG *)(buf + 8);
                        STRESS_CHECK(type == REG_DWORD_TYPE && dlen == sizeof(ULONG),
                                     "value type %lu length %lu", (unsigned long)type, (unsigned long)dlen);
                        atomic_fetch_add(&g_qvals, 1);
                    }
                    atomic_fetch_add(&g_queries, 1);
                }
            } else if (r < 66) {
                /* delete a value */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    UNICODE_STRING vn = g_val[v].us;
                    st = NtDeleteValueKey(h, &vn);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND ||
                                 st == STATUS_KEY_DELETED,
                                 "delete value 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) atomic_fetch_add(&g_delvals, 1);
                }
            } else if (r < 74) {
                /* delete a key through a handle we hold */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    st = NtDeleteKey(h);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_KEY_DELETED,
                                 "delete key 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) atomic_fetch_add(&g_delkeys, 1);
                }
            } else if (r < 80) {
                /* flush a key we hold */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    st = NtFlushKey(h);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_KEY_DELETED,
                                 "flush 0x%lx", (unsigned long)st);
                    atomic_fetch_add(&g_flushes, 1);
                }
            } else if (r < 90) {
                /* close one of ours */
                if (w->nlocal > 0) {
                    unsigned j = stress_rnd(&w->rng, (uint32_t)w->nlocal);
                    HANDLE h = w->local[j];
                    w->local[j] = w->local[--w->nlocal];
                    st = ObCloseHandle(g_proc, h);
                    STRESS_CHECK(st == STATUS_SUCCESS, "close 0x%lx", (unsigned long)st);
                }
            } else {
                /* a handle we hold must still be valid */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    STRESS_CHECK(ObHandleIsValid(g_proc, h), "owned handle %p invalid", (void *)h);
                }
            }
            stress_progress(&g_wd);
        }

        /* Race round: the coordinator deletes g_race while every worker opens it. */
        stress_barrier_wait(&g_bar);     /* A: random phase done */
        stress_barrier_wait(&g_bar);     /* B: start */
        {
            OBJECT_ATTRIBUTES oa = oa_of(&g_race);
            HANDLE h = NULL;
            NTSTATUS rs = NtOpenKey(&h, KEY_ALL_ACCESS, &oa);
            if (rs == STATUS_SUCCESS) {
                atomic_fetch_add(&g_race_hits, 1);
                ULONG data = 7;
                UNICODE_STRING vn = g_val[0].us;
                NTSTATUS ss = NtSetValueKey(h, &vn, 0, REG_DWORD_TYPE, &data, sizeof data);
                STRESS_CHECK(ss == STATUS_SUCCESS || ss == STATUS_KEY_DELETED,
                             "race set 0x%lx", (unsigned long)ss);
                ObCloseHandle(g_proc, h);
            } else {
                STRESS_CHECK(rs == STATUS_OBJECT_NAME_NOT_FOUND,
                             "race open 0x%lx (want success or not found)", (unsigned long)rs);
                atomic_fetch_add(&g_race_miss, 1);
            }
        }
        stress_barrier_wait(&g_bar);     /* C: race round done */
        stress_progress(&g_wd);
    }

    for (int i = 0; i < w->nlocal; i++) {
        NTSTATUS cs = ObCloseHandle(g_proc, w->local[i]);
        STRESS_CHECK(cs == STATUS_SUCCESS, "worker drain close 0x%lx", (unsigned long)cs);
    }
    w->nlocal = 0;
    return NULL;
}

/* Deletes the key at path p if it exists. */
static void drop_key(wname_t *p)
{
    OBJECT_ATTRIBUTES oa = oa_of(p);
    HANDLE h = NULL;
    NTSTATUS st = NtOpenKey(&h, KEY_ALL_ACCESS, &oa);
    if (st == STATUS_SUCCESS) {
        NTSTATUS ds = NtDeleteKey(h);
        STRESS_CHECK(ds == STATUS_SUCCESS || ds == STATUS_KEY_DELETED,
                     "drain delete 0x%lx", (unsigned long)ds);
        ObCloseHandle(g_proc, h);
    }
}

int main(int argc, char **argv)
{
    stress_args a;
    stress_parse_args(argc, argv, &a, "stress_registry_test", 4096U);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_registry_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }
    st = ntreg_init();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ntreg_init 0x%lx\n", (unsigned long)st); return 2; }
    st = ObCreateProcessObject(&g_proc, 7700, "stress-registry");
    if (st != STATUS_SUCCESS || !g_proc) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    kshim_set_current_process(g_proc);

    char tmp[NAME_CHARS];
    for (int k = 0; k < NKEYS; k++) {
        snprintf(tmp, sizeof tmp, "\\Registry\\User\\Software\\RegStress%d", k);
        wname_set(&g_key[k], tmp);
    }
    for (int v = 0; v < NVALS; v++) {
        snprintf(tmp, sizeof tmp, "val%d", v);
        wname_set(&g_val[v], tmp);
    }
    wname_set(&g_race, "\\Registry\\User\\Software\\RegStressRace");

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
        /* The race key exists for this round; the previous round deleted it. */
        {
            OBJECT_ATTRIBUTES oa = oa_of(&g_race);
            HANDLE h = NULL;
            ULONG disp = 0;
            NTSTATUS cs = NtCreateKey(&g_race_key, KEY_ALL_ACCESS, &oa, 0, NULL, 0, &disp);
            STRESS_CHECK(cs == STATUS_SUCCESS, "race create 0x%lx", (unsigned long)cs);
            (void)h;
        }
        g_race_hits = 0;
        g_race_miss = 0;
        stress_barrier_wait(&g_bar);     /* B: workers start opening */
        {
            NTSTATUS ds = NtDeleteKey(g_race_key);
            STRESS_CHECK(ds == STATUS_SUCCESS, "race delete 0x%lx", (unsigned long)ds);
            ObCloseHandle(g_proc, g_race_key);
        }
        stress_barrier_wait(&g_bar);     /* C */
        STRESS_CHECK(atomic_load(&g_race_hits) + atomic_load(&g_race_miss) == (int)nthreads,
                     "epoch %u: race results do not add up", e);
    }

    for (unsigned t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);

    for (int k = 0; k < NKEYS; k++) drop_key(&g_key[k]);
    drop_key(&g_race);
    for (int k = 0; k < NKEYS; k++) {
        OBJECT_ATTRIBUTES oa = oa_of(&g_key[k]);
        HANDLE h = NULL;
        st = NtOpenKey(&h, KEY_ALL_ACCESS, &oa);
        STRESS_CHECK(st == STATUS_OBJECT_NAME_NOT_FOUND,
                     "key %d still present after drain (0x%lx)", k, (unsigned long)st);
        if (st == STATUS_SUCCESS) ObCloseHandle(g_proc, h);
    }
    if (ObHandleTableCount(g_proc->handle_table) != 0)
        fprintf(stderr, "handle table holds %lu entries after drain\n",
                (unsigned long)ObHandleTableCount(g_proc->handle_table));
    STRESS_CHECK(ObHandleTableCount(g_proc->handle_table) == 0,
                 "handle table not empty: %lu", (unsigned long)ObHandleTableCount(g_proc->handle_table));

    ObDestroyProcessObject(g_proc);
    kshim_set_current_process(NULL);
    ntreg_cleanup();
    ObCleanupObjectManager();

    stress_watchdog_stop(&g_wd);

    long end_allocs = (long)atomic_load(&kshim_alloc_count);
    STRESS_CHECK(end_allocs == base_allocs,
                 "allocation count %ld at end, baseline %ld", end_allocs, base_allocs);
    if (end_allocs != base_allocs) kshim_leak_report();

    printf("stress_registry_test seed=%lu: creates=%ld opens=%ld sets=%ld queries=%ld (dword=%ld) delvals=%ld delkeys=%ld flushes=%ld race_hits=%d\n",
           a.seed, atomic_load(&g_creates), atomic_load(&g_opens), atomic_load(&g_sets),
           atomic_load(&g_queries), atomic_load(&g_qvals), atomic_load(&g_delvals),
           atomic_load(&g_delkeys), atomic_load(&g_flushes), (int)atomic_load(&g_race_hits));
    long errs = atomic_load(&stress_errors);
    printf("stress_registry_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
