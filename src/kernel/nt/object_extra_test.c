/* object_extra_test.c - wrapper-level tests for the object namespace services
 * (nt_svc_ entry points in nt_obj.c): NtMakePermanentObject, the directory
 * object services, and the symbolic link object services.
 *
 * Links nt_obj.c, so the userspace (USERSPACE_KSHIM) signatures are used. The
 * core namespace logic is in object.c and is covered by object_test.c.
 */

#include "object.h"
#include "handle.h"
#include "security.h"
#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; return; \
    } \
} while(0)

#define ASSERT_STATUS(expr, expect) do { \
    NTSTATUS _st = (expr); \
    if (_st != (expect)) { \
        fprintf(stderr, "FAIL %s:%d: expected 0x%08lx got 0x%08lx for %s\n", __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_st, #expr); \
        g_fail++; return; \
    } \
} while(0)

#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while(0)

NTSTATUS nt_svc_NtMakePermanentObject(HANDLE Handle);
NTSTATUS nt_svc_NtMakeTemporaryObject(HANDLE Handle);
NTSTATUS nt_svc_NtOpenDirectoryObject(PHANDLE DirectoryHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtCreateDirectoryObject(PHANDLE DirectoryHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);

static WCHAR wbuf[256];

static void make_us(UNICODE_STRING *us, const char *s, WCHAR *buf, size_t buflen)
{
    size_t len = strlen(s);
    ASSERT(len < buflen);
    for (size_t i = 0; i < len; i++) buf[i] = (WCHAR)(unsigned char)s[i];
    buf[len] = 0;
    us->Buffer = buf;
    us->Length = (USHORT)(len * sizeof(WCHAR));
    us->MaximumLength = (USHORT)((len + 1) * sizeof(WCHAR));
}

static OBJECT_ATTRIBUTES make_oa(UNICODE_STRING *us, ULONG attr)
{
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = us;
    oa.Attributes = attr;
    return oa;
}

static void reset_ob(void)
{
    ObCleanupObjectManager();
    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "FAIL init\n"); g_fail++; }
}

/* Process whose primary token holds SeCreatePermanentPrivilege when priv is
 * true. The token is replaced in tests with privilege_token(). */
static struct nt_eprocess *make_proc(ULONG pid, const char *name)
{
    struct nt_eprocess *p = NULL;
    NTSTATUS st = ObCreateProcessObject(&p, pid, name);
    if (st != STATUS_SUCCESS || !p) { fprintf(stderr, "make_proc failed\n"); g_fail++; return NULL; }
    kshim_set_current_process(p);
    return p;
}

/* Primary token for a user, holding the given privilege enabled. */
static struct _ACCESS_TOKEN *make_token(ULONG privilege, BOOLEAN with_privilege)
{
    PSID user = NULL;
    LUID_AND_ATTRIBUTES priv;
    ACCESS_TOKEN *tok = NULL;
    if (sid_from_string("S-1-5-21-0-0-0-1000", &user) != STATUS_SUCCESS)
        return NULL;
    priv.Luid.LowPart = privilege;
    priv.Luid.HighPart = 0;
    priv.Attributes = SE_PRIVILEGE_ENABLED;
    tok = token_create_primary(user, NULL, 0, with_privilege ? &priv : NULL,
                               with_privilege ? 1 : 0, NULL, NULL, NULL, NULL, 0);
    sid_free(user);
    return tok;
}

/* Replaces the primary token of proc. */
static void set_primary_token(struct nt_eprocess *proc, struct _ACCESS_TOKEN *tok)
{
    if (proc->primary_token)
        token_dereference(proc->primary_token);
    proc->primary_token = tok;
}

