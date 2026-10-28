/* drvhost_ioext.c - the rest of the I/O manager's driver-facing surface that does not need a device tree: remove
 * locks, the cancel spin lock, StartIo packet queueing, IRP re-initialization, named events, shutdown notification and
 * the process and thread accessors.
 *
 * The cancel and StartIo protocols follow the Windows ones: IoStartPacket holds the cancel lock while it installs the
 * cancel routine, and a cancel routine is called with the lock held and releases it itself.
 */

#include "drvhost_int.h"

/* -------------------------------------------------------------------------- */
/* Remove locks                                                               */
/* -------------------------------------------------------------------------- */
#define DH_REMLOCK_SIGNATURE 0x4c4d4552   /* "REML" */

DRVCALL void IoInitializeRemoveLockEx(PIO_REMOVE_LOCK Lock, ULONG AllocateTag, ULONG MaxLockedMinutes,
                                      ULONG HighWatermark, ULONG RemlockSize)
{
    (void)MaxLockedMinutes; (void)HighWatermark; (void)RemlockSize;
    memset(Lock, 0, sizeof *Lock);
    Lock->Common.Removed = FALSE;
    Lock->Common.IoCount = 1;                       /* the reference the lock owner holds until removal */
    KeInitializeEvent(&Lock->Common.RemoveEvent, NotificationEvent, FALSE);
    Lock->Dbg.Signature = DH_REMLOCK_SIGNATURE;
    Lock->Dbg.AllocateTag = (LONG)AllocateTag;
    Lock->Dbg.LockList.Flink = Lock->Dbg.LockList.Blink = &Lock->Dbg.LockList;
}

DRVCALL NTSTATUS IoAcquireRemoveLockEx(PIO_REMOVE_LOCK Lock, PVOID Tag, const CHAR *File, ULONG Line,
                                       ULONG RemlockSize)
{
    (void)Tag; (void)File; (void)Line; (void)RemlockSize;
    __atomic_add_fetch(&Lock->Common.IoCount, 1, __ATOMIC_SEQ_CST);
    if (Lock->Common.Removed) {
        if (__atomic_sub_fetch(&Lock->Common.IoCount, 1, __ATOMIC_SEQ_CST) == 0)
            KeSetEvent(&Lock->Common.RemoveEvent, 0, FALSE);
        return STATUS_DELETE_PENDING;
    }
    return STATUS_SUCCESS;
}

DRVCALL void IoReleaseRemoveLockEx(PIO_REMOVE_LOCK Lock, PVOID Tag, ULONG RemlockSize)
{
    (void)Tag; (void)RemlockSize;
    LONG n = __atomic_sub_fetch(&Lock->Common.IoCount, 1, __ATOMIC_SEQ_CST);
    if (n < 0) dh_bugcheck(DH_BUGCHECK_BAD_HOST_STATE, 0x30, (ULONG_PTR)Lock, (ULONG_PTR)n, 0);
    if (n == 0) KeSetEvent(&Lock->Common.RemoveEvent, 0, FALSE);
}

DRVCALL void IoReleaseRemoveLockAndWaitEx(PIO_REMOVE_LOCK Lock, PVOID Tag, ULONG RemlockSize)
{
    Lock->Common.Removed = TRUE;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    IoReleaseRemoveLockEx(Lock, Tag, RemlockSize);          /* the caller's own acquisition */
    LONG n = __atomic_sub_fetch(&Lock->Common.IoCount, 1, __ATOMIC_SEQ_CST);   /* the owner's reference */
    if (n > 0)
        KeWaitForSingleObject(&Lock->Common.RemoveEvent, Executive, KernelMode, FALSE, NULL);
    else if (n == 0)
        KeSetEvent(&Lock->Common.RemoveEvent, 0, FALSE);
}

/* -------------------------------------------------------------------------- */
/* Cancel spin lock and cancel routines                                       */
/* -------------------------------------------------------------------------- */
static KSPIN_LOCK g_cancel_lock;

DRVCALL void IoAcquireCancelSpinLock(PKIRQL Irql)
{
    KeAcquireSpinLock(&g_cancel_lock, Irql);
}

DRVCALL void IoReleaseCancelSpinLock(KIRQL Irql)
{
    KeReleaseSpinLock(&g_cancel_lock, Irql);
}

DRVCALL PDRIVER_CANCEL IoSetCancelRoutine(PIRP Irp, PDRIVER_CANCEL CancelRoutine)
{
    return __atomic_exchange_n(&Irp->CancelRoutine, CancelRoutine, __ATOMIC_SEQ_CST);
}

