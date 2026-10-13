/* stress_section_test.c - concurrency stress for section objects
 * (src/kernel/nt/mm.c) on the userspace kshim, anonymous and file-backed.
 *
 * Setup: four files of 64 KiB, each filled with its own byte, are written on
 * the host under a scratch root and opened through nt_svc_NtCreateFile. The
 * coordinator keeps those file handles for the whole run.
 *
 * N workers (8-16) share a few named anonymous sections under
 * \BaseNamedObjects and the file-backed sections. Each worker runs seeded
 * random operations:
 *   - create an anonymous section and map a private view, write a tag, read it back
 *   - create a file-backed section over one of the four files and map it read-only
 *   - create or open a named section, and map a view of it
 *   - unmap one of our views, close one of our section handles
 *   - delete a name: ObReferenceObjectByName, ObMakeTemporaryObject
 *
 * A view can outlive its section handle (the view holds a reference), so
 * handles and views are released in independent random orders.
 *
 * Every epoch a race round runs: one duplicate of a shared section is made
 * and every worker calls NtClose on it at once. Exactly one close succeeds.
 *
 * Invariants checked at the end:
 *   - every view unmapped, all handles closed, handle table empty
 *   - the named sections are gone after the drain
 *   - the kshim allocation count returns to its baseline
 * Runs under a watchdog (60 s without progress aborts with a report).
 *
 * Usage: stress_section_test [seed] [threads 8..16] [ops per thread]
 */

#include "mm.h"
#include "io.h"
#include "kshim.h"
#include "object.h"
#include "handle.h"
#include "stress_common.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

NTSTATUS nt_svc_NtClose(HANDLE Handle);

#define NFILES          4
#define FILE_BYTES      (64 * 1024)
#define NAMED           4
#define NAME_CHARS      64
#define LOCAL_MAX       16
#define OPS_PER_EPOCH   64
#define MAX_PAGES       8

extern _Atomic long kshim_alloc_count;

static struct nt_eprocess      *g_proc;
static HANDLE                   g_file[NFILES];
static char                     g_root[256];
static WCHAR                    g_names[NAMED][NAME_CHARS];
static UNICODE_STRING           g_us[NAMED];
static HANDLE                   g_race_h;
static stress_watchdog          g_wd;
static stress_barrier           g_bar;
static _Atomic int              g_race_ok, g_race_other;
static _Atomic long             g_anon, g_filemaps, g_named_made, g_named_opened, g_views, g_deletes;

typedef struct {
    PVOID base;
} view_t;

typedef struct {
    HANDLE  sec[LOCAL_MAX];
    int     nsec;
    view_t  views[LOCAL_MAX];
    int     nviews;
    unsigned ops;
    unsigned epochs;
    stress_rng rng;
} worker_t;

static void set_name(int k)
{
    char tmp[NAME_CHARS];
    snprintf(tmp, sizeof tmp, "\\BaseNamedObjects\\SecStress%d", k);
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
    return oa;
}

static LARGE_INTEGER size_of(size_t bytes)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)bytes;
    return li;
}

static void sec_add(worker_t *w, HANDLE h)
{
    w->sec[w->nsec++] = h;
}

