/* drvhost_sync.c - executive synchronization beyond events and fast mutexes:
 * kernel mutexes and semaphores, multi-object waits, guarded mutexes, critical
 * regions, device queues, executive resources (ERESOURCE), sequenced lists
 * (SLIST), lookaside lists and the interlocked list helpers.
 *
 * Everything runs under the executive monitor (dh_lock), the same model as the
 * rest of the host. Structure layouts follow the x64 WDK; tests/kernel-drivers/
 * layout checks them against Wine's WDK headers. The semantics follow the
 * documented behaviour of the NT executive (ReactOS ntoskrnl/ke and ex were the
 * cross-check for the corner cases).
 */

#include "drvhost_int.h"

#define IRQL_APC      1
#define IRQL_DISPATCH 2

static void init_header(DISPATCHER_HEADER *h, UCHAR type, LONG state, size_t object_size)
{
    memset(h, 0, sizeof *h);
    h->Type = type;
    h->Size = (UCHAR)(object_size / sizeof(LONG));
    h->SignalState = state;
    h->WaitListHead.Flink = &h->WaitListHead;
    h->WaitListHead.Blink = &h->WaitListHead;
}

static void list_init(LIST_ENTRY *l) { l->Flink = l->Blink = l; }
static int  list_empty(const LIST_ENTRY *l) { return l->Flink == l; }

static void list_insert_tail(LIST_ENTRY *head, LIST_ENTRY *e)
{
    LIST_ENTRY *b = head->Blink;
    e->Flink = head;
    e->Blink = b;
    b->Flink = e;
    head->Blink = e;
}

static void list_insert_head(LIST_ENTRY *head, LIST_ENTRY *e)
{
    LIST_ENTRY *f = head->Flink;
    e->Flink = f;
    e->Blink = head;
    f->Blink = e;
    head->Flink = e;
}

static void list_remove(LIST_ENTRY *e)
{
    e->Blink->Flink = e->Flink;
    e->Flink->Blink = e->Blink;
}

/* -------------------------------------------------------------------------- */
/* Mutexes (KMUTEX)                                                           */
/* -------------------------------------------------------------------------- */
DRVCALL void KeInitializeMutex(PKMUTEX Mutex, ULONG Level)
{
    (void)Level;
    init_header(&Mutex->Header, DISPATCHER_MUTANT_OBJECT, 1, sizeof *Mutex);
    list_init(&Mutex->MutantListEntry);
    Mutex->OwnerThread = NULL;
    Mutex->Abandoned = FALSE;
    Mutex->ApcDisable = 1;
}

DRVCALL LONG KeReadStateMutex(PKMUTEX Mutex)
{
    dh_lock();
    LONG s = Mutex->Header.SignalState;
    dh_unlock();
    return s;
}

/* Returns the previous signal state: 0 for a mutex owned once, negative while the owner holds it recursively. */
DRVCALL LONG KeReleaseMutex(PKMUTEX Mutex, BOOLEAN Wait)
{
    (void)Wait;
    dh_lock();
    if (Mutex->OwnerThread != (PVOID)dh_current_thread()) {
        dh_unlock();
        dh_bugcheck(STATUS_MUTANT_NOT_OWNED, (ULONG_PTR)Mutex, 0, 0, 0);
    }
    LONG prev = Mutex->Header.SignalState;
    Mutex->Header.SignalState++;
    if (Mutex->Header.SignalState == 1) {
        Mutex->OwnerThread = NULL;
        dh_notify_all();
    }
    dh_unlock();
    return prev;
}

/* -------------------------------------------------------------------------- */
/* Semaphores                                                                 */
/* -------------------------------------------------------------------------- */
DRVCALL void KeInitializeSemaphore(PKSEMAPHORE Semaphore, LONG Count, LONG Limit)
{
    init_header(&Semaphore->Header, DISPATCHER_SEMAPHORE_OBJECT, Count, sizeof *Semaphore);
    Semaphore->Limit = Limit;
}

DRVCALL LONG KeReadStateSemaphore(PKSEMAPHORE Semaphore)
{
    dh_lock();
    LONG s = Semaphore->Header.SignalState;
    dh_unlock();
    return s;
}

DRVCALL LONG KeReleaseSemaphore(PKSEMAPHORE Semaphore, LONG Increment, LONG Adjustment, BOOLEAN Wait)
{
    (void)Increment; (void)Wait;
    dh_lock();
    LONG prev = Semaphore->Header.SignalState;
    LONG next = prev + Adjustment;
    if (Adjustment <= 0 || next > Semaphore->Limit || next < prev) {
        dh_unlock();
        dh_bugcheck(STATUS_SEMAPHORE_LIMIT_EXCEEDED, (ULONG_PTR)Semaphore, (ULONG_PTR)Adjustment, 0, 0);
    }
    Semaphore->Header.SignalState = next;
    dh_notify_all();
    dh_unlock();
    return prev;
}

