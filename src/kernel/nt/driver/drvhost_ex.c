/* drvhost_ex.c - executive services used by drivers: pool, fast mutexes, spin
 * locks and IRQL, events and waits, DPC queue, timers, the executive worker
 * thread, system threads, and the handle table.
 */

#include "drvhost_int.h"

/* -------------------------------------------------------------------------- */
/* Global counters and state                                                  */
/* -------------------------------------------------------------------------- */
#ifndef __KERNEL__
__thread KIRQL dh_irql = 0;      /* kernel: per-task slots in drvhost_plat.c */
__thread dh_thread_t *dh_tls_thread = NULL;
#endif

SIZE_T g_pool_outstanding = 0;
SIZE_T g_pool_bytes = 0;
ULONG  g_devices_live = 0;
ULONG  g_irps_live = 0;
ULONG  g_images_loaded = 0;
ULONG  g_dpcs_run = 0;
ULONG  g_timers_fired = 0;
ULONG  g_io_timer_ticks = 0;

#define IRQL_PASSIVE  PASSIVE_LEVEL
#define IRQL_APC      APC_LEVEL
#define IRQL_DISPATCH DISPATCH_LEVEL

/* Stand-in OBJECT_TYPE markers.  Drivers compare these by value through the
 * exported PsThreadType / IoFileObjectType variables. */
static char g_otype_thread[64];
static char g_otype_file[64];
static char g_otype_device[64];
static char g_otype_event[64];
static PVOID g_otype_thread_ptr = g_otype_thread;
static PVOID g_otype_file_ptr = g_otype_file;
static PVOID g_otype_device_ptr = g_otype_device;
static PVOID g_otype_event_ptr = g_otype_event;
PVOID *PsThreadType = &g_otype_thread_ptr;
PVOID *IoFileObjectType = &g_otype_file_ptr;
PVOID *IoDeviceObjectType = &g_otype_device_ptr;
PVOID *ExEventObjectType = &g_otype_event_ptr;
ULONG NtBuildNumber = 26200;
ULONG KeNumberProcessors = 1;

/* -------------------------------------------------------------------------- */
/* Bugcheck                                                                   */
/* -------------------------------------------------------------------------- */
void dh_bugcheck(ULONG code, ULONG_PTR p1, ULONG_PTR p2, ULONG_PTR p3, ULONG_PTR p4)
{
#ifdef __KERNEL__
    panic("drvhost: BUGCHECK 0x%08x (0x%lx 0x%lx 0x%lx 0x%lx)\n",
          (unsigned)code, (unsigned long)p1, (unsigned long)p2,
          (unsigned long)p3, (unsigned long)p4);
#else
    fprintf(stderr, "drvhost: BUGCHECK 0x%08x (0x%lx 0x%lx 0x%lx 0x%lx)\n",
            (unsigned)code, (unsigned long)p1, (unsigned long)p2,
            (unsigned long)p3, (unsigned long)p4);
    fflush(stderr);
    abort();
#endif
}

DRVCALL void KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
{
    dh_bugcheck(BugCheckCode, P1, P2, P3, P4);
}

DRVCALL void ExRaiseStatus(NTSTATUS Status)
{
    dh_bugcheck((ULONG)Status, 0, 0, 0, 0);
}

/* -------------------------------------------------------------------------- */
/* Pool                                                                       */
/* -------------------------------------------------------------------------- */
#define DH_POOL_MAGIC 0x4450504f4f4c4648ull /* "HFLOPPD" marker */

typedef struct dh_pool_hdr {
    uint64_t magic;
    SIZE_T   size;
    ULONG    tag;
    ULONG    type;
    uint64_t reserved;
} dh_pool_hdr_t;

_Static_assert(sizeof(dh_pool_hdr_t) == 32, "pool header keeps 16-byte alignment");

static PVOID pool_alloc(ULONG type, SIZE_T n, ULONG tag)
{
    if (n == 0 || n > ((SIZE_T)1 << 31)) return NULL;
    dh_pool_hdr_t *h = dh_alloc(sizeof *h + n);
    if (!h) return NULL;
    h->magic = DH_POOL_MAGIC;
    h->size = n;
    h->tag = tag;
    h->type = type;
    __atomic_add_fetch(&g_pool_outstanding, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&g_pool_bytes, n, __ATOMIC_SEQ_CST);
    return h + 1;
}

DRVCALL PVOID ExAllocatePoolWithTag(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag)
{
    return pool_alloc((ULONG)PoolType, NumberOfBytes, Tag);
}

