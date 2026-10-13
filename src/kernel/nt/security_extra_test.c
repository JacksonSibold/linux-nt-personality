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
NTSTATUS nt_svc_NtQuerySecurityObject(HANDLE Handle, ULONG SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG Length, PULONG LengthNeeded);
NTSTATUS nt_svc_NtSetSecurityObject(HANDLE Handle, ULONG SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor);

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
/* -------------------------------------------------------------------------- */
/* Security descriptor services: NtQuerySecurityObject, NtSetSecurityObject   */
/* -------------------------------------------------------------------------- */

#define SDT_U1 "S-1-5-21-0-0-0-1000"
#define SDT_U2 "S-1-5-21-0-0-0-2000"
#define SDT_U3 "S-1-5-21-0-0-0-3000"
#define SDT_G1 "S-1-5-21-0-0-0-513"

/* Self-relative SD: owner, group, and a DACL with up to two allowed ACEs. */
static void *sdt_make_sd(const char *owner, const char *group,
                         const char *ace1, ACCESS_MASK mask1,
                         const char *ace2, ACCESS_MASK mask2, ULONG *out_len)
{
    PSID o = owner ? make_sid(owner) : NULL;
    PSID g = group ? make_sid(group) : NULL;
    PSID u1 = make_sid(ace1);
    PSID u2 = ace2 ? make_sid(ace2) : NULL;
    PACL dacl = acl_create(512);
    SECURITY_DESCRIPTOR *sd = NULL;
    void *buf = NULL;
    ULONG idx = 0;
    if (!u1 || !dacl || (ace2 && !u2) || (owner && !o) || (group && !g)) {
        buf = NULL;
    } else {
        acl_add_allowed_ace(dacl, mask1, 0, u1, &idx);
        if (u2) acl_add_allowed_ace(dacl, mask2, 0, u2, &idx);
        sd = sd_create_absolute(o, g, dacl, NULL, SE_DACL_PRESENT);
        if (!sd || sd_to_self_relative(sd, &buf, out_len) != STATUS_SUCCESS)
            buf = NULL;
    }
    sd_free(sd);
    if (dacl) acl_free(dacl);
    if (u1) sid_free(u1);
    if (u2) sid_free(u2);
    if (o) sid_free(o);
    if (g) sid_free(g);
    return buf;
}

/* Frees the components sd_from_self_relative allocated, and the struct. */
static void sdt_free_abs(SECURITY_DESCRIPTOR *sd)
{
    if (!sd) return;
    if (sd->Owner) sid_free(sd->Owner);
    if (sd->Group) sid_free(sd->Group);
    if (sd->Dacl) acl_free(sd->Dacl);
    if (sd->Sacl) acl_free(sd->Sacl);
    sd_free(sd);
}

/* A Test object in p's handle table with the given access. The creator's
 * reference is returned in *hdr_out; sdt_close releases both. */
static HANDLE sdt_object(struct nt_eprocess *p, ACCESS_MASK access,
                         struct nt_object_header **hdr_out)
{
    struct nt_object_header *h = NULL;
    void *b = NULL;
    HANDLE hd = NULL;
    if (ObCreateObject(&nt_test_type, NULL, sizeof(struct nt_test_body), &h, &b) != STATUS_SUCCESS)
        return NULL;
    if (ObInsertObject(h, NULL, access, p, &hd) != STATUS_SUCCESS) {
        ObDereferenceObject(h);
        return NULL;
    }
    *hdr_out = h;
    return hd;
}

static void sdt_close(struct nt_eprocess *p, HANDLE hd, struct nt_object_header *h)
{
    ObCloseHandle(p, hd);
    ObMakeTemporaryObject(h);
    ObDereferenceObject(h);
}

/* Sets an object SD through the object layer (the kernel-side attach path). */
static NTSTATUS sdt_attach(struct nt_object_header *h, ULONG len, void *sd)
{
    NTSTATUS st = ObSetObjectSecurityDescriptor(h, sd, len);
    return st;
}