/* -------------------------------------------------------------------------- */
/* Waiting on several objects                                                 */
/* -------------------------------------------------------------------------- */
/* Takes the signal of object i for self without consuming it twice: all-or-nothing for WaitAll. The monitor is held. */
static int all_signalled_locked(DISPATCHER_HEADER **objs, ULONG count, dh_thread_t *self)
{
    for (ULONG i = 0; i < count; i++) {
        DISPATCHER_HEADER *h = objs[i];
        int ok;
        switch (h->Type) {
        case DISPATCHER_MUTANT_OBJECT:
            ok = ((PKMUTANT)h)->OwnerThread == (PVOID)self || h->SignalState == 1;
            break;
        case DISPATCHER_SEMAPHORE_OBJECT:
        case DISPATCHER_EVENT_SYNCHRONIZATION:
        case DISPATCHER_TIMER_SYNCHRONIZATION:
        default:
            ok = h->SignalState > 0;
            break;
        }
        if (!ok) return 0;
    }
    return 1;
}

DRVCALL NTSTATUS KeWaitForMultipleObjects(ULONG Count, PVOID Object[], WAIT_TYPE WaitType, ULONG WaitReason,
                                          KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout,
                                          PKWAIT_BLOCK WaitBlockArray)
{
    (void)WaitReason; (void)WaitMode; (void)Alertable;
    if (Count == 0 || Count > MAXIMUM_WAIT_OBJECTS || !Object) return STATUS_INVALID_PARAMETER;
    if (Count > THREAD_WAIT_OBJECTS && !WaitBlockArray) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < Count; i++)
        if (!Object[i]) return STATUS_INVALID_PARAMETER;

    DISPATCHER_HEADER *objs[MAXIMUM_WAIT_OBJECTS];
    for (ULONG i = 0; i < Count; i++) objs[i] = Object[i];

    uint64_t deadline = 0;
    int mode = dh_timeout_to_deadline(Timeout, &deadline);
    if (mode != 2 && dh_irql >= IRQL_DISPATCH)
        dh_bugcheck(DH_BUGCHECK_IRQL, 10, dh_irql, 0, 0);

    dh_thread_t *self = dh_current_thread();
    dh_lock();
    for (;;) {
        if (WaitType == WaitAny) {
            for (ULONG i = 0; i < Count; i++) {
                if (dh_dispatcher_acquire_locked(objs[i], self)) {
                    dh_unlock();
                    return STATUS_WAIT_0 + (NTSTATUS)i;
                }
            }
        } else if (all_signalled_locked(objs, Count, self)) {
            for (ULONG i = 0; i < Count; i++) dh_dispatcher_acquire_locked(objs[i], self);
            dh_unlock();
            return STATUS_WAIT_0;
        }
        if (mode == 2) { dh_unlock(); return STATUS_TIMEOUT; }
        if (mode == 0) { dh_wait(); continue; }
        if (dh_timedwait_until(deadline)) {
            /* one last look: a signal may have arrived together with the deadline */
            if (WaitType == WaitAny) {
                for (ULONG i = 0; i < Count; i++)
                    if (dh_dispatcher_acquire_locked(objs[i], self)) { dh_unlock(); return STATUS_WAIT_0 + (NTSTATUS)i; }
            } else if (all_signalled_locked(objs, Count, self)) {
                for (ULONG i = 0; i < Count; i++) dh_dispatcher_acquire_locked(objs[i], self);
                dh_unlock();
                return STATUS_WAIT_0;
            }
            dh_unlock();
            return STATUS_TIMEOUT;
        }
    }
}

DRVCALL NTSTATUS KeWaitForMutexObject(PVOID Mutex, ULONG WaitReason, KPROCESSOR_MODE WaitMode,
                                      BOOLEAN Alertable, PLARGE_INTEGER Timeout)
{
    return KeWaitForSingleObject(Mutex, WaitReason, WaitMode, Alertable, Timeout);
}

/* -------------------------------------------------------------------------- */
/* Critical and guarded regions, guarded mutexes                              */
/* -------------------------------------------------------------------------- */
DRVCALL void KeEnterCriticalRegion(void) { dh_current_thread()->crit_region++; }