DRVCALL PVOID ExAllocatePool2(ULONGLONG Flags, SIZE_T NumberOfBytes, ULONG Tag)
{
    ULONG type = (Flags & POOL_FLAG_PAGED) ? PagedPool : NonPagedPool;
    return pool_alloc(type, NumberOfBytes, Tag);
}

DRVCALL PVOID ExAllocatePool(POOL_TYPE PoolType, SIZE_T NumberOfBytes)
{
    return pool_alloc((ULONG)PoolType, NumberOfBytes, 0);
}

DRVCALL void ExFreePoolWithTag(PVOID P, ULONG Tag)
{
    if (!P) return;
    dh_pool_hdr_t *h = (dh_pool_hdr_t *)P - 1;
    if (h->magic != DH_POOL_MAGIC)
        dh_bugcheck(DH_BUGCHECK_BAD_POOL_CALLER, 0x1, (ULONG_PTR)P, 0, 0);
    if (Tag != 0 && h->tag != Tag)
        dh_bugcheck(DH_BUGCHECK_BAD_POOL_CALLER, 0x2, (ULONG_PTR)h->tag, (ULONG_PTR)Tag, 0);
    h->magic = 0;   /* a second free now fails the magic check */
    __atomic_sub_fetch(&g_pool_outstanding, 1, __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&g_pool_bytes, h->size, __ATOMIC_SEQ_CST);
    dh_free(h);
}

DRVCALL void ExFreePool(PVOID P)
{
    if (!P) return;
    dh_pool_hdr_t *h = (dh_pool_hdr_t *)P - 1;
    ExFreePoolWithTag(P, h->tag);
}

/* -------------------------------------------------------------------------- */
/* Fast mutex                                                                 */
/* -------------------------------------------------------------------------- */
DRVCALL void ExInitializeFastMutex(PFAST_MUTEX FastMutex)
{
    memset(FastMutex, 0, sizeof *FastMutex);
    FastMutex->Count = 1;
    FastMutex->Event.Header.Type = DISPATCHER_EVENT_SYNCHRONIZATION;
}

DRVCALL void ExAcquireFastMutex(PFAST_MUTEX FastMutex)
{
    KIRQL old = dh_irql;
    if (old > IRQL_APC) dh_bugcheck(DH_BUGCHECK_IRQL, 1, old, 0, 0);
    dh_irql = IRQL_APC;
    dh_lock();
    if (FastMutex->Owner == dh_current_thread())
        dh_bugcheck(DH_BUGCHECK_LOCK_RECURSION, (ULONG_PTR)FastMutex, 0, 0, 0);
    while (FastMutex->Count != 1) {
        FastMutex->Contention++;
        dh_wait();
    }
    FastMutex->Count = 0;
    FastMutex->Owner = dh_current_thread();
    FastMutex->OldIrql = old;
    dh_unlock();
}

DRVCALL BOOLEAN ExTryToAcquireFastMutex(PFAST_MUTEX FastMutex)
{
    KIRQL old = dh_irql;
    if (old > IRQL_APC) dh_bugcheck(DH_BUGCHECK_IRQL, 2, old, 0, 0);
    dh_lock();
    if (FastMutex->Count != 1) { dh_unlock(); return FALSE; }
    FastMutex->Count = 0;
    FastMutex->Owner = dh_current_thread();
    FastMutex->OldIrql = old;
    dh_unlock();
    dh_irql = IRQL_APC;
    return TRUE;
}

DRVCALL void ExReleaseFastMutex(PFAST_MUTEX FastMutex)
{
    if (FastMutex->Owner != dh_current_thread())
        dh_bugcheck(DH_BUGCHECK_LOCK_RECURSION, (ULONG_PTR)FastMutex, 1, 0, 0);
    KIRQL restore = (KIRQL)FastMutex->OldIrql;
    dh_lock();
    FastMutex->Owner = NULL;
    FastMutex->Count = 1;
    dh_notify_all();
    dh_unlock();
    dh_irql = restore;
}

/* -------------------------------------------------------------------------- */
/* IRQL and spin locks                                                        */
/* -------------------------------------------------------------------------- */
DRVCALL KIRQL KeGetCurrentIrql(void) { return dh_irql; }

DRVCALL KIRQL KeRaiseIrqlToDpcLevel(void)
{
    KIRQL old = dh_irql;
    if (old > IRQL_DISPATCH) dh_bugcheck(DH_BUGCHECK_IRQL, 3, old, 0, 0);
    dh_irql = IRQL_DISPATCH;
    return old;
}

DRVCALL void KeRaiseIrql(KIRQL NewIrql, PKIRQL OldIrql)
{
    if (NewIrql < dh_irql || NewIrql > IRQL_DISPATCH)
        dh_bugcheck(DH_BUGCHECK_IRQL, 4, NewIrql, dh_irql, 0);
    if (OldIrql) *OldIrql = dh_irql;
    dh_irql = NewIrql;
}

