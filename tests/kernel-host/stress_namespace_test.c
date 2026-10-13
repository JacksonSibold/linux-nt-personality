/* stress_namespace_test.c - concurrency stress for the object namespace:
 * directory and symbolic-link objects, permanent and temporary names, and
 * open-by-name races (src/kernel/nt/object.c, nt_obj.c) on the userspace kshim.
 *
 * N workers (8-16) share a few directories and links under \BaseNamedObjects.
 * Each worker runs seeded random operations:
 *   - create a directory (collisions are expected)
 *   - create an event inside a directory, which may be missing
 *   - create a symbolic link to a directory path, and open a link object
 *   - open an event through a path that may cross a link
 *   - make a name permanent or temporary, and delete one (make temporary)
 *   - close one of our handles
 *
 * Every epoch a race round runs: the coordinator makes one event temporary
 * while every worker opens its name by path at the same moment. Each open
 * must return success or STATUS_OBJECT_NAME_NOT_FOUND, never anything else.
 *
 * Invariants checked at the end:
 *   - every name is gone after the drain, and no directory still holds entries
 *   - all handles closed, handle table empty
 *   - the kshim allocation count returns to its baseline
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Usage: stress_namespace_test [seed] [threads 8..16] [ops per thread]
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

NTSTATUS nt_svc_NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes,
                              ULONG EventType, BOOLEAN InitialState);
NTSTATUS nt_svc_NtOpenEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                            POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtCreateDirectoryObject(PHANDLE DirectoryHandle, ACCESS_MASK DesiredAccess,
                                        POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtCreateSymbolicLinkObject(PHANDLE LinkHandle, ACCESS_MASK DesiredAccess,
                                           POBJECT_ATTRIBUTES ObjectAttributes,
                                           PUNICODE_STRING LinkTarget);
NTSTATUS nt_svc_NtOpenSymbolicLinkObject(PHANDLE LinkHandle, ACCESS_MASK DesiredAccess,
                                         POBJECT_ATTRIBUTES ObjectAttributes);

#ifndef OBJ_PERMANENT
#define OBJ_PERMANENT 0x00000010UL
#endif

#define NDIRS           3
#define NEVENTS         4              /* events per directory */
#define NLINKS          3
#define NAME_CHARS      160
#define LOCAL_MAX       24
#define OPS_PER_EPOCH   64
#define NRACE_EVENTS    1

extern _Atomic long kshim_alloc_count;

static struct nt_eprocess      *g_proc;
static stress_watchdog          g_wd;
static stress_barrier           g_bar;
static _Atomic int              g_race_hits, g_race_miss;
static _Atomic long             g_dirs, g_events, g_links, g_opens_thru_link, g_perm, g_temp, g_deletes;

/* Each named path is kept as a UTF-16 string, built once. */
typedef struct {
    WCHAR            buf[NAME_CHARS];
    UNICODE_STRING   us;
} wname_t;

static wname_t g_dir[NDIRS];
static wname_t g_ev[NDIRS][NEVENTS];
static wname_t g_link[NLINKS];
static wname_t g_linktarget[NLINKS];        /* link target: a directory path */
static wname_t g_thru[NLINKS][NEVENTS];     /* event path reached through a link */
static wname_t g_race;                       /* event opened by every worker in the race round */

