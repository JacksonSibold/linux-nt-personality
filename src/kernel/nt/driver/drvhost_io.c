/* drvhost_io.c - I/O manager: devices, the device and symbolic-link namespace,
 * IRP allocation and dispatch, completion with stack-location unwinding, and
 * IoTimers.
 *
 * IRP stack model (WDK): an IRP has StackCount locations.  CurrentLocation is
 * 1-based and starts at StackCount + 1.  IoCallDriver decrements it and makes
 * that location current; IoCompleteRequest walks back up.  A completion routine
 * is stored in the lower location by the driver above it, and runs with the
 * upper device object.
 */

#include "drvhost_int.h"

#define DH_MAX_SYMLINKS 256
#define DH_IO_TIMER_PERIOD_NS 1000000000ull

typedef struct dh_symlink {
    char link[DH_NAME_MAX];
    char target[DH_NAME_MAX];
    int  used;
} dh_symlink_t;

static dh_symlink_t g_symlinks[DH_MAX_SYMLINKS];
static dh_device_t *g_all_devices = NULL;

static int ascii_ieq(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

void dh_ascii_from_unicode(const UNICODE_STRING *u, char *out, size_t cap)
{
    size_t n = 0;
    if (!u || !u->Buffer) { if (cap) out[0] = 0; return; }
    size_t chars = u->Length / 2u;
    for (size_t i = 0; i < chars && n + 1 < cap; i++) {
        WCHAR c = u->Buffer[i];
        out[n++] = (c < 0x80) ? (char)c : '?';
    }
    out[n] = 0;
}

dh_device_t *dh_device_by_name(const char *nt_name)
{
    dh_device_t *found = NULL;
    dh_lock();
    for (dh_device_t *d = g_all_devices; d; d = d->all_next) {
        if (d->name_registered && ascii_ieq(d->name, nt_name)) { found = d; break; }
    }
    dh_unlock();
    return found;
}

int dh_symlink_lookup(const char *nt_link, char *target, size_t cap)
{
    int ok = 0;
    dh_lock();
    for (int i = 0; i < DH_MAX_SYMLINKS; i++) {
        if (g_symlinks[i].used && ascii_ieq(g_symlinks[i].link, nt_link)) {
            size_t n = strlen(g_symlinks[i].target);
            if (n + 1 <= cap) { memcpy(target, g_symlinks[i].target, n + 1); ok = 1; }
            break;
        }
    }
    dh_unlock();
    return ok;
}

/* -------------------------------------------------------------------------- */
/* Devices                                                                    */
/* -------------------------------------------------------------------------- */
DRVCALL NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize,
                                PUNICODE_STRING DeviceName, ULONG DeviceType,
                                ULONG DeviceCharacteristics, BOOLEAN Exclusive,
                                PDEVICE_OBJECT *DeviceObject)
{
    (void)Exclusive;
    if (!DriverObject || !DeviceObject) return STATUS_INVALID_PARAMETER;
    size_t hdr = (sizeof(dh_device_t) + 15u) & ~(size_t)15u;
    dh_device_t *d = dh_alloc(hdr + DeviceExtensionSize);
    if (!d) return STATUS_INSUFFICIENT_RESOURCES;

    if (DeviceName && DeviceName->Length) {
        dh_ascii_from_unicode(DeviceName, d->name, sizeof d->name);
        if (dh_device_by_name(d->name)) {
            dh_free(d);
            return STATUS_OBJECT_NAME_COLLISION;
        }
        d->name_registered = 1;
    }

    d->dev.Type = 3;                                   /* IO_TYPE_DEVICE */
    d->dev.Size = (USHORT)sizeof(DEVICE_OBJECT);
    d->dev.ReferenceCount = 1;
    d->dev.DriverObject = DriverObject;
    d->dev.Flags = DO_DEVICE_INITIALIZING;
    d->dev.Characteristics = DeviceCharacteristics;
    d->dev.DeviceType = DeviceType;
    d->dev.StackSize = 1;
    d->dev.DeviceExtension = (DeviceExtensionSize != 0) ? (char *)d + hdr : NULL;
    d->dev.DeviceObjectExtension = &d->ext;
    d->ext.Type = 13;                                  /* IO_TYPE_DEVICE_OBJECT_EXTENSION */
    d->ext.Size = (USHORT)sizeof(DEVOBJ_EXTENSION);
    d->ext.DeviceObject = &d->dev;
    d->dev.Queue.ListEntry.Flink = d->dev.Queue.ListEntry.Blink = &d->dev.Queue.ListEntry;
    KeInitializeDeviceQueue(&d->dev.DeviceQueue);
    KeInitializeEvent(&d->dev.DeviceLock, SynchronizationEvent, TRUE);

    dh_lock();
    d->dev.NextDevice = DriverObject->DeviceObject;
    DriverObject->DeviceObject = &d->dev;
    d->all_next = g_all_devices;
    g_all_devices = d;
    g_devices_live++;
    dh_unlock();

    *DeviceObject = &d->dev;
    return STATUS_SUCCESS;
}