DRVCALL void KeLowerIrql(KIRQL NewIrql)
{
    if (NewIrql > dh_irql) dh_bugcheck(DH_BUGCHECK_IRQL, 5, NewIrql, dh_irql, 0);
    dh_irql = NewIrql;
}

static void spin_acquire_raw(PKSPIN_LOCK SpinLock)
{
    uintptr_t self = (uintptr_t)dh_current_thread();
    for (;;) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n((uintptr_t *)SpinLock, &expected, self, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        if (expected == self)
            dh_bugcheck(DH_BUGCHECK_LOCK_RECURSION, (ULONG_PTR)SpinLock, 2, 0, 0);
        sched_yield();
    }
}

static void spin_release_raw(PKSPIN_LOCK SpinLock)
{
    uintptr_t self = (uintptr_t)dh_current_thread();
    if (__atomic_load_n((uintptr_t *)SpinLock, __ATOMIC_RELAXED) != self)
        dh_bugcheck(DH_BUGCHECK_LOCK_RECURSION, (ULONG_PTR)SpinLock, 3, 0, 0);
    __atomic_store_n((uintptr_t *)SpinLock, 0, __ATOMIC_RELEASE);
}

DRVCALL void KeInitializeSpinLock(PKSPIN_LOCK SpinLock)
{
    *SpinLock = 0;
}

DRVCALL void KeAcquireSpinLock(PKSPIN_LOCK SpinLock, PKIRQL OldIrql)
{
    KIRQL old = KeRaiseIrqlToDpcLevel();
    if (OldIrql) *OldIrql = old;
    spin_acquire_raw(SpinLock);
}

DRVCALL void KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql)
{
    spin_release_raw(SpinLock);
    if (NewIrql > dh_irql) dh_bugcheck(DH_BUGCHECK_IRQL, 6, NewIrql, dh_irql, 0);
    dh_irql = NewIrql;
}

DRVCALL void KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock)
{
    if (dh_irql < IRQL_DISPATCH) dh_bugcheck(DH_BUGCHECK_IRQL, 7, dh_irql, 0, 0);
    spin_acquire_raw(SpinLock);
}

DRVCALL void KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK SpinLock)
{
    spin_release_raw(SpinLock);
}

DRVCALL void KeStallExecutionProcessor(ULONG MicroSeconds)
{
    if (dh_irql > IRQL_DISPATCH) dh_bugcheck(DH_BUGCHECK_IRQL, 8, dh_irql, 0, 0);
    dh_sleep_ns((uint64_t)MicroSeconds * 1000ull);
}

/* -------------------------------------------------------------------------- */
/* Events and waits                                                           */
/* -------------------------------------------------------------------------- */
static void init_header(DISPATCHER_HEADER *h, UCHAR type, LONG state)
{
    memset(h, 0, sizeof *h);
    h->Type = type;
    h->Size = (UCHAR)(sizeof *h / sizeof(LONG));
    h->SignalState = state;
    h->WaitListHead.Flink = &h->WaitListHead;
    h->WaitListHead.Blink = &h->WaitListHead;
}

DRVCALL void KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State)
{
    init_header(&Event->Header, (UCHAR)Type, State ? 1 : 0);
}

DRVCALL LONG KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait)
{
    (void)Increment; (void)Wait;
    dh_lock();
    LONG prev = Event->Header.SignalState;
    Event->Header.SignalState = 1;
    dh_notify_all();
    dh_unlock();
    return prev;
}

DRVCALL LONG KeResetEvent(PKEVENT Event)
{
    dh_lock();
    LONG prev = Event->Header.SignalState;
    Event->Header.SignalState = 0;
    dh_unlock();
    return prev;
}

DRVCALL void KeClearEvent(PKEVENT Event)
{
    dh_lock();
    Event->Header.SignalState = 0;
    dh_unlock();
}

DRVCALL LONG KeReadStateEvent(PKEVENT Event)
{
    dh_lock();
    LONG s = Event->Header.SignalState;
    dh_unlock();
    return s;
}

/* Converts a KeWait/KeDelay timeout into a monotonic deadline.  Returns 0 for
 * "no timeout" (infinite), 1 with *deadline set for a deadline, and 2 for an
 * instant poll (timeout already expired or zero). */