DRVCALL void KeLeaveCriticalRegion(void)
{
    dh_thread_t *t = dh_current_thread();
    if (t->crit_region <= 0) dh_bugcheck(DH_BUGCHECK_BAD_HOST_STATE, 0x10, 0, 0, 0);
    t->crit_region--;
}

DRVCALL void KeEnterGuardedRegion(void) { dh_current_thread()->guarded_region++; }

DRVCALL void KeLeaveGuardedRegion(void)
{
    dh_thread_t *t = dh_current_thread();
    if (t->guarded_region <= 0) dh_bugcheck(DH_BUGCHECK_BAD_HOST_STATE, 0x11, 0, 0, 0);
    t->guarded_region--;
}

DRVCALL BOOLEAN KeAreApcsDisabled(void)
{
    dh_thread_t *t = dh_current_thread();
    return (BOOLEAN)(t->crit_region > 0 || t->guarded_region > 0 || dh_irql >= IRQL_APC);
}

DRVCALL void KeInitializeGuardedMutex(PKGUARDED_MUTEX Mutex)
{
    memset(Mutex, 0, sizeof *Mutex);
    Mutex->Count = 1;
    Mutex->Event.Header.Type = DISPATCHER_GATE_OBJECT;
    Mutex->Event.Header.Size = (UCHAR)(sizeof(KEVENT) / sizeof(LONG));
    Mutex->Event.Header.WaitListHead.Flink = Mutex->Event.Header.WaitListHead.Blink = &Mutex->Event.Header.WaitListHead;
}

static void guarded_acquire(PKGUARDED_MUTEX m, int enter_region)
{
    if (dh_irql > IRQL_APC) dh_bugcheck(DH_BUGCHECK_IRQL, 11, dh_irql, 0, 0);
    dh_thread_t *self = dh_current_thread();
    if (enter_region) self->guarded_region++;
    dh_lock();
    if (m->Owner == (PVOID)self)
        dh_bugcheck(DH_BUGCHECK_LOCK_RECURSION, (ULONG_PTR)m, 4, 0, 0);
    while (m->Count != 1) {
        m->Contention++;
        dh_wait();
    }
    m->Count = 0;
    m->Owner = self;
    dh_unlock();
}

static void guarded_release(PKGUARDED_MUTEX m, int leave_region)
{
    dh_thread_t *self = dh_current_thread();
    dh_lock();
    if (m->Owner != (PVOID)self) {
        dh_unlock();
        dh_bugcheck(DH_BUGCHECK_LOCK_RECURSION, (ULONG_PTR)m, 5, 0, 0);
    }
    m->Owner = NULL;
    m->Count = 1;
    dh_notify_all();
    dh_unlock();
    if (leave_region) {
        if (self->guarded_region <= 0) dh_bugcheck(DH_BUGCHECK_BAD_HOST_STATE, 0x12, 0, 0, 0);
        self->guarded_region--;
    }
}

DRVCALL void KeAcquireGuardedMutex(PKGUARDED_MUTEX Mutex) { guarded_acquire(Mutex, 1); }
DRVCALL void KeAcquireGuardedMutexUnsafe(PKGUARDED_MUTEX Mutex) { guarded_acquire(Mutex, 0); }
DRVCALL void KeReleaseGuardedMutex(PKGUARDED_MUTEX Mutex) { guarded_release(Mutex, 1); }
DRVCALL void KeReleaseGuardedMutexUnsafe(PKGUARDED_MUTEX Mutex) { guarded_release(Mutex, 0); }

DRVCALL BOOLEAN KeTryToAcquireGuardedMutex(PKGUARDED_MUTEX Mutex)
{
    if (dh_irql > IRQL_APC) dh_bugcheck(DH_BUGCHECK_IRQL, 12, dh_irql, 0, 0);
    dh_thread_t *self = dh_current_thread();
    self->guarded_region++;
    dh_lock();
    if (Mutex->Count != 1) {
        dh_unlock();
        self->guarded_region--;
        return FALSE;
    }
    Mutex->Count = 0;
    Mutex->Owner = self;
    dh_unlock();
    return TRUE;
}

/* -------------------------------------------------------------------------- */
/* Device queues (KDEVICE_QUEUE), the machinery behind IoStartPacket          */
/* -------------------------------------------------------------------------- */
#define DH_DEVICE_QUEUE_OBJECT 20

DRVCALL void KeInitializeDeviceQueue(PKDEVICE_QUEUE Queue)
{
    Queue->Type = DH_DEVICE_QUEUE_OBJECT;
    Queue->Size = (CSHORT)sizeof *Queue;
    list_init(&Queue->DeviceListHead);
    Queue->Lock = 0;
    Queue->Busy = FALSE;
}

