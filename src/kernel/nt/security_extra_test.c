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
#include <stddef.h>

#define PROCESS_ALL_ACCESS_TEST 0x001FFFFFUL
#define THREAD_ALL_ACCESS_TEST  0x001FFFFFUL

/* Syscall entry points (typed host form, see nt_obj.c). */
NTSTATUS nt_svc_NtOpenProcessToken(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess, PHANDLE TokenHandle);
NTSTATUS nt_svc_NtOpenProcessTokenEx(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess, ULONG HandleAttributes, PHANDLE TokenHandle);
NTSTATUS nt_svc_NtOpenThreadToken(HANDLE ThreadHandle, ACCESS_MASK DesiredAccess, BOOLEAN OpenAsSelf, PHANDLE TokenHandle);
NTSTATUS nt_svc_NtOpenThreadTokenEx(HANDLE ThreadHandle, ACCESS_MASK DesiredAccess, BOOLEAN OpenAsSelf, ULONG HandleAttributes, PHANDLE TokenHandle);

/* Layouts of TOKEN_GROUPS and TOKEN_PRIVILEGES (the variable tail is indexed). */
struct tg_hdr { ULONG GroupCount; SID_AND_ATTRIBUTES Groups[1]; };
struct tp_hdr { ULONG PrivilegeCount; LUID_AND_ATTRIBUTES Privileges[1]; };

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
/* NtOpenProcessToken(Ex) / NtOpenThreadToken(Ex) and access checks           */
/* -------------------------------------------------------------------------- */

/* Process whose primary token belongs to user_sid. */
static struct nt_eprocess *make_process_as(const char *name, const char *user_sid)
{
    struct nt_eprocess *p = NULL;
    if (ObCreateProcessObject(&p, 0, name) != STATUS_SUCCESS) return NULL;
    ACCESS_TOKEN *tok = make_user_token(user_sid);
    if (!tok) { ObDestroyProcessObject(p); return NULL; }
    token_dereference(p->primary_token);
    p->primary_token = tok;
    return p;
}

/* Handle to target's process object in from's handle table. */
static HANDLE handle_to_process(struct nt_eprocess *from, struct nt_eprocess *target, ACCESS_MASK access)
{
    HANDLE h = NULL;
    if (ObCreateHandle(from, target->obj, access, 0, &h) != STATUS_SUCCESS) return NULL;
    return h;
}

static void set_default_dacl(ACCESS_TOKEN *t, PACL dacl)
{
    if (t->default_dacl) acl_free(t->default_dacl);
    t->default_dacl = dacl;
}

TEST(open_process_token_same_user)
{
    struct nt_eprocess *caller = make_current_process("opt_caller");
    struct nt_eprocess *target = make_process_as("opt_target", "S-1-5-21-0-0-0-1000");
    ASSERT(caller != NULL && target != NULL);
    HANDLE ph = handle_to_process(caller, target, PROCESS_ALL_ACCESS_TEST);
    ASSERT(ph != NULL);

    LONG before = target->primary_token->ref_count;
    HANDLE th = NULL;
    ASSERT_STATUS(nt_svc_NtOpenProcessToken(ph, TOKEN_QUERY, &th), STATUS_SUCCESS);
    ASSERT(th != NULL);
    ASSERT(target->primary_token->ref_count == before + 1);

    /* The opened handle is a Token object with the granted access. */
    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(caller, th, &nt_token_type, KernelMode,
                                            TOKEN_QUERY, &hdr, NULL), STATUS_SUCCESS);
    ObDereferenceObject(hdr);
    ASSERT_STATUS(ObCloseHandle(caller, th), STATUS_SUCCESS);
    ASSERT(target->primary_token->ref_count == before);

    /* The owner gets TOKEN_ALL_ACCESS and MAXIMUM_ALLOWED. */
    HANDLE th2 = NULL;
    ASSERT_STATUS(nt_svc_NtOpenProcessTokenEx(NtCurrentProcess(), TOKEN_ALL_ACCESS,
                                              OBJ_INHERIT, &th2), STATUS_SUCCESS);
    ASSERT(th2 != NULL);
    ASSERT_STATUS(ObCloseHandle(caller, th2), STATUS_SUCCESS);
    HANDLE th3 = NULL;
    ASSERT_STATUS(nt_svc_NtOpenProcessToken(NtCurrentProcess(), MAXIMUM_ALLOWED, &th3), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, th3), STATUS_SUCCESS);

    ASSERT_STATUS(ObCloseHandle(caller, ph), STATUS_SUCCESS);
    ObDestroyProcessObject(target);
    ObDestroyProcessObject(caller);
    PASS();
}

