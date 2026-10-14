/* handle_edge_test.c - edge cases of the NT handle table (src/kernel/nt/handle.c).
 *
 * object_test.c covers handle reuse and inheritance in passing. This file
 * checks the cases its header comments document but nothing else exercises:
 *
 *   - the first user handle of a fresh process is 0xC8 and handles step by 4
 *   - the low two bits of a handle carry flags and are ignored on lookup
 *   - values the table must reject: NULL, pseudo-handles, never-issued slots,
 *     and a value whose high bits would alias a live slot if truncated
 *   - OBJ_PROTECT_FROM_CLOSE refuses ObCloseHandle and keeps the handle
 *   - a second close fails and a closed value is not reissued at once
 *   - ObReferenceObjectByHandle access and type checks take no reference on failure
 *   - growth across a level-3 page boundary (index 256)
 *   - the reference lifecycle: last close frees, process destroy closes handles
 *
 * Built and run by tests/kernel-host/Makefile like the module tests (userspace
 * kshim, no kernel boot). Test style follows src/kernel/nt/object_test.c.
 */

#include "object.h"
#include "handle.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <stdio.h>
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

#define HV(h) ((unsigned long long)(ULONG_PTR)(h))

/* A fresh process with its own handle table. */
static struct nt_eprocess *new_proc(ULONG pid)
{
    struct nt_eprocess *p = NULL;
    NTSTATUS st = ObCreateProcessObject(&p, pid, "handle_edge");
    if (st != STATUS_SUCCESS || !p || !p->handle_table) {
        fprintf(stderr, "new_proc: ObCreateProcessObject failed\n");
        g_fail++;
        return NULL;
    }
    kshim_set_current_process(p);
    return p;
}

/* An unnamed object of the test type. The caller owns the creator reference. */
static struct nt_object_header *new_obj(void)
{
    struct nt_object_header *hdr = NULL;
    void *body = NULL;
    NTSTATUS st = ObCreateObject(&nt_test_type, NULL, sizeof(struct nt_test_body), &hdr, &body);
    if (st != STATUS_SUCCESS || !hdr) {
        fprintf(stderr, "new_obj: ObCreateObject failed\n");
        g_fail++;
        return NULL;
    }
    return hdr;
}

static long allocs(void)
{
    return (long)atomic_load(&kshim_alloc_count);
}

static void end_proc(struct nt_eprocess *p)
{
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
}

/* -------------------------------------------------------------------------- */
/* 1. first handle value and stride                                            */
/* -------------------------------------------------------------------------- */
TEST(first_handle_and_stride)
{
    struct nt_eprocess *p = new_proc(900);
    ASSERT(p != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE h[3];
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &h[0]), STATUS_SUCCESS);
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &h[1]), STATUS_SUCCESS);
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &h[2]), STATUS_SUCCESS);

    ASSERT(HV(h[0]) == 0xC8ULL);
    ASSERT(HV(h[1]) == 0xCCULL);
    ASSERT(HV(h[2]) == 0xD0ULL);
    ASSERT(ObHandleTableCount(p->handle_table) == 3);
    /* creator reference plus one per handle; one handle count per handle */
    ASSERT(atomic64_read(&o->ref_count) == 4);
    ASSERT(atomic64_read(&o->handle_count) == 3);

    ASSERT_STATUS(ObCloseHandle(p, h[0]), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(p, h[1]), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(p, h[2]), STATUS_SUCCESS);
    ObDereferenceObject(o);
    end_proc(p);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 2. flag bits live in the low two bits and are ignored on lookup             */
/* -------------------------------------------------------------------------- */
TEST(flag_bits_ignored_on_lookup)
{
    struct nt_eprocess *p = new_proc(901);
    ASSERT(p != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateHandle(p, o, 0x3, OBJ_INHERIT, &h), STATUS_SUCCESS);
    ASSERT(HV(h) == 0xCAULL);  /* index 50, HANDLE_FLAG_INHERIT */
    ASSERT(ObHandleIsValid(p, h));

    /* Lookup with the flag bits cleared, and with them set, hits the same entry. */
    HANDLE clear = (HANDLE)((ULONG_PTR)h & ~(ULONG_PTR)0x3);
    HANDLE dirty = (HANDLE)((ULONG_PTR)h | 0x1);
    struct nt_object_header *got = NULL;
    ACCESS_MASK ga = 0;
    ASSERT_STATUS(ObReferenceObjectByHandle(p, clear, NULL, KernelMode, 0x1, &got, &ga), STATUS_SUCCESS);
    ASSERT(got == o && ga == 0x3);
    ObDereferenceObject(got);

    got = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(p, dirty, NULL, KernelMode, 0x1, &got, &ga), STATUS_SUCCESS);
    ASSERT(got == o);
    ObDereferenceObject(got);

    ASSERT_STATUS(ObCloseHandle(p, h), STATUS_SUCCESS);
    ObDereferenceObject(o);
    end_proc(p);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 3. values the table must reject                                             */