static BOOLEAN device_queue_insert(PKDEVICE_QUEUE Queue, PKDEVICE_QUEUE_ENTRY Entry, int by_key, ULONG SortKey)
{
    dh_lock();
    BOOLEAN inserted;
    if (!Queue->Busy) {
        Queue->Busy = TRUE;
        Entry->Inserted = FALSE;
        inserted = FALSE;
    } else {
        Entry->SortKey = SortKey;
        Entry->Inserted = TRUE;
        if (!by_key) {
            list_insert_tail(&Queue->DeviceListHead, &Entry->DeviceListEntry);
        } else {
            /* After every entry whose key is not larger. */
            LIST_ENTRY *pos = Queue->DeviceListHead.Flink;
            while (pos != &Queue->DeviceListHead &&
                   ((PKDEVICE_QUEUE_ENTRY)pos)->SortKey <= SortKey)
                pos = pos->Flink;
            list_insert_tail(pos, &Entry->DeviceListEntry);
        }
        inserted = TRUE;
    }
    dh_unlock();
    return inserted;
}

DRVCALL BOOLEAN KeInsertDeviceQueue(PKDEVICE_QUEUE Queue, PKDEVICE_QUEUE_ENTRY Entry)
{
    return device_queue_insert(Queue, Entry, 0, 0);
}

DRVCALL BOOLEAN KeInsertByKeyDeviceQueue(PKDEVICE_QUEUE Queue, PKDEVICE_QUEUE_ENTRY Entry, ULONG SortKey)
{
    return device_queue_insert(Queue, Entry, 1, SortKey);
}

static PKDEVICE_QUEUE_ENTRY device_queue_pop_locked(PKDEVICE_QUEUE Queue, LIST_ENTRY *at)
{
    list_remove(at);
    PKDEVICE_QUEUE_ENTRY e = (PKDEVICE_QUEUE_ENTRY)at;
    e->Inserted = FALSE;
    return e;
}

DRVCALL PKDEVICE_QUEUE_ENTRY KeRemoveDeviceQueue(PKDEVICE_QUEUE Queue)
{
    dh_lock();
    PKDEVICE_QUEUE_ENTRY e = NULL;
    if (list_empty(&Queue->DeviceListHead)) Queue->Busy = FALSE;
    else e = device_queue_pop_locked(Queue, Queue->DeviceListHead.Flink);
    dh_unlock();
    return e;
}

/* The first entry whose key is not smaller than SortKey; when there is none, the first entry. */
DRVCALL PKDEVICE_QUEUE_ENTRY KeRemoveByKeyDeviceQueue(PKDEVICE_QUEUE Queue, ULONG SortKey)
{
    dh_lock();
    PKDEVICE_QUEUE_ENTRY e = NULL;
    if (list_empty(&Queue->DeviceListHead)) {
        Queue->Busy = FALSE;
    } else {
        LIST_ENTRY *pos = Queue->DeviceListHead.Flink;
        while (pos != &Queue->DeviceListHead && ((PKDEVICE_QUEUE_ENTRY)pos)->SortKey < SortKey)
            pos = pos->Flink;
        if (pos == &Queue->DeviceListHead) pos = Queue->DeviceListHead.Flink;
        e = device_queue_pop_locked(Queue, pos);
    }
    dh_unlock();
    return e;
}

DRVCALL BOOLEAN KeRemoveEntryDeviceQueue(PKDEVICE_QUEUE Queue, PKDEVICE_QUEUE_ENTRY Entry)
{
    (void)Queue;
    dh_lock();
    BOOLEAN was = Entry->Inserted;
    if (was) {
        list_remove(&Entry->DeviceListEntry);
        Entry->Inserted = FALSE;
    }
    dh_unlock();
    return was;
}

/* -------------------------------------------------------------------------- */
/* Executive resources (ERESOURCE)                                            */
/*                                                                            */
/* ActiveCount is the number of acquisitions in force. ResourceOwnedExclusive */
/* in Flag marks exclusive ownership. OwnerEntry holds the first owner, and   */
/* OwnerTable the rest of the shared owners (entry 0 holds the table size).   */
/* -------------------------------------------------------------------------- */
static LIST_ENTRY g_resources;
static int g_resources_ready;

static void resources_init_locked(void)
{
    if (!g_resources_ready) { list_init(&g_resources); g_resources_ready = 1; }
}