/* Inserts a named test object into the namespace and returns its handle. */
static NTSTATUS insert_named(struct nt_eprocess *proc, const char *path, HANDLE *out_handle)
{
    UNICODE_STRING us;
    struct nt_object_header *hdr = NULL;
    void *body = NULL;
    make_us(&us, path, wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    NTSTATUS st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &hdr, &body);
    if (st != STATUS_SUCCESS) return st;
    st = ObInsertObject(hdr, &oa, 0, proc, out_handle);
    ObDereferenceObject(hdr);
    return st;
}

/* -------------------------------------------------------------------------- */
/* 1. NtMakePermanentObject: privilege and handle validation                   */
/* -------------------------------------------------------------------------- */

TEST(make_permanent_requires_privilege)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(301, "perm1");
    ASSERT(proc != NULL);
    set_primary_token(proc, make_token(SE_CREATE_PERMANENT_PRIVILEGE, FALSE));
    ASSERT(proc->primary_token != NULL);

    HANDLE h = NULL;
    ASSERT_STATUS(insert_named(proc, "\\BaseNamedObjects\\PermA", &h), STATUS_SUCCESS);
    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, 0, &hdr, NULL), STATUS_SUCCESS);
    ASSERT((hdr->flags & OBJ_PERMANENT) == 0);
    ObDereferenceObject(hdr);

    ASSERT_STATUS(nt_svc_NtMakePermanentObject(h), STATUS_PRIVILEGE_NOT_HELD);
    ASSERT(!(hdr->flags & OBJ_PERMANENT));

    ObCloseHandle(proc, h);
    ObDestroyProcessObject(proc);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(make_permanent_with_privilege)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(302, "perm2");
    ASSERT(proc != NULL);
    set_primary_token(proc, make_token(SE_CREATE_PERMANENT_PRIVILEGE, TRUE));

    HANDLE h = NULL;
    ASSERT_STATUS(insert_named(proc, "\\BaseNamedObjects\\PermB", &h), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtMakePermanentObject(h), STATUS_SUCCESS);

    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, 0, &hdr, NULL), STATUS_SUCCESS);
    ASSERT((hdr->flags & OBJ_PERMANENT) != 0);
    ASSERT(hdr->is_permanent);
    ObDereferenceObject(hdr);

    /* Idempotent, and NtMakeTemporaryObject clears it again. */
    ASSERT_STATUS(nt_svc_NtMakePermanentObject(h), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtMakeTemporaryObject(h), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, 0, &hdr, NULL), STATUS_SUCCESS);
    ASSERT((hdr->flags & OBJ_PERMANENT) == 0);
    ASSERT(!hdr->is_permanent);
    ObDereferenceObject(hdr);

    ObCloseHandle(proc, h);
    ObDestroyProcessObject(proc);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(make_permanent_bad_handle)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(303, "perm3");
    ASSERT(proc != NULL);
    set_primary_token(proc, make_token(SE_CREATE_PERMANENT_PRIVILEGE, TRUE));

    ASSERT_STATUS(nt_svc_NtMakePermanentObject(NULL), STATUS_INVALID_HANDLE);
    ASSERT_STATUS(nt_svc_NtMakePermanentObject((HANDLE)(uintptr_t)0x7777), STATUS_INVALID_HANDLE);

    ObDestroyProcessObject(proc);
    kshim_set_current_process(NULL);
    PASS();
}

TEST(make_permanent_no_token)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(304, "perm4");
    ASSERT(proc != NULL);
    set_primary_token(proc, NULL);
    /* No token means no privilege, which is checked before the handle. */
    ASSERT_STATUS(nt_svc_NtMakePermanentObject(NULL), STATUS_PRIVILEGE_NOT_HELD);

    ObDestroyProcessObject(proc);
    kshim_set_current_process(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 2. NtCreateDirectoryObject                                                  */
/* -------------------------------------------------------------------------- */

static void teardown(struct nt_eprocess *proc)
{
    ObDestroyProcessObject(proc);
    kshim_set_current_process(NULL);
}

TEST(create_directory_anonymous)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(311, "dir1");
    ASSERT(proc != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h, DIRECTORY_QUERY | DIRECTORY_TRAVERSE, NULL), STATUS_SUCCESS);
    ASSERT(h != NULL);
    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, &nt_directory_type, KernelMode, DIRECTORY_QUERY, &hdr, NULL), STATUS_SUCCESS);
    ASSERT(ObGetObjectType(hdr) == &nt_directory_type);
    ObDereferenceObject(hdr);
    /* Only the requested rights are granted. */
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, DIRECTORY_CREATE_OBJECT, &hdr, NULL), STATUS_ACCESS_DENIED);
    ObCloseHandle(proc, h);
    teardown(proc);
    PASS();
}