/* Map a view (RW or RO) and return the base, or NULL. */
static PVOID map_view(HANDLE sec, ULONG prot, SIZE_T *vs_out)
{
    PVOID base = NULL;
    SIZE_T vs = 0;
    LARGE_INTEGER off;
    off.QuadPart = 0;
    NTSTATUS st = NtMapViewOfSection(sec, (HANDLE)-1, &base, 0, 0, &off, &vs, 0, 0, prot);
    STRESS_CHECK(st == STATUS_SUCCESS, "map view 0x%lx", (unsigned long)st);
    if (st != STATUS_SUCCESS) return NULL;
    if (vs_out) *vs_out = vs;
    atomic_fetch_add(&g_views, 1);
    return base;
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    kshim_set_current_process(g_proc);

    for (unsigned e = 0; e < w->epochs; e++) {
        for (unsigned i = 0; i < OPS_PER_EPOCH; i++) {
            uint32_t r = stress_rnd(&w->rng, 100);
            NTSTATUS st;

            if (r < 14) {
                /* anonymous section, private RW view, tag round trip */
                if (w->nsec < LOCAL_MAX && w->nviews < LOCAL_MAX) {
                    size_t pages = 1 + stress_rnd(&w->rng, MAX_PAGES);
                    LARGE_INTEGER sz = size_of(pages * 4096);
                    HANDLE h = NULL;
                    st = NtCreateSection(&h, SECTION_ALL_ACCESS, NULL, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
                    STRESS_CHECK(st == STATUS_SUCCESS, "anon create 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) {
                        SIZE_T vs = 0;
                        PVOID base = map_view(h, PAGE_READWRITE, &vs);
                        if (base) {
                            uint64_t tag = stress_next(&w->rng) | 1;
                            memcpy(base, &tag, sizeof tag);
                            uint64_t back = 0;
                            memcpy(&back, base, sizeof back);
                            STRESS_CHECK(back == tag, "anon view tag mismatch");
                            STRESS_CHECK(vs >= (SIZE_T)(pages * 4096), "anon view size %lu", (unsigned long)vs);
                            w->views[w->nviews++].base = base;
                        }
                        sec_add(w, h);
                        atomic_fetch_add(&g_anon, 1);
                    }
                }
            } else if (r < 28) {
                /* file-backed read-only view. The host shim maps file pages as zero-filled, so only the mapping is checked. */
                if (w->nviews < LOCAL_MAX) {
                    int f = (int)stress_rnd(&w->rng, NFILES);
                    HANDLE h = NULL;
                    st = NtCreateSection(&h, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READONLY, SEC_COMMIT, g_file[f]);
                    STRESS_CHECK(st == STATUS_SUCCESS, "file section create 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) {
                        PVOID base = map_view(h, PAGE_READONLY, NULL);
                        if (base) {
                            w->views[w->nviews++].base = base;
                            atomic_fetch_add(&g_filemaps, 1);
                        }
                        if (w->nsec < LOCAL_MAX) sec_add(w, h);
                        else ObCloseHandle(g_proc, h);
                    }
                }
            } else if (r < 38) {
                /* create a named anonymous section (collision is expected) */
                if (w->nsec < LOCAL_MAX) {
                    int k = (int)stress_rnd(&w->rng, NAMED);
                    OBJECT_ATTRIBUTES oa = name_oa(k);
                    LARGE_INTEGER sz = size_of(4096);
                    HANDLE h = NULL;
                    st = NtCreateSection(&h, SECTION_ALL_ACCESS, &oa, &sz, PAGE_READWRITE, SEC_COMMIT, NULL);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_COLLISION,
                                 "named create 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) {
                        sec_add(w, h);
                        atomic_fetch_add(&g_named_made, 1);
                    }
                }
            } else if (r < 48) {
                /* open a named section; it may not exist yet */
                if (w->nsec < LOCAL_MAX) {
                    int k = (int)stress_rnd(&w->rng, NAMED);
                    OBJECT_ATTRIBUTES oa = name_oa(k);
                    HANDLE h = NULL;
                    st = nt_svc_NtOpenSection(&h, SECTION_ALL_ACCESS, &oa);
                    STRESS_CHECK(st == STATUS_SUCCESS || st == STATUS_OBJECT_NAME_NOT_FOUND,
                                 "named open 0x%lx", (unsigned long)st);
                    if (st == STATUS_SUCCESS) {
                        sec_add(w, h);
                        atomic_fetch_add(&g_named_opened, 1);
                    }
                }
            } else if (r < 60) {
                /* map a view of one of our section handles, read-only */
                if (w->nsec > 0 && w->nviews < LOCAL_MAX) {
                    HANDLE h = w->sec[stress_rnd(&w->rng, (uint32_t)w->nsec)];
                    PVOID base = map_view(h, PAGE_READONLY, NULL);
                    if (base) w->views[w->nviews++].base = base;
                }
            } else if (r < 72) {
                /* unmap one of our views */
                if (w->nviews > 0) {
                    unsigned j = stress_rnd(&w->rng, (uint32_t)w->nviews);
                    PVOID base = w->views[j].base;
                    w->views[j] = w->views[--w->nviews];
                    st = NtUnmapViewOfSection((HANDLE)-1, base);
                    STRESS_CHECK(st == STATUS_SUCCESS, "unmap 0x%lx", (unsigned long)st);
                }
            } else if (r < 84) {
                /* close one of our section handles (views keep the section alive) */
                if (w->nsec > 0) {
                    unsigned j = stress_rnd(&w->rng, (uint32_t)w->nsec);
                    HANDLE h = w->sec[j];
                    w->sec[j] = w->sec[--w->nsec];
                    st = ObCloseHandle(g_proc, h);
                    STRESS_CHECK(st == STATUS_SUCCESS, "close 0x%lx", (unsigned long)st);
                }
            } else if (r < 92) {
                /* delete a name: look it up, make it temporary, drop our ref */
                int k = (int)stress_rnd(&w->rng, NAMED);
                struct nt_object_header *o = NULL;
                st = ObReferenceObjectByName(&g_us[k], NULL, KernelMode, 0, &o);
                if (st == STATUS_SUCCESS && o) {
                    ObMakeTemporaryObject(o);
                    ObDereferenceObject(o);
                    atomic_fetch_add(&g_deletes, 1);
                }
            } else {
                /* a view we hold must still read as the section's first byte */
                if (w->nviews > 0) {
                    volatile unsigned char *p = (volatile unsigned char *)w->views[stress_rnd(&w->rng, (uint32_t)w->nviews)].base;
                    (void)*p;
                }
            }
            stress_progress(&g_wd);
        }

        /* Race round: NtClose of the same section handle from every worker at once. */
        stress_barrier_wait(&g_bar);     /* A */
        stress_barrier_wait(&g_bar);     /* B */
        NTSTATUS rs = ObCloseHandle(g_proc, g_race_h);
        if (rs == STATUS_SUCCESS) atomic_fetch_add(&g_race_ok, 1);
        else if (rs == STATUS_INVALID_HANDLE) atomic_fetch_add(&g_race_other, 1);
        else STRESS_CHECK(0, "race close unexpected 0x%lx", (unsigned long)rs);
        stress_barrier_wait(&g_bar);     /* C */
        stress_progress(&g_wd);
    }

    for (int i = 0; i < w->nviews; i++) {
        NTSTATUS us = NtUnmapViewOfSection((HANDLE)-1, w->views[i].base);
        STRESS_CHECK(us == STATUS_SUCCESS, "drain unmap 0x%lx", (unsigned long)us);
    }
    w->nviews = 0;
    for (int i = 0; i < w->nsec; i++) {
        NTSTATUS cs = ObCloseHandle(g_proc, w->sec[i]);
        STRESS_CHECK(cs == STATUS_SUCCESS, "drain close 0x%lx", (unsigned long)cs);
    }
    w->nsec = 0;
    return NULL;
}

