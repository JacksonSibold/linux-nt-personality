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
NTSTATUS nt_svc_NtCreateMutant(PHANDLE MutantHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN InitialOwner);
NTSTATUS nt_svc_NtCreateTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                              ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                              ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtOpenTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                            ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                            ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtSetTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                           ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                           ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtCancelTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                              ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                              ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtQueryTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                             ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                             ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtSignalAndWaitForSingleObject(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                               ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                               ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtYieldExecution(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                  ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                  ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS nt_svc_NtCreateKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                   ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                   ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtOpenKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                 ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                 ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtWaitForKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                    ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                    ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);
NTSTATUS nt_svc_NtReleaseKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                    ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                    ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

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

static NTSTATUS t_set_timer(HANDLE h, LARGE_INTEGER *due, void (*apc)(void *), void *ctx,
                            LONG period, BOOLEAN *prev) {
    return nt_svc_NtSetTimer((ULONG_PTR)h, (ULONG_PTR)due, (ULONG_PTR)apc, (ULONG_PTR)ctx,
                             0, (ULONG_PTR)period, (ULONG_PTR)prev, 0, 0, 0, 0, 0);
}
static NTSTATUS t_cancel_timer(HANDLE h, BOOLEAN *cur) {
    return nt_svc_NtCancelTimer((ULONG_PTR)h, (ULONG_PTR)cur, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
/* Wait through the wait engine directly: the handle-level single-object
 * wait in nt_obj.c does not accept timers. */
static NTSTATUS t_wait_handle(HANDLE h, LARGE_INTEGER *timeout) {
    struct nt_object_header *hdr = NULL;
    NTSTATUS st = ObReferenceObjectByHandle(kshim_current_process(), h, NULL, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = nt_wait_for_single_object((struct nt_dispatcher_header *)nt_object_body(hdr), 0, timeout);
    ObDereferenceObject(hdr);
    return st;
}
/* TIMER_BASIC_INFORMATION, as NtQueryTimer returns it. */
struct test_timer_basic_info { LARGE_INTEGER RemainingTime; BOOLEAN TimerState; };
static NTSTATUS t_query_timer(HANDLE h, ULONG_PTR cls, void *buf, ULONG_PTR len, ULONG *ret) {
    return nt_svc_NtQueryTimer((ULONG_PTR)h, cls, (ULONG_PTR)buf, len, (ULONG_PTR)ret, 0, 0, 0, 0, 0, 0, 0);
}
static LARGE_INTEGER rel100(int64_t ms) { LARGE_INTEGER li; li.QuadPart = -ms * 10000LL; return li; }

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

/* ------------------------------------------------------------------ */
/* NtSetTimer / NtCancelTimer                                           */
/* ------------------------------------------------------------------ */
TEST(set_timer_fires_and_reports_previous) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(50);
    BOOLEAN prev = 1;
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, &prev), STATUS_SUCCESS);
    ASSERT(prev == 0);
    LARGE_INTEGER wait = rel100(3000);
    ASSERT_STATUS(t_wait_handle(h, &wait), STATUS_WAIT_0);
    /* Still signalled: a notification timer stays set until it is set again. */
    LARGE_INTEGER poll = rel100(0);
    ASSERT_STATUS(t_wait_handle(h, &poll), STATUS_WAIT_0);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, &prev), STATUS_SUCCESS);
    ASSERT(prev == 1);
    PASS();
}

TEST(set_timer_bad_arguments) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, SynchronizationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(10);
    ASSERT_STATUS(t_set_timer(h, NULL, NULL, NULL, 0, NULL), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, -1, NULL), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(t_set_timer(NULL, &due, NULL, NULL, 0, NULL), STATUS_INVALID_HANDLE);
    PASS();
}

TEST(set_timer_wrong_type) {
    reset_ob();
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, NULL, NotificationEvent, 0), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(10);
    ASSERT_STATUS(t_set_timer(ev, &due, NULL, NULL, 0, NULL), STATUS_OBJECT_TYPE_MISMATCH);
    PASS();
}

TEST(cancel_timer_before_fire) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(400);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, NULL), STATUS_SUCCESS);
    BOOLEAN cur = 1;
    ASSERT_STATUS(t_cancel_timer(h, &cur), STATUS_SUCCESS);
    ASSERT(cur == 0);
    LARGE_INTEGER wait = rel100(700);
    ASSERT_STATUS(t_wait_handle(h, &wait), STATUS_TIMEOUT);
    PASS();
}

TEST(cancel_timer_reports_signalled) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(20);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, NULL), STATUS_SUCCESS);
    LARGE_INTEGER wait = rel100(3000);
    ASSERT_STATUS(t_wait_handle(h, &wait), STATUS_WAIT_0);
    BOOLEAN cur = 0;
    ASSERT_STATUS(t_cancel_timer(h, &cur), STATUS_SUCCESS);
    ASSERT(cur == 1);
    /* CurrentState is optional. */
    ASSERT_STATUS(t_cancel_timer(h, NULL), STATUS_SUCCESS);
    PASS();
}