TEST(create_directory_named_and_collision)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(312, "dir2");
    ASSERT(proc != NULL);
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\DirA", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE h1 = NULL, h2 = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h1, DIRECTORY_QUERY, &oa), STATUS_SUCCESS);
    ASSERT(h1 != NULL);
    struct nt_object_header *found = NULL;
    ASSERT_STATUS(ObReferenceObjectByName(&us, &nt_directory_type, KernelMode, 0, &found), STATUS_SUCCESS);
    ObDereferenceObject(found);

    /* Same name without OBJ_OPENIF: collision, and no handle is written. */
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h2, DIRECTORY_QUERY, &oa), STATUS_OBJECT_NAME_COLLISION);
    ASSERT(h2 == NULL);

    ObCloseHandle(proc, h1);
    teardown(proc);
    PASS();
}

TEST(create_directory_openif)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(313, "dir3");
    ASSERT(proc != NULL);
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\DirB", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE h1 = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h1, DIRECTORY_QUERY, &oa), STATUS_SUCCESS);

    /* OBJ_OPENIF opens the existing directory and reports NAME_EXISTS. */
    OBJECT_ATTRIBUTES oa_if = make_oa(&us, OBJ_OPENIF);
    HANDLE h2 = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h2, DIRECTORY_QUERY, &oa_if), STATUS_OBJECT_NAME_EXISTS);
    ASSERT(h2 != NULL);
    struct nt_object_header *a = NULL, *b = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h1, NULL, KernelMode, 0, &a, NULL), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h2, NULL, KernelMode, 0, &b, NULL), STATUS_SUCCESS);
    ASSERT(a == b);
    ObDereferenceObject(a);
    ObDereferenceObject(b);

    ObCloseHandle(proc, h2);
    ObCloseHandle(proc, h1);
    teardown(proc);
    PASS();
}

TEST(create_directory_type_mismatch)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(314, "dir4");
    ASSERT(proc != NULL);
    HANDLE ht = NULL;
    ASSERT_STATUS(insert_named(proc, "\\BaseNamedObjects\\NotADir", &ht), STATUS_SUCCESS);

    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\NotADir", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_OPENIF);
    HANDLE h = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(h == NULL);

    ObCloseHandle(proc, ht);
    teardown(proc);
    PASS();
}

TEST(create_directory_bad_paths_and_args)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(315, "dir5");
    ASSERT(proc != NULL);
    HANDLE h = NULL;
    UNICODE_STRING us;

    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(NULL, DIRECTORY_QUERY, NULL), STATUS_INVALID_PARAMETER);

    make_us(&us, "\\BaseNamedObjects\\NoSuch\\Dir", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_OBJECT_PATH_NOT_FOUND);
    ASSERT(h == NULL);

    make_us(&us, "\\BaseNamedObjects\\", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_OBJECT_NAME_INVALID);

    make_us(&us, "BaseNamedObjects\\Rel", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_OBJECT_PATH_SYNTAX_BAD);

    oa = make_oa(&us, 0);
    oa.Length = 0;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_INVALID_PARAMETER);

    teardown(proc);
    PASS();
}