TEST(open_process_token_other_user_denied)
{
    struct nt_eprocess *caller = make_process_as("opt_caller2", "S-1-5-21-7-7-7-42");
    struct nt_eprocess *target = make_process_as("opt_target2", "S-1-5-21-0-0-0-1000");
    ASSERT(caller != NULL && target != NULL);
    kshim_set_current_process(caller);
    kshim_set_current_thread(NULL);
    HANDLE ph = handle_to_process(caller, target, PROCESS_ALL_ACCESS_TEST);
    ASSERT(ph != NULL);

    HANDLE th = NULL;
    ASSERT_STATUS(nt_svc_NtOpenProcessToken(ph, TOKEN_QUERY, &th), STATUS_ACCESS_DENIED);
    ASSERT(th == NULL);

    ASSERT_STATUS(ObCloseHandle(caller, ph), STATUS_SUCCESS);
    ObDestroyProcessObject(target);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(open_process_token_dacl_denies_query)
{
    struct nt_eprocess *caller = make_current_process("opt_caller3");
    struct nt_eprocess *target = make_process_as("opt_target3", "S-1-5-21-0-0-0-1000");
    ASSERT(caller != NULL && target != NULL);
    HANDLE ph = handle_to_process(caller, target, PROCESS_ALL_ACCESS_TEST);
    ASSERT(ph != NULL);

    /* The owner is denied TOKEN_QUERY and allowed everything else. */
    PSID owner = make_sid("S-1-5-21-0-0-0-1000");
    ASSERT(owner != NULL);
    PACL dacl = acl_create(256);
    ASSERT(dacl != NULL);
    ULONG idx = 0;
    ASSERT_STATUS(acl_add_denied_ace(dacl, TOKEN_QUERY, 0, owner, &idx), STATUS_SUCCESS);
    ASSERT_STATUS(acl_add_allowed_ace(dacl, TOKEN_ALL_ACCESS, 0, owner, &idx), STATUS_SUCCESS);
    set_default_dacl(target->primary_token, dacl);
    sid_free(owner);

    HANDLE th = NULL;
    ASSERT_STATUS(nt_svc_NtOpenProcessToken(ph, TOKEN_QUERY, &th), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(nt_svc_NtOpenProcessToken(ph, TOKEN_DUPLICATE, &th), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, th), STATUS_SUCCESS);

    ASSERT_STATUS(ObCloseHandle(caller, ph), STATUS_SUCCESS);
    ObDestroyProcessObject(target);
    ObDestroyProcessObject(caller);
    PASS();
}

TEST(open_process_token_bad_arguments)
{
    struct nt_eprocess *caller = make_current_process("opt_args");
    ASSERT(caller != NULL);
    HANDLE th = NULL;
    ASSERT_STATUS(nt_svc_NtOpenProcessToken(NULL, TOKEN_QUERY, &th), STATUS_INVALID_HANDLE);
    ASSERT_STATUS(nt_svc_NtOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, NULL),
                  STATUS_INVALID_PARAMETER);

    /* A token handle is not a process handle. */
    ACCESS_TOKEN *tok = make_user_token("S-1-5-21-1-1-1-1");
    ASSERT(tok != NULL);
    HANDLE wrong = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok, TOKEN_ALL_ACCESS, 0, &wrong), STATUS_SUCCESS);
    token_dereference(tok);
    ASSERT_STATUS(nt_svc_NtOpenProcessToken(wrong, TOKEN_QUERY, &th), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT_STATUS(ObCloseHandle(caller, wrong), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    PASS();
}

TEST(open_thread_token_impersonation)
{
    struct nt_eprocess *caller = make_current_process("otk_caller");
    struct nt_eprocess *owner_proc = make_process_as("otk_owner", "S-1-5-21-0-0-0-1000");
    ASSERT(caller != NULL && owner_proc != NULL);
    struct nt_ethread *thr = NULL;
    ASSERT_STATUS(ObCreateThreadObject(&thr, owner_proc, 0), STATUS_SUCCESS);
    HANDLE thh = NULL;
    ASSERT_STATUS(ObCreateHandle(caller, thr->obj, THREAD_ALL_ACCESS_TEST, 0, &thh), STATUS_SUCCESS);

    /* Not impersonating: no thread token. */
    HANDLE tokh = NULL;
    ASSERT_STATUS(nt_svc_NtOpenThreadToken(thh, TOKEN_QUERY, FALSE, &tokh), STATUS_NO_TOKEN);

    /* The thread impersonates a token of another user. */
    PSID other = make_sid("S-1-5-21-5-5-5-5");
    ASSERT(other != NULL);
    thr->impersonation_token = token_create_primary(other, NULL, 0, NULL, 0, NULL, NULL,
                                                    NULL, NULL, 0);
    sid_free(other);
    ASSERT(thr->impersonation_token != NULL);

    /* The caller runs with its primary token (user 1000), which is denied.
     * OpenAsSelf only changes which token the access check uses. */
    ASSERT_STATUS(nt_svc_NtOpenThreadToken(thh, TOKEN_QUERY, FALSE, &tokh), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(nt_svc_NtOpenThreadToken(thh, TOKEN_QUERY, TRUE, &tokh), STATUS_ACCESS_DENIED);

    /* The caller thread impersonates the same user, so the effective token passes. */
    ACCESS_TOKEN *caller_imp = token_create_primary(thr->impersonation_token->user,
                                                    NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, 0);
    ASSERT(caller_imp != NULL);
    struct nt_ethread *self = NULL;
    ASSERT_STATUS(ObCreateThreadObject(&self, caller, 0), STATUS_SUCCESS);
    self->impersonation_token = caller_imp;
    kshim_set_current_thread(self);
    ASSERT_STATUS(nt_svc_NtOpenThreadToken(thh, TOKEN_QUERY, FALSE, &tokh), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, tokh), STATUS_SUCCESS);

    /* NtCurrentThread() is the caller thread, impersonating user 5-5-5-5. With
     * OpenAsSelf the primary token (user 1000) is checked and denied; without it
     * the impersonation token is checked and allowed. */
    ASSERT_STATUS(nt_svc_NtOpenThreadTokenEx(NtCurrentThread(), TOKEN_QUERY, TRUE, OBJ_INHERIT, &tokh),
                  STATUS_ACCESS_DENIED);
    ASSERT_STATUS(nt_svc_NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, FALSE, &tokh), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, tokh), STATUS_SUCCESS);
    kshim_set_current_thread(NULL);

    ASSERT_STATUS(ObCloseHandle(caller, thh), STATUS_SUCCESS);
    ObDestroyThreadObject(self);
    ObDestroyThreadObject(thr);
    ObDestroyProcessObject(owner_proc);
    ObDestroyProcessObject(caller);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* NtQueryInformationToken                                                    */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtQueryInformationToken(HANDLE TokenHandle, ULONG TokenInformationClass,
                                        PVOID TokenInformation, ULONG TokenInformationLength,
                                        PULONG ReturnLength);

