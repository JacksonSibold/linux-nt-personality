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
NTSTATUS nt_svc_NtCreateSymbolicLinkObject(PHANDLE LinkHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PUNICODE_STRING LinkTarget);
NTSTATUS nt_svc_NtOpenSymbolicLinkObject(PHANDLE LinkHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtQuerySymbolicLinkObject(HANDLE LinkHandle, PUNICODE_STRING LinkTarget, PULONG ReturnedLength);
NTSTATUS nt_svc_NtQueryDirectoryObject(HANDLE DirectoryHandle, PVOID Buffer, ULONG Length, BOOLEAN ReturnSingleEntry, BOOLEAN RestartScan, PULONG Context, PULONG ReturnLength);
NTSTATUS nt_svc_NtOpenDirectoryObject(PHANDLE DirectoryHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtCreateDirectoryObject(PHANDLE DirectoryHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);

static WCHAR wbuf[256];
static WCHAR wbuf2[256];

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
/* 4. NtQueryDirectoryObject                                                   */
/* -------------------------------------------------------------------------- */

static BOOLEAN us_is(const UNICODE_STRING *u, const char *s)
{
    size_t n = strlen(s);
    if (u->Buffer == NULL || u->Length != n * sizeof(WCHAR))
        return FALSE;
    for (size_t i = 0; i < n; i++) {
        if (u->Buffer[i] != (WCHAR)(unsigned char)s[i])
            return FALSE;
    }
    return TRUE;
}

/* Creates \BaseNamedObjects\QD with entries e1, e2, e3 (test objects). The
 * entries are inserted without handles, so only the directory holds them. */
static void make_query_dir(HANDLE *out_dir)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    static const char *names[3] = { "e1", "e2", "e3" };
    NTSTATUS st;
    make_us(&us, "\\BaseNamedObjects\\QD", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    oa = make_oa(&us, 0);
    st = nt_svc_NtCreateDirectoryObject(out_dir, DIRECTORY_QUERY | DIRECTORY_CREATE_OBJECT, &oa);
    if (st != STATUS_SUCCESS) { g_fail++; fprintf(stderr, "FAIL make_query_dir create 0x%08lx\n", (unsigned long)st); return; }
    for (int i = 0; i < 3; i++) {
        char path[64];
        struct nt_object_header *hdr = NULL;
        void *body = NULL;
        snprintf(path, sizeof(path), "\\BaseNamedObjects\\QD\\%s", names[i]);
        make_us(&us, path, wbuf, sizeof(wbuf) / sizeof(WCHAR));
        oa = make_oa(&us, 0);
        st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &hdr, &body);
        if (st != STATUS_SUCCESS) { g_fail++; return; }
        st = ObInsertObject(hdr, &oa, 0, NULL, NULL);
        ObDereferenceObject(hdr);
        if (st != STATUS_SUCCESS) { g_fail++; fprintf(stderr, "FAIL make_query_dir insert 0x%08lx\n", (unsigned long)st); return; }
    }
}

TEST(query_directory_all_entries)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(331, "qdir1");
    ASSERT(proc != NULL);
    HANDLE hd = NULL;
    make_query_dir(&hd);
    ASSERT(hd != NULL);

    unsigned char buf[4096];
    ULONG ctx = 0, ret = 0;
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), FALSE, TRUE, &ctx, &ret), STATUS_SUCCESS);
    OBJECT_DIRECTORY_INFORMATION *recs = (OBJECT_DIRECTORY_INFORMATION *)buf;
    ASSERT(us_is(&recs[0].Name, "e1") && us_is(&recs[0].TypeName, "TestObject"));
    ASSERT(us_is(&recs[1].Name, "e2") && us_is(&recs[1].TypeName, "TestObject"));
    ASSERT(us_is(&recs[2].Name, "e3") && us_is(&recs[2].TypeName, "TestObject"));
    ASSERT(recs[3].Name.Buffer == NULL && recs[3].TypeName.Buffer == NULL);
    ASSERT(ret >= 4 * sizeof(OBJECT_DIRECTORY_INFORMATION) && ret <= sizeof(buf));
    ASSERT(ctx == 3);
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), FALSE, FALSE, &ctx, &ret), STATUS_NO_MORE_ENTRIES);
    ASSERT(ctx == 3);

    ObCloseHandle(proc, hd);
    teardown(proc);
    PASS();
}