int dh_timeout_to_deadline(const LARGE_INTEGER *t, uint64_t *deadline)
{
    if (!t) return 0;
    uint64_t now = dh_now_ns();
    if (t->QuadPart < 0) {
        uint64_t rel100 = (uint64_t)(-t->QuadPart);
        if (rel100 == 0) return 2;
        *deadline = now + rel100 * 100ull;
        return 1;
    }
    /* Absolute, in 100ns units since 1601. */
    uint64_t sys_now = dh_system_time_100ns();
    uint64_t abs100 = (uint64_t)t->QuadPart;
    if (abs100 <= sys_now) return 2;
    *deadline = now + (abs100 - sys_now) * 100ull;
    return 1;
}
#define timeout_to_deadline dh_timeout_to_deadline

int dh_dispatcher_acquire_locked(DISPATCHER_HEADER *h, dh_thread_t *self)
{
    switch (h->Type) {
    case DISPATCHER_MUTANT_OBJECT: {
        PKMUTANT m = (PKMUTANT)h;
        if (m->OwnerThread == (PVOID)self) {
            if (h->SignalState == -2147483647 - 1) dh_bugcheck(STATUS_MUTANT_LIMIT_EXCEEDED, (ULONG_PTR)m, 0, 0, 0);
            h->SignalState--;                 /* recursive acquisition */
            return 1;
        }
        if (h->SignalState != 1) return 0;
        h->SignalState = 0;
        m->OwnerThread = self;
        return 1;
    }
    case DISPATCHER_SEMAPHORE_OBJECT:
        if (h->SignalState <= 0) return 0;
        h->SignalState--;
        return 1;
    case DISPATCHER_EVENT_SYNCHRONIZATION:
    case DISPATCHER_TIMER_SYNCHRONIZATION:
        if (h->SignalState <= 0) return 0;
        h->SignalState = 0;
        return 1;
    default:                                 /* notification objects, threads, gates */
        return h->SignalState > 0;
    }
}

DRVCALL NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason, KPROCESSOR_MODE WaitMode,
                                       BOOLEAN Alertable, PLARGE_INTEGER Timeout)
{
    (void)WaitReason; (void)WaitMode; (void)Alertable;
    if (!Object) return STATUS_INVALID_PARAMETER;
    DISPATCHER_HEADER *h = Object;
    uint64_t deadline = 0;
    int mode = timeout_to_deadline(Timeout, &deadline);
    if (mode != 2 && dh_irql >= IRQL_DISPATCH)
        dh_bugcheck(DH_BUGCHECK_IRQL, 9, dh_irql, 0, 0);

    dh_thread_t *self = dh_current_thread();
    dh_lock();
    for (;;) {
        if (dh_dispatcher_acquire_locked(h, self)) {
            dh_unlock();
            return STATUS_SUCCESS;
        }
        if (mode == 2) { dh_unlock(); return STATUS_TIMEOUT; }
        if (mode == 0) { dh_wait(); continue; }
        if (dh_timedwait_until(deadline)) {
            if (dh_dispatcher_acquire_locked(h, self)) { dh_unlock(); return STATUS_SUCCESS; }
            dh_unlock();
            return STATUS_TIMEOUT;
        }
    }
}

DRVCALL NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval)
{
    (void)WaitMode; (void)Alertable;
    if (!Interval) return STATUS_INVALID_PARAMETER;
    uint64_t deadline = 0;
    int mode = timeout_to_deadline(Interval, &deadline);
    if (mode == 1) dh_sleep_ns(deadline - dh_now_ns());
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* DPC queue (FIFO, intrusive through DpcListEntry; NULL Next means not queued)*/
/* -------------------------------------------------------------------------- */
static SINGLE_LIST_ENTRY g_dpc_end;          /* queue terminator marker */
static SINGLE_LIST_ENTRY *g_dpc_head = NULL;
static SINGLE_LIST_ENTRY *g_dpc_tail = NULL;
static int g_dpc_running = 0;                /* a DPC routine is executing on the worker */

/* Waits until the DPC queue is empty and no DPC routine is running.  Call it
 * after KeRemoveQueueDpc, before freeing the memory a DPC routine uses.  Do not
 * call it from inside a DPC routine: that would wait on itself. */
DRVCALL void KeFlushQueuedDpcs(void)
{
    dh_lock();
    while (g_dpc_head || g_dpc_running) dh_wait();
    dh_unlock();
}

DRVCALL void KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext)
{
    memset(Dpc, 0, sizeof *Dpc);
    Dpc->Type = 19;          /* DpcObject */
    Dpc->Importance = 1;     /* MediumImportance */
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
}

void dh_dpc_enqueue_locked(PKDPC dpc, PVOID a1, PVOID a2)
{
    if (dpc->DpcListEntry.Next) return;       /* already queued */
    dpc->SystemArgument1 = a1;
    dpc->SystemArgument2 = a2;
    SINGLE_LIST_ENTRY *e = &dpc->DpcListEntry;
    e->Next = &g_dpc_end;
    if (g_dpc_tail) g_dpc_tail->Next = e;
    else g_dpc_head = e;
    g_dpc_tail = e;
}

