/* sync_extra_test.c - host tests for the timer, keyed-event and combined
 * wait syscall wrappers in sync.c (userspace shim). Object-manager backed,
 * so it links the object, handle and nt_obj modules as well as sync. */
#include "object.h"
#include "handle.h"
#include "sync.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"

/* Syscall wrappers in nt_obj.c and sync.c (userspace shim). */
NTSTATUS nt_svc_NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes,
                              ULONG EventType, BOOLEAN InitialState);
NTSTATUS nt_svc_NtCreateTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                              ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                              ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtOpenTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                            ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                            ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdatomic.h>

static int g_pass = 0, g_fail = 0;
#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while (0)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; return; } } while (0)
#define ASSERT_STATUS(expr, expect) do { NTSTATUS _s = (expr); if (_s != (expect)) { fprintf(stderr, "FAIL %s:%d: expect 0x%lx got 0x%lx for %s\n", __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_s, #expr); g_fail++; return; } } while (0)
#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while (0)

static WCHAR wbuf1[512], wbuf2[512];

static void make_us(UNICODE_STRING *us, const char *s, WCHAR *buf, size_t buflen) {
    size_t len = strlen(s);
    ASSERT(len * sizeof(WCHAR) < buflen);
    for (size_t i = 0; i < len; i++) buf[i] = (WCHAR)(unsigned char)s[i];
    us->Buffer = buf;
    us->Length = (USHORT)(len * sizeof(WCHAR));
    us->MaximumLength = us->Length;
    buf[len] = 0;
}
static OBJECT_ATTRIBUTES make_oa(UNICODE_STRING *us, ULONG attr) {
    OBJECT_ATTRIBUTES oa = {0};
    oa.Length = sizeof(oa);
    oa.ObjectName = us;
    oa.Attributes = attr;
    return oa;
}
static void make_proc_thread(void) {
    struct nt_eprocess *p = NULL;
    NTSTATUS st = ObCreateProcessObject(&p, 2000, "extproc");
    if (st != STATUS_SUCCESS || !p) { fprintf(stderr, "make_proc failed\n"); g_fail++; return; }
    kshim_set_current_process(p);
    struct nt_ethread *t = NULL;
    st = ObCreateThreadObject(&t, p, 1);
    if (st != STATUS_SUCCESS || !t) { fprintf(stderr, "make_thread failed\n"); g_fail++; return; }
    kshim_set_current_thread(t);
    struct nt_thread *nt_thr = NULL;
    if (nt_thread_create(&nt_thr) == STATUS_SUCCESS) nt_set_current_thread(nt_thr);
}
static void reset_ob(void) {
    ObCleanupObjectManager();
    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager failed\n"); g_fail++; }
    make_proc_thread();
}

/* Typed wrappers that pad the 12-slot dispatcher argument list. */
static NTSTATUS t_create_timer(HANDLE *h, ACCESS_MASK acc, POBJECT_ATTRIBUTES oa, ULONG_PTR type) {
    return nt_svc_NtCreateTimer((ULONG_PTR)h, acc, (ULONG_PTR)oa, type, 0, 0, 0, 0, 0, 0, 0, 0);
}
static NTSTATUS t_open_timer(HANDLE *h, ACCESS_MASK acc, POBJECT_ATTRIBUTES oa) {
    return nt_svc_NtOpenTimer((ULONG_PTR)h, acc, (ULONG_PTR)oa, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* NtCreateTimer                                                        */
/* ------------------------------------------------------------------ */
TEST(create_timer_types) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    ASSERT(h != NULL);
    HANDLE h2 = NULL;
    ASSERT_STATUS(t_create_timer(&h2, 0x1F0003, NULL, SynchronizationTimer), STATUS_SUCCESS);
    ASSERT(h2 != NULL && h2 != h);
    /* Timer type outside the two TIMER_TYPE values is rejected. */
    HANDLE h3 = NULL;
    ASSERT_STATUS(t_create_timer(&h3, 0x1F0003, NULL, 7), STATUS_INVALID_PARAMETER);
    /* Null output handle is rejected. */
    ASSERT_STATUS(t_create_timer(NULL, 0x1F0003, NULL, NotificationTimer), STATUS_INVALID_PARAMETER);
    PASS();
}

TEST(create_timer_named_collision) {
    reset_ob();
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtTimer", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, &oa, NotificationTimer), STATUS_SUCCESS);
    /* Creating the same name again opens the existing timer and reports the collision. */
    HANDLE h2 = NULL;
    ASSERT_STATUS(t_create_timer(&h2, 0x1F0003, &oa, NotificationTimer), STATUS_OBJECT_NAME_COLLISION);
    ASSERT(h2 != NULL && h2 != h);
    PASS();
}

/* ------------------------------------------------------------------ */
/* NtOpenTimer                                                          */
/* ------------------------------------------------------------------ */
TEST(open_timer_by_name) {
    reset_ob();
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtTimerOpen", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE created = NULL;
    ASSERT_STATUS(t_create_timer(&created, 0x1F0003, &oa, SynchronizationTimer), STATUS_SUCCESS);
    HANDLE opened = NULL;
    ASSERT_STATUS(t_open_timer(&opened, 0x1F0003, &oa), STATUS_SUCCESS);
    ASSERT(opened != NULL && opened != created);
    /* Opening a name that does not exist fails. */
    UNICODE_STRING missing;
    make_us(&missing, "\\BaseNamedObjects\\ExtTimerMissing", wbuf2, sizeof(wbuf2));
    OBJECT_ATTRIBUTES oa_missing = make_oa(&missing, OBJ_CASE_INSENSITIVE);
    HANDLE none = NULL;
    ASSERT_STATUS(t_open_timer(&none, 0x1F0003, &oa_missing), STATUS_OBJECT_NAME_NOT_FOUND);
    PASS();
}

TEST(open_timer_bad_arguments) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_open_timer(&h, 0x1F0003, NULL), STATUS_INVALID_PARAMETER);
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtTimerNoObj", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    ASSERT_STATUS(t_open_timer(NULL, 0x1F0003, &oa), STATUS_INVALID_PARAMETER);
    PASS();
}

TEST(open_timer_type_mismatch) {
    reset_ob();
    /* A named event must not open as a timer. */
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtNotATimer", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    HANDLE h = NULL;
    ASSERT_STATUS(t_open_timer(&h, 0x1F0003, &oa), STATUS_OBJECT_TYPE_MISMATCH);
    PASS();
}

int main(void) {
    printf("sync extra tests starting\n");
    ObInitObjectManager();
    RUN(create_timer_types);
    RUN(create_timer_named_collision);
    RUN(open_timer_by_name);
    RUN(open_timer_bad_arguments);
    RUN(open_timer_type_mismatch);
    printf("sync extra tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    ObCleanupObjectManager();
    return 0;
}