TEST(query_directory_empty)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(332, "qdir2");
    ASSERT(proc != NULL);
    HANDLE hd = NULL;
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&hd, DIRECTORY_QUERY, NULL), STATUS_SUCCESS);
    unsigned char buf[256];
    ULONG ctx = 0, ret = 99;
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), FALSE, TRUE, &ctx, &ret), STATUS_NO_MORE_ENTRIES);
    ASSERT(ctx == 0);
    ObCloseHandle(proc, hd);
    teardown(proc);
    PASS();
}

TEST(query_directory_single_entry_and_restart)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(333, "qdir3");
    ASSERT(proc != NULL);
    HANDLE hd = NULL;
    make_query_dir(&hd);
    ASSERT(hd != NULL);

    unsigned char buf[1024];
    OBJECT_DIRECTORY_INFORMATION *recs = (OBJECT_DIRECTORY_INFORMATION *)buf;
    ULONG ctx = 0, ret = 0;
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), TRUE, TRUE, &ctx, &ret), STATUS_SUCCESS);
    ASSERT(us_is(&recs[0].Name, "e1") && recs[1].Name.Buffer == NULL);
    ASSERT(ctx == 1);
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), TRUE, FALSE, &ctx, &ret), STATUS_SUCCESS);
    ASSERT(us_is(&recs[0].Name, "e2") && recs[1].Name.Buffer == NULL);
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), TRUE, FALSE, &ctx, &ret), STATUS_SUCCESS);
    ASSERT(us_is(&recs[0].Name, "e3"));
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), TRUE, FALSE, &ctx, &ret), STATUS_NO_MORE_ENTRIES);

    /* RestartScan ignores the context and starts at the first entry again. */
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), TRUE, TRUE, &ctx, &ret), STATUS_SUCCESS);
    ASSERT(us_is(&recs[0].Name, "e1"));
    ASSERT(ctx == 1);

    ObCloseHandle(proc, hd);
    teardown(proc);
    PASS();
}

TEST(query_directory_buffer_too_small_and_partial)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(334, "qdir4");
    ASSERT(proc != NULL);
    HANDLE hd = NULL;
    make_query_dir(&hd);
    ASSERT(hd != NULL);

    unsigned char small[sizeof(OBJECT_DIRECTORY_INFORMATION) * 2];
    ULONG ctx = 0, ret = 0;
    /* Room for the terminator only: even one entry does not fit. */
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, small, sizeof(small), FALSE, TRUE, &ctx, &ret), STATUS_BUFFER_TOO_SMALL);
    ASSERT(ctx == 0);
    ASSERT(ret > sizeof(small));
    ULONG need_one = ret;

    /* Exactly enough for e1 and the terminator: only e1 is returned. */
    unsigned char buf[1024];
    OBJECT_DIRECTORY_INFORMATION *recs = (OBJECT_DIRECTORY_INFORMATION *)buf;
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, need_one, FALSE, TRUE, &ctx, &ret), STATUS_SUCCESS);
    ASSERT(us_is(&recs[0].Name, "e1") && recs[1].Name.Buffer == NULL);
    ASSERT(ctx == 1);
    ASSERT(ret == need_one);

    /* The rest in one call. */
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), FALSE, FALSE, &ctx, &ret), STATUS_SUCCESS);
    ASSERT(us_is(&recs[0].Name, "e2") && us_is(&recs[1].Name, "e3") && recs[2].Name.Buffer == NULL);
    ASSERT(ctx == 3);

    ObCloseHandle(proc, hd);
    teardown(proc);
    PASS();
}