/* ------------------------------------------------------------------ */
/* NtQueryTimer                                                         */
/* ------------------------------------------------------------------ */
TEST(query_timer_state_and_remaining) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    struct test_timer_basic_info info;
    ULONG ret = 0;
    /* Unarmed: not signalled, nothing remaining. */
    ASSERT_STATUS(t_query_timer(h, 0, &info, sizeof(info), &ret), STATUS_SUCCESS);
    ASSERT(ret == sizeof(info));
    ASSERT(info.TimerState == 0);
    ASSERT(info.RemainingTime.QuadPart == 0);
    /* Armed for 300 ms: still pending, some time remaining, at most the full interval. */
    LARGE_INTEGER due = rel100(300);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, NULL), STATUS_SUCCESS);
    ASSERT_STATUS(t_query_timer(h, 0, &info, sizeof(info), &ret), STATUS_SUCCESS);
    ASSERT(info.TimerState == 0);
    ASSERT(info.RemainingTime.QuadPart > 0);
    ASSERT(info.RemainingTime.QuadPart <= 300LL * 10000LL);
    /* After it fires, the state is signalled and nothing remains. */
    LARGE_INTEGER wait = rel100(3000);
    ASSERT_STATUS(t_wait_handle(h, &wait), STATUS_WAIT_0);
    ASSERT_STATUS(t_query_timer(h, 0, &info, sizeof(info), &ret), STATUS_SUCCESS);
    ASSERT(info.TimerState == 1);
    ASSERT(info.RemainingTime.QuadPart == 0);
    PASS();
}

TEST(query_timer_bad_arguments) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, SynchronizationTimer), STATUS_SUCCESS);
    struct test_timer_basic_info info;
    ULONG ret = 0;
    ASSERT_STATUS(t_query_timer(h, 1, &info, sizeof(info), &ret), STATUS_INVALID_INFO_CLASS);
    ASSERT_STATUS(t_query_timer(h, 0, &info, 8, &ret), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT(ret == sizeof(info));
    ASSERT_STATUS(t_query_timer(NULL, 0, &info, sizeof(info), &ret), STATUS_INVALID_HANDLE);
    PASS();
}

/* ------------------------------------------------------------------ */
/* NtSignalAndWaitForSingleObject                                       */
/* ------------------------------------------------------------------ */
static NTSTATUS t_signal_and_wait(HANDLE sig, HANDLE wait, BOOLEAN alertable, LARGE_INTEGER *to) {
    return nt_svc_NtSignalAndWaitForSingleObject((ULONG_PTR)sig, (ULONG_PTR)wait, alertable, (ULONG_PTR)to,
                                                 0, 0, 0, 0, 0, 0, 0, 0);
}

TEST(signal_and_wait_event_pair) {
    reset_ob();
    HANDLE a = NULL, b = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&a, 0x1F0003, NULL, SynchronizationEvent, 0), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtCreateEvent(&b, 0x1F0003, NULL, SynchronizationEvent, 1), STATUS_SUCCESS);
    /* Signal a, then wait on b, which is already signalled. */
    LARGE_INTEGER to = rel100(1000);
    ASSERT_STATUS(t_signal_and_wait(a, b, 0, &to), STATUS_WAIT_0);
    /* b was consumed by the wait (synchronization event); a is now signalled. */
    ASSERT_STATUS(t_wait_handle(a, &to), STATUS_WAIT_0);
    ASSERT_STATUS(t_wait_handle(b, &(LARGE_INTEGER){ .QuadPart = 0 }), STATUS_TIMEOUT);
    PASS();
}

TEST(signal_and_wait_mutant_release) {
    reset_ob();
    HANDLE m = NULL;
    ASSERT_STATUS(nt_svc_NtCreateMutant(&m, 0x1F0001, NULL, 0), STATUS_SUCCESS);
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, NULL, NotificationEvent, 1), STATUS_SUCCESS);
    /* Not owned by this thread: the release fails and nothing is waited on. */
    LARGE_INTEGER to = rel100(100);
    ASSERT_STATUS(t_signal_and_wait(m, ev, 0, &to), STATUS_MUTANT_NOT_OWNED);
    PASS();
}

TEST(signal_and_wait_bad_objects) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, NULL, NotificationEvent, 1), STATUS_SUCCESS);
    LARGE_INTEGER to = rel100(100);
    /* A timer cannot be signalled. */
    ASSERT_STATUS(t_signal_and_wait(h, ev, 0, &to), STATUS_OBJECT_TYPE_MISMATCH);
    /* A bad handle is rejected before anything is signalled. */
    ASSERT_STATUS(t_signal_and_wait(ev, NULL, 0, &to), STATUS_INVALID_HANDLE);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Keyed events                                                         */
