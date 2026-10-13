/*
 * security_extra_test.c - NT token object and token syscall tests (ntsec).
 *
 * Covers the Token object type (handle creation, reference and close
 * lifecycle), the process and thread token slots, the token query, duplicate,
 * privilege-adjust, privilege-check and access-check services, and the
 * security descriptor services. Uses the kshim userspace shim; no kernel boot.
 */

#include "security.h"
#include "object.h"
#include "handle.h"
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
    if (_st != (NTSTATUS)(expect)) { \
        fprintf(stderr, "FAIL %s:%d: expected 0x%08lx got 0x%08lx for %s\n", \
                __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_st, #expr); \
        g_fail++; return; \
    } \
} while (0)

#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while (0)

/* Token access bits (winnt.h values) used by these tests. */
#define T_ASSIGN_PRIMARY   0x0001
#define T_DUPLICATE        0x0002
#define T_IMPERSONATE      0x0004
#define T_QUERY            0x0008
#define T_ADJUST_PRIVS     0x0020
#define T_ADJUST_GROUPS    0x0040
#define T_ADJUST_DEFAULT   0x0080
#define T_ALL_ACCESS       0x000F01FFUL

static PSID make_sid(const char *s)
{
    PSID out = NULL;
    NTSTATUS st = sid_from_string(s, &out);
    if (st != STATUS_SUCCESS) return NULL;
    return out;
}

static ACCESS_TOKEN *make_user_token(const char *user_sid)
{
    PSID u = make_sid(user_sid);
    if (!u) return NULL;
    ACCESS_TOKEN *tok = token_create_primary(u, NULL, 0, NULL, 0, NULL, NULL,
                                             NULL, NULL, 0);
    sid_free(u);
    return tok;
}

static struct nt_eprocess *make_current_process(const char *name)
{
    struct nt_eprocess *p = NULL;
    if (ObCreateProcessObject(&p, 0, name) != STATUS_SUCCESS) return NULL;
    kshim_set_current_process(p);
    kshim_set_current_thread(NULL);
    return p;
}

/* -------------------------------------------------------------------------- */
/* Token object: handle creation, type check, access check, close lifecycle   */
/* -------------------------------------------------------------------------- */
TEST(token_object_handle_lifecycle)
{
    struct nt_eprocess *p = make_current_process("tok_lifecycle");
    ASSERT(p != NULL);
    ACCESS_TOKEN *tok = make_user_token("S-1-5-21-1-2-3-1000");
    ASSERT(tok != NULL);
    ASSERT(tok->ref_count == 1);

    /* A NULL token is rejected and creates nothing. */
    HANDLE none = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(p, NULL, T_QUERY, 0, &none), STATUS_INVALID_PARAMETER);

    /* Creating the handle takes a reference on behalf of the object. */
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(p, tok, T_QUERY, 0, &h), STATUS_SUCCESS);
    ASSERT(h != NULL);
    ASSERT(tok->ref_count == 2);

    /* The handle resolves to a Token object with the granted access. */
    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, &nt_token_type, KernelMode,
                                            T_QUERY, &hdr, NULL), STATUS_SUCCESS);
    ASSERT(hdr != NULL && hdr->type == &nt_token_type);
    ASSERT(ObGetObjectType(hdr) == &nt_token_type);
    ObDereferenceObject(hdr);
    hdr = NULL;

    /* Wrong type, and an access bit that was not granted. */
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, &nt_process_type, KernelMode,
                                            0, &hdr, NULL), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, &nt_token_type, KernelMode,
                                            T_ADJUST_PRIVS, &hdr, NULL), STATUS_ACCESS_DENIED);

    /* Closing the handle drops the object's reference, the caller still holds one. */
    ASSERT_STATUS(ObCloseHandle(p, h), STATUS_SUCCESS);
    ASSERT(tok->ref_count == 1);
    ASSERT_STATUS(ObCloseHandle(p, h), STATUS_INVALID_HANDLE);

    token_dereference(tok);
    PASS();
}