TEST(query_directory_access_type_args)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(335, "qdir5");
    ASSERT(proc != NULL);
    HANDLE hd = NULL;
    make_query_dir(&hd);
    ASSERT(hd != NULL);

    /* A handle without DIRECTORY_QUERY cannot query. */
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\QD", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE ht = NULL;
    ASSERT_STATUS(nt_svc_NtOpenDirectoryObject(&ht, DIRECTORY_TRAVERSE, &oa), STATUS_SUCCESS);
    unsigned char buf[256];
    ULONG ctx = 0, ret = 0;
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(ht, buf, sizeof(buf), FALSE, TRUE, &ctx, &ret), STATUS_ACCESS_DENIED);
    ObCloseHandle(proc, ht);

    /* An event handle is not a directory. */
    HANDLE hobj = NULL;
    ASSERT_STATUS(insert_named(proc, "\\BaseNamedObjects\\QNotDir", &hobj), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hobj, buf, sizeof(buf), FALSE, TRUE, &ctx, &ret), STATUS_OBJECT_TYPE_MISMATCH);
    ObCloseHandle(proc, hobj);

    /* Bad arguments. */
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, NULL, sizeof(buf), FALSE, TRUE, &ctx, &ret), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtQueryDirectoryObject(hd, buf, sizeof(buf), FALSE, TRUE, NULL, &ret), STATUS_INVALID_PARAMETER);

    ObCloseHandle(proc, hd);
    teardown(proc);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 5. NtCreateSymbolicLinkObject                                               */
/* -------------------------------------------------------------------------- */

/* Inserts a test object named target_path and returns its handle. */
static HANDLE make_link_target(struct nt_eprocess *proc, const char *target_path)
{
    HANDLE h = NULL;
    NTSTATUS st = insert_named(proc, target_path, &h);
    if (st != STATUS_SUCCESS) { g_fail++; fprintf(stderr, "FAIL make_link_target 0x%08lx\n", (unsigned long)st); return NULL; }
    return h;
}

TEST(create_symlink_resolves)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(341, "sym1");
    ASSERT(proc != NULL);
    HANDLE ht = make_link_target(proc, "\\BaseNamedObjects\\SymTarget");
    ASSERT(ht != NULL);

    UNICODE_STRING us, target;
    make_us(&us, "\\BaseNamedObjects\\SymA", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    make_us(&target, "\\BaseNamedObjects\\SymTarget", wbuf2, sizeof(wbuf2) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE hl = NULL;
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&hl, SYMBOLIC_LINK_QUERY, &oa, &target), STATUS_SUCCESS);
    ASSERT(hl != NULL);

    /* A name lookup through the link finds the target object. */
    struct nt_object_header *via_link = NULL, *direct = NULL;
    ASSERT_STATUS(ObReferenceObjectByName(&us, &nt_test_type, KernelMode, 0, &via_link), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, ht, NULL, KernelMode, 0, &direct, NULL), STATUS_SUCCESS);
    ASSERT(via_link == direct);
    ObDereferenceObject(via_link);
    ObDereferenceObject(direct);

    ObCloseHandle(proc, hl);
    ObCloseHandle(proc, ht);
    teardown(proc);
    PASS();
}

TEST(create_symlink_collision_and_openif)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(342, "sym2");
    ASSERT(proc != NULL);
    UNICODE_STRING us, target;
    make_us(&us, "\\BaseNamedObjects\\SymC", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    make_us(&target, "\\BaseNamedObjects\\SymTarget", wbuf2, sizeof(wbuf2) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE h1 = NULL, h2 = NULL;
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h1, SYMBOLIC_LINK_QUERY, &oa, &target), STATUS_SUCCESS);

    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h2, SYMBOLIC_LINK_QUERY, &oa, &target), STATUS_OBJECT_NAME_COLLISION);
    ASSERT(h2 == NULL);

    OBJECT_ATTRIBUTES oa_if = make_oa(&us, OBJ_OPENIF);
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h2, SYMBOLIC_LINK_QUERY, &oa_if, &target), STATUS_OBJECT_NAME_EXISTS);
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