static PKDPC dpc_dequeue_locked(void)
{
    SINGLE_LIST_ENTRY *e = g_dpc_head;
    if (!e) return NULL;
    if (e == g_dpc_tail) { g_dpc_head = NULL; g_dpc_tail = NULL; }
    else g_dpc_head = e->Next;
    e->Next = NULL;
    return (PKDPC)((char *)e - offsetof(KDPC, DpcListEntry));
}

DRVCALL BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2)
{
    dh_lock();
    BOOLEAN queued = Dpc->DpcListEntry.Next == NULL;
    dh_dpc_enqueue_locked(Dpc, SystemArgument1, SystemArgument2);
    dh_notify_all();
    dh_unlock();
    return queued;
}

DRVCALL BOOLEAN KeRemoveQueueDpc(PKDPC Dpc)
{
    BOOLEAN removed = FALSE;
    dh_lock();
    SINGLE_LIST_ENTRY *prev = NULL;
    SINGLE_LIST_ENTRY *e = g_dpc_head;
    while (e) {
        SINGLE_LIST_ENTRY *nxt = (e == g_dpc_tail) ? NULL : e->Next;
        if (e == &Dpc->DpcListEntry) {
            if (prev) prev->Next = (e == g_dpc_tail) ? &g_dpc_end : nxt;
            else g_dpc_head = nxt;
            if (e == g_dpc_tail) g_dpc_tail = prev;
            e->Next = NULL;
            removed = TRUE;
            break;
        }
        prev = e;
        e = nxt;
    }
    dh_unlock();
    return removed;
}

/* -------------------------------------------------------------------------- */
/* Timers (table of armed KTIMERs; fired by the worker)                       */
/* -------------------------------------------------------------------------- */
#define DH_MAX_TIMERS 256
typedef struct dh_timer_slot {
    PKTIMER timer;
    int     active;
    uint64_t due_ns;
    ULONG   period_ms;
    PKDPC   dpc;
} dh_timer_slot_t;
static dh_timer_slot_t g_timers[DH_MAX_TIMERS];

static dh_timer_slot_t *timer_find_locked(PKTIMER t, int create)
{
    dh_timer_slot_t *free_slot = NULL;
    for (int i = 0; i < DH_MAX_TIMERS; i++) {
        if (g_timers[i].timer == t) return &g_timers[i];
        if (!g_timers[i].timer && !free_slot) free_slot = &g_timers[i];
    }
    if (!create || !free_slot) return NULL;
    free_slot->timer = t;
    free_slot->active = 0;
    return free_slot;
}

DRVCALL void KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
{
    memset(Timer, 0, sizeof *Timer);
    Timer->Header.Type = (UCHAR)(Type == NotificationTimer ? DISPATCHER_TIMER_NOTIFICATION
                                                          : DISPATCHER_TIMER_SYNCHRONIZATION);
    Timer->Header.Size = (UCHAR)(sizeof Timer->Header / sizeof(LONG));
    Timer->Header.WaitListHead.Flink = &Timer->Header.WaitListHead;
    Timer->Header.WaitListHead.Blink = &Timer->Header.WaitListHead;
}

DRVCALL void KeInitializeTimer(PKTIMER Timer)
{
    KeInitializeTimerEx(Timer, NotificationTimer);
}

DRVCALL BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc)
{
    uint64_t deadline = 0;
    int mode = timeout_to_deadline(&DueTime, &deadline);
    if (mode == 2) deadline = dh_now_ns();
    else if (mode == 0) deadline = dh_now_ns();
    dh_lock();
    dh_timer_slot_t *s = timer_find_locked(Timer, 1);
    if (!s) { dh_unlock(); return FALSE; }
    BOOLEAN was = s->active != 0;
    s->active = 1;
    s->due_ns = deadline;
    s->period_ms = Period > 0 ? (ULONG)Period : 0;
    s->dpc = Dpc;
    Timer->DueTime.QuadPart = (ULONGLONG)deadline;
    Timer->Period = s->period_ms;
    Timer->Dpc = Dpc;
    Timer->Header.SignalState = 0;
    dh_notify_all();
    dh_unlock();
    return was;
}

