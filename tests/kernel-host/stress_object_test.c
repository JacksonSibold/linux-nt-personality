/* stress_object_test.c - concurrency stress for the object manager
 * (src/kernel/nt/object.c) on the userspace kshim.
 *
 * N workers (8-16) share a small set of object names under
 * \BaseNamedObjects, so collisions, opens and deletes race on the same
 * directory entries. Each worker runs seeded random operations:
 *   - create a named object and insert it (collisions are expected)
 *   - open a name to a handle, close a handle
 *   - reference a name (ObReferenceObjectByName) and drop it
 *   - delete a name: ObReferenceObjectByName then ObMakeTemporaryObject
 *   - reference and dereference unnamed pool objects directly
 *
 * Several workers can delete the same name at once; ObMakeTemporaryObject
 * must tolerate that (only the call that really removes the entry may drop
 * the directory's reference).
 *
 * Invariants checked at the end:
 *   - all handles closed, handle tables empty
 *   - no named object is left in the directory
 *   - pool objects are back to ref_count 1 and handle_count 0
 *   - the kshim allocation count returns to its baseline (no leaks)
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Usage: stress_object_test [seed] [threads 8..16] [ops per thread]
 */

#include "object.h"
#include "handle.h"
#include "stress_common.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define NAMES           16
#define NAME_CHARS      64
#define POOL_OBJS       32
#define LOCAL_MAX       24
#define OPS_PER_EPOCH   64

extern _Atomic long kshim_alloc_count;

static WCHAR                    g_names[NAMES][NAME_CHARS];
static UNICODE_STRING           g_us[NAMES];
static struct nt_eprocess      *g_proc;
static struct nt_object_header *g_pool[POOL_OBJS];
static stress_watchdog          g_wd;
static _Atomic long             g_creates, g_collisions, g_opens, g_refs, g_deletes;

typedef struct {
    HANDLE   local[LOCAL_MAX];
    int      nlocal;
    unsigned ops;
    stress_rng rng;
} worker_t;

static void set_name(int k)
{
    char tmp[NAME_CHARS];
    snprintf(tmp, sizeof tmp, "\\BaseNamedObjects\\Stress%d", k);
    size_t n = strlen(tmp);
    for (size_t i = 0; i < n; i++) g_names[k][i] = (WCHAR)(unsigned char)tmp[i];
    g_names[k][n] = 0;
    g_us[k].Buffer = g_names[k];
    g_us[k].Length = (USHORT)(n * sizeof(WCHAR));
    g_us[k].MaximumLength = g_us[k].Length;
}

static OBJECT_ATTRIBUTES name_oa(int k)
{
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.ObjectName = &g_us[k];
    oa.Attributes = 0;
    return oa;
}

static void local_add(worker_t *w, HANDLE h)
{
    w->local[w->nlocal++] = h;
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    kshim_set_current_process(g_proc);

    for (unsigned i = 0; i < w->ops; i++) {
        uint32_t r = stress_rnd(&w->rng, 100);
        int k = (int)stress_rnd(&w->rng, NAMES);
        OBJECT_ATTRIBUTES oa = name_oa(k);

        if (r < 20) {
            /* create + insert a named object */
            struct nt_object_header *hdr = NULL;
            void *body = NULL;
            NTSTATUS st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &hdr, &body);
            STRESS_CHECK(st == STATUS_SUCCESS, "ObCreateObject 0x%lx", (unsigned long)st);
            if (st != STATUS_SUCCESS) goto progress;
            atomic_fetch_add(&g_creates, 1);
            HANDLE h = NULL;
            st = ObInsertObject(hdr, &oa, 0, g_proc, &h);
            if (st == STATUS_SUCCESS) {
                /* insert took a directory ref and a handle; drop the creator ref */
                ObDereferenceObject(hdr);
                if (w->nlocal < LOCAL_MAX) local_add(w, h);
                else ObCloseHandle(g_proc, h);
            } else {
                /* collision or failure: no directory entry, creator ref frees it */
                STRESS_CHECK(st == STATUS_OBJECT_NAME_COLLISION,
                             "insert status 0x%lx (want success or collision)", (unsigned long)st);
                atomic_fetch_add(&g_collisions, 1);
                ObDereferenceObject(hdr);
            }
        } else if (r < 35) {
            /* open a name to a new handle */
            HANDLE h = NULL;
            NTSTATUS st = ObOpenObjectByName(&oa, &nt_test_type, UserMode, 0, g_proc, &h);
            STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND,
                         "open status 0x%lx", (unsigned long)st);
            if (st == STATUS_SUCCESS) {
                atomic_fetch_add(&g_opens, 1);
                if (w->nlocal < LOCAL_MAX) local_add(w, h);
                else ObCloseHandle(g_proc, h);
            }
        } else if (r < 45) {
            /* close one of ours */
            if (w->nlocal > 0) {
                unsigned idx = stress_rnd(&w->rng, (uint32_t)w->nlocal);
                HANDLE h = w->local[idx];
                w->local[idx] = w->local[--w->nlocal];
                NTSTATUS st = ObCloseHandle(g_proc, h);
                STRESS_CHECK(st == STATUS_SUCCESS, "close 0x%lx", (unsigned long)st);
            }
        } else if (r < 55) {
            /* reference a name and drop it */
            struct nt_object_header *o = NULL;
            NTSTATUS st = ObReferenceObjectByName(&g_us[k], &nt_test_type, KernelMode, 0, &o);
            STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND,
                         "ref-by-name 0x%lx", (unsigned long)st);
            if (st == STATUS_SUCCESS && o) {
                ObDereferenceObject(o);
                atomic_fetch_add(&g_refs, 1);
            }
        } else if (r < 65) {
            /* delete a name: look it up, make it temporary, drop our ref.
             * Several workers can do this on the same name at once. */
            struct nt_object_header *o = NULL;
            NTSTATUS st = ObReferenceObjectByName(&g_us[k], &nt_test_type, KernelMode, 0, &o);
            if (st == STATUS_SUCCESS && o) {
                ObMakeTemporaryObject(o);
                ObDereferenceObject(o);
                atomic_fetch_add(&g_deletes, 1);
            }
        } else if (r < 80) {
            /* unnamed pool object: extra reference then drop */
            struct nt_object_header *o = g_pool[stress_rnd(&w->rng, POOL_OBJS)];
            ObReferenceObject(o);
            ObDereferenceObject(o);
            atomic_fetch_add(&g_refs, 1);
        } else {
            /* validity of a handle we own */
            if (w->nlocal > 0) {
                HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                STRESS_CHECK(ObHandleIsValid(g_proc, h), "owned handle %p invalid", (void *)h);
            }
        }
    progress:
        stress_progress(&g_wd);
    }
    return NULL;
}