/* -------------------------------------------------------------------------- */
TEST(invalid_values_rejected)
{
    struct nt_eprocess *p = new_proc(902);
    ASSERT(p != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE live = NULL;
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &live), STATUS_SUCCESS);

    struct nt_object_header *got = NULL;
    ASSERT_STATUS(ObCloseHandle(p, NULL), STATUS_INVALID_HANDLE);
    ASSERT_STATUS(ObReferenceObjectByHandle(p, NULL, NULL, KernelMode, 0, &got, NULL), STATUS_INVALID_HANDLE);
    ASSERT(got == NULL);

    /* Index 1 is inside the first page but never issued. Index 1024 is past the
     * high-water mark and its page does not exist. */
    ASSERT_STATUS(ObCloseHandle(p, (HANDLE)(ULONG_PTR)0x4), STATUS_INVALID_HANDLE);
    ASSERT_STATUS(ObCloseHandle(p, (HANDLE)(ULONG_PTR)0x1000), STATUS_INVALID_HANDLE);

    /* 0xC8 + 2^32 has index 2^30 + 50, above HANDLE_MAX_INDEX. It must be
     * rejected, not truncated onto the live slot at index 50. */
    HANDLE alias = (HANDLE)((ULONG_PTR)live + ((ULONG_PTR)1 << 32));
    ASSERT(!ObHandleIsValid(p, alias));
    ASSERT_STATUS(ObCloseHandle(p, alias), STATUS_INVALID_HANDLE);
    ASSERT_STATUS(ObReferenceObjectByHandle(p, alias, NULL, KernelMode, 0, &got, NULL), STATUS_INVALID_HANDLE);

    /* Pseudo-handles are not table entries: closing one is a no-op that
     * succeeds (probe_handles golden). */
    ASSERT_STATUS(ObCloseHandle(p, NtCurrentProcess()), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(p, NtCurrentThread()), STATUS_SUCCESS);

    /* None of the failed calls touched the live entry. */
    ASSERT(ObHandleTableCount(p->handle_table) == 1);
    ASSERT(atomic64_read(&o->handle_count) == 1);
    ASSERT(atomic64_read(&o->ref_count) == 2);
    ASSERT(ObHandleIsValid(p, live));

    ASSERT_STATUS(ObCloseHandle(p, live), STATUS_SUCCESS);
    ObDereferenceObject(o);
    end_proc(p);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 4. OBJ_PROTECT_FROM_CLOSE                                                   */
/* -------------------------------------------------------------------------- */
TEST(protect_from_close)
{
    struct nt_eprocess *p = new_proc(903);
    ASSERT(p != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, OBJ_PROTECT_FROM_CLOSE, &h), STATUS_SUCCESS);
    ASSERT(HV(h) == 0xC9ULL);  /* HANDLE_FLAG_PROTECT_FROM_CLOSE in the low bit */

    ASSERT_STATUS(ObCloseHandle(p, h), STATUS_HANDLE_NOT_CLOSABLE);
    ASSERT(ObHandleIsValid(p, h));
    ASSERT(atomic64_read(&o->handle_count) == 1);
    ASSERT(ObHandleTableCount(p->handle_table) == 1);

    /* The handle still resolves. */
    struct nt_object_header *got = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, NULL, KernelMode, 0x1, &got, NULL), STATUS_SUCCESS);
    ASSERT(got == o);
    ObDereferenceObject(got);

    /* Process teardown closes it regardless of the flag. The creator reference
     * is dropped first, so the object is freed by that teardown. */
    ObDereferenceObject(o);
    end_proc(p);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 5. double close, and no immediate reuse of a closed value                   */
/* -------------------------------------------------------------------------- */
TEST(double_close_and_quarantine)
{
    struct nt_eprocess *p = new_proc(904);
    ASSERT(p != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE h1 = NULL;
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &h1), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(p, h1), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(p, h1), STATUS_INVALID_HANDLE);
    ASSERT(ObHandleTableCount(p->handle_table) == 0);
    ASSERT(atomic64_read(&o->handle_count) == 0);
    ASSERT(atomic64_read(&o->ref_count) == 1);  /* creator only */

    /* A closed slot goes to the tail of the free list and is not handed out
     * again until HANDLE_QUARANTINE other slots have been freed. */
    HANDLE h2 = NULL;
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &h2), STATUS_SUCCESS);
    ASSERT(h2 != h1);
    ASSERT(HV(h2) == 0xCCULL);  /* the next fresh index, not the closed one */
    ASSERT_STATUS(ObCloseHandle(p, h2), STATUS_SUCCESS);

    ObDereferenceObject(o);
    end_proc(p);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 6. access and type checks in ObReferenceObjectByHandle                      */