DRVCALL BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
{
    return KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

DRVCALL BOOLEAN KeCancelTimer(PKTIMER Timer)
{
    dh_lock();
    dh_timer_slot_t *s = timer_find_locked(Timer, 0);
    BOOLEAN was = s && s->active;
    if (s) s->active = 0;
    dh_unlock();
    return was;
}

/* -------------------------------------------------------------------------- */
/* Executive worker: runs DPCs, fires timers, drives IoTimers                  */
/* -------------------------------------------------------------------------- */
static int          g_worker_stop = 0;
static void        *g_worker_native = NULL;
static int          g_ex_started = 0;

/* Fires every due timer.  Caller holds the monitor.  Queues each timer's DPC. */
static void fire_due_timers_locked(uint64_t now)
{
    for (int i = 0; i < DH_MAX_TIMERS; i++) {
        dh_timer_slot_t *s = &g_timers[i];
        if (!s->timer || !s->active || s->due_ns > now) continue;
        s->timer->Header.SignalState = 1;
        s->timer->Header.Inserted = 0;
        g_timers_fired++;
        if (s->period_ms) {
            s->due_ns = now + (uint64_t)s->period_ms * 1000000ull;
            s->timer->DueTime.QuadPart = (ULONGLONG)s->due_ns;
        } else {
            s->active = 0;
        }
        if (s->dpc) dh_dpc_enqueue_locked(s->dpc, NULL, NULL);
        dh_notify_all();
    }
}

/* Earliest pending deadline across armed timers and IoTimers, or 0 for none. */
static uint64_t next_deadline_locked(void)
{
    uint64_t best = 0;
    for (int i = 0; i < DH_MAX_TIMERS; i++) {
        dh_timer_slot_t *s = &g_timers[i];
        if (s->timer && s->active && (best == 0 || s->due_ns < best)) best = s->due_ns;
    }
    return best;
}

void dh_worker_run(void)
{
    dh_thread_t *self = dh_current_thread();
    (void)self;
    dh_lock();
    while (!g_worker_stop) {
        if (g_dpc_head) {
            PKDPC dpc = dpc_dequeue_locked();
            PVOID a1 = dpc->SystemArgument1, a2 = dpc->SystemArgument2;
            PKDEFERRED_ROUTINE fn = dpc->DeferredRoutine;
            PVOID ctx = dpc->DeferredContext;
            g_dpc_running = 1;
            dh_unlock();
            KIRQL saved = dh_irql;
            dh_irql = IRQL_DISPATCH;
            if (fn) fn(dpc, ctx, a1, a2);
            dh_irql = saved;
            __atomic_add_fetch(&g_dpcs_run, 1, __ATOMIC_SEQ_CST);
            dh_lock();
            g_dpc_running = 0;
            dh_notify_all();
            continue;
        }
        uint64_t now = dh_now_ns();
        fire_due_timers_locked(now);
        if (g_dpc_head) continue;

        /* IoTimers: collect due ones, call them without the monitor held. */
        dh_device_t *due[32];
        int nd = dh_io_timer_collect(now, due, 32);
        if (nd > 0) {
            dh_unlock();
            for (int i = 0; i < nd; i++) {
                PIO_TIMER_ROUTINE r = due[i]->io_timer_routine;
                if (r) r(&due[i]->dev, due[i]->io_timer_ctx);
                __atomic_add_fetch(&g_io_timer_ticks, 1, __ATOMIC_SEQ_CST);
            }
            dh_lock();
            for (int i = 0; i < nd; i++) due[i]->io_timer_inflight--;
            dh_notify_all();
            continue;
        }

        uint64_t next = next_deadline_locked();
        uint64_t io_next = dh_io_timer_next_deadline_locked();
        if (io_next && (next == 0 || io_next < next)) next = io_next;
        uint64_t cap = dh_now_ns() + 50ull * 1000000ull; /* re-check at least every 50ms */
        if (next == 0 || next > cap) next = cap;
        dh_timedwait_until(next);
    }
    dh_unlock();
}

static void *worker_main(void *arg)
{
    (void)arg;
    dh_thread_t *th = dh_current_thread();
    dh_worker_run();
    dh_lock();
    th->hdr.SignalState = 1;
    dh_notify_all();
    dh_unlock();
    /* The worker's adopted thread object dies with the thread; nothing else
     * references it once the worker has stopped. */
    dh_tls_thread = NULL;
    dh_free(th);
    return NULL;
}

int dh_ex_init(void)
{
    if (g_ex_started) return 0;
    g_worker_stop = 0;
    int err = 0;
    g_worker_native = dh_thread_start(worker_main, NULL, &err);
    if (!g_worker_native) return -1;
    g_ex_started = 1;
    return 0;
}

void dh_ex_shutdown(void)
{
    if (!g_ex_started) return;
    dh_lock();
    g_worker_stop = 1;
    dh_notify_all();
    dh_unlock();
    dh_thread_join(g_worker_native);
    g_worker_native = NULL;
    g_ex_started = 0;
}

/* -------------------------------------------------------------------------- */
/* Threads, the current-thread object, and the handle table                   */
/* -------------------------------------------------------------------------- */
static dh_thread_t *g_threads = NULL;

dh_thread_t *dh_current_thread(void)
{
    if (!dh_tls_thread) {
        dh_thread_t *t = dh_alloc(sizeof *t);
        if (!t) dh_bugcheck(DH_BUGCHECK_BAD_HOST_STATE, 1, 0, 0, 0);
        t->hdr.Type = DISPATCHER_THREAD_OBJECT;
        t->hdr.Size = (UCHAR)(sizeof t->hdr / sizeof(LONG));
        dh_tls_thread = t;
    }
    return dh_tls_thread;
}

/* Signals the thread object and wakes waiters. Runs once per thread, on both
 * the normal return path and PsTerminateSystemThread. */
static void thread_finish(dh_thread_t *th)
{
    th->exit_armed = 0;
    dh_lock();
    th->hdr.SignalState = 1;
    dh_notify_all();
    dh_unlock();
}

static void thread_main_common(dh_thread_t *th)
{
    dh_tls_thread = th;
    dh_irql = IRQL_PASSIVE;
#ifdef __KERNEL__
    /* No setjmp in the kernel: PsTerminateSystemThread finishes the thread
     * and exits the kthread itself, so the normal path just returns here. */
    th->exit_armed = 1;
    th->start(th->start_ctx);
    th->exit_status = STATUS_SUCCESS;
    thread_finish(th);
#else
    if (setjmp(th->exit_jb) == 0) {
        th->exit_armed = 1;
        th->start(th->start_ctx);
        th->exit_status = STATUS_SUCCESS;
    }
    thread_finish(th);
#endif
}

static void *system_thread_entry(void *arg)
{
    dh_thread_t *th = arg;
    thread_main_common(th);
    return NULL;
}

DRVCALL NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                                      POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                                      PCLIENT_ID ClientId, PKSTART_ROUTINE StartRoutine,
                                      PVOID StartContext)
{
    (void)DesiredAccess; (void)ObjectAttributes; (void)ProcessHandle;
    if (!ThreadHandle || !StartRoutine) return STATUS_INVALID_PARAMETER;
    dh_thread_t *th = dh_alloc(sizeof *th);
    if (!th) return STATUS_INSUFFICIENT_RESOURCES;
    th->hdr.Type = DISPATCHER_THREAD_OBJECT;
    th->hdr.Size = (UCHAR)(sizeof th->hdr / sizeof(LONG));
    th->start = StartRoutine;
    th->start_ctx = StartContext;
    th->owned_by_pool = 1;
    int err = 0;
    th->native = dh_thread_start(system_thread_entry, th, &err);
    if (!th->native) { dh_free(th); return STATUS_INSUFFICIENT_RESOURCES; }
    dh_lock();
    th->next = g_threads;
    g_threads = th;
    dh_unlock();
    HANDLE h = dh_handle_insert(DH_KIND_THREAD, th);
    if (!h) return STATUS_INSUFFICIENT_RESOURCES;
    *ThreadHandle = h;
    if (ClientId) {
        ClientId->UniqueProcess = (HANDLE)(uintptr_t)getpid();
        ClientId->UniqueThread = (HANDLE)th;
    }
    return STATUS_SUCCESS;
}

