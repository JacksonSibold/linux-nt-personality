/* stress_sd_test.c - concurrency stress for per-object security descriptors
 * (src/kernel/nt/security.c, object.c) on the userspace kshim.
 *
 * The coordinator builds K self-relative security descriptors. Variant k has a
 * DACL with k+1 allowed ACEs, so the ACE count identifies which variant a
 * query returned. A few event objects are shared; N workers (8-16) run seeded
 * random operations on them through duplicated handles:
 *   - NtSetSecurityObject(DACL) with a random variant
 *   - NtQuerySecurityObject(DACL) into a large buffer, then the result is parsed:
 *     it must be a valid self-relative SD whose DACL is one of the K variants
 *   - NtQuerySecurityObject into a small buffer: BUFFER_TOO_SMALL with LengthNeeded
 *   - NtDuplicateObject of a shared object, and ObCloseHandle of an owned handle
 *
 * Every epoch a race round runs: one duplicate of a shared object is made and
 * every worker does a set or query on it, then closes it, all at once. Each
 * close must succeed or return STATUS_INVALID_HANDLE, and exactly one succeeds.
 *
 * Invariants checked at the end:
 *   - all handles closed, handle tables empty
 *   - no query ever returned a torn or unknown descriptor
 *   - the kshim allocation count returns to its baseline: each installed SD
 *     is freed once, when its object goes away
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Usage: stress_sd_test [seed] [threads 8..16] [ops per thread]
 */

#include "object.h"
#include "handle.h"
#include "security.h"
#include "sync.h"
#include "stress_common.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"

#include <stdio.h>
#include <stdlib.h>

NTSTATUS nt_svc_NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes,
                              ULONG EventType, BOOLEAN InitialState);
NTSTATUS nt_svc_NtDuplicateObject(HANDLE SourceProcessHandle, HANDLE SourceHandle,
                                  HANDLE TargetProcessHandle, PHANDLE TargetHandle,
                                  ACCESS_MASK DesiredAccess, ULONG HandleAttributes, ULONG Options);
NTSTATUS nt_svc_NtSetSecurityObject(HANDLE Handle, ULONG SecurityInformation,
                                    PSECURITY_DESCRIPTOR SecurityDescriptor);
NTSTATUS nt_svc_NtQuerySecurityObject(HANDLE Handle, ULONG SecurityInformation,
                                      PSECURITY_DESCRIPTOR SecurityDescriptor,
                                      ULONG Length, PULONG LengthNeeded);

#ifndef DACL_SECURITY_INFORMATION
#define DACL_SECURITY_INFORMATION 0x00000004UL
#endif

#define NVARIANTS       4                  /* DACL variants, k+1 ACEs each */
#define NSHARED         3                  /* shared event objects */
#define LOCAL_MAX       24
#define OPS_PER_EPOCH   64
#define QBUF            4096
#define SMALLBUF        8

extern _Atomic long kshim_alloc_count;

static struct nt_eprocess      *g_proc;
static HANDLE                   g_master[NSHARED];
static HANDLE                   g_race_h;
static void                    *g_sr[NVARIANTS];      /* self-relative SD bytes */
static ULONG                    g_srlen[NVARIANTS];
static PSID                     g_sids[NVARIANTS * (NVARIANTS + 1) / 2 + 2]; /* ACE SIDs, k+1 per variant */
static PACL                     g_acls[NVARIANTS];
static SECURITY_DESCRIPTOR     *g_abs[NVARIANTS];
static stress_watchdog          g_wd;
static stress_barrier           g_bar;
static _Atomic int              g_race_ok, g_race_other;
static _Atomic long             g_sets, g_queries, g_small, g_dups, g_torn;

typedef struct {
    HANDLE     local[LOCAL_MAX];
    int        nlocal;
    unsigned   ops;
    unsigned   epochs;
    stress_rng rng;
} worker_t;

/* Parse a self-relative SD returned by a query and return its DACL ACE
 * count, or -1 if the buffer is not a valid descriptor with a DACL. */