TEST(create_symlink_anonymous_and_bad_args)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(343, "sym3");
    ASSERT(proc != NULL);
    UNICODE_STRING target, bad;
    make_us(&target, "\\BaseNamedObjects\\SymTarget", wbuf2, sizeof(wbuf2) / sizeof(WCHAR));
    HANDLE h = NULL;

    /* Anonymous link: no name, still a valid handle. */
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, NULL, &target), STATUS_SUCCESS);
    ASSERT(h != NULL);
    ObCloseHandle(proc, h);
    h = NULL;

    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(NULL, SYMBOLIC_LINK_QUERY, NULL, &target), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, NULL, NULL), STATUS_INVALID_PARAMETER);
    ASSERT(h == NULL);

    UNICODE_STRING empty;
    memset(&empty, 0, sizeof(empty));
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, NULL, &empty), STATUS_INVALID_PARAMETER);

    /* Odd byte length is not a UTF-16 string. */
    bad = target;
    bad.Length = 3;
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, NULL, &bad), STATUS_OBJECT_NAME_INVALID);

    teardown(proc);
    PASS();
}

TEST(create_symlink_type_and_path_errors)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(344, "sym4");
    ASSERT(proc != NULL);
    HANDLE ht = make_link_target(proc, "\\BaseNamedObjects\\SymTarget2");
    ASSERT(ht != NULL);
    UNICODE_STRING us, target;
    make_us(&target, "\\BaseNamedObjects\\SymTarget2", wbuf2, sizeof(wbuf2) / sizeof(WCHAR));
    HANDLE h = NULL;

    /* A name held by another object type is a type mismatch with OBJ_OPENIF. */
    make_us(&us, "\\BaseNamedObjects\\SymTarget2", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa_if = make_oa(&us, OBJ_OPENIF);
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, &oa_if, &target), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(h == NULL);

    make_us(&us, "\\BaseNamedObjects\\NoSuch\\L", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, &oa, &target), STATUS_OBJECT_PATH_NOT_FOUND);

    ObCloseHandle(proc, ht);
    teardown(proc);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 6. NtOpenSymbolicLinkObject                                                 */
/* -------------------------------------------------------------------------- */

TEST(open_symlink_is_not_followed)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(351, "osym1");
    ASSERT(proc != NULL);
    HANDLE ht = make_link_target(proc, "\\BaseNamedObjects\\OSymTarget");
    ASSERT(ht != NULL);

    UNICODE_STRING us, target;
    make_us(&us, "\\BaseNamedObjects\\OSymA", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    make_us(&target, "\\BaseNamedObjects\\OSymTarget", wbuf2, sizeof(wbuf2) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE hl = NULL;
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&hl, SYMBOLIC_LINK_QUERY, &oa, &target), STATUS_SUCCESS);
    ObCloseHandle(proc, hl);

    /* Opening by name gives the link, not the object it points to. */
    HANDLE ho = NULL;
    ASSERT_STATUS(nt_svc_NtOpenSymbolicLinkObject(&ho, SYMBOLIC_LINK_QUERY, &oa), STATUS_SUCCESS);
    ASSERT(ho != NULL);
    struct nt_object_header *lhdr = NULL, *thdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, ho, &nt_symlink_type, KernelMode, 0, &lhdr, NULL), STATUS_SUCCESS);
    ASSERT(ObGetObjectType(lhdr) == &nt_symlink_type);
    ObDereferenceObject(lhdr);
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, ho, &nt_test_type, KernelMode, 0, &thdr, NULL), STATUS_OBJECT_TYPE_MISMATCH);
    ObCloseHandle(proc, ho);

    ObCloseHandle(proc, ht);
    teardown(proc);
    PASS();
}