DRVCALL void PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    dh_thread_t *th = dh_current_thread();
    th->exit_status = ExitStatus;
    if (!th->exit_armed)
        dh_bugcheck(DH_BUGCHECK_TERMINATE_NONPOOL, (ULONG_PTR)th, 0, 0, 0);
#ifdef __KERNEL__
    /* kthread_exit does not unwind, so the PE frames are abandoned the way
     * longjmp abandons them on the host. Nothing below this point runs. */
    thread_finish(th);
    dh_thread_exit_current();
#else
    /* longjmp, not pthread_exit: unwinding through PE frames has no DWARF. */
    longjmp(th->exit_jb, 1);
#endif
}

static char g_fake_eprocess[256];

DRVCALL HANDLE PsGetCurrentProcessId(void) { return (HANDLE)(uintptr_t)getpid(); }
DRVCALL HANDLE PsGetCurrentThreadId(void) { return (HANDLE)dh_current_thread(); }
DRVCALL PVOID  PsGetCurrentProcess(void) { return g_fake_eprocess; }
DRVCALL PVOID  KeGetCurrentThread(void) { return dh_current_thread(); }

DRVCALL void KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
{
    if (CurrentTime) CurrentTime->QuadPart = (LONGLONG)dh_system_time_100ns();
}

/* The counter is CLOCK_MONOTONIC in nanoseconds, so the frequency is 1e9. */
DRVCALL LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER PerformanceFrequency)
{
    if (PerformanceFrequency) PerformanceFrequency->QuadPart = 1000000000LL;
    LARGE_INTEGER now;
    now.QuadPart = (LONGLONG)dh_now_ns();
    return now;
}