/* A primary token with two groups, one privilege, owner, primary group,
 * session 3 and the default medium integrity label. */
static ACCESS_TOKEN *make_rich_token(void)
{
    PSID user = make_sid("S-1-5-21-11-22-33-1001");
    PSID users = make_sid("S-1-5-32-545");
    PSID admins = make_sid("S-1-5-32-544");
    SID_AND_ATTRIBUTES groups[2];
    LUID_AND_ATTRIBUTES priv;
    ACCESS_TOKEN *tok = NULL;
    if (!user || !users || !admins) goto out;
    groups[0].Sid = users;
    groups[0].Attributes = SE_GROUP_ENABLED | SE_GROUP_MANDATORY;
    groups[1].Sid = admins;
    groups[1].Attributes = SE_GROUP_USE_FOR_DENY_ONLY;
    priv.Luid.LowPart = SE_DEBUG_PRIVILEGE;
    priv.Luid.HighPart = 0;
    priv.Attributes = SE_PRIVILEGE_ENABLED;
    tok = token_create_primary(user, groups, 2, &priv, 1, user, users, NULL, NULL, 3);
out:
    if (user) sid_free(user);
    if (users) sid_free(users);
    if (admins) sid_free(admins);
    return tok;
}

static NTSTATUS query_token(HANDLE h, ULONG cls, void *buf, ULONG len, ULONG *ret)
{
    return nt_svc_NtQueryInformationToken(h, cls, buf, len, ret);
}