TEST(open_symlink_missing_and_wrong_type)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(352, "osym2");
    ASSERT(proc != NULL);
    HANDLE ht = make_link_target(proc, "\\BaseNamedObjects\\OSymNotLink");
    ASSERT(ht != NULL);

    UNICODE_STRING us;
    HANDLE h = NULL;
    make_us(&us, "\\BaseNamedObjects\\OSymMissing", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtOpenSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, &oa), STATUS_OBJECT_NAME_NOT_FOUND);
    ASSERT(h == NULL);

    make_us(&us, "\\BaseNamedObjects\\OSymNotLink", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtOpenSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, &oa), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(h == NULL);

    ASSERT_STATUS(nt_svc_NtOpenSymbolicLinkObject(NULL, SYMBOLIC_LINK_QUERY, &oa), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(nt_svc_NtOpenSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, NULL), STATUS_INVALID_PARAMETER);

    ObCloseHandle(proc, ht);
    teardown(proc);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 7. NtQuerySymbolicLinkObject                                                */
/* -------------------------------------------------------------------------- */

TEST(query_symlink_target)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(361, "qsym1");
    ASSERT(proc != NULL);
    UNICODE_STRING us, target;
    make_us(&us, "\\BaseNamedObjects\\QSymA", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    make_us(&target, "\\BaseNamedObjects\\QSymTarget", wbuf2, sizeof(wbuf2) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE hl = NULL;
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&hl, SYMBOLIC_LINK_QUERY, &oa, &target), STATUS_SUCCESS);

    WCHAR outbuf[256];
    UNICODE_STRING out;
    out.Buffer = outbuf;
    out.Length = 0;
    out.MaximumLength = sizeof(outbuf);
    ULONG ret = 0;
    ASSERT_STATUS(nt_svc_NtQuerySymbolicLinkObject(hl, &out, &ret), STATUS_SUCCESS);
    ASSERT(us_is(&out, "\\BaseNamedObjects\\QSymTarget"));
    ASSERT(ret == target.Length);

    /* An exact fit is accepted. */
    out.Buffer = outbuf;
    out.Length = 0;
    out.MaximumLength = target.Length;
    ASSERT_STATUS(nt_svc_NtQuerySymbolicLinkObject(hl, &out, NULL), STATUS_SUCCESS);
    ASSERT(out.Length == target.Length);

    ObCloseHandle(proc, hl);
    teardown(proc);
    PASS();
}

TEST(query_symlink_too_small_and_access)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(362, "qsym2");
    ASSERT(proc != NULL);
    UNICODE_STRING us, target;
    make_us(&us, "\\BaseNamedObjects\\QSymB", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    make_us(&target, "\\BaseNamedObjects\\QSymTarget", wbuf2, sizeof(wbuf2) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    HANDLE hl = NULL;
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&hl, SYMBOLIC_LINK_QUERY, &oa, &target), STATUS_SUCCESS);

    WCHAR outbuf[256];
    UNICODE_STRING out;
    ULONG ret = 0;
    out.Buffer = outbuf;
    out.Length = 0;
    out.MaximumLength = 4;
    ASSERT_STATUS(nt_svc_NtQuerySymbolicLinkObject(hl, &out, &ret), STATUS_BUFFER_TOO_SMALL);
    ASSERT(ret == target.Length);
    ASSERT(out.Length == 0);

    /* A handle without SYMBOLIC_LINK_QUERY cannot query. */
    HANDLE hn = NULL;
    ASSERT_STATUS(nt_svc_NtOpenSymbolicLinkObject(&hn, 0, &oa), STATUS_SUCCESS);
    out.Buffer = outbuf;
    out.MaximumLength = sizeof(outbuf);
    ASSERT_STATUS(nt_svc_NtQuerySymbolicLinkObject(hn, &out, &ret), STATUS_ACCESS_DENIED);
    ObCloseHandle(proc, hn);

    /* A directory handle is not a link. */
    HANDLE hd = NULL;
    make_us(&us, "\\BaseNamedObjects\\QSymDir", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    oa = make_oa(&us, 0);
    ASSERT_STATUS(nt_svc_NtCreateDirectoryObject(&hd, DIRECTORY_QUERY, &oa), STATUS_SUCCESS);
    out.MaximumLength = sizeof(outbuf);
    ASSERT_STATUS(nt_svc_NtQuerySymbolicLinkObject(hd, &out, &ret), STATUS_OBJECT_TYPE_MISMATCH);
    ObCloseHandle(proc, hd);

    ASSERT_STATUS(nt_svc_NtQuerySymbolicLinkObject(hl, NULL, &ret), STATUS_INVALID_PARAMETER);

    ObCloseHandle(proc, hl);
    teardown(proc);
    PASS();
}