/* Unlike the version in drvhost_io.c's older sketch, this follows the Windows protocol: Cancel is set whether or not a
 * routine is installed, the cancel lock is held across the call, and the routine releases it (at Irp->CancelIrql). */
DRVCALL BOOLEAN IoCancelIrp(PIRP Irp)
{
    KIRQL irql;
    Irp->Cancel = TRUE;
    IoAcquireCancelSpinLock(&irql);
    Irp->CancelIrql = irql;
    PDRIVER_CANCEL routine = __atomic_exchange_n(&Irp->CancelRoutine, NULL, __ATOMIC_SEQ_CST);
    if (!routine) {
        IoReleaseCancelSpinLock(irql);
        return FALSE;
    }
    PDEVICE_OBJECT dev = (Irp->CurrentLocation <= Irp->StackCount)
                             ? Irp->Tail.Overlay.CurrentStackLocation->DeviceObject
                             : NULL;
    routine(dev, Irp);
    return TRUE;
}

/* -------------------------------------------------------------------------- */
/* StartIo packet queueing                                                    */
/* -------------------------------------------------------------------------- */
static void call_start_io(PDEVICE_OBJECT dev, PIRP irp)
{
    PDRIVER_STARTIO fn = dev->DriverObject ? dev->DriverObject->DriverStartIo : NULL;
    if (!fn) dh_bugcheck(DH_BUGCHECK_BAD_HOST_STATE, 0x31, (ULONG_PTR)dev, (ULONG_PTR)irp, 0);
    fn(dev, irp);
}

DRVCALL void IoStartPacket(PDEVICE_OBJECT DeviceObject, PIRP Irp, PULONG Key, PDRIVER_CANCEL CancelFunction)
{
    KIRQL old, cancel_irql = 0;
    BOOLEAN queued;
    PKDEVICE_QUEUE_ENTRY entry = &Irp->Tail.Overlay.DeviceQueueEntry;

    old = KeRaiseIrqlToDpcLevel();
    if (CancelFunction) {
        IoAcquireCancelSpinLock(&cancel_irql);
        Irp->CancelRoutine = CancelFunction;
    }
    if (Key) queued = KeInsertByKeyDeviceQueue(&DeviceObject->DeviceQueue, entry, *Key);
    else     queued = KeInsertDeviceQueue(&DeviceObject->DeviceQueue, entry);

    if (!queued) {
        /* The device was idle: this packet is the current one. */
        DeviceObject->CurrentIrp = Irp;
        if (CancelFunction) {
            Irp->CancelRoutine = NULL;
            IoReleaseCancelSpinLock(cancel_irql);
        }
        call_start_io(DeviceObject, Irp);
    } else if (CancelFunction) {
        if (Irp->Cancel && Irp->CancelRoutine) {
            /* Cancelled while it was being queued: take it back out and let its routine run, lock still held. */
            PDRIVER_CANCEL routine = Irp->CancelRoutine;
            Irp->CancelRoutine = NULL;
            Irp->CancelIrql = cancel_irql;
            KeRemoveEntryDeviceQueue(&DeviceObject->DeviceQueue, entry);
            routine(DeviceObject, Irp);
        } else {
            IoReleaseCancelSpinLock(cancel_irql);
        }
    }
    KeLowerIrql(old);
}

static void start_next(PDEVICE_OBJECT dev, BOOLEAN cancelable, const ULONG *key)
{
    KIRQL old, cancel_irql = 0;
    old = KeRaiseIrqlToDpcLevel();
    if (cancelable) IoAcquireCancelSpinLock(&cancel_irql);
    PKDEVICE_QUEUE_ENTRY e = key ? KeRemoveByKeyDeviceQueue(&dev->DeviceQueue, *key)
                                 : KeRemoveDeviceQueue(&dev->DeviceQueue);
    if (e) {
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.DeviceQueueEntry);
        dev->CurrentIrp = irp;
        if (cancelable) IoReleaseCancelSpinLock(cancel_irql);
        call_start_io(dev, irp);
    } else {
        dev->CurrentIrp = NULL;
        if (cancelable) IoReleaseCancelSpinLock(cancel_irql);
    }
    KeLowerIrql(old);
}

DRVCALL void IoStartNextPacket(PDEVICE_OBJECT DeviceObject, BOOLEAN Cancelable)
{
    start_next(DeviceObject, Cancelable, NULL);
}

DRVCALL void IoStartNextPacketByKey(PDEVICE_OBJECT DeviceObject, BOOLEAN Cancelable, ULONG Key)
{
    start_next(DeviceObject, Cancelable, &Key);
}

DRVCALL void IoRequestDpc(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    KeInsertQueueDpc(&DeviceObject->Dpc, Irp, Context);
}