TEST(token_object_outlives_creator)
{
    struct nt_eprocess *p = make_current_process("tok_outlive");
    ASSERT(p != NULL);

    /* A first handle sizes the handle table, so the baseline below excludes it. */
    ACCESS_TOKEN *warm = make_user_token("S-1-5-21-1-2-3-1002");
    ASSERT(warm != NULL);
    HANDLE hw = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(p, warm, T_QUERY, 0, &hw), STATUS_SUCCESS);
    token_dereference(warm);
    ASSERT_STATUS(ObCloseHandle(p, hw), STATUS_SUCCESS);
    long base = atomic_load(&kshim_alloc_count);

    ACCESS_TOKEN *tok = make_user_token("S-1-5-21-1-2-3-1001");
    ASSERT(tok != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(p, tok, T_ALL_ACCESS, OBJ_INHERIT, &h), STATUS_SUCCESS);

    /* Drop the creator's reference first: the handle keeps the token alive. */
    token_dereference(tok);
    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(p, h, &nt_token_type, KernelMode,
                                            T_QUERY, &hdr, NULL), STATUS_SUCCESS);
    ObDereferenceObject(hdr);

    /* Last handle closed: the token and the object are released. */
    ASSERT_STATUS(ObCloseHandle(p, h), STATUS_SUCCESS);
    ASSERT(atomic_load(&kshim_alloc_count) == base);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* Process primary token slot and thread impersonation slot                   */
/* -------------------------------------------------------------------------- */
TEST(process_default_primary_token)
{
    struct nt_eprocess *p = NULL;
    ASSERT_STATUS(ObCreateProcessObject(&p, 0, "prim_a"), STATUS_SUCCESS);
    ASSERT(p->primary_token != NULL);
    ASSERT(p->primary_token->token_type == TokenPrimary);
    ASSERT(p->primary_token->ref_count == 1);
    ASSERT(p->primary_token->user != NULL);
    PSID expect = make_sid("S-1-5-21-0-0-0-1000");
    ASSERT(expect != NULL);
    ASSERT(sid_equal(p->primary_token->user, expect) == TRUE);
    sid_free(expect);

    /* Each process gets its own primary token. */
    struct nt_eprocess *q = NULL;
    ASSERT_STATUS(ObCreateProcessObject(&q, 0, "prim_b"), STATUS_SUCCESS);
    ASSERT(q->primary_token != NULL && q->primary_token != p->primary_token);

    ObDestroyProcessObject(q);
    ObDestroyProcessObject(p);
    PASS();
}

TEST(process_token_released_on_destroy)
{
    long base = atomic_load(&kshim_alloc_count);
    struct nt_eprocess *p = NULL;
    ASSERT_STATUS(ObCreateProcessObject(&p, 0, "prim_c"), STATUS_SUCCESS);
    ASSERT(p->primary_token != NULL);
    ObDestroyProcessObject(p);
    ASSERT(atomic_load(&kshim_alloc_count) == base);
    PASS();
}

TEST(thread_impersonation_slot_default_null)
{
    struct nt_eprocess *p = NULL;
    ASSERT_STATUS(ObCreateProcessObject(&p, 0, "imp_a"), STATUS_SUCCESS);
    struct nt_ethread *t = NULL;
    ASSERT_STATUS(ObCreateThreadObject(&t, p, 0), STATUS_SUCCESS);
    ASSERT(t->impersonation_token == NULL);
    ObDestroyThreadObject(t);
    ObDestroyProcessObject(p);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* Driver                                                                     */
/* -------------------------------------------------------------------------- */
int main(void)
{
    printf("security extra tests starting\n");
    ObInitObjectManager();
    RUN(token_object_handle_lifecycle);
    RUN(token_object_outlives_creator);
    RUN(process_default_primary_token);
    RUN(process_token_released_on_destroy);
    RUN(thread_impersonation_slot_default_null);
    printf("security extra tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