TEST(query_symlink_anonymous)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(363, "qsym3");
    ASSERT(proc != NULL);
    UNICODE_STRING target;
    make_us(&target, "\\BaseNamedObjects\\AnonTarget", wbuf2, sizeof(wbuf2) / sizeof(WCHAR));
    HANDLE hl = NULL;
    ASSERT_STATUS(nt_svc_NtCreateSymbolicLinkObject(&hl, SYMBOLIC_LINK_QUERY, NULL, &target), STATUS_SUCCESS);

    WCHAR outbuf[128];
    UNICODE_STRING out;
    out.Buffer = outbuf;
    out.Length = 0;
    out.MaximumLength = sizeof(outbuf);
    ASSERT_STATUS(nt_svc_NtQuerySymbolicLinkObject(hl, &out, NULL), STATUS_SUCCESS);
    ASSERT(us_is(&out, "\\BaseNamedObjects\\AnonTarget"));

    ObCloseHandle(proc, hl);
    teardown(proc);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 8. NtQueryObject(ObjectHandleFlagInformation) and NtSetInformationObject    */
/* -------------------------------------------------------------------------- */

typedef struct _TEST_OBJECT_HANDLE_FLAG_INFORMATION {
    BOOLEAN Inherit;
    BOOLEAN ProtectFromClose;
} TEST_OBJECT_HANDLE_FLAG_INFORMATION;

NTSTATUS nt_svc_NtQueryObject(HANDLE Handle, ULONG ObjectInformationClass,
                              PVOID ObjectInformation, ULONG Length, PULONG ReturnLength);
NTSTATUS nt_svc_NtSetInformationObject(HANDLE Handle, ULONG ObjectInformationClass,
                                       PVOID ObjectInformation, ULONG Length);

#define TEST_OBJECT_HANDLE_FLAG_CLASS 4UL