/* -------------------------------------------------------------------------- */
/* IRP re-initialization                                                      */
/* -------------------------------------------------------------------------- */
DRVCALL void IoInitializeIrp(PIRP Irp, USHORT PacketSize, CCHAR StackSize)
{
    memset(Irp, 0, PacketSize);
    Irp->Type = 6;                                   /* IO_TYPE_IRP */
    Irp->Size = PacketSize;
    Irp->StackCount = StackSize;
    Irp->CurrentLocation = (CHAR)(StackSize + 1);
    Irp->ThreadListEntry.Flink = Irp->ThreadListEntry.Blink = &Irp->ThreadListEntry;
    Irp->Tail.Overlay.CurrentStackLocation = (PIO_STACK_LOCATION)((PCHAR)Irp + PacketSize);
}

DRVCALL void IoReuseIrp(PIRP Irp, NTSTATUS Status)
{
    UCHAR flags = Irp->AllocationFlags;
    IoInitializeIrp(Irp, Irp->Size, Irp->StackCount);
    Irp->AllocationFlags = flags;
    Irp->IoStatus.Status = Status;
}

/* -------------------------------------------------------------------------- */
/* Named events                                                               */
/* -------------------------------------------------------------------------- */
enum { DH_MAX_NAMED_EVENTS = 64 };
typedef struct { char name[128]; PKEVENT event; } dh_named_event_t;
static dh_named_event_t g_named_events[DH_MAX_NAMED_EVENTS];

static PKEVENT named_event(PUNICODE_STRING Name, PHANDLE Handle, EVENT_TYPE type)
{
    char key[128];
    if (!Name || !Name->Length || !Handle) return NULL;
    dh_ascii_from_unicode(Name, key, sizeof key);
    for (char *c = key; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);

    PKEVENT ev = NULL;
    dh_lock();
    int free_slot = -1;
    for (int i = 0; i < DH_MAX_NAMED_EVENTS; i++) {
        if (!g_named_events[i].event) { if (free_slot < 0) free_slot = i; continue; }
        if (strcmp(g_named_events[i].name, key) == 0) { ev = g_named_events[i].event; break; }
    }
    if (!ev && free_slot >= 0) {
        ev = dh_alloc(sizeof(KEVENT));
        if (ev) {
            KeInitializeEvent(ev, type, TRUE);       /* these events start signalled */
            strcpy(g_named_events[free_slot].name, key);
            g_named_events[free_slot].event = ev;
        }
    }
    dh_unlock();
    if (!ev) return NULL;
    HANDLE h = dh_handle_insert(DH_KIND_EVENT, ev);
    if (!h) return NULL;
    *Handle = h;
    return ev;
}

DRVCALL PKEVENT IoCreateNotificationEvent(PUNICODE_STRING EventName, PHANDLE EventHandle)
{
    return named_event(EventName, EventHandle, NotificationEvent);
}

DRVCALL PKEVENT IoCreateSynchronizationEvent(PUNICODE_STRING EventName, PHANDLE EventHandle)
{
    return named_event(EventName, EventHandle, SynchronizationEvent);
}

void dh_io_named_events_clear(void)
{
    dh_lock();
    for (int i = 0; i < DH_MAX_NAMED_EVENTS; i++) {
        if (g_named_events[i].event) dh_free(g_named_events[i].event);
        g_named_events[i].event = NULL;
        g_named_events[i].name[0] = 0;
    }
    dh_unlock();
}

/* -------------------------------------------------------------------------- */
/* Shutdown notification                                                      */
/* -------------------------------------------------------------------------- */
DRVCALL NTSTATUS IoRegisterShutdownNotification(PDEVICE_OBJECT DeviceObject)
{
    if (!DeviceObject) return STATUS_INVALID_PARAMETER;
    DH_FROM_DEV(DeviceObject)->shutdown_notify = 1;
    return STATUS_SUCCESS;
}

DRVCALL void IoUnregisterShutdownNotification(PDEVICE_OBJECT DeviceObject)
{
    if (DeviceObject) DH_FROM_DEV(DeviceObject)->shutdown_notify = 0;
}

/* Called by drvhost_shutdown before the drivers are unloaded. */
void dh_io_send_shutdown(void)
{
    PDEVICE_OBJECT devs[64];
    int n = dh_devices_with_shutdown(devs, 64);
    for (int i = 0; i < n; i++) {
        PDEVICE_OBJECT dev = devs[i];
        PIRP irp = IoAllocateIrp(dev->StackSize ? dev->StackSize : 1, FALSE);
        if (!irp) continue;
        PIO_STACK_LOCATION sl = irp->Tail.Overlay.CurrentStackLocation - 1;
        sl->MajorFunction = IRP_MJ_SHUTDOWN;
        irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        IoCallDriver(dev, irp);
        IoFreeIrp(irp);
    }
}