TEST(create_directory_generic_access)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(316, "dir6");
    ASSERT(proc != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&h, GENERIC_ALL, NULL), STATUS_SUCCESS);
    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, DIRECTORY_CREATE_SUBDIRECTORY | DIRECTORY_QUERY, &hdr, NULL), STATUS_SUCCESS);
    ObDereferenceObject(hdr);
    ObCloseHandle(proc, h);
    teardown(proc);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 3. NtOpenDirectoryObject                                                    */
/* -------------------------------------------------------------------------- */

TEST(open_directory)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(321, "odir1");
    ASSERT(proc != NULL);
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\DirO", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE hc = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&hc, DIRECTORY_QUERY, &oa), STATUS_SUCCESS);

    HANDLE ho = NULL;
    ASSERT_STATUS(nt_svc_NtOpenDirectoryObject(&ho, DIRECTORY_QUERY, &oa), STATUS_SUCCESS);
    ASSERT(ho != NULL);
    struct nt_object_header *a = NULL, *b = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, hc, NULL, KernelMode, 0, &a, NULL), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, ho, &nt_directory_type, KernelMode, 0, &b, NULL), STATUS_SUCCESS);
    ASSERT(a == b);
    ObDereferenceObject(a);
    ObDereferenceObject(b);

    /* The open handle has only the requested rights. */
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, ho, NULL, KernelMode, DIRECTORY_CREATE_OBJECT, &a, NULL), STATUS_ACCESS_DENIED);

    ObCloseHandle(proc, ho);
    ObCloseHandle(proc, hc);
    teardown(proc);
    PASS();
}

TEST(open_directory_missing_and_wrong_type)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(322, "odir2");
    ASSERT(proc != NULL);
    HANDLE ht = NULL;
    ASSERT_STATUS(insert_named(proc, "\\BaseNamedObjects\\NotDir2", &ht), STATUS_SUCCESS);

    UNICODE_STRING us;
    HANDLE h = NULL;
    make_us(&us, "\\BaseNamedObjects\\Missing", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtOpenDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_OBJECT_NAME_NOT_FOUND);
    ASSERT(h == NULL);

    make_us(&us, "\\BaseNamedObjects\\NoSuch\\X", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtOpenDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_OBJECT_PATH_NOT_FOUND);

    make_us(&us, "\\BaseNamedObjects\\NotDir2", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtOpenDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(h == NULL);

    ObCloseHandle(proc, ht);
    teardown(proc);
    PASS();
}

TEST(open_directory_root_and_args)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(323, "odir3");
    ASSERT(proc != NULL);
    UNICODE_STRING us;
    make_us(&us, "\\", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE h = NULL;
    ASSERT_STATUS(nt_svc_NtOpenDirectoryObject(&h, DIRECTORY_QUERY, &oa), STATUS_SUCCESS);
    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, 0, &hdr, NULL), STATUS_SUCCESS);
    ASSERT(hdr == ObGetRootDirectory());
    ObDereferenceObject(hdr);
    ObCloseHandle(proc, h);

    ASSERT_STATUS(nt_svc_NtOpenDirectoryObject(NULL, DIRECTORY_QUERY, &oa), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtOpenDirectoryObject(&h, DIRECTORY_QUERY, NULL), STATUS_INVALID_PARAMETER);

    teardown(proc);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */

int main(void)
{
    printf("object_extra_test\n");
    RUN(make_permanent_requires_privilege);
    RUN(make_permanent_with_privilege);
    RUN(make_permanent_bad_handle);
    RUN(make_permanent_no_token);
    RUN(open_directory);
    RUN(open_directory_missing_and_wrong_type);
    RUN(open_directory_root_and_args);
    RUN(create_directory_anonymous);
    RUN(create_directory_named_and_collision);
    RUN(create_directory_openif);
    RUN(create_directory_type_mismatch);
    RUN(create_directory_bad_paths_and_args);
    RUN(create_directory_generic_access);

    printf("object_extra_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