static int dacl_ace_count(const void *buf, ULONG len)
{
    SECURITY_DESCRIPTOR *sd = NULL;
    NTSTATUS pst = sd_from_self_relative(buf, len, &sd);
    if (pst != STATUS_SUCCESS || !sd)
        return -1;
    int count = -1;
    if (sd->Control & SE_DACL_PRESENT) {
        PACL dacl = sd->Dacl;
        if (dacl) count = (int)dacl->AceCount;
    }
    /* sd_from_self_relative copies the owner, group and ACLs; release them too. */
    if (sd->Owner) sid_free(sd->Owner);
    if (sd->Group) sid_free(sd->Group);
    if (sd->Dacl) acl_free(sd->Dacl);
    if (sd->Sacl) acl_free(sd->Sacl);
    sd_free(sd);
    return count;
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
    unsigned char qbuf[QBUF];

    for (unsigned e = 0; e < w->epochs; e++) {
        for (unsigned i = 0; i < OPS_PER_EPOCH; i++) {
            uint32_t r = stress_rnd(&w->rng, 100);
            int k = (int)stress_rnd(&w->rng, NVARIANTS);
            NTSTATUS st;

            if (r < 25) {
                /* set the DACL of a handle we own, or of a shared object */
                HANDLE h = (w->nlocal > 0 && stress_rnd(&w->rng, 2))
                               ? w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)]
                               : g_master[stress_rnd(&w->rng, NSHARED)];
                st = nt_svc_NtSetSecurityObject(h, DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)g_sr[k]);
                STRESS_CHECK(st == STATUS_SUCCESS, "set SD 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) atomic_fetch_add(&g_sets, 1);
            } else if (r < 50) {
                /* query into a large buffer and check the descriptor is one we installed */
                HANDLE h = (w->nlocal > 0 && stress_rnd(&w->rng, 2))
                               ? w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)]
                               : g_master[stress_rnd(&w->rng, NSHARED)];
                ULONG need = 0;
                st = nt_svc_NtQuerySecurityObject(h, DACL_SECURITY_INFORMATION,
                                                  (PSECURITY_DESCRIPTOR)qbuf, QBUF, &need);
                STRESS_CHECK(st == STATUS_SUCCESS, "query SD 0x%lx", (unsigned long)st);
                if (st == STATUS_SUCCESS) {
                    int n = dacl_ace_count(qbuf, need);
                    if (n < 1 || n > NVARIANTS) {
                        atomic_fetch_add(&g_torn, 1);
                        STRESS_CHECK(0, "query returned a torn or unknown SD (ace count %d, len %lu)",
                                     n, (unsigned long)need);
                    }
                    atomic_fetch_add(&g_queries, 1);
                }
            } else if (r < 60) {
                /* query into a small buffer: BUFFER_TOO_SMALL, LengthNeeded set */
                unsigned char small[SMALLBUF];
                ULONG need = 0;
                st = nt_svc_NtQuerySecurityObject(g_master[stress_rnd(&w->rng, NSHARED)],
                                                  DACL_SECURITY_INFORMATION,
                                                  (PSECURITY_DESCRIPTOR)small, SMALLBUF, &need);
                STRESS_CHECK(st == STATUS_BUFFER_TOO_SMALL, "small query 0x%lx", (unsigned long)st);
                STRESS_CHECK(need > SMALLBUF, "LengthNeeded %lu not above the buffer", (unsigned long)need);
                if (st == STATUS_BUFFER_TOO_SMALL) atomic_fetch_add(&g_small, 1);
            } else if (r < 72) {
                /* duplicate a shared object into this worker */
                if (w->nlocal < LOCAL_MAX) {
                    HANDLE dst = NULL;
                    st = nt_svc_NtDuplicateObject(NtCurrentProcess(), g_master[stress_rnd(&w->rng, NSHARED)],
                                                  NtCurrentProcess(), &dst, 0, 0, DUPLICATE_SAME_ACCESS);
                    STRESS_CHECK(st == STATUS_SUCCESS, "dup 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) { local_add(w, dst); atomic_fetch_add(&g_dups, 1); }
                }
            } else if (r < 88) {
                /* close one of ours: the object's SD must be freed only with the last handle */
                if (w->nlocal > 0) {
                    unsigned j = stress_rnd(&w->rng, (uint32_t)w->nlocal);
                    HANDLE h = w->local[j];
                    w->local[j] = w->local[--w->nlocal];
                    st = ObCloseHandle(g_proc, h);
                    STRESS_CHECK(st == STATUS_SUCCESS, "close 0x%lx", (unsigned long)st);
                }
            } else {
                /* a handle we own must still be valid */
                if (w->nlocal > 0) {
                    HANDLE h = w->local[stress_rnd(&w->rng, (uint32_t)w->nlocal)];
                    STRESS_CHECK(ObHandleIsValid(g_proc, h), "owned handle %p invalid", (void *)h);
                }
            }
            stress_progress(&g_wd);
        }

        /* Race round: set or query, then close, on one shared handle, all at once. */
        stress_barrier_wait(&g_bar);     /* A */
        stress_barrier_wait(&g_bar);     /* B */
        {
            NTSTATUS rs;
            if (stress_rnd(&w->rng, 2))
                rs = nt_svc_NtSetSecurityObject(g_race_h, DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)g_sr[0]);
            else {
                ULONG need = 0;
                rs = nt_svc_NtQuerySecurityObject(g_race_h, DACL_SECURITY_INFORMATION,
                                                  (PSECURITY_DESCRIPTOR)qbuf, QBUF, &need);
            }
            STRESS_CHECK(rs == STATUS_SUCCESS || rs == STATUS_INVALID_HANDLE,
                         "race set/query 0x%lx", (unsigned long)rs);
            NTSTATUS cs = ObCloseHandle(g_proc, g_race_h);
            if (cs == STATUS_SUCCESS) atomic_fetch_add(&g_race_ok, 1);
            else if (cs == STATUS_INVALID_HANDLE) atomic_fetch_add(&g_race_other, 1);
            else STRESS_CHECK(0, "race close unexpected 0x%lx", (unsigned long)cs);
        }
        stress_barrier_wait(&g_bar);     /* C */
        stress_progress(&g_wd);
    }

    for (int i = 0; i < w->nlocal; i++) {
        NTSTATUS cs = ObCloseHandle(g_proc, w->local[i]);
        STRESS_CHECK(cs == STATUS_SUCCESS, "worker drain close 0x%lx", (unsigned long)cs);
    }
    w->nlocal = 0;
    return NULL;
}