typedef struct {
    HANDLE   local[LOCAL_MAX];
    int      nlocal;
    unsigned ops;
    unsigned epochs;
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

static OBJECT_ATTRIBUTES oa_of(wname_t *w, ULONG attrs)
{
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.ObjectName = &w->us;
    oa.Attributes = attrs;
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

    for (unsigned e = 0; e < w->epochs; e++) {
        for (unsigned i = 0; i < OPS_PER_EPOCH; i++) {
            uint32_t r = stress_rnd(&w->rng, 100);
            int d = (int)stress_rnd(&w->rng, NDIRS);
            int ev = (int)stress_rnd(&w->rng, NEVENTS);
            int l = (int)stress_rnd(&w->rng, NLINKS);
            NTSTATUS st;

            if (r < 12) {
                /* directory: create (collision expected once it exists) */
                OBJECT_ATTRIBUTES oa = oa_of(&g_dir[d], 0);
                HANDLE h = NULL;
                st = nt_svc_NtCreateDirectoryObject(&h, 0x000F000FUL, &oa);
                STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_COLLISION,
                             "dir create 0x%lx", (unsigned long)st);
                /* a collision still returns a handle to the existing directory */
                if (h) local_add(w, h);
                if (st == STATUS_SUCCESS) atomic_fetch_add(&g_dirs, 1);
            } else if (r < 26) {
                /* event inside a directory: the directory may not exist yet */
                OBJECT_ATTRIBUTES oa = oa_of(&g_ev[d][ev], 0);
                HANDLE h = NULL;
                st = nt_svc_NtCreateEvent(&h, 0x1F0003UL, &oa, NotificationEvent, 0);
                STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_COLLISION ||
                             st == STATUS_OBJECT_PATH_NOT_FOUND || st == STATUS_OBJECT_NAME_NOT_FOUND,
                             "event in dir 0x%lx", (unsigned long)st);
                if (h) local_add(w, h);
                if (st == STATUS_SUCCESS) atomic_fetch_add(&g_events, 1);
            } else if (r < 36) {
                /* symbolic link to a directory path */
                OBJECT_ATTRIBUTES oa = oa_of(&g_link[l], 0);
                HANDLE h = NULL;
                st = nt_svc_NtCreateSymbolicLinkObject(&h, 0x000F0001UL, &oa, &g_linktarget[l].us);
                STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_COLLISION,
                             "link create 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) { local_add(w, h); atomic_fetch_add(&g_links, 1); }
            } else if (r < 46) {
                /* open a link object by name */
                OBJECT_ATTRIBUTES oa = oa_of(&g_link[l], 0);
                HANDLE h = NULL;
                st = nt_svc_NtOpenSymbolicLinkObject(&h, 0x0001UL, &oa);
                STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND,
                             "link open 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) local_add(w, h);
            } else if (r < 56) {
                /* open an event through a path that crosses a link */
                OBJECT_ATTRIBUTES oa = oa_of(&g_thru[l][ev], 0);
                HANDLE h = NULL;
                st = nt_svc_NtOpenEvent(&h, 0x1F0003UL, &oa);
                STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND ||
                             st == STATUS_OBJECT_PATH_NOT_FOUND,
                             "open through link 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) {
                    atomic_fetch_add(&g_opens_thru_link, 1);
                    local_add(w, h);
                }
            } else if (r < 66) {
                /* permanent or temporary: reference the event, change, drop the ref */
                struct nt_object_header *o = NULL;
                st = ObReferenceObjectByName(&g_ev[d][ev].us, NULL, KernelMode, 0, &o);
                if (st == STATUS_SUCCESS && o) {
                    if (stress_rnd(&w->rng, 2)) { ObMakePermanentObject(o); atomic_fetch_add(&g_perm, 1); }
                    else { ObMakeTemporaryObject(o); atomic_fetch_add(&g_temp, 1); }
                    ObDereferenceObject(o);
                }
            } else if (r < 76) {
                /* delete a name: look it up, make it temporary, drop our ref */
                struct nt_object_header *o = NULL;
                wname_t *target = (stress_rnd(&w->rng, 2)) ? &g_ev[d][ev] : &g_link[l];
                st = ObReferenceObjectByName(&target->us, NULL, KernelMode, 0, &o);
                if (st == STATUS_SUCCESS && o) {
                    ObMakeTemporaryObject(o);
                    ObDereferenceObject(o);
                    atomic_fetch_add(&g_deletes, 1);
                }
            } else if (r < 88) {
                /* close one of our handles */
                if (w->nlocal > 0) {
                    unsigned j = stress_rnd(&w->rng, (uint32_t)w->nlocal);
                    HANDLE h = w->local[j];
                    w->local[j] = w->local[--w->nlocal];
                    st = ObCloseHandle(g_proc, h);
                    STRESS_CHECK(st == STATUS_SUCCESS, "close 0x%lx", (unsigned long)st);
                }
            } else {
                /* validity of a handle we still hold */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    STRESS_CHECK(ObHandleIsValid(g_proc, h), "owned handle %p invalid", (void *)h);
                }
            }
            stress_progress(&g_wd);
        }

        /* Race round: every worker opens g_race by path while the coordinator deletes it. */
        stress_barrier_wait(&g_bar);     /* A: random phase done */
        stress_barrier_wait(&g_bar);     /* B: start */
        {
            OBJECT_ATTRIBUTES oa = oa_of(&g_race, 0);
            HANDLE h = NULL;
            NTSTATUS rs = nt_svc_NtOpenEvent(&h, 0x1F0003UL, &oa);
            if (rs == STATUS_SUCCESS) {
                atomic_fetch_add(&g_race_hits, 1);
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

/* On a failed drain, list the surviving handles by object type. */
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
            if (shown < 6)
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
    stress_parse_args(argc, argv, &a, "stress_namespace_test", 4096U);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_namespace_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }
    st = ObCreateProcessObject(&g_proc, 7500, "stress-namespace");
    if (st != STATUS_SUCCESS || !g_proc) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    kshim_set_current_process(g_proc);

    struct nt_thread *main_nt = NULL;
    st = nt_thread_create(&main_nt);
    if (st != STATUS_SUCCESS) return 2;
    nt_set_current_thread(main_nt);

    char tmp[NAME_CHARS];
    for (int d = 0; d < NDIRS; d++) {
        snprintf(tmp, sizeof tmp, "\\BaseNamedObjects\\NsDir%d", d);
        wname_set(&g_dir[d], tmp);
        for (int e = 0; e < NEVENTS; e++) {
            snprintf(tmp, sizeof tmp, "\\BaseNamedObjects\\NsDir%d\\NsEv%d", d, e);
            wname_set(&g_ev[d][e], tmp);
        }
    }
    for (int l = 0; l < NLINKS; l++) {
        snprintf(tmp, sizeof tmp, "\\BaseNamedObjects\\NsLink%d", l);
        wname_set(&g_link[l], tmp);
        snprintf(tmp, sizeof tmp, "\\BaseNamedObjects\\NsDir%d", l % NDIRS);
        wname_set(&g_linktarget[l], tmp);
        for (int e = 0; e < NEVENTS; e++) {
            snprintf(tmp, sizeof tmp, "\\BaseNamedObjects\\NsLink%d\\NsEv%d", l, e);
            wname_set(&g_thru[l][e], tmp);
        }
    }
    wname_set(&g_race, "\\BaseNamedObjects\\NsRace");

    /* The race event exists before the first round. */
    {
        OBJECT_ATTRIBUTES oa = oa_of(&g_race, 0);
        HANDLE h = NULL;
        st = nt_svc_NtCreateEvent(&h, 0x1F0003UL, &oa, NotificationEvent, 0);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "race event 0x%lx\n", (unsigned long)st); return 2; }
        ObCloseHandle(g_proc, h);
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
        /* Make the race name exist again (it was deleted in the previous round). */
        {
            OBJECT_ATTRIBUTES oa = oa_of(&g_race, 0);
            HANDLE h = NULL;
            NTSTATUS cs = nt_svc_NtCreateEvent(&h, 0x1F0003UL, &oa, NotificationEvent, 0);
            STRESS_CHECK(cs == STATUS_SUCCESS || cs == STATUS_OBJECT_NAME_COLLISION,
                         "race recreate 0x%lx", (unsigned long)cs);
            if (h) ObCloseHandle(g_proc, h);   /* also on collision */
        }
        g_race_hits = 0;
        g_race_miss = 0;
        stress_barrier_wait(&g_bar);     /* B: workers start opening */
        {
            struct nt_object_header *o = NULL;
            NTSTATUS ds = ObReferenceObjectByName(&g_race.us, NULL, KernelMode, 0, &o);
            if (ds == STATUS_SUCCESS && o) {
                ObMakeTemporaryObject(o);
                ObDereferenceObject(o);
            }
        }
        stress_barrier_wait(&g_bar);     /* C */
        STRESS_CHECK(atomic_load(&g_race_hits) + atomic_load(&g_race_miss) == (int)nthreads,
                     "epoch %u: race results do not add up", e);
    }

    for (unsigned t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);

    /* Drain: make every name temporary, then check that the namespace is empty. */
    for (int d = 0; d < NDIRS; d++) {
        for (int e = 0; e < NEVENTS; e++) {
            struct nt_object_header *o = NULL;
            st = ObReferenceObjectByName(&g_ev[d][e].us, NULL, KernelMode, 0, &o);
            if (st == STATUS_SUCCESS && o) { ObMakeTemporaryObject(o); ObDereferenceObject(o); }
        }
    }
    for (int l = 0; l < NLINKS; l++) {
        struct nt_object_header *o = NULL;
        st = ObReferenceObjectByName(&g_link[l].us, NULL, KernelMode, 0, &o);
        if (st == STATUS_SUCCESS && o) { ObMakeTemporaryObject(o); ObDereferenceObject(o); }
    }
    {
        struct nt_object_header *o = NULL;
        st = ObReferenceObjectByName(&g_race.us, NULL, KernelMode, 0, &o);
        if (st == STATUS_SUCCESS && o) { ObMakeTemporaryObject(o); ObDereferenceObject(o); }
    }
    for (int d = 0; d < NDIRS; d++) {
        for (int e = 0; e < NEVENTS; e++) {
            struct nt_object_header *o = NULL;
            st = ObReferenceObjectByName(&g_ev[d][e].us, NULL, KernelMode, 0, &o);
            STRESS_CHECK(st == STATUS_OBJECT_NAME_NOT_FOUND || st == STATUS_OBJECT_PATH_NOT_FOUND,
                         "event %d/%d still in namespace (0x%lx)", d, e, (unsigned long)st);
            if (st == STATUS_SUCCESS && o) ObDereferenceObject(o);
        }
    }
    for (int l = 0; l < NLINKS; l++) {
        struct nt_object_header *o = NULL;
        st = ObReferenceObjectByName(&g_link[l].us, NULL, KernelMode, 0, &o);
        STRESS_CHECK(st == STATUS_OBJECT_NAME_NOT_FOUND,
                     "link %d still in namespace (0x%lx)", l, (unsigned long)st);
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

    printf("stress_namespace_test seed=%lu: dirs=%ld events=%ld links=%ld thru_link=%ld perm=%ld temp=%ld deletes=%ld\n",
           a.seed, atomic_load(&g_dirs), atomic_load(&g_events), atomic_load(&g_links),
           atomic_load(&g_opens_thru_link), atomic_load(&g_perm), atomic_load(&g_temp),
           atomic_load(&g_deletes));
    long errs = atomic_load(&stress_errors);
    printf("stress_namespace_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