TEST(query_handle_flags)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(371, "qflags");
    ASSERT(proc != NULL);

    /* A handle created with OBJ_INHERIT reports Inherit. */
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\FlagObj", wbuf, sizeof(wbuf) / sizeof(WCHAR));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_INHERIT);
    struct nt_object_header *hdr = NULL;
    void *body = NULL;
    ASSERT_STATUS(ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &hdr, &body), STATUS_SUCCESS);
    HANDLE h = NULL;
    ASSERT_STATUS(ObInsertObject(hdr, &oa, 0, proc, &h), STATUS_SUCCESS);
    ObDereferenceObject(hdr);

    TEST_OBJECT_HANDLE_FLAG_INFORMATION fi;
    memset(&fi, 0xAA, sizeof(fi));
    ULONG ret = 0;
    ASSERT_STATUS(nt_svc_NtQueryObject(h, TEST_OBJECT_HANDLE_FLAG_CLASS, &fi, sizeof(fi), &ret), STATUS_SUCCESS);
    ASSERT(ret == sizeof(fi));
    ASSERT(fi.Inherit == TRUE);
    ASSERT(fi.ProtectFromClose == FALSE);

    /* NtSetInformationObject changes both flags, and the query shows them. */
    fi.Inherit = FALSE;
    fi.ProtectFromClose = TRUE;
    ASSERT_STATUS(nt_svc_NtSetInformationObject(h, TEST_OBJECT_HANDLE_FLAG_CLASS, &fi, sizeof(fi)), STATUS_SUCCESS);
    TEST_OBJECT_HANDLE_FLAG_INFORMATION got;
    memset(&got, 0, sizeof(got));
    ASSERT_STATUS(nt_svc_NtQueryObject(h, TEST_OBJECT_HANDLE_FLAG_CLASS, &got, sizeof(got), NULL), STATUS_SUCCESS);
    ASSERT(got.Inherit == FALSE);
    ASSERT(got.ProtectFromClose == TRUE);

    /* A buffer too small for the record gives its size as ReturnLength. */
    ASSERT_STATUS(nt_svc_NtQueryObject(h, TEST_OBJECT_HANDLE_FLAG_CLASS, &got, 1, &ret), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT(ret == sizeof(fi));

    /* Clear ProtectFromClose so the handle can be closed. */
    fi.ProtectFromClose = FALSE;
    ASSERT_STATUS(nt_svc_NtSetInformationObject(h, TEST_OBJECT_HANDLE_FLAG_CLASS, &fi, sizeof(fi)), STATUS_SUCCESS);
    ObCloseHandle(proc, h);
    teardown(proc);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 9. NtDuplicateObject with an optional TargetHandle                          */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtDuplicateObject(HANDLE SourceProcessHandle, HANDLE SourceHandle,
                                  HANDLE TargetProcessHandle, PHANDLE TargetHandle,
                                  ACCESS_MASK DesiredAccess, ULONG HandleAttributes, ULONG Options);

TEST(duplicate_null_target)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(381, "dupnull");
    ASSERT(proc != NULL);
    HANDLE h = NULL;
    ASSERT_STATUS(insert_named(proc, "\\BaseNamedObjects\\DupNullObj", &h), STATUS_SUCCESS);

    struct nt_object_header *hdr = NULL;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, 0, &hdr, NULL), STATUS_SUCCESS);
    long long before = atomic64_read(&hdr->handle_count);
    ObDereferenceObject(hdr);

    /* TargetHandle is optional: the duplicate is made and closed again, so no
     * handle is returned and the handle count is unchanged. */
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), h, NtCurrentProcess(), NULL,
                                           0, 0, DUPLICATE_SAME_ACCESS), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, 0, &hdr, NULL), STATUS_SUCCESS);
    ASSERT(atomic64_read(&hdr->handle_count) == before);
    ObDereferenceObject(hdr);

    /* A duplicate that is closed again must not stay open because it was made
     * protect-from-close: the handle count is still unchanged. */
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), h, NtCurrentProcess(), NULL,
                                           0, OBJ_PROTECT_FROM_CLOSE, DUPLICATE_SAME_ACCESS), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, 0, &hdr, NULL), STATUS_SUCCESS);
    ASSERT(atomic64_read(&hdr->handle_count) == before);
    ObDereferenceObject(hdr);

    /* With DUPLICATE_CLOSE_SOURCE the source is closed even without a target. */
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), h, NtCurrentProcess(), NULL,
                                           0, 0, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, h, NULL, KernelMode, 0, &hdr, NULL), STATUS_INVALID_HANDLE);

    /* A bad source is still an error. */
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), NULL, NtCurrentProcess(), NULL,
                                           0, 0, DUPLICATE_SAME_ACCESS), STATUS_INVALID_HANDLE);

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
    RUN(duplicate_null_target);
    RUN(query_handle_flags);
    RUN(query_symlink_target);
    RUN(query_symlink_too_small_and_access);
    RUN(query_symlink_anonymous);
    RUN(open_symlink_is_not_followed);
    RUN(open_symlink_missing_and_wrong_type);
    RUN(create_symlink_resolves);
    RUN(create_symlink_collision_and_openif);
    RUN(create_symlink_anonymous_and_bad_args);
    RUN(create_symlink_type_and_path_errors);
    RUN(query_directory_all_entries);
    RUN(query_directory_empty);
    RUN(query_directory_single_entry_and_restart);
    RUN(query_directory_buffer_too_small_and_partial);
    RUN(query_directory_access_type_args);
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