/* Builds variant k: owner LOCAL_SYSTEM, a DACL with k+1 allowed ACEs. */
static NTSTATUS build_variant(int k, PSID owner, PSID *sids_pool, int *nsids)
{
    static const enum ntsec_well_known_sid wk[] = {
        SID_WK_EVERYONE, SID_WK_AUTHENTICATED_USERS, SID_WK_USERS, SID_WK_ADMINISTRATORS, SID_WK_LOCAL_SERVICE,
    };
    PACL dacl = acl_create(512);
    if (!dacl) return STATUS_NO_MEMORY;
    for (int a = 0; a <= k; a++) {
        PSID s = sid_well_known(wk[a]);
        if (!s) { acl_free(dacl); return STATUS_NO_MEMORY; }
        sids_pool[(*nsids)++] = s;
        ULONG idx = 0;
        NTSTATUS st = acl_add_allowed_ace(dacl, GENERIC_READ | (ACCESS_MASK)(1UL << a), 0, s, &idx);
        if (st != STATUS_SUCCESS) { acl_free(dacl); return st; }
    }
    g_acls[k] = dacl;
    g_abs[k] = sd_create_absolute(owner, NULL, dacl, NULL, SE_DACL_PRESENT);
    if (!g_abs[k]) return STATUS_NO_MEMORY;
    void *buf = NULL;
    ULONG len = 0;
    NTSTATUS st = sd_to_self_relative(g_abs[k], &buf, &len);
    if (st != STATUS_SUCCESS) return st;
    g_sr[k] = buf;
    g_srlen[k] = len;
    return STATUS_SUCCESS;
}