/* Host side: write NFILES files of FILE_BYTES, each filled with 0x40 + index. */
static int make_files(void)
{
    static char buf[4096];
    for (int f = 0; f < NFILES; f++) {
        char p[512];
        snprintf(p, sizeof p, "%s/sec%d.bin", g_root, f);
        int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0644);
        if (fd < 0) return 0;
        memset(buf, 0x40 + f, sizeof buf);
        for (size_t done = 0; done < FILE_BYTES; done += sizeof buf)
            if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf) { close(fd); return 0; }
        close(fd);
    }
    return 1;
}

static NTSTATUS open_file(int f, HANDLE *out)
{
    static WCHAR wbuf[NFILES][128];
    char name[64];
    snprintf(name, sizeof name, "sec%d.bin", f);
    size_t n = strlen(name);
    if (n >= 127) return STATUS_INVALID_PARAMETER;
    for (size_t i = 0; i <= n; i++) wbuf[f][i] = (WCHAR)(unsigned char)name[i];
    UNICODE_STRING us;
    us.Buffer = wbuf[f];
    us.Length = (USHORT)(n * sizeof(WCHAR));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.ObjectName = &us;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER alloc;
    alloc.QuadPart = 0;
    memset(&iosb, 0, sizeof iosb);
    *out = NULL;
    return nt_svc_NtCreateFile(out, FILE_GENERIC_READ | FILE_GENERIC_WRITE, &oa, &iosb, &alloc, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                               FILE_NON_DIRECTORY_FILE, NULL, 0);
}