/* -------------------------------------------------------------------------- */
/* Processes, threads and driver-visible odds and ends                        */
/* -------------------------------------------------------------------------- */
DRVCALL PVOID PsGetCurrentThread(void)
{
    return KeGetCurrentThread();
}

DRVCALL PVOID IoGetCurrentProcess(void)
{
    return PsGetCurrentProcess();
}

DRVCALL HANDLE PsGetProcessId(PVOID Process)
{
    return Process == PsGetCurrentProcess() ? PsGetCurrentProcessId() : NULL;
}

DRVCALL HANDLE PsGetThreadId(PVOID Thread)
{
    return (HANDLE)Thread;                          /* the host's thread identity is the thread object */
}

DRVCALL NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId, PVOID *Process)
{
    if (!Process) return STATUS_INVALID_PARAMETER;
    if (ProcessId != PsGetCurrentProcessId()) return STATUS_INVALID_CID;
    *Process = PsGetCurrentProcess();
    return STATUS_SUCCESS;
}

DRVCALL PVOID IoGetRequestorProcess(PIRP Irp)
{
    return Irp->Tail.Overlay.Thread ? PsGetCurrentProcess() : NULL;
}

DRVCALL HANDLE IoGetRequestorProcessId(PIRP Irp)
{
    return IoGetRequestorProcess(Irp) ? PsGetCurrentProcessId() : NULL;
}

DRVCALL PDEVICE_OBJECT IoGetLowerDeviceObject(PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT lower = DeviceObject ? DeviceObject->DeviceObjectExtension->AttachedTo : NULL;
    if (lower) ObfReferenceObject(lower);
    return lower;
}

DRVCALL BOOLEAN IoIsWdmVersionAvailable(UCHAR MajorVersion, UCHAR MinorVersion)
{
    /* WDM 6.0x30 is what current Windows reports. */
    return MajorVersion < 6 || (MajorVersion == 6 && MinorVersion <= 0x30);
}

DRVCALL NTSTATUS IoCreateDeviceSecure(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize,
                                      PUNICODE_STRING DeviceName, ULONG DeviceType, ULONG DeviceCharacteristics,
                                      BOOLEAN Exclusive, PCUNICODE_STRING DefaultSDDLString,
                                      LPCGUID DeviceClassGuid, PDEVICE_OBJECT *DeviceObject)
{
    (void)DefaultSDDLString; (void)DeviceClassGuid;       /* the personality has no per-device security descriptors yet */
    return IoCreateDevice(DriverObject, DeviceExtensionSize, DeviceName, DeviceType, DeviceCharacteristics,
                          Exclusive, DeviceObject);
}

DRVCALL PVOID IoAllocateErrorLogEntry(PVOID IoObject, UCHAR EntrySize)
{
    (void)IoObject;
    if (EntrySize < 0x30) return NULL;                    /* sizeof(IO_ERROR_LOG_PACKET) */
    return dh_alloc(EntrySize);
}

DRVCALL void IoWriteErrorLogEntry(PVOID ElEntry)
{
    /* The packet is consumed. A system event log is not part of the host yet, so it is dropped. */
    dh_free(ElEntry);
}

DRVCALL NTSTATUS ExUuidCreate(UUID *Uuid)
{
    if (!Uuid) return STATUS_INVALID_PARAMETER;
    dh_random_bytes(Uuid, sizeof *Uuid);
    Uuid->Data3 = (USHORT)((Uuid->Data3 & 0x0fff) | 0x4000);     /* version 4 */
    Uuid->Data4[0] = (UCHAR)((Uuid->Data4[0] & 0x3f) | 0x80);    /* RFC 4122 variant */
    return STATUS_SUCCESS;
}

DRVCALL ULONG KeQueryActiveProcessorCountEx(USHORT GroupNumber)
{
    return GroupNumber == 0 || GroupNumber == 0xffff ? dh_cpu_count() : 0;
}

DRVCALL ULONG KeQueryActiveProcessorCount(PKAFFINITY ActiveProcessors)
{
    unsigned n = dh_cpu_count();
    if (ActiveProcessors) *ActiveProcessors = n >= 64 ? ~(KAFFINITY)0 : (((KAFFINITY)1 << n) - 1);
    return n;
}

DRVCALL ULONG KeGetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber)
{
    unsigned c = dh_cpu_current();
    if (ProcNumber) { ProcNumber->Group = 0; ProcNumber->Number = (UCHAR)c; ProcNumber->Reserved = 0; }
    return c;
}