int main(int argc, char **argv)
{
    stress_args a;
    stress_parse_args(argc, argv, &a, "stress_sd_test", 16384U);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_sd_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }
    st = ObCreateProcessObject(&g_proc, 7600, "stress-sd");
    if (st != STATUS_SUCCESS || !g_proc) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    kshim_set_current_process(g_proc);

    struct nt_thread *main_nt = NULL;
    st = nt_thread_create(&main_nt);
    if (st != STATUS_SUCCESS) return 2;
    nt_set_current_thread(main_nt);

    /* The owner is the process token's user. The owner keeps WRITE_DAC, so the
     * caller can change the DACL later. The token owns the SID, so it is not freed here. */
    PSID owner = ((ACCESS_TOKEN *)g_proc->primary_token)->user;
    int nsids = 0;
    for (int k = 0; k < NVARIANTS; k++) {
        st = build_variant(k, owner, g_sids, &nsids);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "variant %d 0x%lx\n", k, (unsigned long)st); return 2; }
    }

    for (int i = 0; i < NSHARED; i++) {
        st = nt_svc_NtCreateEvent(&g_master[i], 0x1F0003UL, NULL, NotificationEvent, 0);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "event 0x%lx\n", (unsigned long)st); return 2; }
        /* An object without a descriptor cannot be changed through the API, so the
         * first descriptor goes in through the object manager. Later sets go
         * through NtSetSecurityObject. */
        struct nt_object_header *hdr = NULL;
        st = ObReferenceObjectByHandle(g_proc, g_master[i], NULL, KernelMode, 0, &hdr, NULL);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "reference 0x%lx\n", (unsigned long)st); return 2; }
        st = ObSetObjectSecurityDescriptor(hdr, g_sr[i % NVARIANTS], g_srlen[i % NVARIANTS]);
        STRESS_CHECK(st == STATUS_SUCCESS, "initial SD 0x%lx", (unsigned long)st);
        ObDereferenceObject(hdr);
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
        st = nt_svc_NtDuplicateObject(NtCurrentProcess(), g_master[e % NSHARED],
                                      NtCurrentProcess(), &g_race_h, 0, 0, DUPLICATE_SAME_ACCESS);
        STRESS_CHECK(st == STATUS_SUCCESS, "race dup 0x%lx", (unsigned long)st);
        g_race_ok = 0;
        g_race_other = 0;
        stress_barrier_wait(&g_bar);     /* B */
        stress_barrier_wait(&g_bar);     /* C */
        STRESS_CHECK(atomic_load(&g_race_ok) == 1,
                     "epoch %u: %d closes succeeded (want exactly 1)", e, atomic_load(&g_race_ok));
        STRESS_CHECK(atomic_load(&g_race_ok) + atomic_load(&g_race_other) == (int)nthreads,
                     "epoch %u: race results do not add up", e);
    }

    for (unsigned t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);

    for (int i = 0; i < NSHARED; i++) {
        st = ObCloseHandle(g_proc, g_master[i]);
        STRESS_CHECK(st == STATUS_SUCCESS, "master close 0x%lx", (unsigned long)st);
    }
    STRESS_CHECK(ObHandleTableCount(g_proc->handle_table) == 0,
                 "handle table not empty: %lu", (unsigned long)ObHandleTableCount(g_proc->handle_table));

    /* Release the test's own SDs and ACLs; sd_free and acl_free do not free SIDs. */
    for (int k = 0; k < NVARIANTS; k++) {
        kfree(g_sr[k]);
        sd_free(g_abs[k]);
        acl_free(g_acls[k]);
    }
    for (int i = 0; i < nsids; i++) sid_free(g_sids[i]);

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

    printf("stress_sd_test seed=%lu: sets=%ld queries=%ld small=%ld dups=%ld torn=%ld\n",
           a.seed, atomic_load(&g_sets), atomic_load(&g_queries), atomic_load(&g_small),
           atomic_load(&g_dups), atomic_load(&g_torn));
    long errs = atomic_load(&stress_errors);
    printf("stress_sd_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