TEST(query_sd_info_selection)
{
    struct nt_eprocess *p = make_current_process_as("sq_sel", SDT_U1);
    ASSERT(p != NULL);
    struct nt_object_header *h = NULL;
    HANDLE hd = sdt_object(p, READ_CONTROL | WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY, &h);
    ASSERT(hd != NULL);
    ULONG full = 0;
    void *sd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL | WRITE_DAC, NULL, 0, &full);
    ASSERT(sd != NULL && full > 0);
    ASSERT_STATUS(sdt_attach(h, full, sd), STATUS_SUCCESS);
    kfree(sd);

    UCHAR buf[512];
    ULONG need = 0;
    const SECURITY_DESCRIPTOR_RELATIVE *r = (const SECURITY_DESCRIPTOR_RELATIVE *)buf;
    SECURITY_DESCRIPTOR *abs = NULL;

    /* Owner only: the output carries an owner and no other component. */
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, OWNER_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_SUCCESS);
    ASSERT(r->Revision == 1 && (r->Control & SE_SELF_RELATIVE));
    ASSERT(r->Owner != 0 && r->Group == 0 && r->Dacl == 0 && r->Sacl == 0);
    ASSERT(need < full);
    ASSERT_STATUS(sd_from_self_relative(buf, need, &abs), STATUS_SUCCESS);
    PSID want_owner = make_sid(SDT_U1);
    ASSERT(want_owner != NULL && sid_equal(abs->Owner, want_owner));
    sid_free(want_owner);
    sdt_free_abs(abs);
    abs = NULL;

    /* Group only. */
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, GROUP_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_SUCCESS);
    ASSERT(r->Group != 0 && r->Owner == 0 && r->Dacl == 0 && r->Sacl == 0);

    /* DACL only: the ACE survives the round trip. */
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, DACL_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_SUCCESS);
    ASSERT(r->Dacl != 0 && r->Owner == 0 && r->Group == 0 && r->Sacl == 0);
    ASSERT_STATUS(sd_from_self_relative(buf, need, &abs), STATUS_SUCCESS);
    ASSERT(abs->Dacl != NULL && abs->Dacl->AceCount == 1);
    sdt_free_abs(abs);
    abs = NULL;

    /* Everything requested reproduces the stored descriptor's size. */
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, OWNER_SECURITY_INFORMATION |
                                               GROUP_SECURITY_INFORMATION |
                                               DACL_SECURITY_INFORMATION,
                                               buf, sizeof(buf), &need), STATUS_SUCCESS);
    ASSERT(need == full);
    ASSERT(r->Owner != 0 && r->Group != 0 && r->Dacl != 0 && r->Sacl == 0);

    sdt_close(p, hd, h);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(query_sd_buffer_too_small)
{
    struct nt_eprocess *p = make_current_process_as("sq_buf", SDT_U1);
    ASSERT(p != NULL);
    struct nt_object_header *h = NULL;
    HANDLE hd = sdt_object(p, READ_CONTROL, &h);
    ASSERT(hd != NULL);
    ULONG full = 0;
    void *sd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL, NULL, 0, &full);
    ASSERT(sd != NULL);
    ASSERT_STATUS(sdt_attach(h, full, sd), STATUS_SUCCESS);
    kfree(sd);

    ULONG all = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    UCHAR buf[512];
    ULONG need = 0;
    /* A probe with no buffer reports the required length. */
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, all, NULL, 0, &need), STATUS_BUFFER_TOO_SMALL);
    ASSERT(need == full);
    /* One byte short is still too small, and the length is still reported. */
    need = 0;
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, all, buf, full - 1, &need), STATUS_BUFFER_TOO_SMALL);
    ASSERT(need == full);
    /* Exactly the required length succeeds. The return pointer is optional. */
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, all, buf, full, &need), STATUS_SUCCESS);
    ASSERT(need == full);
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, all, buf, full, NULL), STATUS_SUCCESS);

    sdt_close(p, hd, h);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(query_sd_access_and_errors)
{
    struct nt_eprocess *p = make_current_process_as("sq_err", SDT_U1);
    ASSERT(p != NULL);
    UCHAR buf[512];
    ULONG need = 0;

    /* Handle without READ_CONTROL. */
    struct nt_object_header *h1 = NULL;
    HANDLE hd1 = sdt_object(p, WRITE_DAC, &h1);
    ASSERT(hd1 != NULL);
    ULONG len1 = 0;
    void *sd1 = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL | WRITE_DAC, NULL, 0, &len1);
    ASSERT(sd1 != NULL);
    ASSERT_STATUS(sdt_attach(h1, len1, sd1), STATUS_SUCCESS);
    kfree(sd1);
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd1, OWNER_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_ACCESS_DENIED);

    /* DACL denies the caller: the object belongs to and allows only U2. */
    struct nt_object_header *h2 = NULL;
    HANDLE hd2 = sdt_object(p, READ_CONTROL, &h2);
    ASSERT(hd2 != NULL);
    ULONG len2 = 0;
    void *sd2 = sdt_make_sd(SDT_U2, SDT_G1, SDT_U2, READ_CONTROL, NULL, 0, &len2);
    ASSERT(sd2 != NULL);
    ASSERT_STATUS(sdt_attach(h2, len2, sd2), STATUS_SUCCESS);
    kfree(sd2);
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd2, OWNER_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_ACCESS_DENIED);

    /* SACL needs ACCESS_SYSTEM_SECURITY on the handle, which READ_CONTROL alone lacks. */
    struct nt_object_header *h3 = NULL;
    HANDLE hd3 = sdt_object(p, READ_CONTROL, &h3);
    ASSERT(hd3 != NULL);
    ULONG len3 = 0;
    void *sd3 = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL | ACCESS_SYSTEM_SECURITY, NULL, 0, &len3);
    ASSERT(sd3 != NULL);
    ASSERT_STATUS(sdt_attach(h3, len3, sd3), STATUS_SUCCESS);
    kfree(sd3);
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd3, SACL_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_ACCESS_DENIED);

    /* Information classes: none selected, or an unsupported bit, are invalid. */
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd3, 0, buf, sizeof(buf), &need), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd3, LABEL_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_INVALID_PARAMETER);

    /* A closed handle is invalid. */
    struct nt_object_header *h4 = NULL;
    HANDLE hd4 = sdt_object(p, READ_CONTROL, &h4);
    ASSERT(hd4 != NULL);
    ASSERT_STATUS(ObCloseHandle(p, hd4), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd4, OWNER_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_INVALID_HANDLE);
    ObMakeTemporaryObject(h4);
    ObDereferenceObject(h4);

    /* An object without a descriptor reports an empty self-relative header. */
    struct nt_object_header *h5 = NULL;
    HANDLE hd5 = sdt_object(p, READ_CONTROL, &h5);
    ASSERT(hd5 != NULL);
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd5, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                               buf, sizeof(buf), &need), STATUS_SUCCESS);
    ASSERT(need == sizeof(SECURITY_DESCRIPTOR_RELATIVE));
    const SECURITY_DESCRIPTOR_RELATIVE *r = (const SECURITY_DESCRIPTOR_RELATIVE *)buf;
    ASSERT(r->Owner == 0 && r->Dacl == 0 && r->Group == 0 && r->Sacl == 0);

    sdt_close(p, hd1, h1);
    sdt_close(p, hd2, h2);
    sdt_close(p, hd3, h3);
    sdt_close(p, hd5, h5);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(set_sd_dacl_and_owner)
{
    struct nt_eprocess *p = make_current_process_as("ss_dacl", SDT_U1);
    ASSERT(p != NULL);
    struct nt_object_header *h = NULL;
    ACCESS_MASK rw = READ_CONTROL | WRITE_DAC | WRITE_OWNER;
    HANDLE hd = sdt_object(p, rw, &h);
    ASSERT(hd != NULL);
    ULONG len = 0;
    void *sd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, rw, NULL, 0, &len);
    ASSERT(sd != NULL);
    ASSERT_STATUS(sdt_attach(h, len, sd), STATUS_SUCCESS);
    kfree(sd);

    /* Replace the DACL: U1 keeps full rights, U2 gets read. The owner is untouched. */
    ULONG nlen = 0;
    void *nsd = sdt_make_sd(SDT_U3, SDT_G1, SDT_U1, rw, SDT_U2, READ_CONTROL, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd, DACL_SECURITY_INFORMATION, nsd), STATUS_SUCCESS);
    kfree(nsd);

    UCHAR buf[512];
    ULONG need = 0;
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, OWNER_SECURITY_INFORMATION |
                                               DACL_SECURITY_INFORMATION,
                                               buf, sizeof(buf), &need), STATUS_SUCCESS);
    SECURITY_DESCRIPTOR *abs = NULL;
    ASSERT_STATUS(sd_from_self_relative(buf, need, &abs), STATUS_SUCCESS);
    PSID want = make_sid(SDT_U1);
    ASSERT(want != NULL && sid_equal(abs->Owner, want));
    ASSERT(abs->Dacl != NULL && abs->Dacl->AceCount == 2);
    sid_free(want);
    sdt_free_abs(abs);

    /* Owner change needs WRITE_OWNER; the new owner must be in the token. */
    nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, rw, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd, OWNER_SECURITY_INFORMATION, nsd), STATUS_SUCCESS);
    kfree(nsd);
    nsd = sdt_make_sd(SDT_U3, SDT_G1, SDT_U1, rw, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd, OWNER_SECURITY_INFORMATION, nsd), STATUS_INVALID_OWNER);
    kfree(nsd);

    /* A primary group that the token does not carry is rejected. */
    nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, rw, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd, GROUP_SECURITY_INFORMATION, nsd),
                  STATUS_INVALID_PRIMARY_GROUP);
    kfree(nsd);

    sdt_close(p, hd, h);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(set_sd_denied_and_implicit_owner)
{
    struct nt_eprocess *p = make_current_process_as("ss_deny", SDT_U1);
    ASSERT(p != NULL);
    ULONG len = 0, nlen = 0;
    UCHAR before[512];
    ULONG need_before = 0, need_after = 0;
    void *sd = NULL, *nsd = NULL;

    /* (a) Handle without WRITE_DAC. */
    struct nt_object_header *h1 = NULL;
    HANDLE hd1 = sdt_object(p, READ_CONTROL, &h1);
    ASSERT(hd1 != NULL);
    sd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL | WRITE_DAC, NULL, 0, &len);
    ASSERT(sd != NULL);
    ASSERT_STATUS(sdt_attach(h1, len, sd), STATUS_SUCCESS);
    kfree(sd);
    nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd1, DACL_SECURITY_INFORMATION, nsd), STATUS_ACCESS_DENIED);
    kfree(nsd);

    /* (b) Handle has WRITE_DAC, but the DACL denies the caller. The object is
     * owned by U2, so U1 gets no implicit rights. The descriptor is unchanged. */
    struct nt_object_header *h2 = NULL;
    HANDLE hd2 = sdt_object(p, READ_CONTROL | WRITE_DAC, &h2);
    ASSERT(hd2 != NULL);
    sd = sdt_make_sd(SDT_U2, SDT_G1, SDT_U2, READ_CONTROL | WRITE_DAC, NULL, 0, &len);
    ASSERT(sd != NULL);
    ASSERT_STATUS(sdt_attach(h2, len, sd), STATUS_SUCCESS);
    kfree(sd);
    /* Snapshot the stored descriptor at the object layer: the caller cannot
     * read it through the DACL that is about to be tested. */
    need_before = h2->security_descriptor_length;
    ASSERT(need_before > 0 && need_before <= sizeof(before));
    memcpy(before, h2->security_descriptor, need_before);
    nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL | WRITE_DAC, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd2, DACL_SECURITY_INFORMATION, nsd), STATUS_ACCESS_DENIED);
    kfree(nsd);
    need_after = h2->security_descriptor_length;
    ASSERT(need_after == need_before && memcmp(before, h2->security_descriptor, need_after) == 0);

    /* (c) The owner keeps WRITE_DAC implicitly even when the DACL omits it. */
    struct nt_object_header *h3 = NULL;
    HANDLE hd3 = sdt_object(p, WRITE_DAC, &h3);
    ASSERT(hd3 != NULL);
    sd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U2, READ_CONTROL, NULL, 0, &len);
    ASSERT(sd != NULL);
    ASSERT_STATUS(sdt_attach(h3, len, sd), STATUS_SUCCESS);
    kfree(sd);
    nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U2, READ_CONTROL, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd3, DACL_SECURITY_INFORMATION, nsd), STATUS_SUCCESS);
    kfree(nsd);

    /* (d) An object with no descriptor is unprotected: nothing can be set on it. */
    struct nt_object_header *h4 = NULL;
    HANDLE hd4 = sdt_object(p, WRITE_DAC | WRITE_OWNER, &h4);
    ASSERT(hd4 != NULL);
    nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd4, DACL_SECURITY_INFORMATION, nsd), STATUS_ACCESS_DENIED);
    kfree(nsd);

    /* (e) Bad arguments. A descriptor with a bad revision, a NULL descriptor,
     * and an empty information mask are all rejected. */
    struct nt_object_header *h5 = NULL;
    HANDLE hd5 = sdt_object(p, READ_CONTROL | WRITE_DAC | WRITE_OWNER, &h5);
    ASSERT(hd5 != NULL);
    sd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL | WRITE_DAC, NULL, 0, &len);
    ASSERT(sd != NULL);
    ASSERT_STATUS(sdt_attach(h5, len, sd), STATUS_SUCCESS);
    kfree(sd);
    UCHAR junk[sizeof(SECURITY_DESCRIPTOR_RELATIVE)];
    memset(junk, 0, sizeof(junk));
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd5, DACL_SECURITY_INFORMATION, junk),
                  STATUS_INVALID_SECURITY_DESCR);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd5, DACL_SECURITY_INFORMATION, NULL),
                  STATUS_INVALID_PARAMETER);
    nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, READ_CONTROL, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd5, 0, nsd), STATUS_INVALID_PARAMETER);
    /* A part requested but absent from the descriptor is invalid. */
    nsd = sdt_make_sd(NULL, SDT_G1, SDT_U1, READ_CONTROL, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd5, OWNER_SECURITY_INFORMATION, nsd), STATUS_INVALID_OWNER);
    kfree(nsd);
    nsd = sdt_make_sd(SDT_U1, NULL, SDT_U1, READ_CONTROL, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd5, GROUP_SECURITY_INFORMATION, nsd),
                  STATUS_INVALID_PRIMARY_GROUP);
    kfree(nsd);

    sdt_close(p, hd1, h1);
    sdt_close(p, hd2, h2);
    sdt_close(p, hd3, h3);
    sdt_close(p, hd4, h4);
    sdt_close(p, hd5, h5);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(set_sd_replace_does_not_leak)
{
    struct nt_eprocess *p = make_current_process_as("ss_leak", SDT_U1);
    ASSERT(p != NULL);
    struct nt_object_header *h = NULL;
    ACCESS_MASK rw = READ_CONTROL | WRITE_DAC;
    HANDLE hd = sdt_object(p, rw, &h);
    ASSERT(hd != NULL);
    ULONG len = 0;
    void *sd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, rw, NULL, 0, &len);
    ASSERT(sd != NULL);
    ASSERT_STATUS(sdt_attach(h, len, sd), STATUS_SUCCESS);
    kfree(sd);

    /* One pass of set and query settles the allocation count. */
    UCHAR buf[512];
    ULONG need = 0;
    ULONG nlen = 0;
    void *nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, rw, NULL, 0, &nlen);
    ASSERT(nsd != NULL);
    ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd, DACL_SECURITY_INFORMATION, nsd), STATUS_SUCCESS);
    kfree(nsd);
    ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, DACL_SECURITY_INFORMATION, buf, sizeof(buf), &need),
                  STATUS_SUCCESS);
    long base = atomic_load(&kshim_alloc_count);

    for (int i = 0; i < 50; i++) {
        nsd = sdt_make_sd(SDT_U1, SDT_G1, SDT_U1, rw, (i & 1) ? SDT_U2 : NULL,
                          READ_CONTROL, &nlen);
        ASSERT(nsd != NULL);
        ASSERT_STATUS(nt_svc_NtSetSecurityObject(hd, DACL_SECURITY_INFORMATION, nsd), STATUS_SUCCESS);
        kfree(nsd);
        ASSERT_STATUS(nt_svc_NtQuerySecurityObject(hd, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                                   buf, sizeof(buf), &need), STATUS_SUCCESS);
    }
    ASSERT(atomic_load(&kshim_alloc_count) == base);

    sdt_close(p, hd, h);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* NtSetInformationToken: TokenOwner, TokenPrimaryGroup, TokenDefaultDacl     */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtSetInformationToken(HANDLE TokenHandle, ULONG TokenInformationClass,
                                      PVOID TokenInformation, ULONG TokenInformationLength);

