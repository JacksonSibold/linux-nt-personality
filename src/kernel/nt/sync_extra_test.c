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
NTSTATUS nt_svc_NtCreateSemaphore(PHANDLE SemaphoreHandle, ACCESS_MASK DesiredAccess,
                                  POBJECT_ATTRIBUTES ObjectAttributes,
                                  LONG InitialCount, LONG MaximumCount);
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
#include <time.h>

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
/* Wait through the wait engine directly, without the handle lookup and type
 * check that nt_svc_NtWaitForSingleObject (nt_obj.c) performs. */
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
/* NtCreateEvent over a name held by another type                        */
/* ------------------------------------------------------------------ */
/* The create path looks the name up by type, misses, and then inserts. The
 * insert reports NAME_COLLISION and the path used to return that status without
 * setting its handle, so the service copied an uninitialised value to the caller.
 * Two creators racing on a new name take the same branch. A mutant holding the
 * name makes the type lookup miss deterministically. Windows answers with
 * OBJECT_TYPE_MISMATCH here, and so must this. */
TEST(create_event_name_held_by_mutant) {
    reset_ob();
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtEvOverMutant", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE m = NULL;
    ASSERT_STATUS(nt_svc_NtCreateMutant(&m, 0x1F0003, &oa, 0), STATUS_SUCCESS);
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, &oa, NotificationEvent, 0), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(ev == NULL);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Named creates over a name held by another type (SYNC-3 policy)       */
/* ------------------------------------------------------------------ */
/* ObInsertObject reports NAME_COLLISION when another type holds the name.
 * The service must not copy an indeterminate or NULL handle back. The holder
 * is opened by type: another type gives OBJECT_TYPE_MISMATCH and no handle.
 * The output stays at its sentinel. */
#define SYNC_SENTINEL ((HANDLE)(ULONG_PTR)0x5E171E1)
extern struct nt_object_type nt_mutant_type; /* nt_obj.c */