TEST(query_token_user_groups_privs)
{
    struct nt_eprocess *caller = make_current_process("qtk_caller");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *tok = make_rich_token();
    ASSERT(tok != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok, TOKEN_QUERY, 0, &h), STATUS_SUCCESS);
    token_dereference(tok);

    unsigned char buf[1024];
    ULONG ret = 0;

    /* TokenUser: header followed by the user SID; the Sid pointer targets it. */
    ASSERT_STATUS(query_token(h, TokenUser, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    SID_AND_ATTRIBUTES *u = (SID_AND_ATTRIBUTES *)buf;
    PSID want = make_sid("S-1-5-21-11-22-33-1001");
    ASSERT(want != NULL && u->Sid != NULL && sid_equal(u->Sid, want) == TRUE);
    ASSERT((unsigned char *)u->Sid >= buf && (unsigned char *)u->Sid < buf + sizeof(buf));
    sid_free(want);
    ASSERT(ret >= sizeof(SID_AND_ATTRIBUTES) + sid_length(u->Sid));

    /* TokenGroups: count, then one entry per group with its attributes. */
    ASSERT_STATUS(query_token(h, TokenGroups, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ULONG gcount = *(ULONG *)buf;
    ASSERT(gcount == 2);
    SID_AND_ATTRIBUTES *g = (SID_AND_ATTRIBUTES *)(buf + offsetof(struct tg_hdr, Groups));
    PSID users = make_sid("S-1-5-32-545");
    ASSERT(users != NULL && sid_equal(g[0].Sid, users) == TRUE);
    ASSERT(g[0].Attributes == (SE_GROUP_ENABLED | SE_GROUP_MANDATORY));
    ASSERT(g[1].Attributes == SE_GROUP_USE_FOR_DENY_ONLY);
    sid_free(users);

    /* TokenPrivileges: one entry with the debug LUID, enabled. */
    ASSERT_STATUS(query_token(h, TokenPrivileges, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ULONG pcount = *(ULONG *)buf;
    ASSERT(pcount == 1);
    LUID_AND_ATTRIBUTES *pv = (LUID_AND_ATTRIBUTES *)(buf + offsetof(struct tp_hdr, Privileges));
    ASSERT(pv[0].Luid.LowPart == SE_DEBUG_PRIVILEGE && pv[0].Luid.HighPart == 0);
    ASSERT(pv[0].Attributes == SE_PRIVILEGE_ENABLED);

    ASSERT_STATUS(ObCloseHandle(caller, h), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    PASS();
}

TEST(query_token_owner_group_type_stats_session_il)
{
    struct nt_eprocess *caller = make_current_process("qtk_more");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *tok = make_rich_token();
    ASSERT(tok != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok, TOKEN_QUERY, 0, &h), STATUS_SUCCESS);
    token_dereference(tok);
    unsigned char buf[512];
    ULONG ret = 0;

    /* TokenOwner and TokenPrimaryGroup: a pointer to a SID stored in the buffer. */
    ASSERT_STATUS(query_token(h, TokenOwner, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    PSID want = make_sid("S-1-5-21-11-22-33-1001");
    ASSERT(want != NULL && sid_equal(*(PSID *)buf, want) == TRUE);
    sid_free(want);
    ASSERT_STATUS(query_token(h, TokenPrimaryGroup, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    want = make_sid("S-1-5-32-545");
    ASSERT(want != NULL && sid_equal(*(PSID *)buf, want) == TRUE);
    sid_free(want);

    /* TokenType and TokenImpersonationLevel. */
    ASSERT_STATUS(query_token(h, TokenType, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ASSERT(*(ULONG *)buf == TokenPrimary);
    ASSERT(ret == sizeof(ULONG));
    ASSERT_STATUS(query_token(h, TokenImpersonationLevel, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ASSERT(*(ULONG *)buf == SecurityAnonymous);

    /* TokenStatistics: counts and the token type. */
    ASSERT_STATUS(query_token(h, TokenStatistics, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    struct {
        ULONG TokenIdLow; LONG TokenIdHigh;
        ULONG AuthIdLow;  LONG AuthIdHigh;
        long long Expiration;
        ULONG TokenType; ULONG ImpLevel; ULONG DynCharged; ULONG DynAvail;
        ULONG GroupCount; ULONG PrivilegeCount;
        ULONG ModLow; LONG ModHigh;
    } *stats = (void *)buf;
    ASSERT(stats->TokenType == TokenPrimary);
    ASSERT(stats->GroupCount == 2);
    ASSERT(stats->PrivilegeCount == 1);
    ASSERT(ret == sizeof(*stats));

    /* TokenSessionId and TokenIntegrityLevel (default medium). */
    ASSERT_STATUS(query_token(h, TokenSessionId, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ASSERT(*(ULONG *)buf == 3);
    ASSERT_STATUS(query_token(h, TokenIntegrityLevel, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    SID_AND_ATTRIBUTES *label = (SID_AND_ATTRIBUTES *)buf;
    PSID medium = sid_well_known(SID_WK_IL_MEDIUM);
    ASSERT(medium != NULL && sid_equal(label->Sid, medium) == TRUE);
    sid_free(medium);

    ASSERT_STATUS(ObCloseHandle(caller, h), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    PASS();
}

/* TokenStatistics TokenId is a LUID from a counter: it must not be the token's
 * address (the old value leaked the low bits of a kernel pointer). Ids are
 * distinct and increase from one token to a later duplicate. */
struct tstats_view {
    ULONG IdLow; LONG IdHigh;
    ULONG AuthLow; LONG AuthHigh;
    long long Expiration;
    ULONG TokenType; ULONG ImpLevel; ULONG DynCharged; ULONG DynAvail;
    ULONG GroupCount; ULONG PrivilegeCount;
    ULONG ModLow; LONG ModHigh;
};

static unsigned long long token_stats_id(HANDLE h)
{
    unsigned char buf[sizeof(struct tstats_view)];
    ULONG ret = 0;
    struct tstats_view v;
    if (!NT_SUCCESS(query_token(h, TokenStatistics, buf, sizeof(buf), &ret))) return 0;
    memcpy(&v, buf, sizeof(v));
    return ((unsigned long long)(ULONG)v.IdHigh << 32) | v.IdLow;
}

TEST(token_statistics_id_is_counter_not_address)
{
    struct nt_eprocess *caller = make_current_process("tid_caller");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *a = make_rich_token();
    ASSERT(a != NULL);
    ULONG a_low_addr = (ULONG)(uintptr_t)a;
    HANDLE ha = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, a, TOKEN_QUERY, 0, &ha), STATUS_SUCCESS);
    ACCESS_TOKEN *b = token_duplicate(a);
    ASSERT(b != NULL);
    HANDLE hb = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, b, TOKEN_QUERY, 0, &hb), STATUS_SUCCESS);
    token_dereference(a);
    token_dereference(b);

    unsigned long long id_a = token_stats_id(ha);
    unsigned long long id_b = token_stats_id(hb);
    ASSERT(id_a != 0);
    ASSERT((ULONG)id_a != a_low_addr);
    ASSERT(id_b > id_a);
    PASS();
}

TEST(query_token_buffer_and_errors)
{
    struct nt_eprocess *caller = make_current_process("qtk_err");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *tok = make_rich_token();
    ASSERT(tok != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok, TOKEN_QUERY, 0, &h), STATUS_SUCCESS);
    token_dereference(tok);
    unsigned char buf[512];
    ULONG ret = 0;

    /* Short buffer: BUFFER_TOO_SMALL with the required length. */
    ASSERT_STATUS(query_token(h, TokenGroups, buf, 4, &ret), STATUS_BUFFER_TOO_SMALL);
    ASSERT(ret > 4);
    ULONG needed = ret;
    ASSERT_STATUS(query_token(h, TokenGroups, buf, needed, &ret), STATUS_SUCCESS);
    ASSERT(ret == needed);

    /* Unknown class. */
    ASSERT_STATUS(query_token(h, 999, buf, sizeof(buf), &ret), STATUS_INVALID_INFO_CLASS);
    /* Invalid handle, and a handle without TOKEN_QUERY. */
    ASSERT_STATUS(query_token(NULL, TokenUser, buf, sizeof(buf), &ret), STATUS_INVALID_HANDLE);
    HANDLE noq = NULL;
    ACCESS_TOKEN *tok2 = make_user_token("S-1-5-21-1-2-3-9");
    ASSERT(tok2 != NULL);
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok2, TOKEN_ASSIGN_PRIMARY, 0, &noq), STATUS_SUCCESS);
    token_dereference(tok2);
    ASSERT_STATUS(query_token(noq, TokenUser, buf, sizeof(buf), &ret), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(ObCloseHandle(caller, noq), STATUS_SUCCESS);

    ASSERT_STATUS(ObCloseHandle(caller, h), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* NtDuplicateToken                                                           */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtDuplicateToken(HANDLE ExistingTokenHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN EffectiveOnly,
                                 TOKEN_TYPE TokenType, PHANDLE NewTokenHandle);

/* Current process whose primary token is user_sid (the rich token's user, so
 * the owner-based DACL of duplicates admits the caller). */
static struct nt_eprocess *make_current_process_as(const char *name, const char *user_sid)
{
    struct nt_eprocess *p = make_process_as(name, user_sid);
    if (p) {
        kshim_set_current_process(p);
        kshim_set_current_thread(NULL);
    }
    return p;
}

static ACCESS_TOKEN *token_of_handle(struct nt_eprocess *p, HANDLE h)
{
    struct nt_object_header *hdr = NULL;
    ACCESS_TOKEN *tok = NULL;
    if (ObReferenceObjectByHandle(p, h, &nt_token_type, KernelMode, 0, &hdr, NULL) != STATUS_SUCCESS)
        return NULL;
    tok = ((struct nt_token_body *)nt_object_body(hdr))->token;
    ObDereferenceObject(hdr);
    return tok;
}

TEST(duplicate_primary_token)
{
    struct nt_eprocess *caller = make_current_process_as("dup_caller", "S-1-5-21-11-22-33-1001");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *src = make_rich_token();
    ASSERT(src != NULL);
    HANDLE hs = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, src, TOKEN_ALL_ACCESS, 0, &hs), STATUS_SUCCESS);
    token_dereference(src);

    HANDLE hd = NULL;
    ASSERT_STATUS(nt_svc_NtDuplicateToken(hs, TOKEN_QUERY | TOKEN_DUPLICATE, NULL, FALSE,
                                          TokenPrimary, &hd), STATUS_SUCCESS);
    ASSERT(hd != NULL && hd != hs);
    ASSERT(token_of_handle(caller, hd) != token_of_handle(caller, hs));

    unsigned char buf[64];
    ULONG ret = 0;
    ASSERT_STATUS(query_token(hd, TokenType, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ASSERT(*(ULONG *)buf == TokenPrimary);

    ASSERT_STATUS(ObCloseHandle(caller, hd), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, hs), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(duplicate_impersonation_token)
{
    struct nt_eprocess *caller = make_current_process_as("dup_imp", "S-1-5-21-11-22-33-1001");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *src = make_rich_token();
    ASSERT(src != NULL);
    HANDLE hs = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, src, TOKEN_ALL_ACCESS, 0, &hs), STATUS_SUCCESS);
    token_dereference(src);

    HANDLE hi = NULL;
    ASSERT_STATUS(nt_svc_NtDuplicateToken(hs, TOKEN_QUERY, NULL, FALSE, TokenImpersonation, &hi),
                  STATUS_SUCCESS);
    unsigned char buf[64];
    ULONG ret = 0;
    ASSERT_STATUS(query_token(hi, TokenType, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ASSERT(*(ULONG *)buf == TokenImpersonation);
    ASSERT_STATUS(query_token(hi, TokenImpersonationLevel, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ASSERT(*(ULONG *)buf == SecurityImpersonation);

    ASSERT_STATUS(ObCloseHandle(caller, hi), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, hs), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(duplicate_is_independent_of_source)
{
    struct nt_eprocess *caller = make_current_process_as("dup_indep", "S-1-5-21-11-22-33-1001");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *src = make_rich_token();
    ASSERT(src != NULL);
    HANDLE hs = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, src, TOKEN_ALL_ACCESS, 0, &hs), STATUS_SUCCESS);
    token_dereference(src);

    HANDLE hd = NULL;
    ASSERT_STATUS(nt_svc_NtDuplicateToken(hs, TOKEN_ALL_ACCESS, NULL, FALSE, TokenPrimary, &hd),
                  STATUS_SUCCESS);
    ACCESS_TOKEN *dup = token_of_handle(caller, hd);
    ASSERT(dup != NULL);
    ASSERT_STATUS(token_adjust_privileges(dup, TRUE, NULL, 0, 0), STATUS_SUCCESS);

    unsigned char buf[256];
    ULONG ret = 0;
    ASSERT_STATUS(query_token(hs, TokenPrivileges, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    LUID_AND_ATTRIBUTES *pv = (LUID_AND_ATTRIBUTES *)(buf + offsetof(struct tp_hdr, Privileges));
    ASSERT(pv[0].Attributes == SE_PRIVILEGE_ENABLED);
    ASSERT_STATUS(query_token(hd, TokenPrivileges, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    pv = (LUID_AND_ATTRIBUTES *)(buf + offsetof(struct tp_hdr, Privileges));
    ASSERT((pv[0].Attributes & SE_PRIVILEGE_ENABLED) == 0);

    ASSERT_STATUS(ObCloseHandle(caller, hd), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, hs), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(duplicate_effective_only)
{
    struct nt_eprocess *caller = make_current_process_as("dup_eff", "S-1-5-21-11-22-33-1001");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *src = make_rich_token();
    ASSERT(src != NULL);
    HANDLE hs = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, src, TOKEN_ALL_ACCESS, 0, &hs), STATUS_SUCCESS);
    token_dereference(src);

    /* The source has two groups; only one is enabled. */
    HANDLE he = NULL;
    ASSERT_STATUS(nt_svc_NtDuplicateToken(hs, TOKEN_QUERY, NULL, TRUE, TokenPrimary, &he),
                  STATUS_SUCCESS);
    unsigned char buf[256];
    ULONG ret = 0;
    ASSERT_STATUS(query_token(he, TokenGroups, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ASSERT(*(ULONG *)buf == 1);
    ASSERT_STATUS(query_token(hs, TokenGroups, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    ASSERT(*(ULONG *)buf == 2);

    ASSERT_STATUS(ObCloseHandle(caller, he), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, hs), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(duplicate_token_rights_and_arguments)
{
    struct nt_eprocess *caller = make_current_process_as("dup_args", "S-1-5-21-11-22-33-1001");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *src = make_rich_token();
    ASSERT(src != NULL);
    HANDLE hq = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, src, TOKEN_QUERY, 0, &hq), STATUS_SUCCESS);
    token_dereference(src);

    HANDLE hd = NULL;
    /* No TOKEN_DUPLICATE on the source handle. */
    ASSERT_STATUS(nt_svc_NtDuplicateToken(hq, TOKEN_QUERY, NULL, FALSE, TokenPrimary, &hd),
                  STATUS_ACCESS_DENIED);
    ASSERT(hd == NULL);

    /* Invalid token type and NULL output. */
    ASSERT_STATUS(nt_svc_NtDuplicateToken(hq, TOKEN_QUERY, NULL, FALSE, (TOKEN_TYPE)3, &hd),
                  STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtDuplicateToken(hq, TOKEN_QUERY, NULL, FALSE, TokenPrimary, NULL),
                  STATUS_INVALID_PARAMETER);

    /* OBJ_INHERIT in ObjectAttributes is carried onto the new handle. */
    ACCESS_TOKEN *src2 = make_rich_token();
    ASSERT(src2 != NULL);
    HANDLE hs2 = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, src2, TOKEN_ALL_ACCESS, 0, &hs2), STATUS_SUCCESS);
    token_dereference(src2);
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.Attributes = OBJ_INHERIT;
    ASSERT_STATUS(nt_svc_NtDuplicateToken(hs2, TOKEN_QUERY, &oa, FALSE, TokenPrimary, &hd),
                  STATUS_SUCCESS);
    ASSERT(hd != NULL);
    ASSERT_STATUS(ObCloseHandle(caller, hd), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, hs2), STATUS_SUCCESS);
    ASSERT_STATUS(ObCloseHandle(caller, hq), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* NtAdjustPrivilegesToken, NtPrivilegeCheck, NtAccessCheck                   */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtAdjustPrivilegesToken(HANDLE TokenHandle, BOOLEAN DisableAllPrivileges,
                                        PTOKEN_PRIVILEGES NewState, ULONG BufferLength,
                                        PTOKEN_PRIVILEGES PreviousState, PULONG ReturnLength);
NTSTATUS nt_svc_NtPrivilegeCheck(HANDLE ClientToken, PPRIVILEGE_SET RequiredPrivileges,
                                 PBOOLEAN Result);
NTSTATUS nt_svc_NtAccessCheck(PVOID SecurityDescriptor, HANDLE ClientToken,
                              ACCESS_MASK DesiredAccess, PGENERIC_MAPPING GenericMapping,
                              PPRIVILEGE_SET PrivilegeSet, PULONG PrivilegeSetLength,
                              PACCESS_MASK GrantedAccess, PNTSTATUS AccessStatus);

/* Token with privilege 20 (debug) disabled and privilege 5 enabled. */
static ACCESS_TOKEN *make_priv_token(const char *user_sid)
{
    PSID u = make_sid(user_sid);
    LUID_AND_ATTRIBUTES pv[2];
    ACCESS_TOKEN *tok = NULL;
    if (!u) return NULL;
    pv[0].Luid.LowPart = SE_DEBUG_PRIVILEGE; pv[0].Luid.HighPart = 0; pv[0].Attributes = 0;
    pv[1].Luid.LowPart = 5; pv[1].Luid.HighPart = 0;
    pv[1].Attributes = SE_PRIVILEGE_ENABLED;
    tok = token_create_primary(u, NULL, 0, pv, 2, NULL, NULL, NULL, NULL, 0);
    sid_free(u);
    return tok;
}

static ULONG priv_attr_of(HANDLE h, ULONG luid_low)
{
    unsigned char buf[256];
    ULONG ret = 0;
    ULONG n, i;
    LUID_AND_ATTRIBUTES *pv;
    if (query_token(h, TokenPrivileges, buf, sizeof(buf), &ret) != STATUS_SUCCESS) return 0xFFFFFFFFUL;
    n = *(ULONG *)buf;
    pv = (LUID_AND_ATTRIBUTES *)(buf + offsetof(struct tp_hdr, Privileges));
    for (i = 0; i < n; i++)
        if (pv[i].Luid.LowPart == luid_low) return pv[i].Attributes;
    return 0xFFFFFFFEUL;
}

TEST(adjust_privileges_enable_disable)
{
    struct nt_eprocess *caller = make_current_process_as("adj_caller", "S-1-5-21-0-0-0-1000");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *tok = make_priv_token("S-1-5-21-0-0-0-1000");
    ASSERT(tok != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok, TOKEN_ALL_ACCESS, 0, &h), STATUS_SUCCESS);
    token_dereference(tok);
    ULONG dbg = SE_DEBUG_PRIVILEGE, pagefile = 5; /* privilege 5 starts enabled */

    /* Enable the debug privilege: all assigned. */
    struct { ULONG n; LUID_AND_ATTRIBUTES p[1]; } ns;
    ns.n = 1; ns.p[0].Luid.LowPart = dbg; ns.p[0].Luid.HighPart = 0; ns.p[0].Attributes = SE_PRIVILEGE_ENABLED;
    ASSERT_STATUS(nt_svc_NtAdjustPrivilegesToken(h, FALSE, (PTOKEN_PRIVILEGES)&ns, sizeof(ns), NULL, NULL),
                  STATUS_SUCCESS);
    ASSERT(priv_attr_of(h, dbg) == SE_PRIVILEGE_ENABLED);

    /* Disable it again. BufferLength is the PreviousState size: too small
     * gives BUFFER_TOO_SMALL with the required length, and nothing changes. */
    struct { ULONG n; LUID_AND_ATTRIBUTES p[1]; } ns2, prev;
    ns2.n = 1; ns2.p[0].Luid.LowPart = dbg; ns2.p[0].Luid.HighPart = 0; ns2.p[0].Attributes = 0;
    ULONG need = 0;
    ASSERT_STATUS(nt_svc_NtAdjustPrivilegesToken(h, FALSE, (PTOKEN_PRIVILEGES)&ns2, 4,
                                                 (PTOKEN_PRIVILEGES)&prev, &need), STATUS_BUFFER_TOO_SMALL);
    ASSERT(need == offsetof(struct tp_hdr, Privileges) + sizeof(LUID_AND_ATTRIBUTES));
    ASSERT(priv_attr_of(h, dbg) == SE_PRIVILEGE_ENABLED);

    /* Large enough: the change applies and PreviousState holds the old value (enabled). */
    ASSERT_STATUS(nt_svc_NtAdjustPrivilegesToken(h, FALSE, (PTOKEN_PRIVILEGES)&ns2, sizeof(prev),
                                                 (PTOKEN_PRIVILEGES)&prev, &need), STATUS_SUCCESS);
    ASSERT(priv_attr_of(h, dbg) == 0);
    ASSERT(prev.n == 1 && prev.p[0].Luid.LowPart == dbg);
    ASSERT(prev.p[0].Attributes == SE_PRIVILEGE_ENABLED);

    /* Disable a present privilege and name an absent one: NOT_ALL_ASSIGNED,
     * the present one is still changed. */
    struct { ULONG n; LUID_AND_ATTRIBUTES p[2]; } ns3;
    ns3.n = 2;
    ns3.p[0].Luid.LowPart = pagefile; ns3.p[0].Luid.HighPart = 0; ns3.p[0].Attributes = 0;
    ns3.p[1].Luid.LowPart = 99; ns3.p[1].Luid.HighPart = 0; ns3.p[1].Attributes = SE_PRIVILEGE_ENABLED;
    ASSERT_STATUS(nt_svc_NtAdjustPrivilegesToken(h, FALSE, (PTOKEN_PRIVILEGES)&ns3, sizeof(ns3), NULL, NULL),
                  STATUS_NOT_ALL_ASSIGNED);
    ASSERT(priv_attr_of(h, pagefile) == 0);

    /* DisableAllPrivileges clears every enable bit. */
    ASSERT_STATUS(nt_svc_NtAdjustPrivilegesToken(h, TRUE, NULL, 0, NULL, NULL), STATUS_SUCCESS);
    ASSERT(priv_attr_of(h, dbg) == 0);
    ASSERT(priv_attr_of(h, pagefile) == 0);

    /* A NULL NewState without DisableAll is an invalid parameter. */
    ASSERT_STATUS(nt_svc_NtAdjustPrivilegesToken(h, FALSE, NULL, 0, NULL, NULL), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(ObCloseHandle(caller, h), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(adjust_privileges_needs_adjust_right)
{
    struct nt_eprocess *caller = make_current_process_as("adj_rights", "S-1-5-21-0-0-0-1000");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *tok = make_priv_token("S-1-5-21-0-0-0-1000");
    ASSERT(tok != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok, TOKEN_QUERY, 0, &h), STATUS_SUCCESS);
    token_dereference(tok);
    ASSERT_STATUS(nt_svc_NtAdjustPrivilegesToken(h, TRUE, NULL, 0, NULL, NULL), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(ObCloseHandle(caller, h), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(privilege_check_all_and_any)
{
    struct nt_eprocess *caller = make_current_process_as("pchk", "S-1-5-21-0-0-0-1000");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *tok = make_priv_token("S-1-5-21-0-0-0-1000");
    ASSERT(tok != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok, TOKEN_QUERY, 0, &h), STATUS_SUCCESS);
    token_dereference(tok);
    BOOLEAN result = 0;

    /* Enabled privilege alone: TRUE. */
    struct { ULONG n; ULONG ctl; LUID_AND_ATTRIBUTES p[2]; } rq;
    rq.n = 1; rq.ctl = 1 /* PRIVILEGE_SET_ALL_NECESSARY */;
    rq.p[0].Luid.LowPart = 5; rq.p[0].Luid.HighPart = 0; rq.p[0].Attributes = 0;
    ASSERT_STATUS(nt_svc_NtPrivilegeCheck(h, (PPRIVILEGE_SET)&rq, &result), STATUS_SUCCESS);
    ASSERT(result == TRUE);

    /* Enabled and disabled together: ALL_NECESSARY is FALSE, ANY is TRUE. */
    rq.n = 2; rq.ctl = 1;
    rq.p[1].Luid.LowPart = SE_DEBUG_PRIVILEGE; rq.p[1].Luid.HighPart = 0; rq.p[1].Attributes = 0;
    ASSERT_STATUS(nt_svc_NtPrivilegeCheck(h, (PPRIVILEGE_SET)&rq, &result), STATUS_SUCCESS);
    ASSERT(result == FALSE);
    rq.ctl = 0;
    ASSERT_STATUS(nt_svc_NtPrivilegeCheck(h, (PPRIVILEGE_SET)&rq, &result), STATUS_SUCCESS);
    ASSERT(result == TRUE);

    /* Disabled privilege alone is FALSE even when held. */
    rq.n = 1; rq.ctl = 1;
    rq.p[0].Luid.LowPart = SE_DEBUG_PRIVILEGE; rq.p[0].Luid.HighPart = 0;
    ASSERT_STATUS(nt_svc_NtPrivilegeCheck(h, (PPRIVILEGE_SET)&rq, &result), STATUS_SUCCESS);
    ASSERT(result == FALSE);

    ASSERT_STATUS(ObCloseHandle(caller, h), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
    PASS();
}

/* Builds a self-relative SD with one allow ACE for user_sid, mask allow_mask. */
static void *make_allow_sd(const char *user_sid, ACCESS_MASK allow_mask, ULONG *out_len)
{
    PSID u = make_sid(user_sid);
    PACL dacl = acl_create(256);
    ULONG idx = 0;
    SECURITY_DESCRIPTOR *sd;
    void *buf = NULL;
    ULONG len = 0;
    if (!u || !dacl) return NULL;
    acl_add_allowed_ace(dacl, allow_mask, 0, u, &idx);
    sd = sd_create_absolute(u, NULL, dacl, NULL, SE_DACL_PRESENT);
    if (sd && sd_to_self_relative(sd, &buf, &len) == STATUS_SUCCESS) {
        *out_len = len;
    } else {
        buf = NULL;
    }
    sd_free(sd);
    acl_free(dacl);
    sid_free(u);
    return buf;
}

TEST(access_check_self_relative_sd)
{
    struct nt_eprocess *caller = make_current_process_as("acc_caller", "S-1-5-21-0-0-0-1000");
    ASSERT(caller != NULL);
    ACCESS_TOKEN *tok = make_user_token("S-1-5-21-0-0-0-1000");
    ASSERT(tok != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok, TOKEN_QUERY, 0, &h), STATUS_SUCCESS);
    token_dereference(tok);

    /* SD allows the caller's user 0x1 only; the mapping sends GENERIC_READ to 0x1. */
    ULONG sdlen = 0;
    void *sdbuf = make_allow_sd("S-1-5-21-0-0-0-1000", 0x1, &sdlen);
    ASSERT(sdbuf != NULL && sdlen > 0);
    GENERIC_MAPPING gm = { 0x1, 0x2, 0x4, 0x7 };
    ACCESS_MASK granted = 0xDEAD;
    NTSTATUS access_status = 0x12345678;

    ASSERT_STATUS(nt_svc_NtAccessCheck(sdbuf, h, 0x1, &gm, NULL, NULL, &granted, &access_status),
                  STATUS_SUCCESS);
    ASSERT(access_status == STATUS_SUCCESS && granted == 0x1);

    ASSERT_STATUS(nt_svc_NtAccessCheck(sdbuf, h, 0x80000000UL /* GENERIC_READ */, &gm, NULL, NULL,
                                       &granted, &access_status), STATUS_SUCCESS);
    ASSERT(access_status == STATUS_SUCCESS && granted == 0x1);

    /* Right not allowed: AccessStatus is ACCESS_DENIED and nothing is granted. */
    ASSERT_STATUS(nt_svc_NtAccessCheck(sdbuf, h, 0x2, &gm, NULL, NULL, &granted, &access_status),
                  STATUS_SUCCESS);
    ASSERT(access_status == STATUS_ACCESS_DENIED && granted == 0);

    /* The privilege-set output needs its header: a shorter length is BUFFER_TOO_SMALL. */
    ULONG psl = 1;
    ASSERT_STATUS(nt_svc_NtAccessCheck(sdbuf, h, 0x1, &gm, NULL, &psl, &granted, &access_status),
                  STATUS_BUFFER_TOO_SMALL);
    ASSERT(psl == offsetof(PRIVILEGE_SET, Privilege));

    /* A token without TOKEN_QUERY cannot be used as the client token. */
    ACCESS_TOKEN *tok2 = make_user_token("S-1-5-21-0-0-0-1000");
    ASSERT(tok2 != NULL);
    HANDLE hn = NULL;
    ASSERT_STATUS(ObCreateTokenHandle(caller, tok2, TOKEN_DUPLICATE, 0, &hn), STATUS_SUCCESS);
    token_dereference(tok2);
    ASSERT_STATUS(nt_svc_NtAccessCheck(sdbuf, hn, 0x1, &gm, NULL, NULL, &granted, &access_status),
                  STATUS_ACCESS_DENIED);
    ASSERT_STATUS(ObCloseHandle(caller, hn), STATUS_SUCCESS);

    kfree(sdbuf);
    ASSERT_STATUS(ObCloseHandle(caller, h), STATUS_SUCCESS);
    ObDestroyProcessObject(caller);
    kshim_set_current_process(NULL);
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
    RUN(open_process_token_same_user);
    RUN(open_process_token_other_user_denied);
    RUN(open_process_token_dacl_denies_query);
    RUN(open_process_token_bad_arguments);
    RUN(open_thread_token_impersonation);
    RUN(query_token_user_groups_privs);
    RUN(query_token_owner_group_type_stats_session_il);
    RUN(token_statistics_id_is_counter_not_address);
    RUN(query_token_buffer_and_errors);
    RUN(duplicate_primary_token);
    RUN(duplicate_impersonation_token);
    RUN(duplicate_is_independent_of_source);
    RUN(duplicate_effective_only);
    RUN(duplicate_token_rights_and_arguments);
    RUN(adjust_privileges_enable_disable);
    RUN(adjust_privileges_needs_adjust_right);
    RUN(privilege_check_all_and_any);
    RUN(access_check_self_relative_sd);
    printf("security extra tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