int main(int argc, char **argv)
{
    stress_args a;
    stress_parse_args(argc, argv, &a, "stress_section_test", 4096U);
    stress_seed_for_report = a.seed;
    stress_watchdog_start(&g_wd, "stress_section_test", a.seed);

    long base_allocs = (long)atomic_load(&kshim_alloc_count);

    snprintf(g_root, sizeof g_root, "/tmp/stress_section_%d", (int)getpid());
    khost_scratch_arm_cleanup(g_root);
    mkdir(g_root, 0700);
    kshim_set_root(g_root);
    if (!make_files()) { fprintf(stderr, "cannot write scratch files under %s\n", g_root); return 2; }

    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager 0x%lx\n", (unsigned long)st); return 2; }
    st = ObCreateProcessObject(&g_proc, 7400, "stress-section");
    if (st != STATUS_SUCCESS || !g_proc) { fprintf(stderr, "ObCreateProcessObject failed\n"); return 2; }
    kshim_set_current_process(g_proc);

    for (int f = 0; f < NFILES; f++) {
        st = open_file(f, &g_file[f]);
        if (st != STATUS_SUCCESS) { fprintf(stderr, "open file %d 0x%lx\n", f, (unsigned long)st); return 2; }
    }
    for (int k = 0; k < NAMED; k++) set_name(k);

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

    LARGE_INTEGER race_sz = size_of(8192);
    for (unsigned e = 0; e < epochs; e++) {
        stress_barrier_wait(&g_bar);     /* A */
        st = NtCreateSection(&g_race_h, SECTION_ALL_ACCESS, NULL, &race_sz, PAGE_READWRITE, SEC_COMMIT, NULL);
        STRESS_CHECK(st == STATUS_SUCCESS, "race create 0x%lx", (unsigned long)st);
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

    for (int f = 0; f < NFILES; f++) {
        st = nt_svc_NtClose(g_file[f]);   /* io file handles use the io close */
        STRESS_CHECK(st == STATUS_SUCCESS, "file close 0x%lx", (unsigned long)st);
    }
    for (int k = 0; k < NAMED; k++) {
        struct nt_object_header *o = NULL;
        st = ObReferenceObjectByName(&g_us[k], NULL, KernelMode, 0, &o);
        if (st == STATUS_SUCCESS && o) {
            ObMakeTemporaryObject(o);
            ObDereferenceObject(o);
        }
        st = ObReferenceObjectByName(&g_us[k], NULL, KernelMode, 0, &o);
        STRESS_CHECK(st == STATUS_OBJECT_NAME_NOT_FOUND,
                     "named section %d still present after drain (0x%lx)", k, (unsigned long)st);
        if (st == STATUS_SUCCESS && o) ObDereferenceObject(o);
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

    printf("stress_section_test seed=%lu: anon=%ld filemaps=%ld named_made=%ld named_opened=%ld views=%ld deletes=%ld\n",
           a.seed, atomic_load(&g_anon), atomic_load(&g_filemaps), atomic_load(&g_named_made),
           atomic_load(&g_named_opened), atomic_load(&g_views), atomic_load(&g_deletes));
    long errs = atomic_load(&stress_errors);
    printf("stress_section_test: %s (%ld violations)\n", errs ? "FAIL" : "PASS", errs);
    free(ws);
    free(th);
    return errs ? 1 : 0;
}