/* Input layouts (TOKEN_OWNER, TOKEN_PRIMARY_GROUP, TOKEN_DEFAULT_DACL). */
struct ts_owner { PSID Owner; };
struct ts_pgroup { PSID PrimaryGroup; };
struct ts_dacl { PACL DefaultDacl; };

/* A user token with one enabled group, held through a handle in p. */
static HANDLE ts_token_handle(struct nt_eprocess *p, ACCESS_MASK access,
                              ACCESS_TOKEN **live_out)
{
    ACCESS_TOKEN *tok = make_user_token(SDT_U1);
    PSID g1 = make_sid(SDT_G1);
    HANDLE h = NULL;
    if (!tok || !g1) return NULL;
    if (token_add_group(tok, g1, 0x7) != STATUS_SUCCESS) { sid_free(g1); token_dereference(tok); return NULL; }
    sid_free(g1);
    if (ObCreateTokenHandle(p, tok, access, 0, &h) != STATUS_SUCCESS) { token_dereference(tok); return NULL; }
    token_dereference(tok);
    *live_out = token_of_handle(p, h);
    return h;
}

TEST(set_token_owner_and_primary_group)
{
    struct nt_eprocess *p = make_current_process_as("st_grp", SDT_U1);
    ASSERT(p != NULL);
    ACCESS_TOKEN *live = NULL;
    HANDLE h = ts_token_handle(p, TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &live);
    ASSERT(h != NULL && live != NULL);
    PSID g1 = make_sid(SDT_G1), u1 = make_sid(SDT_U1), u3 = make_sid(SDT_U3);
    ASSERT(g1 && u1 && u3);

    /* Owner: the token's group (enabled) is assignable. */
    struct ts_owner o;
    o.Owner = g1;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenOwner, &o, sizeof(o)), STATUS_SUCCESS);
    ASSERT(sid_equal(live->owner, g1));
    /* A SID the token does not hold, NULL, and a short length are rejected. */
    o.Owner = u3;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenOwner, &o, sizeof(o)), STATUS_INVALID_OWNER);
    ASSERT(sid_equal(live->owner, g1));
    o.Owner = NULL;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenOwner, &o, sizeof(o)), STATUS_INVALID_OWNER);
    o.Owner = u1;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenOwner, &o, 0), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT(sid_equal(live->owner, g1));
    /* The user SID is assignable too. */
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenOwner, &o, sizeof(o)), STATUS_SUCCESS);
    ASSERT(sid_equal(live->owner, u1));

    /* Primary group must be one of the token's groups. */
    struct ts_pgroup pg;
    pg.PrimaryGroup = g1;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenPrimaryGroup, &pg, sizeof(pg)), STATUS_SUCCESS);
    ASSERT(sid_equal(live->primary_group, g1));
    pg.PrimaryGroup = u3;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenPrimaryGroup, &pg, sizeof(pg)),
                  STATUS_INVALID_PRIMARY_GROUP);
    ASSERT(sid_equal(live->primary_group, g1));

    sid_free(g1); sid_free(u1); sid_free(u3);
    ObCloseHandle(p, h);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(set_token_default_dacl)
{
    struct nt_eprocess *p = make_current_process_as("st_dacl", SDT_U1);
    ASSERT(p != NULL);
    ACCESS_TOKEN *live = NULL;
    HANDLE h = ts_token_handle(p, TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &live);
    ASSERT(h != NULL && live != NULL);
    PSID u1 = make_sid(SDT_U1);
    ASSERT(u1 != NULL);

    /* A supplied ACL is copied: the token keeps its own, so freeing ours is safe. */
    PACL acl = acl_create(256);
    ULONG idx = 0;
    ASSERT(acl != NULL);
    ASSERT_STATUS(acl_add_allowed_ace(acl, 0x1, 0, u1, &idx), STATUS_SUCCESS);
    struct ts_dacl d;
    d.DefaultDacl = acl;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenDefaultDacl, &d, sizeof(d)), STATUS_SUCCESS);
    ASSERT(live->default_dacl != NULL && live->default_dacl != acl);
    ASSERT(live->default_dacl->AceCount == 1);
    acl_free(acl);
    ASSERT(live->default_dacl != NULL && live->default_dacl->AceCount == 1);

    /* An ACL with an allow ACE before a deny ACE is not canonical. The stored
     * default DACL stays as it was. */
    PACL bad = acl_create(256);
    ASSERT(bad != NULL);
    ASSERT_STATUS(acl_add_allowed_ace(bad, 0x1, 0, u1, &idx), STATUS_SUCCESS);
    ASSERT_STATUS(acl_add_denied_ace(bad, 0x1, 0, u1, &idx), STATUS_SUCCESS);
    d.DefaultDacl = bad;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenDefaultDacl, &d, sizeof(d)), STATUS_INVALID_ACL);
    ASSERT(live->default_dacl->AceCount == 1);
    acl_free(bad);

    /* NULL removes the default DACL. */
    d.DefaultDacl = NULL;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(h, TokenDefaultDacl, &d, sizeof(d)), STATUS_SUCCESS);
    ASSERT(live->default_dacl == NULL);

    sid_free(u1);
    ObCloseHandle(p, h);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(set_token_info_access_and_arguments)
{
    struct nt_eprocess *p = make_current_process_as("st_acc", SDT_U1);
    ASSERT(p != NULL);
    ACCESS_TOKEN *live = NULL;
    HANDLE hq = ts_token_handle(p, TOKEN_QUERY, &live);
    ASSERT(hq != NULL && live != NULL);
    PSID u1 = make_sid(SDT_U1);
    ASSERT(u1 != NULL);

    /* A handle without TOKEN_ADJUST_DEFAULT cannot change these classes. */
    struct ts_owner o; o.Owner = u1;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(hq, TokenOwner, &o, sizeof(o)), STATUS_ACCESS_DENIED);
    struct ts_dacl d; d.DefaultDacl = NULL;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(hq, TokenDefaultDacl, &d, sizeof(d)), STATUS_ACCESS_DENIED);

    HANDLE ha = ts_token_handle(p, TOKEN_ADJUST_DEFAULT, &live);
    ASSERT(ha != NULL);
    ASSERT_STATUS(nt_svc_NtSetInformationToken(ha, TokenOwner, NULL, sizeof(o)), STATUS_INVALID_PARAMETER);
    /* A class this service does not set. */
    ASSERT_STATUS(nt_svc_NtSetInformationToken(ha, TokenUser, &o, sizeof(o)), STATUS_INVALID_INFO_CLASS);

    /* Repeated owner changes do not leak SID copies. */
    PSID g1 = make_sid(SDT_G1);
    ASSERT(g1 != NULL);
    struct ts_owner oo;
    oo.Owner = g1;
    ASSERT_STATUS(nt_svc_NtSetInformationToken(ha, TokenOwner, &oo, sizeof(oo)), STATUS_SUCCESS);
    long base = atomic_load(&kshim_alloc_count);
    for (int i = 0; i < 50; i++) {
        oo.Owner = (i & 1) ? u1 : g1;
        ASSERT_STATUS(nt_svc_NtSetInformationToken(ha, TokenOwner, &oo, sizeof(oo)), STATUS_SUCCESS);
    }
    ASSERT(atomic_load(&kshim_alloc_count) == base);

    sid_free(g1);
    sid_free(u1);
    ObCloseHandle(p, hq);
    ObCloseHandle(p, ha);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* NtAccessCheckAndAuditAlarm: access check against the effective token; the  */