DRVCALL NTSTATUS ExInitializeResourceLite(PERESOURCE Resource)
{
    memset(Resource, 0, sizeof *Resource);
    dh_lock();
    resources_init_locked();
    list_insert_tail(&g_resources, &Resource->SystemResourcesList);
    dh_unlock();
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS ExReinitializeResourceLite(PERESOURCE Resource)
{
    dh_lock();
    if (Resource->OwnerTable) dh_free(Resource->OwnerTable);
    LIST_ENTRY keep = Resource->SystemResourcesList;
    memset(Resource, 0, sizeof *Resource);
    Resource->SystemResourcesList = keep;
    dh_unlock();
    return STATUS_SUCCESS;
}

DRVCALL ERESOURCE_THREAD ExGetCurrentResourceThread(void)
{
    return (ERESOURCE_THREAD)(ULONG_PTR)dh_current_thread();
}

/* The owner entry of `thread`, or NULL. */
static OWNER_ENTRY *owner_find(PERESOURCE r, ERESOURCE_THREAD thread)
{
    if (r->OwnerEntry.OwnerThread == thread && r->OwnerEntry.OwnerCount) return &r->OwnerEntry;
    if (r->OwnerTable) {
        for (ULONG i = 1; i < r->OwnerTable[0].TableSize; i++)
            if (r->OwnerTable[i].OwnerThread == thread && r->OwnerTable[i].OwnerCount) return &r->OwnerTable[i];
    }
    return NULL;
}

/* A free owner entry, growing the table when needed; NULL when memory is short. */
static OWNER_ENTRY *owner_add(PERESOURCE r, ERESOURCE_THREAD thread)
{
    OWNER_ENTRY *e = NULL;
    if (r->OwnerEntry.OwnerCount == 0) {
        e = &r->OwnerEntry;
    } else {
        if (r->OwnerTable) {
            for (ULONG i = 1; i < r->OwnerTable[0].TableSize; i++)
                if (r->OwnerTable[i].OwnerCount == 0) { e = &r->OwnerTable[i]; break; }
        }
        if (!e) {
            ULONG old = r->OwnerTable ? r->OwnerTable[0].TableSize : 1;
            ULONG grown = old + 4;
            OWNER_ENTRY *t = dh_alloc(grown * sizeof *t);
            if (!t) return NULL;
            if (r->OwnerTable) { memcpy(t, r->OwnerTable, old * sizeof *t); dh_free(r->OwnerTable); }
            t[0].TableSize = grown;
            r->OwnerTable = t;
            e = &t[old];
        }
    }
    e->OwnerThread = thread;
    e->OwnerCount = 0;
    return e;
}

static BOOLEAN resource_exclusive(PERESOURCE r) { return (r->Flag & ResourceOwnedExclusive) != 0; }

/* kind: 0 exclusive; 1 shared; 2 shared, starving exclusive waiters; 3 shared, waiting behind exclusive waiters */
static BOOLEAN resource_acquire(PERESOURCE r, BOOLEAN wait, int kind)
{
    ERESOURCE_THREAD me = ExGetCurrentResourceThread();
    dh_lock();
    for (;;) {
        OWNER_ENTRY *mine = owner_find(r, me);
        if (kind == 0) {
            if (r->ActiveCount == 0) {
                OWNER_ENTRY *e = owner_add(r, me);
                if (!e) { dh_unlock(); return FALSE; }
                e->OwnerCount = 1;
                r->ActiveCount = 1;
                r->ActiveEntries = 1;
                r->Flag |= ResourceOwnedExclusive;
                dh_unlock();
                return TRUE;
            }
            if (resource_exclusive(r) && mine) {         /* recursive */
                mine->OwnerCount++;
                r->ActiveCount++;
                dh_unlock();
                return TRUE;
            }
        } else {
            if (resource_exclusive(r)) {
                if (mine) {                              /* shared request by the exclusive owner is recursive */
                    mine->OwnerCount++;
                    r->ActiveCount++;
                    dh_unlock();
                    return TRUE;
                }
            } else if (r->ActiveCount == 0 || mine ||
                       (r->NumberOfExclusiveWaiters == 0 || kind == 2)) {
                if (kind == 3 && r->NumberOfExclusiveWaiters && !mine) goto must_wait;
                OWNER_ENTRY *e = mine ? mine : owner_add(r, me);
                if (!e) { dh_unlock(); return FALSE; }
                if (!mine) r->ActiveEntries++;
                e->OwnerCount++;
                r->ActiveCount++;
                dh_unlock();
                return TRUE;
            }
        }
must_wait:
        if (!wait) { dh_unlock(); return FALSE; }
        r->ContentionCount++;
        if (kind == 0) r->NumberOfExclusiveWaiters++; else r->NumberOfSharedWaiters++;
        dh_wait();
        if (kind == 0) r->NumberOfExclusiveWaiters--; else r->NumberOfSharedWaiters--;
    }
}

DRVCALL BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE Resource, BOOLEAN Wait)
{
    return resource_acquire(Resource, Wait, 0);
}