/* Regression: concurrent ObMakeTemporaryObject calls on one named object.
 *
 * Each round, RR_THREADS threads release the same object's directory entry
 * at the same moment. The entry is removed once, so the directory's reference
 * must be dropped once. The coordinator holds 2*RR_THREADS references so that
 * even a double drop cannot free the object; the count is then read directly.
 * The correct count is exactly 2*RR_THREADS. */
#define RR_THREADS 8
#define RR_ROUNDS  300

static struct nt_object_header *rr_obj;
static stress_barrier rr_start, rr_end;

static void *rr_thread(void *arg)
{
    (void)arg;
    for (int r = 0; r < RR_ROUNDS; r++) {
        stress_barrier_wait(&rr_start);
        ObMakeTemporaryObject(rr_obj);
        ObDereferenceObject(rr_obj);   /* this thread's own reference */
        stress_barrier_wait(&rr_end);
    }
    return NULL;
}

static void regress_make_temporary(void)
{
    pthread_t th[RR_THREADS];
    stress_barrier_init(&rr_start, RR_THREADS + 1);
    stress_barrier_init(&rr_end, RR_THREADS + 1);
    for (int i = 0; i < RR_THREADS; i++)
        pthread_create(&th[i], NULL, rr_thread, NULL);

    for (int r = 0; r < RR_ROUNDS; r++) {
        char name[96];
        snprintf(name, sizeof name, "\\BaseNamedObjects\\RegressMk%d", r);
        size_t n = strlen(name);
        WCHAR wname[96];
        for (size_t i = 0; i <= n; i++) wname[i] = (WCHAR)(unsigned char)name[i];
        UNICODE_STRING us;
        us.Buffer = wname;
        us.Length = (USHORT)(n * sizeof(WCHAR));
        us.MaximumLength = us.Length;
        OBJECT_ATTRIBUTES oa;
        memset(&oa, 0, sizeof oa);
        oa.Length = sizeof oa;
        oa.ObjectName = &us;

        void *body = NULL;
        NTSTATUS st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &rr_obj, &body);
        STRESS_CHECK(st == STATUS_SUCCESS, "regress create 0x%lx", (unsigned long)st);
        st = ObInsertObject(rr_obj, &oa, 0, NULL, NULL);     /* directory ref: count 2 */
        STRESS_CHECK(st == STATUS_SUCCESS, "regress insert 0x%lx", (unsigned long)st);
        for (int k = 0; k < 2 * RR_THREADS - 1; k++)
            ObReferenceObject(rr_obj);                        /* coordinator's refs */
        for (int k = 0; k < RR_THREADS; k++)
            ObReferenceObject(rr_obj);                        /* one per thread */

        stress_barrier_wait(&rr_start);
        stress_barrier_wait(&rr_end);

        long long rc = atomic64_read(&rr_obj->ref_count);
        STRESS_CHECK(rc == 2 * RR_THREADS,
                     "round %d: ref_count %lld after concurrent make-temporary (want %d)",
                     r, rc, 2 * RR_THREADS);
        struct nt_object_header *found = NULL;
        NTSTATUS fs = ObReferenceObjectByName(&us, &nt_test_type, KernelMode, 0, &found);
        STRESS_CHECK(fs == STATUS_OBJECT_NAME_NOT_FOUND,
                     "round %d: name still in directory (0x%lx)", r, (unsigned long)fs);
        if (fs == STATUS_SUCCESS && found) ObDereferenceObject(found);

        for (int k = 0; k < 2 * RR_THREADS; k++)
            ObDereferenceObject(rr_obj);                      /* frees on the last one */
    }
    for (int i = 0; i < RR_THREADS; i++)
        pthread_join(th[i], NULL);
}

