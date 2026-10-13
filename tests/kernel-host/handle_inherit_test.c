/* handle_inherit_test.c - handle inheritance into a new table (P2, M4).
 *
 * Covers ObInheritHandleTable and ObInheritHandles in src/kernel/nt/handle.c:
 *
 *   - only OBJ_INHERIT handles are copied; the others are not visible to the child
 *   - a copy keeps the parent's handle value (same index, same low flag bits)
 *     and granted access, including an index on a level-3 page boundary
 *   - the copy takes one object reference and one handle count per handle;
 *     PROTECT_FROM_CLOSE is not inherited
 *   - closing the parent's handles after inheritance leaves the child's copies
 *     valid, and the object is freed only when the last handle is gone
 *   - a slot already in use in the child makes the whole call fail, and the
 *     copies it made are closed again, so the child is as it was before
 *   - an empty parent copies nothing; NULL tables are rejected
 *   - no allocation outlives the test (leak tracker)
 *
 * Built and run by tests/kernel-host/Makefile like the other module tests
 * (userspace kshim, no kernel boot). Style follows handle_edge_test.c.
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
#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while (0)

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

#define HV(h) ((unsigned long long)(ULONG_PTR)(h))

static struct nt_eprocess *new_proc(ULONG pid)
{
    struct nt_eprocess *p = NULL;
    NTSTATUS st = ObCreateProcessObject(&p, pid, "handle_inherit");
    if (st != STATUS_SUCCESS || !p || !p->handle_table) {
        fprintf(stderr, "new_proc: ObCreateProcessObject failed\n");
        g_fail++;
        return NULL;
    }
    return p;
}

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
}

/* -------------------------------------------------------------------------- */
/* 1. only inheritable handles are copied, with the same values and access     */
/* -------------------------------------------------------------------------- */
TEST(copies_only_inheritable_with_same_values)
{
    struct nt_eprocess *par = new_proc(1000);
    struct nt_eprocess *chi = new_proc(1001);
    ASSERT(par != NULL && chi != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE plain = NULL, inh = NULL, inh_hi = NULL;
    ASSERT_STATUS(ObCreateHandle(par, o, 0x1, 0, &plain), STATUS_SUCCESS);
    ASSERT_STATUS(ObCreateHandle(par, o, 0x3, OBJ_INHERIT, &inh), STATUS_SUCCESS);
    /* index 300 is on the second level-3 page */
    ASSERT_STATUS(ObpCreateHandleAtIndex(par->handle_table, o, 0x7, OBJ_INHERIT, 300, &inh_hi),
                  STATUS_SUCCESS);
    ASSERT(HV(inh_hi) == (300ULL << 2) + HANDLE_FLAG_INHERIT);

    u32 copied = 99;
    ASSERT_STATUS(ObInheritHandleTable(par->handle_table, chi->handle_table, &copied), STATUS_SUCCESS);
    ASSERT(copied == 2);
    ASSERT(ObHandleTableCount(chi->handle_table) == 2);

    /* not inheritable: invisible to the child */
    ASSERT(!ObHandleIsValid(chi, plain));
    /* inheritable: same value, same access */
    ASSERT(ObHandleIsValid(chi, inh));
    ASSERT(ObHandleIsValid(chi, inh_hi));
    ULONG ga = 0;
    struct nt_object_header *got = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(chi, inh, NULL, KernelMode, 0, &got, &ga), STATUS_SUCCESS);
    ASSERT(got == o && ga == 0x3);
    ObDereferenceObject(got);
    got = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(chi, inh_hi, NULL, KernelMode, 0, &got, &ga), STATUS_SUCCESS);
    ASSERT(got == o && ga == 0x7);
    ObDereferenceObject(got);

    ASSERT_STATUS(ObCloseHandle(par, plain), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(par, inh), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(par, inh_hi), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(chi, inh), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(chi, inh_hi), STATUS_SUCCESS);
    ObDereferenceObject(o);
    end_proc(chi);
    end_proc(par);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 2. reference counts and flags                                               */