enum { DH_HANDLE_SLOTS = 4096 };
static dh_handle_t g_handles[DH_HANDLE_SLOTS];

static HANDLE handle_value(int index) { return (HANDLE)(uintptr_t)((index + 1) * 4); }
static int handle_index(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    if (v == 0 || (v & 3) != 0) return -1;
    uintptr_t i = v / 4 - 1;
    return i < DH_HANDLE_SLOTS ? (int)i : -1;
}

HANDLE dh_handle_insert(int kind, void *object)
{
    HANDLE out = NULL;
    dh_lock();
    for (int i = 0; i < DH_HANDLE_SLOTS; i++) {
        if (g_handles[i].kind == DH_KIND_NONE) {
            g_handles[i].kind = kind;
            g_handles[i].object = object;
            out = handle_value(i);
            break;
        }
    }
    dh_unlock();
    return out;
}

void *dh_handle_lookup(HANDLE h, int kind)
{
    int i = handle_index(h);
    if (i < 0) return NULL;
    dh_lock();
    void *obj = (g_handles[i].kind == kind) ? g_handles[i].object : NULL;
    dh_unlock();
    return obj;
}

int dh_handle_remove(HANDLE h, void **object_out)
{
    int i = handle_index(h);
    if (i < 0) return 0;
    dh_lock();
    int ok = g_handles[i].kind != DH_KIND_NONE;
    if (ok) {
        if (object_out) *object_out = g_handles[i].object;
        g_handles[i].kind = DH_KIND_NONE;
        g_handles[i].object = NULL;
    }
    dh_unlock();
    return ok;
}

DRVCALL NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, ACCESS_MASK DesiredAccess,
                                           PVOID ObjectType, KPROCESSOR_MODE AccessMode,
                                           PVOID *Object, PVOID HandleInformation)
{
    (void)DesiredAccess; (void)AccessMode; (void)HandleInformation;
    if (!Object) return STATUS_INVALID_PARAMETER;
    int kind = DH_KIND_NONE;
    PVOID thread_t = *PsThreadType, file_t = *IoFileObjectType;
    if (!ObjectType || ObjectType == thread_t) kind = DH_KIND_THREAD;
    if (ObjectType == file_t) kind = DH_KIND_FILE;
    if (ObjectType && ObjectType != thread_t && ObjectType != file_t)
        return STATUS_OBJECT_TYPE_MISMATCH;
    /* With no type given, accept either kind. */
    void *obj = dh_handle_lookup(Handle, kind);
    if (!obj && !ObjectType) obj = dh_handle_lookup(Handle, DH_KIND_FILE);
    if (!obj) return STATUS_INVALID_HANDLE;
    *Object = obj;
    return STATUS_SUCCESS;
}

DRVCALL void ObfReferenceObject(PVOID Object) { (void)Object; }
DRVCALL void ObfDereferenceObject(PVOID Object) { (void)Object; }

DRVCALL NTSTATUS ZwClose(HANDLE Handle)
{
    void *obj = NULL;
    int i = handle_index(Handle);
    if (i < 0) return STATUS_INVALID_HANDLE;
    int kind = 0;
    dh_lock();
    kind = g_handles[i].kind;
    dh_unlock();
    if (!dh_handle_remove(Handle, &obj)) return STATUS_INVALID_HANDLE;
    if (kind == DH_KIND_FILE) dh_nt_file_closed(obj);
    else if (kind == DH_KIND_KEY) dh_reg_key_closed(obj);
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS NtClose(HANDLE Handle)
{
    return ZwClose(Handle);
}

/* Teardown used by drvhost_shutdown: joins every system thread it created. */
void dh_ex_join_threads(void)
{
    dh_lock();
    dh_thread_t *list = g_threads;
    g_threads = NULL;
    dh_unlock();
    for (dh_thread_t *t = list; t; ) {
        dh_thread_t *next = t->next;
        if (t->native) dh_thread_join(t->native);
        dh_free(t);
        t = next;
    }
}

/* WDK name for KeAcquireSpinLock on x64: returns the previous IRQL. */
DRVCALL KIRQL KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK SpinLock)
{
    KIRQL old = 0;
    KeAcquireSpinLock(SpinLock, &old);
    return old;
}