int main(int argc, char **argv)
{
    stress_args a;
    stress_parse_args(argc, argv, &a, "stress_object_test", 16384U);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_object_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }

    st = ObCreateProcessObject(&g_proc, 7100, "stress-object");
    if (st != STATUS_SUCCESS || !g_proc) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    kshim_set_current_process(g_proc);

    for (int k = 0; k < NAMES; k++) set_name(k);
    for (int i = 0; i < POOL_OBJS; i++) {
        void *body = NULL;
        st = ObCreateObject(&nt_test_type, NULL, sizeof(struct nt_test_body), &g_pool[i], &body);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "pool ObCreateObject failed\n"); return 2; }
    }

    regress_make_temporary();

    unsigned nthreads = a.threads;
    worker_t *ws = calloc(nthreads, sizeof *ws);
    pthread_t *th = calloc(nthreads, sizeof *th);
    if (!ws || !th) return 2;
    for (unsigned t = 0; t < nthreads; t++) {
        ws[t].ops = a.ops;
        stress_rng_seed(&ws[t].rng, a.seed, t);
        pthread_create(&th[t], NULL, worker_main, &ws[t]);
    }
    for (unsigned t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);

    /* Drain: close handles still held, then delete every remaining name. */
    for (unsigned t = 0; t < nthreads; t++) {
        for (int i = 0; i < ws[t].nlocal; i++) {
            st = ObCloseHandle(g_proc, ws[t].local[i]);
            STRESS_CHECK(st == STATUS_SUCCESS, "drain close 0x%lx", (unsigned long)st);
        }
        ws[t].nlocal = 0;
    }
    for (int k = 0; k < NAMES; k++) {
        struct nt_object_header *o = NULL;
        st = ObReferenceObjectByName(&g_us[k], &nt_test_type, KernelMode, 0, &o);
        if (st == STATUS_SUCCESS && o) {
            ObMakeTemporaryObject(o);
            ObDereferenceObject(o);
        }
    }

    for (int k = 0; k < NAMES; k++) {
        struct nt_object_header *o = NULL;
        st = ObReferenceObjectByName(&g_us[k], &nt_test_type, KernelMode, 0, &o);
        STRESS_CHECK(st == STATUS_OBJECT_NAME_NOT_FOUND,
                     "name %d still in directory after drain (status 0x%lx)", k, (unsigned long)st);
        if (st == STATUS_SUCCESS && o) ObDereferenceObject(o);
    }
    STRESS_CHECK(ObHandleTableCount(g_proc->handle_table) == 0,
                 "handle table not empty: %lu", (unsigned long)ObHandleTableCount(g_proc->handle_table));

    for (int i = 0; i < POOL_OBJS; i++) {
        long long rc = atomic64_read(&g_pool[i]->ref_count);
        long long hc = atomic64_read(&g_pool[i]->handle_count);
        STRESS_CHECK(rc == 1, "pool[%d] ref_count %lld (want 1)", i, rc);
        STRESS_CHECK(hc == 0, "pool[%d] handle_count %lld (want 0)", i, hc);
        ObDereferenceObject(g_pool[i]);
    }

    ObDestroyProcessObject(g_proc);
    kshim_set_current_process(NULL);
    ObCleanupObjectManager();

    stress_watchdog_stop(&g_wd);

    long end_allocs = (long)atomic_load(&kshim_alloc_count);
    STRESS_CHECK(end_allocs == base_allocs,
                 "allocation count %ld at end, baseline %ld", end_allocs, base_allocs);
    if (end_allocs != base_allocs) kshim_leak_report();

    printf("stress_object_test seed=%lu: creates=%ld collisions=%ld opens=%ld refs=%ld deletes=%ld\n",
           a.seed, atomic_load(&g_creates), atomic_load(&g_collisions),
           atomic_load(&g_opens), atomic_load(&g_refs), atomic_load(&g_deletes));
    long errs = atomic_load(&stress_errors);
    printf("stress_object_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