/* ------------------------------------------------------------------ */
static NTSTATUS t_create_keyed(HANDLE *h, ACCESS_MASK acc, POBJECT_ATTRIBUTES oa, ULONG_PTR flags) {
    return nt_svc_NtCreateKeyedEvent((ULONG_PTR)h, acc, (ULONG_PTR)oa, flags, 0, 0, 0, 0, 0, 0, 0, 0);
}
static NTSTATUS t_open_keyed(HANDLE *h, ACCESS_MASK acc, POBJECT_ATTRIBUTES oa) {
    return nt_svc_NtOpenKeyedEvent((ULONG_PTR)h, acc, (ULONG_PTR)oa, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
static NTSTATUS t_wait_keyed(HANDLE h, void *key, LARGE_INTEGER *to) {
    return nt_svc_NtWaitForKeyedEvent((ULONG_PTR)h, (ULONG_PTR)key, 0, (ULONG_PTR)to, 0, 0, 0, 0, 0, 0, 0, 0);
}
static NTSTATUS t_release_keyed(HANDLE h, void *key, LARGE_INTEGER *to) {
    return nt_svc_NtReleaseKeyedEvent((ULONG_PTR)h, (ULONG_PTR)key, 0, (ULONG_PTR)to, 0, 0, 0, 0, 0, 0, 0, 0);
}

TEST(keyed_create_and_open) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_keyed(&h, 0x1F0003, NULL, 0), STATUS_SUCCESS);
    ASSERT(h != NULL);
    /* Flags are reserved and must be zero. */
    HANDLE bad = NULL;
    ASSERT_STATUS(t_create_keyed(&bad, 0x1F0003, NULL, 1), STATUS_INVALID_PARAMETER);
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtKeyed", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE named = NULL;
    ASSERT_STATUS(t_create_keyed(&named, 0x1F0003, &oa, 0), STATUS_SUCCESS);
    HANDLE opened = NULL;
    ASSERT_STATUS(t_open_keyed(&opened, 0x1F0003, &oa), STATUS_SUCCESS);
    UNICODE_STRING miss;
    make_us(&miss, "\\BaseNamedObjects\\ExtKeyedMissing", wbuf2, sizeof(wbuf2));
    OBJECT_ATTRIBUTES oa_miss = make_oa(&miss, OBJ_CASE_INSENSITIVE);
    HANDLE none = NULL;
    ASSERT_STATUS(t_open_keyed(&none, 0x1F0003, &oa_miss), STATUS_OBJECT_NAME_NOT_FOUND);
    PASS();
}

TEST(keyed_wait_times_out) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_keyed(&h, 0x1F0003, NULL, 0), STATUS_SUCCESS);
    int key_marker = 0;
    LARGE_INTEGER to = rel100(50);
    ASSERT_STATUS(t_wait_keyed(h, &key_marker, &to), STATUS_TIMEOUT);
    PASS();
}

struct keyed_wait_arg { HANDLE h; void *key; struct nt_eprocess *proc; NTSTATUS st; };
static void *keyed_waiter_thread(void *p) {
    struct keyed_wait_arg *a = (struct keyed_wait_arg *)p;
    kshim_set_current_process(a->proc);
    struct nt_thread *t = NULL;
    if (nt_thread_create(&t) == STATUS_SUCCESS) nt_set_current_thread(t);
    LARGE_INTEGER to = rel100(3000);
    a->st = t_wait_keyed(a->h, a->key, &to);
    return NULL;
}

TEST(keyed_release_wakes_waiter) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_keyed(&h, 0x1F0003, NULL, 0), STATUS_SUCCESS);
    int key_marker = 0;
    struct keyed_wait_arg arg = { h, &key_marker, kshim_current_process(), STATUS_UNSUCCESSFUL };
    pthread_t pt;
    ASSERT(pthread_create(&pt, NULL, keyed_waiter_thread, &arg) == 0);
    usleep(200000);
    LARGE_INTEGER to = rel100(1000);
    ASSERT_STATUS(t_release_keyed(h, &key_marker, &to), STATUS_SUCCESS);
    pthread_join(pt, NULL);
    ASSERT_STATUS(arg.st, STATUS_WAIT_0);
    PASS();
}