DRVCALL void IoDeleteDevice(PDEVICE_OBJECT DeviceObject)
{
    if (!DeviceObject) return;
    dh_device_t *d = DH_FROM_DEV(DeviceObject);
    dh_lock();
    /* Stop the IoTimer and wait for a call already running on the worker, so
     * the timer routine never sees a freed device extension. */
    d->io_timer_active = 0;
    while (d->io_timer_inflight > 0) dh_wait();
    /* Unlink from the driver's device list. */
    PDRIVER_OBJECT drv = DeviceObject->DriverObject;
    if (drv) {
        if (drv->DeviceObject == DeviceObject) {
            drv->DeviceObject = DeviceObject->NextDevice;
        } else {
            for (PDEVICE_OBJECT p = drv->DeviceObject; p; p = p->NextDevice) {
                if (p->NextDevice == DeviceObject) { p->NextDevice = DeviceObject->NextDevice; break; }
            }
        }
    }
    /* Unlink from the host's device list. */
    if (g_all_devices == d) {
        g_all_devices = d->all_next;
    } else {
        for (dh_device_t *p = g_all_devices; p; p = p->all_next) {
            if (p->all_next == d) { p->all_next = d->all_next; break; }
        }
    }
    if (g_devices_live) g_devices_live--;
    dh_unlock();
    dh_free(d);
}

/* -------------------------------------------------------------------------- */
/* Symbolic links                                                             */
/* -------------------------------------------------------------------------- */
DRVCALL NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName)
{
    if (!SymbolicLinkName || !DeviceName) return STATUS_INVALID_PARAMETER;
    char link[DH_NAME_MAX], target[DH_NAME_MAX];
    dh_ascii_from_unicode(SymbolicLinkName, link, sizeof link);
    dh_ascii_from_unicode(DeviceName, target, sizeof target);
    NTSTATUS st = STATUS_SUCCESS;
    dh_lock();
    int slot = -1;
    for (int i = 0; i < DH_MAX_SYMLINKS; i++) {
        if (g_symlinks[i].used && ascii_ieq(g_symlinks[i].link, link)) { slot = -2; break; }
        if (!g_symlinks[i].used && slot == -1) slot = i;
    }
    if (slot == -2) st = STATUS_OBJECT_NAME_COLLISION;
    else if (slot < 0) st = STATUS_INSUFFICIENT_RESOURCES;
    else {
        strncpy(g_symlinks[slot].link, link, sizeof g_symlinks[slot].link - 1);
        strncpy(g_symlinks[slot].target, target, sizeof g_symlinks[slot].target - 1);
        g_symlinks[slot].used = 1;
    }
    dh_unlock();
    return st;
}

DRVCALL NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING SymbolicLinkName)
{
    if (!SymbolicLinkName) return STATUS_INVALID_PARAMETER;
    char link[DH_NAME_MAX];
    dh_ascii_from_unicode(SymbolicLinkName, link, sizeof link);
    NTSTATUS st = STATUS_OBJECT_NAME_NOT_FOUND;
    dh_lock();
    for (int i = 0; i < DH_MAX_SYMLINKS; i++) {
        if (g_symlinks[i].used && ascii_ieq(g_symlinks[i].link, link)) {
            g_symlinks[i].used = 0;
            st = STATUS_SUCCESS;
            break;
        }
    }
    dh_unlock();
    return st;
}

/* -------------------------------------------------------------------------- */
/* IRPs                                                                       */
/* -------------------------------------------------------------------------- */
PIO_STACK_LOCATION dh_irp_current(PIRP irp)
{
    return irp->Tail.Overlay.CurrentStackLocation;
}

static PIO_STACK_LOCATION irp_locations(PIRP irp)
{
    return (PIO_STACK_LOCATION)(irp + 1);
}

void dh_irp_set_stack(PIRP irp, CCHAR location_count)
{
    irp->StackCount = location_count;
    irp->CurrentLocation = (CHAR)(location_count + 1);
    irp->Tail.Overlay.CurrentStackLocation = irp_locations(irp) + location_count;
}

DRVCALL PIRP IoAllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota)
{
    (void)ChargeQuota;
    if (StackSize < 1) return NULL;
    size_t bytes = sizeof(IRP) + (size_t)StackSize * sizeof(IO_STACK_LOCATION);
    PIRP irp = dh_alloc(bytes);
    if (!irp) return NULL;
    irp->Type = 6;                                   /* IO_TYPE_IRP */
    irp->Size = (USHORT)sizeof(IRP);
    irp->RequestorMode = KernelMode;
    dh_irp_set_stack(irp, StackSize);
    dh_lock();
    g_irps_live++;
    dh_unlock();
    return irp;
}