/* audit part is a documented no-op.                                          */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtAccessCheckAndAuditAlarm(PUNICODE_STRING SubsystemName, PVOID HandleId,
                                           PUNICODE_STRING ObjectTypeName, PUNICODE_STRING ObjectName,
                                           PSECURITY_DESCRIPTOR SecurityDescriptor,
                                           ACCESS_MASK DesiredAccess, PGENERIC_MAPPING GenericMapping,
                                           BOOLEAN ObjectCreation, PACCESS_MASK GrantedAccess,
                                           PNTSTATUS AccessStatus, PBOOLEAN GenerateOnClose);

TEST(access_check_and_audit_alarm)
{
    struct nt_eprocess *p = make_current_process_as("aaa_proc", SDT_U1);
    ASSERT(p != NULL);
    ULONG sdlen = 0;
    void *sdbuf = make_allow_sd(SDT_U1, 0x1, &sdlen);
    ASSERT(sdbuf != NULL && sdlen > 0);
    GENERIC_MAPPING gm = { 0x1, 0x2, 0x4, 0x7 };
    ACCESS_MASK granted = 0xDEAD;
    NTSTATUS astat = 0x12345678;
    BOOLEAN gen = TRUE;

    /* Granted right: the primary token is U1, which the DACL allows 0x1. */
    ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, sdbuf, 0x1, &gm,
                                                    FALSE, &granted, &astat, &gen), STATUS_SUCCESS);
    ASSERT(astat == STATUS_SUCCESS && granted == 0x1);
    /* The audit output is always FALSE: nothing is generated on close. */
    ASSERT(gen == FALSE);

    /* A generic right is mapped; a right the DACL lacks is denied, nothing granted. */
    ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, sdbuf, 0x80000000UL, &gm,
                                                    FALSE, &granted, &astat, &gen), STATUS_SUCCESS);
    ASSERT(astat == STATUS_SUCCESS && granted == 0x1);
    ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, sdbuf, 0x2, &gm,
                                                    FALSE, &granted, &astat, &gen), STATUS_SUCCESS);
    ASSERT(astat == STATUS_ACCESS_DENIED && granted == 0);

    /* Impersonation: the thread's token wins over the process primary token. */
    struct nt_ethread *thr = NULL;
    ASSERT(ObCreateThreadObject(&thr, p, 77) == STATUS_SUCCESS && thr != NULL);
    ACCESS_TOKEN *other = make_user_token(SDT_U2);
    ASSERT(other != NULL);
    thr->impersonation_token = other;   /* the thread now owns this reference */
    kshim_set_current_thread(thr);
    ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, sdbuf, 0x1, &gm,
                                                    FALSE, &granted, &astat, &gen), STATUS_SUCCESS);
    ASSERT(astat == STATUS_ACCESS_DENIED && granted == 0);
    kshim_set_current_thread(NULL);
    ObDestroyThreadObject(thr);

    /* Bad arguments: a required output or the descriptor missing, and junk. */
    ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, NULL, 0x1, &gm,
                                                    FALSE, &granted, &astat, &gen), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, sdbuf, 0x1, &gm,
                                                    FALSE, &granted, &astat, NULL), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, sdbuf, 0x1, &gm,
                                                    FALSE, NULL, &astat, &gen), STATUS_INVALID_PARAMETER);
    UCHAR junk[sizeof(SECURITY_DESCRIPTOR_RELATIVE)];
    memset(junk, 0, sizeof(junk));
    ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, junk, 0x1, &gm,
                                                    FALSE, &granted, &astat, &gen),
                  STATUS_INVALID_SECURITY_DESCR);

    /* Repeated calls do not leak. */
    long base = atomic_load(&kshim_alloc_count);
    for (int i = 0; i < 50; i++) {
        ASSERT_STATUS(nt_svc_NtAccessCheckAndAuditAlarm(NULL, NULL, NULL, NULL, sdbuf, 0x1, &gm,
                                                        FALSE, &granted, &astat, &gen), STATUS_SUCCESS);
    }
    ASSERT(atomic_load(&kshim_alloc_count) == base);

    kfree(sdbuf);
    ObDestroyProcessObject(p);
    kshim_set_current_process(NULL);
    PASS();
}

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
    RUN(query_sd_info_selection);
    RUN(query_sd_buffer_too_small);
    RUN(query_sd_access_and_errors);
    RUN(set_sd_dacl_and_owner);
    RUN(set_sd_denied_and_implicit_owner);
    RUN(set_sd_replace_does_not_leak);
    RUN(set_token_owner_and_primary_group);
    RUN(set_token_default_dacl);
    RUN(set_token_info_access_and_arguments);
    RUN(access_check_and_audit_alarm);
    printf("security extra tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