DRVCALL BOOLEAN ExTryToAcquireResourceExclusiveLite(PERESOURCE Resource)
{
    return resource_acquire(Resource, FALSE, 0);
}

DRVCALL BOOLEAN ExAcquireResourceSharedLite(PERESOURCE Resource, BOOLEAN Wait)
{
    return resource_acquire(Resource, Wait, 1);
}

DRVCALL BOOLEAN ExAcquireSharedStarveExclusive(PERESOURCE Resource, BOOLEAN Wait)
{
    return resource_acquire(Resource, Wait, 2);
}

DRVCALL BOOLEAN ExAcquireSharedWaitForExclusive(PERESOURCE Resource, BOOLEAN Wait)
{
    return resource_acquire(Resource, Wait, 3);
}

DRVCALL void ExReleaseResourceForThreadLite(PERESOURCE Resource, ERESOURCE_THREAD ResourceThreadId)
{
    dh_lock();
    OWNER_ENTRY *e = owner_find(Resource, ResourceThreadId);
    if (!e || Resource->ActiveCount <= 0) {
        dh_unlock();
        dh_bugcheck(DH_BUGCHECK_LOCK_RECURSION, (ULONG_PTR)Resource, 6, 0, 0);
    }
    e->OwnerCount--;
    Resource->ActiveCount--;
    if (e->OwnerCount == 0) {
        e->OwnerThread = 0;
        if (Resource->ActiveEntries) Resource->ActiveEntries--;
    }
    if (Resource->ActiveCount == 0) Resource->Flag &= (USHORT)~ResourceOwnedExclusive;
    dh_notify_all();
    dh_unlock();
}

DRVCALL void ExReleaseResourceLite(PERESOURCE Resource)
{
    ExReleaseResourceForThreadLite(Resource, ExGetCurrentResourceThread());
}

DRVCALL void ExConvertExclusiveToSharedLite(PERESOURCE Resource)
{
    dh_lock();
    if (!resource_exclusive(Resource)) {
        dh_unlock();
        dh_bugcheck(DH_BUGCHECK_LOCK_RECURSION, (ULONG_PTR)Resource, 7, 0, 0);
    }
    Resource->Flag &= (USHORT)~ResourceOwnedExclusive;
    dh_notify_all();
    dh_unlock();
}

DRVCALL BOOLEAN ExIsResourceAcquiredExclusiveLite(PERESOURCE Resource)
{
    dh_lock();
    BOOLEAN b = resource_exclusive(Resource) && owner_find(Resource, ExGetCurrentResourceThread()) != NULL;
    dh_unlock();
    return b;
}

/* The number of shared acquisitions the current thread holds (0 when none). */
DRVCALL ULONG ExIsResourceAcquiredSharedLite(PERESOURCE Resource)
{
    dh_lock();
    OWNER_ENTRY *e = owner_find(Resource, ExGetCurrentResourceThread());
    ULONG n = e ? e->OwnerCount : 0;
    dh_unlock();
    return n;
}

DRVCALL ULONG ExGetExclusiveWaiterCount(PERESOURCE Resource) { return Resource->NumberOfExclusiveWaiters; }
DRVCALL ULONG ExGetSharedWaiterCount(PERESOURCE Resource) { return Resource->NumberOfSharedWaiters; }

DRVCALL void ExSetResourceOwnerPointer(PERESOURCE Resource, PVOID OwnerPointer)
{
    /* Hands the current thread's acquisition to a pointer-keyed owner, so that another thread can release it. */
    dh_lock();
    OWNER_ENTRY *e = owner_find(Resource, ExGetCurrentResourceThread());
    if (e) e->OwnerThread = ((ERESOURCE_THREAD)(ULONG_PTR)OwnerPointer) | 3;
    dh_unlock();
}