DRVCALL void IoFreeIrp(PIRP Irp)
{
    if (!Irp) return;
    if ((Irp->Flags & DH_IRP_OWNS_SYSBUF) && Irp->AssociatedIrp.SystemBuffer)
        dh_free(Irp->AssociatedIrp.SystemBuffer);
    dh_lock();
    if (g_irps_live) g_irps_live--;
    dh_unlock();
    dh_free(Irp);
}

DRVCALL NTSTATUS dh_invalid_device_request(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    (void)DeviceObject;
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS call_driver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    if (!DeviceObject || !Irp) return STATUS_INVALID_PARAMETER;
    if (Irp->CurrentLocation <= 1) dh_bugcheck(DH_BUGCHECK_BAD_IRP, (ULONG_PTR)Irp, 1, 0, 0);
    Irp->CurrentLocation--;
    Irp->Tail.Overlay.CurrentStackLocation--;
    PIO_STACK_LOCATION sl = Irp->Tail.Overlay.CurrentStackLocation;
    sl->DeviceObject = DeviceObject;
    PDRIVER_OBJECT drv = DeviceObject->DriverObject;
    PDRIVER_DISPATCH fn = NULL;
    if (drv && sl->MajorFunction <= IRP_MJ_MAXIMUM_FUNCTION)
        fn = drv->MajorFunction[sl->MajorFunction];
    if (!fn) fn = dh_invalid_device_request;
    return fn(DeviceObject, Irp);
}

DRVCALL NTSTATUS IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    return call_driver(DeviceObject, Irp);
}

DRVCALL NTSTATUS IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    return call_driver(DeviceObject, Irp);
}

static int completion_should_run(const IO_STACK_LOCATION *sl, const IRP *irp)
{
    NTSTATUS st = irp->IoStatus.Status;
    if (NT_SUCCESS(st) && (sl->Control & SL_INVOKE_ON_SUCCESS)) return 1;
    if (!NT_SUCCESS(st) && (sl->Control & SL_INVOKE_ON_ERROR)) return 1;
    if (irp->Cancel && (sl->Control & SL_INVOKE_ON_CANCEL)) return 1;
    return 0;
}

static void complete_irp(PIRP Irp, CCHAR PriorityBoost)
{
    (void)PriorityBoost;
    for (;;) {
        if (Irp->CurrentLocation > Irp->StackCount) break;
        PIO_STACK_LOCATION sl = Irp->Tail.Overlay.CurrentStackLocation;
        /* Move to the upper location; its device is what a completion routine gets. */
        Irp->CurrentLocation++;
        Irp->Tail.Overlay.CurrentStackLocation++;
        if (sl->CompletionRoutine && completion_should_run(sl, Irp)) {
            PDEVICE_OBJECT upper = (Irp->CurrentLocation <= Irp->StackCount)
                                       ? Irp->Tail.Overlay.CurrentStackLocation->DeviceObject
                                       : NULL;
            NTSTATUS r = sl->CompletionRoutine(upper, Irp, sl->Context);
            if (r == STATUS_MORE_PROCESSING_REQUIRED) return;
        }
    }
    if (Irp->UserIosb) *Irp->UserIosb = Irp->IoStatus;
    if (Irp->UserEvent) KeSetEvent(Irp->UserEvent, 0, FALSE);
}

DRVCALL void IoCompleteRequest(PIRP Irp, CCHAR PriorityBoost)
{
    complete_irp(Irp, PriorityBoost);
}

DRVCALL void IofCompleteRequest(PIRP Irp, CCHAR PriorityBoost)
{
    complete_irp(Irp, PriorityBoost);
}

DRVCALL void IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine, PVOID Context,
                                    BOOLEAN InvokeOnSuccess, BOOLEAN InvokeOnError,
                                    BOOLEAN InvokeOnCancel)
{
    if (Irp->CurrentLocation <= 1) return;
    PIO_STACK_LOCATION next = Irp->Tail.Overlay.CurrentStackLocation - 1;
    next->CompletionRoutine = Routine;
    next->Context = Context;
    next->Control = 0;
    if (Routine) {
        if (InvokeOnSuccess) next->Control |= SL_INVOKE_ON_SUCCESS;
        if (InvokeOnError)   next->Control |= SL_INVOKE_ON_ERROR;
        if (InvokeOnCancel)  next->Control |= SL_INVOKE_ON_CANCEL;
    }
}