/* -------------------------------------------------------------------------- */
TEST(reference_counts_and_flags)
{
    struct nt_eprocess *par = new_proc(1010);
    struct nt_eprocess *chi = new_proc(1011);
    ASSERT(par != NULL && chi != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE a = NULL, b = NULL;
    /* the protect bit must not be inherited */
    ASSERT_STATUS(ObCreateHandle(par, o, 0x1, OBJ_INHERIT | OBJ_PROTECT_FROM_CLOSE, &a), STATUS_SUCCESS);
    ASSERT_STATUS(ObCreateHandle(par, o, 0x1, OBJ_INHERIT, &b), STATUS_SUCCESS);
    /* creator + two parent handles */
    ASSERT(atomic64_read(&o->ref_count) == 3);
    ASSERT(atomic64_read(&o->handle_count) == 2);

    ASSERT_STATUS(ObInheritHandleTable(par->handle_table, chi->handle_table, NULL), STATUS_SUCCESS);
    /* one more reference and one more handle count per copy */
    ASSERT(atomic64_read(&o->ref_count) == 5);
    ASSERT(atomic64_read(&o->handle_count) == 4);

    bool inh = false, prot = true;
    ASSERT_STATUS(ObGetHandleFlags(chi, a, &inh, &prot), STATUS_SUCCESS);
    ASSERT(inh && !prot);
    ASSERT_STATUS(ObGetHandleFlags(chi, b, &inh, &prot), STATUS_SUCCESS);
    ASSERT(inh && !prot);

    ASSERT_STATUS(ObCloseHandle(par, a), STATUS_HANDLE_NOT_CLOSABLE);  /* still protected in the parent */
    ASSERT_STATUS(ObCloseHandle(chi, a), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(chi, b), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(par, b), STATUS_SUCCESS);
    ASSERT(atomic64_read(&o->ref_count) == 2);  /* creator + a, which is protected */
    ObDereferenceObject(o);
    end_proc(chi);
    end_proc(par);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 3. closing the parent's handles after inheritance                           */
/* -------------------------------------------------------------------------- */
TEST(parent_close_after_inherit)
{
    struct nt_eprocess *par = new_proc(1020);
    struct nt_eprocess *chi = new_proc(1021);
    ASSERT(par != NULL && chi != NULL);
    struct nt_object_header *o = new_obj();
    ASSERT(o != NULL);

    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateHandle(par, o, 0x1, OBJ_INHERIT, &h), STATUS_SUCCESS);
    ASSERT_STATUS(ObInheritHandleTable(par->handle_table, chi->handle_table, NULL), STATUS_SUCCESS);

    ASSERT_STATUS(ObCloseHandle(par, h), STATUS_SUCCESS);
    ASSERT(!ObHandleIsValid(par, h));
    ASSERT(ObHandleIsValid(chi, h));
    struct nt_object_header *got = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(chi, h, NULL, KernelMode, 0, &got, NULL), STATUS_SUCCESS);
    ASSERT(got == o);
    ObDereferenceObject(got);

    /* the creator's reference goes first; the child's handle keeps the object alive */
    ObDereferenceObject(o);
    ASSERT(atomic64_read(&o->ref_count) == 1);
    ASSERT_STATUS(ObCloseHandle(chi, h), STATUS_SUCCESS);
    end_proc(chi);
    end_proc(par);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 4. a collision rolls the whole copy back                                    */
/* -------------------------------------------------------------------------- */
TEST(collision_rolls_back)
{
    struct nt_eprocess *par = new_proc(1030);
    struct nt_eprocess *chi = new_proc(1031);
    ASSERT(par != NULL && chi != NULL);
    struct nt_object_header *o = new_obj();
    struct nt_object_header *other = new_obj();
    ASSERT(o != NULL && other != NULL);

    /* parent: inheritable handles at 50 and 300 */
    HANDLE p50 = NULL, p300 = NULL;
    ASSERT_STATUS(ObpCreateHandleAtIndex(par->handle_table, o, 0x1, OBJ_INHERIT, 50, &p50), STATUS_SUCCESS);
    ASSERT_STATUS(ObpCreateHandleAtIndex(par->handle_table, o, 0x1, OBJ_INHERIT, 300, &p300), STATUS_SUCCESS);

    /* child: an unrelated handle already sits at 300, so the second copy collides */
    HANDLE c300 = NULL;
    ASSERT_STATUS(ObpCreateHandleAtIndex(chi->handle_table, other, 0x1, 0, 300, &c300), STATUS_SUCCESS);

    long refs_o = (long)atomic64_read(&o->ref_count);
    long hc_o = (long)atomic64_read(&o->handle_count);
    long refs_other = (long)atomic64_read(&other->ref_count);

    ASSERT_STATUS(ObInheritHandleTable(par->handle_table, chi->handle_table, NULL),
                  STATUS_OBJECT_NAME_COLLISION);

    /* the copy at 50 was made and then undone */
    ASSERT(!ObHandleIsValid(chi, p50));
    ASSERT(ObHandleTableCount(chi->handle_table) == 1);
    ASSERT(ObHandleIsValid(chi, c300));
    ASSERT((long)atomic64_read(&o->ref_count) == refs_o);
    ASSERT((long)atomic64_read(&o->handle_count) == hc_o);
    ASSERT((long)atomic64_read(&other->ref_count) == refs_other);

    ASSERT_STATUS(ObCloseHandle(chi, c300), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(par, p50), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(par, p300), STATUS_SUCCESS);
    ObDereferenceObject(o);
    ObDereferenceObject(other);
    end_proc(chi);
    end_proc(par);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 5. empty parent, NULL tables                                                */
/* -------------------------------------------------------------------------- */
TEST(empty_parent_and_bad_arguments)
{
    struct nt_eprocess *par = new_proc(1040);
    struct nt_eprocess *chi = new_proc(1041);
    ASSERT(par != NULL && chi != NULL);

    u32 copied = 7;
    ASSERT_STATUS(ObInheritHandleTable(par->handle_table, chi->handle_table, &copied), STATUS_SUCCESS);
    ASSERT(copied == 0);
    ASSERT(ObHandleTableCount(chi->handle_table) == 0);

    ASSERT_STATUS(ObInheritHandleTable(NULL, chi->handle_table, NULL), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(ObInheritHandleTable(par->handle_table, NULL, NULL), STATUS_INVALID_PARAMETER);

    /* the process-level wrapper keeps its existing checks */
    ASSERT_STATUS(ObInheritHandles(NULL, chi), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(ObInheritHandles(par, chi), STATUS_SUCCESS);
    end_proc(chi);
    end_proc(par);
    PASS();
}

int main(void)
{
    printf("handle inheritance tests starting\n");
    if (ObInitObjectManager() != STATUS_SUCCESS) {
        fprintf(stderr, "ObInitObjectManager failed\n");
        return 1;
    }
    RUN(copies_only_inheritable_with_same_values);
    RUN(reference_counts_and_flags);
    RUN(parent_close_after_inherit);
    RUN(collision_rolls_back);
    RUN(empty_parent_and_bad_arguments);
    printf("handle inheritance tests: %d passed, %d failed\n", g_pass, g_fail);
    ObCleanupObjectManager();
    if (g_fail) return 1;
    if (allocs() != 0) {
        fprintf(stderr, "LEAK: %ld allocations after ObCleanupObjectManager\n", allocs());
        return 1;
    }
    return 0;
}