DRVCALL NTSTATUS ExDeleteResourceLite(PERESOURCE Resource)
{
    dh_lock();
    if (Resource->SystemResourcesList.Flink && Resource->SystemResourcesList.Blink)
        list_remove(&Resource->SystemResourcesList);
    if (Resource->OwnerTable) { dh_free(Resource->OwnerTable); Resource->OwnerTable = NULL; }
    Resource->SystemResourcesList.Flink = Resource->SystemResourcesList.Blink = NULL;
    dh_unlock();
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Sequenced lists (SLIST)                                                    */
/*                                                                            */
/* Region is the address of the first entry. Alignment keeps the depth in its */
/* low 16 bits, as the WDK's inline ExQueryDepthSList reads it, and a         */
/* sequence number above that.                                                */
/* -------------------------------------------------------------------------- */
#define SLIST_DEPTH(h) ((USHORT)((h)->Alignment & 0xffff))

DRVCALL void RtlInitializeSListHead(PSLIST_HEADER ListHead)
{
    ListHead->Alignment = 0;
    ListHead->Region = 0;
}

DRVCALL PSLIST_ENTRY RtlFirstEntrySList(const SLIST_HEADER *ListHead)
{
    return (PSLIST_ENTRY)(ULONG_PTR)ListHead->Region;
}

DRVCALL USHORT RtlQueryDepthSList(PSLIST_HEADER ListHead)
{
    return SLIST_DEPTH(ListHead);
}

static PSLIST_ENTRY slist_push_locked(PSLIST_HEADER h, PSLIST_ENTRY e)
{
    PSLIST_ENTRY first = (PSLIST_ENTRY)(ULONG_PTR)h->Region;
    e->Next = first;
    h->Region = (ULONGLONG)(ULONG_PTR)e;
    h->Alignment = (((h->Alignment >> 16) + 1) << 16) | (USHORT)(SLIST_DEPTH(h) + 1);
    return first;
}

static PSLIST_ENTRY slist_pop_locked(PSLIST_HEADER h)
{
    PSLIST_ENTRY first = (PSLIST_ENTRY)(ULONG_PTR)h->Region;
    if (!first) return NULL;
    h->Region = (ULONGLONG)(ULONG_PTR)first->Next;
    h->Alignment = (((h->Alignment >> 16) + 1) << 16) | (USHORT)(SLIST_DEPTH(h) - 1);
    return first;
}

DRVCALL PSLIST_ENTRY RtlInterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry)
{
    dh_lock();
    PSLIST_ENTRY prev = slist_push_locked(ListHead, ListEntry);
    dh_unlock();
    return prev;
}

DRVCALL PSLIST_ENTRY RtlInterlockedPopEntrySList(PSLIST_HEADER ListHead)
{
    dh_lock();
    PSLIST_ENTRY e = slist_pop_locked(ListHead);
    dh_unlock();
    return e;
}

DRVCALL PSLIST_ENTRY RtlInterlockedFlushSList(PSLIST_HEADER ListHead)
{
    dh_lock();
    PSLIST_ENTRY first = (PSLIST_ENTRY)(ULONG_PTR)ListHead->Region;
    ListHead->Region = 0;
    ListHead->Alignment = (((ListHead->Alignment >> 16) + 1) << 16);
    dh_unlock();
    return first;
}

/* The WDK's x64 InterlockedPushEntrySList and friends call these names. */
DRVCALL PSLIST_ENTRY ExpInterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry)
{
    return RtlInterlockedPushEntrySList(ListHead, ListEntry);
}

DRVCALL PSLIST_ENTRY ExpInterlockedPopEntrySList(PSLIST_HEADER ListHead)
{
    return RtlInterlockedPopEntrySList(ListHead);
}

DRVCALL PSLIST_ENTRY ExpInterlockedFlushSList(PSLIST_HEADER ListHead)
{
    return RtlInterlockedFlushSList(ListHead);
}

DRVCALL PSLIST_ENTRY ExInterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry, PKSPIN_LOCK Lock)
{
    (void)Lock;
    return RtlInterlockedPushEntrySList(ListHead, ListEntry);
}

DRVCALL PSLIST_ENTRY ExInterlockedPopEntrySList(PSLIST_HEADER ListHead, PKSPIN_LOCK Lock)
{
    (void)Lock;
    return RtlInterlockedPopEntrySList(ListHead);
}

DRVCALL PSLIST_ENTRY ExInterlockedFlushSList(PSLIST_HEADER ListHead)
{
    return RtlInterlockedFlushSList(ListHead);
}

/* -------------------------------------------------------------------------- */
/* Interlocked doubly and singly linked lists                                 */
/* -------------------------------------------------------------------------- */
DRVCALL PLIST_ENTRY ExInterlockedInsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry, PKSPIN_LOCK Lock)
{
    (void)Lock;
    dh_lock();
    PLIST_ENTRY prev = list_empty(ListHead) ? NULL : ListHead->Flink;
    list_insert_head(ListHead, ListEntry);
    dh_unlock();
    return prev;
}