TEST(create_mutant_name_held_by_event) {
    reset_ob();
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtMutOverEv", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    HANDLE m = SYNC_SENTINEL;
    ASSERT_STATUS(nt_svc_NtCreateMutant(&m, 0x1F0003, &oa, 0), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(m == SYNC_SENTINEL);
    PASS();
}

TEST(create_semaphore_name_held_by_event) {
    reset_ob();
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtSemOverEv", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    HANDLE s = SYNC_SENTINEL;
    ASSERT_STATUS(nt_svc_NtCreateSemaphore(&s, 0x1F0003, &oa, 1, 2), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(s == SYNC_SENTINEL);
    PASS();
}

TEST(create_timer_name_held_by_event) {
    reset_ob();
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtTimOverEv", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    HANDLE t = SYNC_SENTINEL;
    ASSERT_STATUS(t_create_timer(&t, 0x1F0003, &oa, NotificationTimer), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(t == SYNC_SENTINEL);
    PASS();
}

/* The shared holder helper, same type: a valid handle with NAME_COLLISION, and
 * closing it returns every allocation (the leak tracker). */
TEST(holder_after_collision_same_type_leaves_no_leak) {
    reset_ob();
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtHolderMut", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE holder = NULL;
    ASSERT_STATUS(nt_svc_NtCreateMutant(&holder, 0x1F0003, &oa, 0), STATUS_SUCCESS);
    long before = atomic_load(&kshim_alloc_count);
    for (int i = 0; i < 25; i++) {
        HANDLE h = SYNC_SENTINEL;
        ASSERT_STATUS(ObOpenNamedHolderAfterCollision(&nt_mutant_type, &oa, 0x1F0003, kshim_current_process(), &h),
                      STATUS_OBJECT_NAME_COLLISION);
        ASSERT(h != NULL && h != SYNC_SENTINEL);
        ASSERT_STATUS(ObCloseHandle(kshim_current_process(), h), STATUS_SUCCESS);
    }
    ASSERT(atomic_load(&kshim_alloc_count) == before);
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

/* A relative due time of INT64_MIN is 2^63 units, which cannot be negated in int64_t.
 * It must not overflow into a past expiry: the timer stays unsignalled. */
TEST(set_timer_int64_min_due_does_not_fire) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due;
    due.QuadPart = INT64_MIN;
    BOOLEAN prev = 0;
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, &prev), STATUS_SUCCESS);
    /* 300 ms: a past expiry from an overflow would have fired by now. */
    LARGE_INTEGER wait = rel100(300);
    ASSERT_STATUS(t_wait_handle(h, &wait), STATUS_TIMEOUT);
    PASS();
}

/* A wait timeout of INT64_MIN is 2^63 units, which cannot be negated in int64_t.
 * It must not overflow into a past deadline: the wait lasts until the timer fires. */
TEST(wait_int64_min_timeout_does_not_expire) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_timer(&h, 0x1F0003, NULL, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = rel100(100);
    ASSERT_STATUS(t_set_timer(h, &due, NULL, NULL, 0, NULL), STATUS_SUCCESS);
    LARGE_INTEGER wait;
    wait.QuadPart = INT64_MIN;
    ASSERT_STATUS(t_wait_handle(h, &wait), STATUS_WAIT_0);
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

/* Rights: the signalled handle needs EVENT_MODIFY_STATE and the wait handle needs
 * SYNCHRONIZE. A handle without the right is refused before anything is signalled. */
#define T_SYNCHRONIZE 0x00100000UL
#define T_EVENT_MODIFY_STATE 0x0002UL
TEST(signal_and_wait_checks_rights) {
    reset_ob();
    HANDLE sync_only = NULL, modify_only = NULL, full = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&sync_only, T_SYNCHRONIZE, NULL, SynchronizationEvent, 0), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtCreateEvent(&modify_only, T_EVENT_MODIFY_STATE, NULL, SynchronizationEvent, 0), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtCreateEvent(&full, 0x1F0003, NULL, SynchronizationEvent, 1), STATUS_SUCCESS);
    LARGE_INTEGER to = rel100(100);
    /* Signalling needs EVENT_MODIFY_STATE: SYNCHRONIZE alone is refused. */
    ASSERT_STATUS(t_signal_and_wait(sync_only, full, 0, &to), STATUS_ACCESS_DENIED);
    /* Waiting needs SYNCHRONIZE: EVENT_MODIFY_STATE alone is refused. */
    ASSERT_STATUS(t_signal_and_wait(full, modify_only, 0, &to), STATUS_ACCESS_DENIED);
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

TEST(keyed_create_name_held_by_event) {
    reset_ob();
    UNICODE_STRING us;
    make_us(&us, "\\BaseNamedObjects\\ExtKeyOverEv", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE ev = NULL;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&ev, 0x1F0003, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    HANDLE k = SYNC_SENTINEL;
    ASSERT_STATUS(t_create_keyed(&k, 0x1F0003, &oa, 0), STATUS_OBJECT_TYPE_MISMATCH);
    ASSERT(k == SYNC_SENTINEL);
    PASS();
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

/* Rights: NtWaitForKeyedEvent needs KEYEDEVENT_WAIT and NtReleaseKeyedEvent needs
 * KEYEDEVENT_WAKE. A handle without the right is refused, and the other right works. */
#define T_KEYEDEVENT_WAIT 0x0001UL
#define T_KEYEDEVENT_WAKE 0x0002UL
TEST(keyed_event_checks_rights) {
    reset_ob();
    HANDLE wake_only = NULL, wait_only = NULL;
    ASSERT_STATUS(t_create_keyed(&wake_only, T_KEYEDEVENT_WAKE, NULL, 0), STATUS_SUCCESS);
    ASSERT_STATUS(t_create_keyed(&wait_only, T_KEYEDEVENT_WAIT, NULL, 0), STATUS_SUCCESS);
    int key_marker = 0;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    ASSERT_STATUS(t_wait_keyed(wake_only, &key_marker, &zero), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(t_release_keyed(wait_only, &key_marker, &zero), STATUS_ACCESS_DENIED);
    ASSERT_STATUS(t_wait_keyed(wait_only, &key_marker, &zero), STATUS_TIMEOUT);
    ASSERT_STATUS(t_release_keyed(wake_only, &key_marker, &zero), STATUS_TIMEOUT);
    PASS();
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

/* With no waiter, a release times out (zero or expired timeout) rather than
 * succeeding and dropping the release. */
TEST(keyed_release_without_waiter_times_out) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_keyed(&h, 0x1F0003, NULL, 0), STATUS_SUCCESS);
    int key_marker = 0;
    LARGE_INTEGER zero = rel100(0);
    ASSERT_STATUS(t_release_keyed(h, &key_marker, &zero), STATUS_TIMEOUT);
    LARGE_INTEGER to = rel100(100);
    ASSERT_STATUS(t_release_keyed(h, &key_marker, &to), STATUS_TIMEOUT);
    PASS();
}

struct keyed_release_arg { HANDLE h; void *key; struct nt_eprocess *proc; NTSTATUS st; _Atomic int done; };
static void *keyed_releaser_thread(void *p) {
    struct keyed_release_arg *a = (struct keyed_release_arg *)p;
    kshim_set_current_process(a->proc);
    struct nt_thread *t = NULL;
    if (nt_thread_create(&t) == STATUS_SUCCESS) nt_set_current_thread(t);
    LARGE_INTEGER to = rel100(3000);
    a->st = t_release_keyed(a->h, a->key, &to);
    atomic_store(&a->done, 1);
    return NULL;
}

/* A release with no waiter blocks until a waiter arrives, then both return. */
TEST(keyed_release_blocks_until_waiter) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_keyed(&h, 0x1F0003, NULL, 0), STATUS_SUCCESS);
    int key_marker = 0;
    struct keyed_release_arg rel = { h, &key_marker, kshim_current_process(), STATUS_UNSUCCESSFUL, 0 };
    pthread_t pr;
    ASSERT(pthread_create(&pr, NULL, keyed_releaser_thread, &rel) == 0);
    usleep(200000);
    int returned_early = atomic_load(&rel.done);
    struct keyed_wait_arg wa = { h, &key_marker, kshim_current_process(), STATUS_UNSUCCESSFUL };
    pthread_t pw;
    ASSERT(pthread_create(&pw, NULL, keyed_waiter_thread, &wa) == 0);
    pthread_join(pw, NULL);
    pthread_join(pr, NULL);
    if (returned_early) { fprintf(stderr, "FAIL %s:%d: release returned before any waiter\n", __FILE__, __LINE__); g_fail++; return; }
    ASSERT_STATUS(wa.st, STATUS_WAIT_0);
    ASSERT_STATUS(rel.st, STATUS_SUCCESS);
    PASS();
}

static void *keyed_releaser_null_thread(void *p) {
    struct keyed_release_arg *a = (struct keyed_release_arg *)p;
    kshim_set_current_process(a->proc);
    struct nt_thread *t = NULL;
    if (nt_thread_create(&t) == STATUS_SUCCESS) nt_set_current_thread(t);
    a->st = t_release_keyed(a->h, a->key, NULL);
    atomic_store(&a->done, 1);
    return NULL;
}

/* A release with a NULL timeout (no limit) that is queued before any waiter is
 * taken by a waiter that arrives later. Both calls return once they match. */
TEST(keyed_release_null_timeout_matched_by_waiter) {
    reset_ob();
    HANDLE h = NULL;
    ASSERT_STATUS(t_create_keyed(&h, 0x1F0003, NULL, 0), STATUS_SUCCESS);
    int key_marker = 0;
    struct keyed_release_arg rel = { h, &key_marker, kshim_current_process(), STATUS_UNSUCCESSFUL, 0 };
    pthread_t pr;
    ASSERT(pthread_create(&pr, NULL, keyed_releaser_null_thread, &rel) == 0);
    usleep(100000);
    struct keyed_wait_arg wa = { h, &key_marker, kshim_current_process(), STATUS_UNSUCCESSFUL };
    pthread_t pw;
    ASSERT(pthread_create(&pw, NULL, keyed_waiter_thread, &wa) == 0);
    pthread_join(pw, NULL);
    pthread_join(pr, NULL);
    ASSERT_STATUS(wa.st, STATUS_WAIT_0);
    ASSERT_STATUS(rel.st, STATUS_SUCCESS);
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

/* Regression tests for the wait engine's wakeup path (see sync.c,
 * wake_waiters_for_header and nt_wait_for_multiple_objects). Each waiter is a
 * pthread with its own nt_thread, as in sync_test.c. Every wait has a timeout,
 * so a lost wakeup fails the test instead of hanging it. */
typedef struct { struct nt_thread *thr; void *(*fn)(void *); void *arg; pthread_t pt; } ext_wrap_t;
static void *ext_wrap_run(void *p) {
    ext_wrap_t *w = (ext_wrap_t *)p;
    nt_set_current_thread(w->thr);
    void *r = w->fn(w->arg);
    nt_set_current_thread(NULL);
    return r;
}
static int ext_start(ext_wrap_t *w, void *(*fn)(void *), void *arg) {
    if (nt_thread_create(&w->thr) != STATUS_SUCCESS) return -1;
    w->fn = fn; w->arg = arg;
    return pthread_create(&w->pt, NULL, ext_wrap_run, w);
}
static void ext_join(ext_wrap_t *w) {
    pthread_join(w->pt, NULL);
    nt_thread_destroy(w->thr);
}
static LARGE_INTEGER ext_rel_ms(long ms) { LARGE_INTEGER li; li.QuadPart = -(int64_t)ms * 10000LL; return li; }

/* A manual-reset set must wake every waiter. The old snapshot woke at most 64,
 * so with 100 waiters the rest stayed asleep until their timeout. */
#define EXT_WAKE_ALL_N 100
static atomic_int g_ext_woken;
static atomic_int g_ext_arrived;
static int64_t g_ext_set_ns;
static int64_t ext_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
/* A waiter counts as woken only if it returns promptly after the set. A
 * waiter whose wakeup was dropped still succeeds once its 5 s timeout expires
 * and it re-checks the (set) event, so success alone does not prove a wakeup. */
static void *ext_wake_all_waiter(void *p) {
    LARGE_INTEGER t = ext_rel_ms(5000);
    atomic_fetch_add(&g_ext_arrived, 1);
    if (nt_wait_for_single_object((struct nt_dispatcher_header *)p, false, &t) == STATUS_WAIT_0 &&
        ext_now_ns() - g_ext_set_ns < 2000LL * 1000000LL)
        atomic_fetch_add(&g_ext_woken, 1);
    return NULL;
}
static void test_wake_all_waiters_beyond_64(void) {
    struct nt_event *ev = NULL;
    ASSERT_STATUS(nt_create_event(&ev, NotificationEvent, false), STATUS_SUCCESS);
    ext_wrap_t w[EXT_WAKE_ALL_N];
    atomic_store(&g_ext_woken, 0);
    atomic_store(&g_ext_arrived, 0);
    for (int i = 0; i < EXT_WAKE_ALL_N; i++) {
        if (ext_start(&w[i], ext_wake_all_waiter, &ev->header) != 0) { fprintf(stderr, "FAIL thread start\n"); g_fail++; return; }
    }
    /* Wait until every waiter has reached its wait, then give them time to
     * link into the event's wait list. Only then set the event. */
    for (int i = 0; i < 10000 && atomic_load(&g_ext_arrived) < EXT_WAKE_ALL_N; i++) usleep(1000);
    usleep(200 * 1000);
    g_ext_set_ns = ext_now_ns();
    nt_set_event(ev, NULL);
    for (int i = 0; i < EXT_WAKE_ALL_N; i++) ext_join(&w[i]);
    if (atomic_load(&g_ext_woken) != EXT_WAKE_ALL_N) {
        fprintf(stderr, "FAIL wake_all: %d of %d waiters woke\n", atomic_load(&g_ext_woken), EXT_WAKE_ALL_N);
        g_fail++; return;
    }
    nt_put_event(ev);
    PASS();
}

/* Auto-reset handoff ping-pong. The signal can land in the gap between a
 * waiter's failed try and its link into the object's wait list; that gap lost
 * the wakeup, so one handoff in a long run would stall. 2000 rounds in each
 * direction, each wait bounded by a timeout. */
#define EXT_PP_ROUNDS 2000
static struct nt_event *g_pp_a, *g_pp_b;
static atomic_int g_pp_fail;
static void *ext_pp_worker(void *p) {
    (void)p;
    for (int i = 0; i < EXT_PP_ROUNDS; i++) {
        LARGE_INTEGER t = ext_rel_ms(5000);
        if (nt_wait_for_single_object(&g_pp_a->header, false, &t) != STATUS_WAIT_0) {
            atomic_store(&g_pp_fail, 1);
            return NULL;
        }
        nt_set_event(g_pp_b, NULL);
    }
    return NULL;
}
static void test_auto_reset_handoff_loop(void) {
    ASSERT_STATUS(nt_create_event(&g_pp_a, SynchronizationEvent, false), STATUS_SUCCESS);
    ASSERT_STATUS(nt_create_event(&g_pp_b, SynchronizationEvent, false), STATUS_SUCCESS);
    atomic_store(&g_pp_fail, 0);
    ext_wrap_t w;
    if (ext_start(&w, ext_pp_worker, NULL) != 0) { fprintf(stderr, "FAIL thread start\n"); g_fail++; return; }
    int stalled = 0;
    for (int i = 0; i < EXT_PP_ROUNDS && !stalled; i++) {
        nt_set_event(g_pp_a, NULL);
        LARGE_INTEGER t = ext_rel_ms(5000);
        if (nt_wait_for_single_object(&g_pp_b->header, false, &t) != STATUS_WAIT_0) stalled = 1;
    }
    ext_join(&w);
    nt_put_event(g_pp_a);
    nt_put_event(g_pp_b);
    g_pp_a = g_pp_b = NULL;
    if (stalled || atomic_load(&g_pp_fail)) { fprintf(stderr, "FAIL handoff: lost wakeup\n"); g_fail++; return; }
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
    RUN(create_event_name_held_by_mutant);
    RUN(create_mutant_name_held_by_event);
    RUN(create_semaphore_name_held_by_event);
    RUN(create_timer_name_held_by_event);
    RUN(holder_after_collision_same_type_leaves_no_leak);
    RUN(set_timer_fires_and_reports_previous);
    RUN(set_timer_int64_min_due_does_not_fire);
    RUN(wait_int64_min_timeout_does_not_expire);
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
    RUN(signal_and_wait_checks_rights);
    RUN(keyed_create_name_held_by_event);
    RUN(keyed_create_and_open);
    RUN(keyed_wait_times_out);
    RUN(keyed_event_checks_rights);
    RUN(keyed_release_wakes_waiter);
    RUN(keyed_release_without_waiter_times_out);
    RUN(keyed_release_blocks_until_waiter);
    RUN(keyed_release_null_timeout_matched_by_waiter);
    RUN(yield_execution_succeeds);
    RUN(wake_all_waiters_beyond_64);
    RUN(auto_reset_handoff_loop);
    printf("sync extra tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    ObCleanupObjectManager();
    return 0;
}
