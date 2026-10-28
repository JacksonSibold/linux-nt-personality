/* driver_sync_test.c - unit tests for the synchronization surface of the NT driver host (drvhost_sync.c): mutexes,
 * semaphores, multi-object waits, guarded mutexes, critical regions, device queues, executive resources, SLISTs,
 * lookaside lists and the interlocked list helpers. Threads are plain pthreads; the host adopts them.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dh_test.h"
#include "drvhost_int.h"

static LARGE_INTEGER rel_ms(long ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(long long)ms * 10000;
    return t;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- mutex --------------------------------------------------------------- */
static KMUTEX g_mutex;
static volatile int g_got_mutex;

static void *mutex_thread(void *arg)
{
    (void)arg;
    LARGE_INTEGER t = rel_ms(2000);
    NTSTATUS st = KeWaitForSingleObject(&g_mutex, Executive, KernelMode, FALSE, &t);
    if (st == STATUS_SUCCESS) {
        g_got_mutex = 1;
        KeReleaseMutex(&g_mutex, FALSE);
    }
    return NULL;
}

static void test_mutex(void)
{
    KeInitializeMutex(&g_mutex, 0);
    CHECK(g_mutex.Header.Type == DISPATCHER_MUTANT_OBJECT);
    CHECK(KeReadStateMutex(&g_mutex) == 1);

    LARGE_INTEGER zero = rel_ms(0);
    CHECK_STATUS(KeWaitForSingleObject(&g_mutex, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    CHECK(KeReadStateMutex(&g_mutex) == 0);
    CHECK_STATUS(KeWaitForSingleObject(&g_mutex, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS); /* recursive */
    CHECK(KeReadStateMutex(&g_mutex) == -1);

    /* Another thread has to wait until the owner has released both levels. */
    g_got_mutex = 0;
    pthread_t th;
    pthread_create(&th, NULL, mutex_thread, NULL);
    sleep_ms(50);
    CHECK(g_got_mutex == 0);
    CHECK(KeReleaseMutex(&g_mutex, FALSE) == -1);
    sleep_ms(30);
    CHECK(g_got_mutex == 0);
    CHECK(KeReleaseMutex(&g_mutex, FALSE) == 0);
    pthread_join(th, NULL);
    CHECK(g_got_mutex == 1);
    CHECK(KeReadStateMutex(&g_mutex) == 1);
}

/* ---- semaphore ----------------------------------------------------------- */
static void test_semaphore(void)
{
    KSEMAPHORE s;
    LARGE_INTEGER zero = rel_ms(0), shortwait = rel_ms(20);

    KeInitializeSemaphore(&s, 0, 3);
    CHECK(s.Header.Type == DISPATCHER_SEMAPHORE_OBJECT);
    CHECK(s.Limit == 3);
    CHECK_STATUS(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);
    CHECK(KeReleaseSemaphore(&s, 0, 2, FALSE) == 0);
    CHECK(KeReadStateSemaphore(&s) == 2);
    CHECK_STATUS(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, &shortwait), STATUS_SUCCESS);
    CHECK_STATUS(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, &shortwait), STATUS_SUCCESS);
    CHECK_STATUS(KeWaitForSingleObject(&s, Executive, KernelMode, FALSE, &shortwait), STATUS_TIMEOUT);
    CHECK(KeReleaseSemaphore(&s, 0, 3, FALSE) == 0);
    CHECK(KeReadStateSemaphore(&s) == 3);
}

/* ---- waiting on several objects ------------------------------------------ */
static KEVENT g_ev_a, g_ev_b;
static KSEMAPHORE g_sem_c;

static void *signal_later(void *arg)
{
    sleep_ms((long)(intptr_t)arg);
    KeSetEvent(&g_ev_b, 0, FALSE);
    return NULL;
}

static void test_wait_multiple(void)
{
    LARGE_INTEGER zero = rel_ms(0), shortwait = rel_ms(30), longwait = rel_ms(2000);
    PVOID objs[3] = { &g_ev_a, &g_ev_b, &g_sem_c };
    KWAIT_BLOCK blocks[3];

    KeInitializeEvent(&g_ev_a, SynchronizationEvent, FALSE);
    KeInitializeEvent(&g_ev_b, NotificationEvent, FALSE);
    KeInitializeSemaphore(&g_sem_c, 0, 5);

    CHECK_STATUS(KeWaitForMultipleObjects(3, objs, WaitAny, Executive, KernelMode, FALSE, &zero, blocks), STATUS_TIMEOUT);

    /* wait-any returns the index of the first signalled object, and consumes only that one */
    KeSetEvent(&g_ev_a, 0, FALSE);
    KeReleaseSemaphore(&g_sem_c, 0, 1, FALSE);
    CHECK_STATUS(KeWaitForMultipleObjects(3, objs, WaitAny, Executive, KernelMode, FALSE, &zero, blocks), STATUS_WAIT_0);
    CHECK(KeReadStateEvent(&g_ev_a) == 0);
    CHECK(KeReadStateSemaphore(&g_sem_c) == 1);
    CHECK_STATUS(KeWaitForMultipleObjects(3, objs, WaitAny, Executive, KernelMode, FALSE, &zero, blocks), STATUS_WAIT_0 + 2);
    CHECK(KeReadStateSemaphore(&g_sem_c) == 0);

    /* wait-all takes all of them together or none */
    KeSetEvent(&g_ev_a, 0, FALSE);
    CHECK_STATUS(KeWaitForMultipleObjects(3, objs, WaitAll, Executive, KernelMode, FALSE, &shortwait, blocks), STATUS_TIMEOUT);
    CHECK(KeReadStateEvent(&g_ev_a) == 1);         /* nothing was consumed */
    KeSetEvent(&g_ev_b, 0, FALSE);
    KeReleaseSemaphore(&g_sem_c, 0, 2, FALSE);
    CHECK_STATUS(KeWaitForMultipleObjects(3, objs, WaitAll, Executive, KernelMode, FALSE, &zero, blocks), STATUS_WAIT_0);
    CHECK(KeReadStateEvent(&g_ev_a) == 0);
    CHECK(KeReadStateEvent(&g_ev_b) == 1);          /* a notification event stays set */
    CHECK(KeReadStateSemaphore(&g_sem_c) == 1);

    /* a signal from another thread wakes the wait */
    KeClearEvent(&g_ev_b);
    KeResetEvent(&g_ev_a);
    KeInitializeSemaphore(&g_sem_c, 0, 5);
    pthread_t th;
    pthread_create(&th, NULL, signal_later, (void *)(intptr_t)40);
    CHECK_STATUS(KeWaitForMultipleObjects(3, objs, WaitAny, Executive, KernelMode, FALSE, &longwait, blocks), STATUS_WAIT_0 + 1);
    pthread_join(th, NULL);

    /* argument checks */
    CHECK_STATUS(KeWaitForMultipleObjects(0, objs, WaitAny, Executive, KernelMode, FALSE, &zero, blocks), STATUS_INVALID_PARAMETER);
    CHECK_STATUS(KeWaitForMultipleObjects(4, objs, WaitAny, Executive, KernelMode, FALSE, &zero, NULL), STATUS_INVALID_PARAMETER);
    CHECK_STATUS(KeWaitForMultipleObjects(65, objs, WaitAny, Executive, KernelMode, FALSE, &zero, blocks), STATUS_INVALID_PARAMETER);
}

/* ---- guarded mutex and regions ------------------------------------------- */
static KGUARDED_MUTEX g_gm;
static volatile int g_gm_inside, g_gm_max;

static void *gm_thread(void *arg)
{
    for (int i = 0; i < 200; i++) {
        KeAcquireGuardedMutex(&g_gm);
        int inside = __atomic_add_fetch(&g_gm_inside, 1, __ATOMIC_SEQ_CST);
        if (inside > g_gm_max) g_gm_max = inside;
        __atomic_sub_fetch(&g_gm_inside, 1, __ATOMIC_SEQ_CST);
        KeReleaseGuardedMutex(&g_gm);
    }
    return arg;
}

static void test_guarded_mutex(void)
{
    CHECK(!KeAreApcsDisabled());
    KeEnterCriticalRegion();
    CHECK(KeAreApcsDisabled());
    KeEnterCriticalRegion();
    KeLeaveCriticalRegion();
    CHECK(KeAreApcsDisabled());
    KeLeaveCriticalRegion();
    CHECK(!KeAreApcsDisabled());
    KeEnterGuardedRegion();
    CHECK(KeAreApcsDisabled());
    KeLeaveGuardedRegion();
    CHECK(!KeAreApcsDisabled());

    KeInitializeGuardedMutex(&g_gm);
    CHECK(g_gm.Count == 1);
    KeAcquireGuardedMutex(&g_gm);
    CHECK(g_gm.Count == 0);
    CHECK(KeAreApcsDisabled());                    /* the guarded region is entered */
    CHECK(!KeTryToAcquireGuardedMutex(&g_gm));  /* held: a try-acquire fails */
    CHECK(KeAreApcsDisabled());
    KeReleaseGuardedMutex(&g_gm);
    CHECK(g_gm.Count == 1);
    CHECK(!KeAreApcsDisabled());

    /* mutual exclusion across threads */
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, gm_thread, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK(g_gm_max == 1);
}

/* ---- device queue -------------------------------------------------------- */
static void test_device_queue(void)
{
    KDEVICE_QUEUE q;
    KDEVICE_QUEUE_ENTRY e[4];

    KeInitializeDeviceQueue(&q);
    CHECK(!q.Busy);
    CHECK(q.Size == sizeof q);
    /* the first insert finds the queue idle and does not queue the entry */
    CHECK(!KeInsertDeviceQueue(&q, &e[0]));
    CHECK(q.Busy);
    CHECK(!e[0].Inserted);
    CHECK(KeInsertDeviceQueue(&q, &e[1]));
    CHECK(KeInsertByKeyDeviceQueue(&q, &e[2], 5));
    CHECK(KeInsertByKeyDeviceQueue(&q, &e[3], 1));  /* e[1] has key 0 */
    CHECK(e[1].Inserted && e[2].Inserted && e[3].Inserted);

    /* removal order: key 0, 1, 5 */
    CHECK(KeRemoveDeviceQueue(&q) == &e[1]);
    CHECK(!e[1].Inserted);
    CHECK(KeRemoveByKeyDeviceQueue(&q, 2) == &e[2]); /* first entry with key >= 2 */
    CHECK(KeRemoveEntryDeviceQueue(&q, &e[3]));
    CHECK(!KeRemoveEntryDeviceQueue(&q, &e[3]));
    CHECK(KeRemoveDeviceQueue(&q) == NULL);
    CHECK(!q.Busy);

    /* a key above every entry wraps to the first entry */
    KeInsertDeviceQueue(&q, &e[0]);
    KeInsertByKeyDeviceQueue(&q, &e[1], 3);
    KeInsertByKeyDeviceQueue(&q, &e[2], 4);
    CHECK(KeRemoveByKeyDeviceQueue(&q, 100) == &e[1]);
    CHECK(KeRemoveDeviceQueue(&q) == &e[2]);
    CHECK(KeRemoveDeviceQueue(&q) == NULL);
}

/* ---- executive resources ------------------------------------------------- */
static ERESOURCE g_res;
static volatile int g_res_phase;

static void *res_reader(void *arg)
{
    (void)arg;
    BOOLEAN got = ExAcquireResourceSharedLite(&g_res, TRUE);
    if (got) {
        __atomic_add_fetch(&g_res_phase, 1, __ATOMIC_SEQ_CST);
        sleep_ms(30);
        ExReleaseResourceLite(&g_res);
    }
    return NULL;
}

static void *res_writer(void *arg)
{
    (void)arg;
    ExAcquireResourceExclusiveLite(&g_res, TRUE);
    g_res_phase = 100;
    ExReleaseResourceLite(&g_res);
    return NULL;
}

static void test_resources(void)
{
    CHECK_STATUS(ExInitializeResourceLite(&g_res), STATUS_SUCCESS);
    CHECK(g_res.ActiveCount == 0);

    /* exclusive, recursive */
    CHECK(ExAcquireResourceExclusiveLite(&g_res, FALSE));
    CHECK(ExIsResourceAcquiredExclusiveLite(&g_res));
    CHECK(ExAcquireResourceExclusiveLite(&g_res, FALSE));
    CHECK(g_res.ActiveCount == 2);
    CHECK(ExIsResourceAcquiredSharedLite(&g_res) == 2);   /* the owner's count */
    ExReleaseResourceLite(&g_res);
    CHECK(g_res.ActiveCount == 1);
    ExConvertExclusiveToSharedLite(&g_res);
    CHECK(!ExIsResourceAcquiredExclusiveLite(&g_res));
    CHECK(ExIsResourceAcquiredSharedLite(&g_res) == 1);
    ExReleaseResourceLite(&g_res);
    CHECK(g_res.ActiveCount == 0);
    CHECK(ExIsResourceAcquiredSharedLite(&g_res) == 0);

    /* shared owners from several threads, then a writer waits for them */
    g_res_phase = 0;
    pthread_t r[3], w;
    for (int i = 0; i < 3; i++) pthread_create(&r[i], NULL, res_reader, NULL);
    sleep_ms(10);
    pthread_create(&w, NULL, res_writer, NULL);
    for (int i = 0; i < 3; i++) pthread_join(r[i], NULL);
    pthread_join(w, NULL);
    CHECK(g_res_phase == 100);                           /* the writer ran after the three readers */
    CHECK(g_res.ActiveCount == 0);

    /* a try-acquire fails while the resource is held shared by this thread */
    CHECK(ExAcquireResourceSharedLite(&g_res, FALSE));
    CHECK(!ExTryToAcquireResourceExclusiveLite(&g_res));
    CHECK(ExAcquireResourceSharedLite(&g_res, FALSE));    /* recursive shared */
    CHECK(ExIsResourceAcquiredSharedLite(&g_res) == 2);
    ExReleaseResourceLite(&g_res);
    ExReleaseResourceLite(&g_res);
    CHECK(ExTryToAcquireResourceExclusiveLite(&g_res));
    ExReleaseResourceLite(&g_res);

    /* more shared owners than the inline entry holds */
    CHECK(g_res.ActiveCount == 0);
    CHECK_STATUS(ExDeleteResourceLite(&g_res), STATUS_SUCCESS);
}

/* ---- SLISTs -------------------------------------------------------------- */
static void test_slist(void)
{
    SLIST_HEADER h;
    SLIST_ENTRY e[4];

    RtlInitializeSListHead(&h);
    CHECK(RtlFirstEntrySList(&h) == NULL);
    CHECK(RtlQueryDepthSList(&h) == 0);
    CHECK(ExpInterlockedPopEntrySList(&h) == NULL);

    CHECK(ExpInterlockedPushEntrySList(&h, &e[0]) == NULL);   /* returns the previous first entry */
    CHECK(ExpInterlockedPushEntrySList(&h, &e[1]) == &e[0]);
    CHECK(RtlInterlockedPushEntrySList(&h, &e[2]) == &e[1]);
    CHECK(RtlQueryDepthSList(&h) == 3);
    CHECK((USHORT)(h.Alignment & 0xffff) == 3);               /* what the WDK's inline ExQueryDepthSList reads */
    CHECK(RtlFirstEntrySList(&h) == &e[2]);
    CHECK(ExpInterlockedPopEntrySList(&h) == &e[2]);
    CHECK(RtlQueryDepthSList(&h) == 2);

    PSLIST_ENTRY all = ExpInterlockedFlushSList(&h);          /* the chain, in pop order */
    CHECK(all == &e[1] && all->Next == &e[0] && e[0].Next == NULL);
    CHECK(RtlQueryDepthSList(&h) == 0);
    CHECK(RtlFirstEntrySList(&h) == NULL);

    KSPIN_LOCK lock = 0;
    CHECK(ExInterlockedPushEntrySList(&h, &e[3], &lock) == NULL);
    CHECK(ExInterlockedPopEntrySList(&h, &lock) == &e[3]);
}

/* ---- lookaside lists ----------------------------------------------------- */
/* The WDK's inline allocate and free for a lookaside list, as the headers write them. */
static PVOID lookaside_alloc(PNPAGED_LOOKASIDE_LIST l)
{
    l->L.TotalAllocates++;
    PVOID e = ExpInterlockedPopEntrySList(&l->L.ListHead);
    if (!e) {
        l->L.AllocateMisses++;
        e = l->L.Allocate(l->L.Type, l->L.Size, l->L.Tag);
    }
    return e;
}

static void lookaside_free(PNPAGED_LOOKASIDE_LIST l, PVOID e)
{
    l->L.TotalFrees++;
    if (RtlQueryDepthSList(&l->L.ListHead) >= l->L.Depth) {
        l->L.FreeMisses++;
        l->L.Free(e);
    } else {
        ExpInterlockedPushEntrySList(&l->L.ListHead, e);
    }
}

static void test_lookaside(void)
{
    drvhost_stats_t a, b;
    NPAGED_LOOKASIDE_LIST l;

    drvhost_get_stats(&a);
    ExInitializeNPagedLookasideList(&l, NULL, NULL, 0, 64, 0x6b6c4c44u, 0);
    CHECK(l.L.Size == 64);
    CHECK(l.L.Tag == 0x6b6c4c44u);
    CHECK(l.L.Type == NonPagedPool);
    CHECK(l.L.Depth >= 1);
    CHECK(l.L.Allocate == ExAllocatePoolWithTag);

    void *p[8];
    for (int i = 0; i < 8; i++) { p[i] = lookaside_alloc(&l); CHECK(p[i] != NULL); }
    CHECK(l.L.TotalAllocates == 8 && l.L.AllocateMisses == 8);
    for (int i = 0; i < 8; i++) lookaside_free(&l, p[i]);
    CHECK(l.L.TotalFrees == 8);
    CHECK(RtlQueryDepthSList(&l.L.ListHead) == l.L.Depth);   /* the list kept up to Depth entries */
    CHECK(l.L.FreeMisses == 8 - l.L.Depth);
    void *again = lookaside_alloc(&l);                       /* served from the list */
    CHECK(again != NULL);
    CHECK(l.L.AllocateMisses == 8);
    lookaside_free(&l, again);

    ExDeleteNPagedLookasideList(&l);
    drvhost_get_stats(&b);
    CHECK(b.pool_outstanding == a.pool_outstanding);          /* every cached entry went back to the pool */

    PAGED_LOOKASIDE_LIST pl;
    ExInitializePagedLookasideList(&pl, NULL, NULL, 0, 32, 1, 0);
    CHECK(pl.L.Type == PagedPool);
    ExDeletePagedLookasideList(&pl);
    ExInitializeNPagedLookasideList(&l, NULL, NULL, 2 /* POOL_NX_ALLOCATION */, 16, 1, 0);
    CHECK(l.L.Type == NonPagedPoolNx);
    ExDeleteNPagedLookasideList(&l);
}

/* ---- interlocked lists --------------------------------------------------- */
static void test_interlocked_lists(void)
{
    LIST_ENTRY head, a, b;
    KSPIN_LOCK lock = 0;

    head.Flink = head.Blink = &head;
    CHECK(ExInterlockedInsertHeadList(&head, &a, &lock) == NULL);   /* the list was empty */
    CHECK(ExInterlockedInsertHeadList(&head, &b, &lock) == &a);     /* returns the previous first entry */
    CHECK(head.Flink == &b && b.Flink == &a && a.Flink == &head);
    CHECK(ExInterlockedRemoveHeadList(&head, &lock) == &b);
    CHECK(ExInterlockedRemoveHeadList(&head, &lock) == &a);
    CHECK(ExInterlockedRemoveHeadList(&head, &lock) == NULL);
    CHECK(ExInterlockedInsertTailList(&head, &a, &lock) == NULL);
    CHECK(ExInterlockedInsertTailList(&head, &b, &lock) == &a);
    CHECK(head.Blink == &b);

    SINGLE_LIST_ENTRY sh = { NULL }, s1, s2;
    CHECK(ExInterlockedPushEntryList(&sh, &s1, &lock) == NULL);
    CHECK(ExInterlockedPushEntryList(&sh, &s2, &lock) == &s1);
    CHECK(ExInterlockedPopEntryList(&sh, &lock) == &s2);
    CHECK(ExInterlockedPopEntryList(&sh, &lock) == &s1);
    CHECK(ExInterlockedPopEntryList(&sh, &lock) == NULL);
}

int main(void)
{
    CHECK_STATUS(drvhost_init(), 0);
    test_mutex();
    test_semaphore();
    test_wait_multiple();
    test_guarded_mutex();
    test_device_queue();
    test_resources();
    test_slist();
    test_lookaside();
    test_interlocked_lists();
    drvhost_shutdown();
    DH_TEST_SUMMARY("driver_sync_test");
}