DRVCALL PLIST_ENTRY ExInterlockedInsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry, PKSPIN_LOCK Lock)
{
    (void)Lock;
    dh_lock();
    PLIST_ENTRY prev = list_empty(ListHead) ? NULL : ListHead->Blink;
    list_insert_tail(ListHead, ListEntry);
    dh_unlock();
    return prev;
}

DRVCALL PLIST_ENTRY ExInterlockedRemoveHeadList(PLIST_ENTRY ListHead, PKSPIN_LOCK Lock)
{
    (void)Lock;
    dh_lock();
    PLIST_ENTRY e = NULL;
    if (!list_empty(ListHead)) { e = ListHead->Flink; list_remove(e); }
    dh_unlock();
    return e;
}

DRVCALL PSINGLE_LIST_ENTRY ExInterlockedPushEntryList(PSINGLE_LIST_ENTRY ListHead, PSINGLE_LIST_ENTRY ListEntry,
                                                      PKSPIN_LOCK Lock)
{
    (void)Lock;
    dh_lock();
    PSINGLE_LIST_ENTRY prev = ListHead->Next;
    ListEntry->Next = prev;
    ListHead->Next = ListEntry;
    dh_unlock();
    return prev;
}

DRVCALL PSINGLE_LIST_ENTRY ExInterlockedPopEntryList(PSINGLE_LIST_ENTRY ListHead, PKSPIN_LOCK Lock)
{
    (void)Lock;
    dh_lock();
    PSINGLE_LIST_ENTRY e = ListHead->Next;
    if (e) ListHead->Next = e->Next;
    dh_unlock();
    return e;
}

/* -------------------------------------------------------------------------- */
/* Lookaside lists                                                            */
/* -------------------------------------------------------------------------- */
static LIST_ENTRY g_lookasides_np, g_lookasides_paged;
static int g_lookasides_ready;

#define POOL_NX_ALLOCATION 0x0002   /* ExInitialize*LookasideList flag */

static void lookaside_init(PGENERAL_LOOKASIDE L, PALLOCATE_FUNCTION Allocate, PFREE_FUNCTION Free, ULONG Flags,
                           SIZE_T Size, ULONG Tag, POOL_TYPE Base, LIST_ENTRY *registry)
{
    memset(L, 0, sizeof *L);
    RtlInitializeSListHead(&L->ListHead);
    L->Depth = 4;
    L->MaximumDepth = 256;
    L->Size = (ULONG)Size;
    L->Tag = Tag;
    L->Type = (POOL_TYPE)((Flags & POOL_NX_ALLOCATION) && Base == NonPagedPool ? NonPagedPoolNx : Base);
    L->Allocate = Allocate ? Allocate : ExAllocatePoolWithTag;
    L->Free = Free ? Free : ExFreePool;
    dh_lock();
    if (!g_lookasides_ready) { list_init(&g_lookasides_np); list_init(&g_lookasides_paged); g_lookasides_ready = 1; }
    list_insert_tail(registry == NULL ? &g_lookasides_np : registry, &L->ListEntry);
    dh_unlock();
}

DRVCALL void ExInitializeNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside, PALLOCATE_FUNCTION Allocate,
                                             PFREE_FUNCTION Free, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth)
{
    (void)Depth;
    lookaside_init(&Lookaside->L, Allocate, Free, Flags, Size, Tag, NonPagedPool, NULL);
}

DRVCALL void ExInitializePagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside, PALLOCATE_FUNCTION Allocate,
                                            PFREE_FUNCTION Free, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth)
{
    (void)Depth;
    dh_lock();
    if (!g_lookasides_ready) { list_init(&g_lookasides_np); list_init(&g_lookasides_paged); g_lookasides_ready = 1; }
    dh_unlock();
    lookaside_init(&Lookaside->L, Allocate, Free, Flags, Size, Tag, PagedPool, &g_lookasides_paged);
}

static void lookaside_delete(PGENERAL_LOOKASIDE L)
{
    dh_lock();
    if (L->ListEntry.Flink && L->ListEntry.Blink) list_remove(&L->ListEntry);
    L->ListEntry.Flink = L->ListEntry.Blink = NULL;
    dh_unlock();
    PSLIST_ENTRY e;
    while ((e = RtlInterlockedPopEntrySList(&L->ListHead)) != NULL) L->Free(e);
}

DRVCALL void ExDeleteNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside) { lookaside_delete(&Lookaside->L); }
DRVCALL void ExDeletePagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside) { lookaside_delete(&Lookaside->L); }