/* ------------------------------------------------------------------ */
/* Timers as waitable objects (NtWaitForSingleObject / ...Multiple...)  */
/* ------------------------------------------------------------------ */
TEST(wait_single_sync_timer_auto_reset) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, SynchronizationTimer), STATUS_SUCCESS);
    LARGE_INTEGER poll = rel100(0);
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(h, 0, &poll), STATUS_TIMEOUT);
    LARGE_INTEGER due = rel100(20);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, NULL), STATUS_SUCCESS);
    LARGE_INTEGER wait = rel100(3000);
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(h, 0, &wait), STATUS_WAIT_0);
    /* A synchronization timer resets when a wait consumes it. */
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(h, 0, &poll), STATUS_TIMEOUT);
    PASS();
}

TEST(wait_single_notification_timer_stays_set) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(20);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, NULL), STATUS_SUCCESS);
    LARGE_INTEGER wait = rel100(3000);
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(h, 0, &wait), STATUS_WAIT_0);
    LARGE_INTEGER poll = rel100(0);
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(h, 0, &poll), STATUS_WAIT_0);
    PASS();
}

/* Due time of two seconds or more: must neither fire early nor after a
 * fixed one-second chunk. */
TEST(set_timer_long_due_time_honoured) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, SynchronizationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(2500);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, NULL), STATUS_SUCCESS);
    LARGE_INTEGER early = rel100(2000);
    ASSERT_STATUS(t_wait_handle(h, &early), STATUS_TIMEOUT);
    LARGE_INTEGER wait = rel100(3000);
    ASSERT_STATUS(t_wait_handle(h, &wait), STATUS_WAIT_0);
    PASS();
}

/* Periodic timer: each period signals once; a waiter consumes each one. */
TEST(set_timer_period_repeats) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, SynchronizationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(100);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 100, NULL), STATUS_SUCCESS);
    int hits = 0;
    for (int i = 0; i < 5; i++) {
        LARGE_INTEGER w = rel100(1000);
        if (t_wait_handle(h, &w) == STATUS_WAIT_0) hits++;
        else break;
    }
    ASSERT(hits == 5);
    ASSERT_STATUS(t_cancel_timer(h, NULL), STATUS_SUCCESS);
    PASS();
}

/* Cancel must return promptly, and the cancelled expiry must not fire. */
TEST(cancel_timer_is_prompt) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, SynchronizationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(5000);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, NULL), STATUS_SUCCESS);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ASSERT_STATUS(t_cancel_timer(h, NULL), STATUS_SUCCESS);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    ASSERT(ms < 500);
    LARGE_INTEGER poll = rel100(100);
    ASSERT_STATUS(t_wait_handle(h, &poll), STATUS_TIMEOUT);
    PASS();
}

/* A user APC routine cannot be delivered (no APC queue exists), so NtSetTimer
 * must refuse it before arming and the routine must never run from the timer. */
static _Atomic int g_apc_calls;
static void test_timer_apc(void *ctx) { (void)ctx; atomic_fetch_add(&g_apc_calls, 1); }
TEST(set_timer_rejects_user_apc) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, SynchronizationTimer), STATUS_SUCCESS);
    atomic_store(&g_apc_calls, 0);
    LARGE_INTEGER due = rel100(20);
    ASSERT_STATUS(t_set_timer(h, &due, test_timer_apc, NULL, 0, NULL), STATUS_NOT_IMPLEMENTED);
    LARGE_INTEGER poll = rel100(200);
    ASSERT_STATUS(t_wait_handle(h, &poll), STATUS_TIMEOUT);
    ASSERT(atomic_load(&g_apc_calls) == 0);
    PASS();
}

/* ------------------------------------------------------------------ */
/* NtYieldExecution                                                     */
/* ------------------------------------------------------------------ */
TEST(yield_execution_succeeds) {
    reset_ob();
    ASSERT_STATUS(nt_svc_NtYieldExecution(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), STATUS_SUCCESS);
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
    RUN(set_timer_fires_and_reports_previous);
    RUN(set_timer_bad_arguments);
    RUN(set_timer_wrong_type);
    RUN(cancel_timer_before_fire);
    RUN(cancel_timer_reports_signalled);
    RUN(query_timer_state_and_remaining);
    RUN(query_timer_bad_arguments);
    RUN(wait_single_sync_timer_auto_reset);
    RUN(wait_single_notification_timer_stays_set);
    RUN(set_timer_long_due_time_honoured);
    RUN(set_timer_period_repeats);
    RUN(cancel_timer_is_prompt);
    RUN(set_timer_rejects_user_apc);
    RUN(signal_and_wait_event_pair);
    RUN(signal_and_wait_mutant_release);
    RUN(signal_and_wait_bad_objects);
    RUN(keyed_create_and_open);
    RUN(keyed_wait_times_out);
    RUN(keyed_release_wakes_waiter);
    RUN(yield_execution_succeeds);
    printf("sync extra tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    ObCleanupObjectManager();
    return 0;
}