/* -------------------------------------------------------------------------- */
TEST(reference_access_and_type_checks)
{
    struct nt_eprocess *p = new_proc(905);
    ASSERT(p != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &h), STATUS_SUCCESS);
    long long refs = atomic64_read(&o->ref_count);  /* creator + handle */
    ASSERT(refs == 2);

    struct nt_object_header *got = (struct nt_object_header *)0x1;
    ACCESS_MASK ga = 0;

    /* Asking for a right the handle does not carry is denied, and the failed
     * call neither returns an object nor takes a reference. */
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, NULL, KernelMode, 0x2, &got, &ga), STATUS_ACCESS_DENIED);
    ASSERT(got == NULL);
    ASSERT(atomic64_read(&o->ref_count) == refs);

    got = (struct nt_object_header *)0x1;
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, &nt_process_type, KernelMode, 0x1, &got, &ga),
                  STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(got == NULL);
    ASSERT(atomic64_read(&o->ref_count) == refs);

    /* Matching type and a subset of the granted rights succeeds with one more
     * reference, and reports the granted mask. desired_access 0 always passes. */
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, &nt_test_type, KernelMode, 0x1, &got, &ga), STATUS_SUCCESS);
    ASSERT(got == o && ga == 0x1);
    ASSERT(atomic64_read(&o->ref_count) == refs + 1);
    ObDereferenceObject(got);

    got = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, NULL, KernelMode, 0, &got, NULL), STATUS_SUCCESS);
    ASSERT(got == o);
    ObDereferenceObject(got);
    ASSERT(atomic64_read(&o->ref_count) == refs);

    ASSERT_STATUS(ObCloseHandle(p, h), STATUS_SUCCESS);
    ObDereferenceObject(o);
    end_proc(p);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 7. growth across the level-3 page boundary                                  */
/* -------------------------------------------------------------------------- */
TEST(page_boundary_growth)
{
    /* Indices start at 50 and run to 50 + N - 1. Index 256 begins a second
     * level-3 page, so this crosses one page boundary. */
    enum { N = 220 };
    struct nt_eprocess *p = new_proc(906);
    ASSERT(p != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    static HANDLE hs[N];
    for (int i = 0; i < N; i++) {
        ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &hs[i]), STATUS_SUCCESS);
    }
    for (int i = 0; i < N; i++) {
        ASSERT(HV(hs[i]) == (unsigned long long)(50 + i) * 4ULL);
        ASSERT(ObHandleIsValid(p, hs[i]));
    }
    ASSERT(ObHandleTableCount(p->handle_table) == N);
    ASSERT(atomic64_read(&o->ref_count) == N + 1);
    ASSERT(atomic64_read(&o->handle_count) == N);

    for (int i = 0; i < N; i++) {
        ASSERT_STATUS(ObCloseHandle(p, hs[i]), STATUS_SUCCESS);
    }
    ASSERT(ObHandleTableCount(p->handle_table) == 0);
    ASSERT(atomic64_read(&o->handle_count) == 0);
    ObDereferenceObject(o);
    end_proc(p);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 8. lifecycle: last close frees, and process teardown closes open handles    */
/* -------------------------------------------------------------------------- */
TEST(last_close_frees_object)
{
    struct nt_eprocess *p = new_proc(907);
    ASSERT(p != NULL);
    long before = allocs();

    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &h), STATUS_SUCCESS);

    /* Dropping the creator reference leaves the handle as the only owner. */
    ObDereferenceObject(o);
    ASSERT(allocs() == before + 1);

    /* Closing that handle releases the last reference, which frees the object.
     * Do not touch o after this call. */
    ASSERT_STATUS(ObCloseHandle(p, h), STATUS_SUCCESS);
    ASSERT(allocs() == before);

    end_proc(p);
    PASS();
}

TEST(process_destroy_closes_handles)
{
    long before = allocs();
    struct nt_eprocess *p = new_proc(908);
    ASSERT(p != NULL);

    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);
    HANDLE h = NULL;
    HANDLE h2 = NULL;
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, 0, &h), STATUS_SUCCESS);
    ASSERT_STATUS(ObCreateHandle(p, o, 0x1, OBJ_PROTECT_FROM_CLOSE, &h2), STATUS_SUCCESS);
    ObDereferenceObject(o);

    /* Teardown must close every open handle, protected ones included, and drop
     * the references they held. Nothing should remain allocated afterwards.
     * h and h2 are not used after this point. */
    end_proc(p);
    ASSERT(allocs() == before);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */
int main(void)
{
    printf("handle edge tests starting\n");
    if (ObInitObjectManager() != STATUS_SUCCESS) {
        fprintf(stderr, "ObInitObjectManager failed\n");
        return 1;
    }
    RUN(first_handle_and_stride);
    RUN(flag_bits_ignored_on_lookup);
    RUN(invalid_values_rejected);
    RUN(protect_from_close);
    RUN(double_close_and_quarantine);
    RUN(reference_access_and_type_checks);
    RUN(page_boundary_growth);
    RUN(last_close_frees_object);
    RUN(process_destroy_closes_handles);
    printf("handle edge tests: %d passed, %d failed\n", g_pass, g_fail);
    ObCleanupObjectManager();
    if (g_fail) return 1;
    if (allocs() != 0) {
        fprintf(stderr, "LEAK: %ld allocations after ObCleanupObjectManager\n", allocs());
        return 1;
    }
    return 0;
}