DRVCALL PIRP IoBuildDeviceIoControlRequest(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject,
                                           PVOID InputBuffer, ULONG InputBufferLength,
                                           PVOID OutputBuffer, ULONG OutputBufferLength,
                                           BOOLEAN InternalDeviceIoControl, PKEVENT Event,
                                           PIO_STATUS_BLOCK IoStatusBlock)
{
    if (!DeviceObject) return NULL;
    CCHAR stack = DeviceObject->StackSize ? DeviceObject->StackSize : 1;
    PIRP irp = IoAllocateIrp(stack, FALSE);
    if (!irp) return NULL;
    PIO_STACK_LOCATION next = irp->Tail.Overlay.CurrentStackLocation - 1;
    next->MajorFunction = InternalDeviceIoControl ? IRP_MJ_INTERNAL_DEVICE_CONTROL : IRP_MJ_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    next->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
    next->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;
    if (METHOD_FROM_CTL_CODE(IoControlCode) == METHOD_BUFFERED) {
        ULONG n = InputBufferLength > OutputBufferLength ? InputBufferLength : OutputBufferLength;
        if (n) {
            irp->AssociatedIrp.SystemBuffer = dh_alloc(n);
            if (!irp->AssociatedIrp.SystemBuffer) { IoFreeIrp(irp); return NULL; }
            irp->Flags |= DH_IRP_OWNS_SYSBUF;
            if (InputBuffer && InputBufferLength)
                memcpy(irp->AssociatedIrp.SystemBuffer, InputBuffer, InputBufferLength);
        }
    } else {
        next->Parameters.DeviceIoControl.Type3InputBuffer = InputBuffer;
        irp->UserBuffer = OutputBuffer;
    }
    irp->UserEvent = Event;
    irp->UserIosb = IoStatusBlock;
    return irp;
}

DRVCALL BOOLEAN IoCancelIrp(PIRP Irp)
{
    if (!Irp || !Irp->CancelRoutine) return FALSE;
    Irp->Cancel = TRUE;
    PDRIVER_CANCEL routine = Irp->CancelRoutine;
    Irp->CancelRoutine = NULL;
    PDEVICE_OBJECT dev = Irp->Tail.Overlay.CurrentStackLocation->DeviceObject;
    routine(dev, Irp);
    return TRUE;
}

/* -------------------------------------------------------------------------- */
/* IoTimers (fired by the worker thread once per second)                      */
/* -------------------------------------------------------------------------- */
DRVCALL void IoInitializeTimer(PDEVICE_OBJECT DeviceObject, PIO_TIMER_ROUTINE TimerRoutine,
                               PVOID Context)
{
    dh_device_t *d = DH_FROM_DEV(DeviceObject);
    dh_lock();
    d->io_timer_routine = TimerRoutine;
    d->io_timer_ctx = Context;
    d->io_timer_active = 0;
    dh_unlock();
}

DRVCALL void IoStartTimer(PDEVICE_OBJECT DeviceObject)
{
    dh_device_t *d = DH_FROM_DEV(DeviceObject);
    dh_lock();
    if (d->io_timer_routine) {
        d->io_timer_active = 1;
        d->io_timer_next_ns = dh_now_ns() + DH_IO_TIMER_PERIOD_NS;
        dh_notify_all();
    }
    dh_unlock();
}

DRVCALL void IoStopTimer(PDEVICE_OBJECT DeviceObject)
{
    dh_device_t *d = DH_FROM_DEV(DeviceObject);
    dh_lock();
    d->io_timer_active = 0;
    dh_unlock();
}

/* Collects IoTimers whose period has elapsed.  Caller holds the monitor. */
int dh_io_timer_collect(uint64_t now, dh_device_t **out, int cap)
{
    int n = 0;
    for (dh_device_t *d = g_all_devices; d && n < cap; d = d->all_next) {
        if (d->io_timer_active && d->io_timer_next_ns <= now) {
            d->io_timer_next_ns = now + DH_IO_TIMER_PERIOD_NS;
            d->io_timer_inflight++;      /* IoDeleteDevice waits for this to drop */
            out[n++] = d;
        }
    }
    return n;
}

/* Earliest IoTimer deadline, or 0.  Caller holds the monitor. */
uint64_t dh_io_timer_next_deadline_locked(void)
{
    uint64_t best = 0;
    for (dh_device_t *d = g_all_devices; d; d = d->all_next) {
        if (d->io_timer_active && (best == 0 || d->io_timer_next_ns < best))
            best = d->io_timer_next_ns;
    }
    return best;
}

/* Used by drvhost_shutdown and tests: how many devices are still registered. */
ULONG dh_devices_registered(void)
{
    ULONG n = 0;
    dh_lock();
    for (dh_device_t *d = g_all_devices; d; d = d->all_next) n++;
    dh_unlock();
    return n;
}

/* Removes every symbolic link (shutdown cleanup of a stopped host). */
void dh_symlinks_clear(void)
{
    dh_lock();
    for (int i = 0; i < DH_MAX_SYMLINKS; i++) g_symlinks[i].used = 0;
    dh_unlock();
}
