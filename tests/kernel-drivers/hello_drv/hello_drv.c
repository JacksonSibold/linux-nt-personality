/* hello_drv.c - test driver for the NT driver host (tests/kernel-drivers).
 *
 * Built by MinGW as a native x64 PE (.sys) that imports from ntoskrnl.exe.  It
 * is a test fixture, not vendor code: it exists to exercise the host's
 * executive, I/O and PnP surface end to end.
 *
 * Interface (userspace drives it through DeviceIoControl):
 *   IOCTL_HELLO_PING    METHOD_BUFFERED  out: {magic 'HELO', version 0x00010000}
 *   IOCTL_HELLO_ECHO    METHOD_BUFFERED  in bytes copied back to out (min of both)
 *   IOCTL_HELLO_ADD     METHOD_BUFFERED  in: two u32, out: u64 sum
 *   IOCTL_HELLO_DEFER   METHOD_BUFFERED  pends the IRP; a system thread queues a
 *                                        DPC that completes it; out: u32 sequence
 *   IOCTL_HELLO_STATS   METHOD_BUFFERED  out: struct hello_stats
 *   IOCTL_HELLO_NEITHER METHOD_NEITHER   copies the input buffer to the output
 */
#include "drvhost_types.h"
#include "drvhost_ntos.h"

#define HELLO_TAG 'OLLH'
#define HELLO_MAGIC 0x4F4C4548u  /* "HELO" */

#define IOCTL_HELLO_PING    CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HELLO_ECHO    CTL_CODE(0x8000, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HELLO_ADD     CTL_CODE(0x8000, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HELLO_DEFER   CTL_CODE(0x8000, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HELLO_STATS   CTL_CODE(0x8000, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HELLO_NEITHER CTL_CODE(0x8000, 0x805, METHOD_NEITHER,  FILE_ANY_ACCESS)

struct hello_stats {
    ULONG magic;
    ULONG opens;
    ULONG closes;
    ULONG pnp_starts;
    ULONG power_d0;
    ULONG dpc_runs;
    ULONG deferred_done;
    ULONG io_ticks;
    ULONG device_power;
    ULONG pnp_removes;
};

typedef struct hello_ext {
    PDEVICE_OBJECT pdo;
    PDEVICE_OBJECT lower;
    KSPIN_LOCK     lock;
    LIST_ENTRY     pending;
    KEVENT         wake;
    KDPC           dpc;
    PVOID          worker;      /* DISPATCHER object of the system thread */
    volatile LONG  stop;
    volatile LONG  opens;
    volatile LONG  closes;
    volatile LONG  pnp_starts;
    volatile LONG  pnp_removes;
    volatile LONG  power_d0;
    volatile LONG  dpc_runs;
    volatile LONG  deferred_done;
    volatile LONG  io_ticks;
    ULONG          device_power;
    ULONG          sequence;
} HELLO_EXT;

static UNICODE_STRING g_dev_name;
static UNICODE_STRING g_link_name;

/* Doubly linked list helpers (the WDK inline ones are macros). */
static void init_list(PLIST_ENTRY h) { h->Flink = h; h->Blink = h; }
static int list_empty(PLIST_ENTRY h) { return h->Flink == h; }
static void insert_tail(PLIST_ENTRY h, PLIST_ENTRY e)
{
    e->Flink = h;
    e->Blink = h->Blink;
    h->Blink->Flink = e;
    h->Blink = e;
}
static PLIST_ENTRY remove_head(PLIST_ENTRY h)
{
    PLIST_ENTRY e = h->Flink;
    h->Flink = e->Flink;
    e->Flink->Blink = h;
    return e;
}

static DRIVER_OBJECT *g_driver;

static NTSTATUS complete(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, 0);
    return st;
}

/* DPC: runs on the host's executive worker at DISPATCH_LEVEL. */

static void DRVCALL dpc_complete(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    HELLO_EXT *ext = Context;
    PIRP irp = Arg1;
    (void)Dpc; (void)Arg2;
    __atomic_add_fetch(&ext->dpc_runs, 1, __ATOMIC_SEQ_CST);
    if (!irp) return;
    ULONG seq = ++ext->sequence;
    if (irp->AssociatedIrp.SystemBuffer)
        *(volatile ULONG *)irp->AssociatedIrp.SystemBuffer = seq;
    __atomic_add_fetch(&ext->deferred_done, 1, __ATOMIC_SEQ_CST);
    complete(irp, STATUS_SUCCESS, sizeof(ULONG));
}

/* System thread: waits for queued IRPs and hands each to the DPC. */
static void DRVCALL worker_main(PVOID StartContext)
{
    HELLO_EXT *ext = StartContext;
    for (;;) {
        KeWaitForSingleObject(&ext->wake, Executive, KernelMode, FALSE, NULL);
        if (ext->stop) break;
        for (;;) {
            KIRQL old;
            KeAcquireSpinLock(&ext->lock, &old);
            if (list_empty(&ext->pending)) { KeReleaseSpinLock(&ext->lock, old); break; }
            PLIST_ENTRY e = remove_head(&ext->pending);
            KeReleaseSpinLock(&ext->lock, old);
            PIRP irp = (PIRP)((char *)e - offsetof(IRP, Tail.Overlay.ListEntry));
            KeInsertQueueDpc(&ext->dpc, irp, NULL);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void DRVCALL io_tick(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    (void)DeviceObject;
    HELLO_EXT *ext = Context;
    __atomic_add_fetch(&ext->io_ticks, 1, __ATOMIC_SEQ_CST);
}

static NTSTATUS DRVCALL dispatch_create(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    HELLO_EXT *ext = DeviceObject->DeviceExtension;
    __atomic_add_fetch(&ext->opens, 1, __ATOMIC_SEQ_CST);
    return complete(Irp, STATUS_SUCCESS, 1);
}

static NTSTATUS DRVCALL dispatch_close(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    HELLO_EXT *ext = DeviceObject->DeviceExtension;
    __atomic_add_fetch(&ext->closes, 1, __ATOMIC_SEQ_CST);
    return complete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS DRVCALL dispatch_ioctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    HELLO_EXT *ext = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sl->Parameters.DeviceIoControl.IoControlCode;
    ULONG inlen = sl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outlen = sl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysbuf = Irp->AssociatedIrp.SystemBuffer;

    switch (code) {
    case IOCTL_HELLO_PING:
        if (outlen < 8) return complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        ((ULONG *)sysbuf)[0] = HELLO_MAGIC;
        ((ULONG *)sysbuf)[1] = 0x00010000u;
        return complete(Irp, STATUS_SUCCESS, 8);

    case IOCTL_HELLO_ECHO: {
        if (inlen == 0) return complete(Irp, STATUS_INVALID_PARAMETER, 0);
        ULONG n = inlen < outlen ? inlen : outlen;
        return complete(Irp, STATUS_SUCCESS, n);
    }

    case IOCTL_HELLO_ADD: {
        if (inlen != 8) return complete(Irp, STATUS_INVALID_PARAMETER, 0);
        if (outlen < 8) return complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        ULONG a = ((ULONG *)sysbuf)[0];
        ULONG b = ((ULONG *)sysbuf)[1];
        *(ULONGLONG *)sysbuf = (ULONGLONG)a + (ULONGLONG)b;
        return complete(Irp, STATUS_SUCCESS, 8);
    }

    case IOCTL_HELLO_DEFER: {
        if (outlen < sizeof(ULONG)) return complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        KIRQL old;
        Irp->IoStatus.Status = STATUS_PENDING;
        Irp->PendingReturned = TRUE;
        sl->Control |= SL_PENDING_RETURNED;
        KeAcquireSpinLock(&ext->lock, &old);
        insert_tail(&ext->pending, &Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&ext->lock, old);
        KeSetEvent(&ext->wake, 0, FALSE);
        return STATUS_PENDING;
    }

    case IOCTL_HELLO_STATS: {
        if (outlen < sizeof(struct hello_stats)) return complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        struct hello_stats *s = sysbuf;
        s->magic = HELLO_MAGIC;
        s->opens = (ULONG)ext->opens;
        s->closes = (ULONG)ext->closes;
        s->pnp_starts = (ULONG)ext->pnp_starts;
        s->power_d0 = (ULONG)ext->power_d0;
        s->dpc_runs = (ULONG)ext->dpc_runs;
        s->deferred_done = (ULONG)ext->deferred_done;
        s->io_ticks = (ULONG)ext->io_ticks;
        s->device_power = ext->device_power;
        s->pnp_removes = (ULONG)ext->pnp_removes;
        return complete(Irp, STATUS_SUCCESS, sizeof(struct hello_stats));
    }

    case IOCTL_HELLO_NEITHER: {
        PVOID in = sl->Parameters.DeviceIoControl.Type3InputBuffer;
        PVOID out = Irp->UserBuffer;
        if (!in || !out || inlen == 0) return complete(Irp, STATUS_INVALID_PARAMETER, 0);
        ULONG n = inlen < outlen ? inlen : outlen;
        const UCHAR *src = in;
        UCHAR *dst = out;
        for (ULONG i = 0; i < n; i++) dst[i] = src[i];
        return complete(Irp, STATUS_SUCCESS, n);
    }

    default:
        return complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

/* Completes every IRP still pending on the device with STATUS_CANCELLED.  The
 * list is taken one entry at a time under the lock, and each IRP is completed
 * outside it, because completion can run other driver code. */
static void cancel_pending(HELLO_EXT *ext)
{
    for (;;) {
        KIRQL old;
        KeAcquireSpinLock(&ext->lock, &old);
        if (list_empty(&ext->pending)) {
            KeReleaseSpinLock(&ext->lock, old);
            return;
        }
        PLIST_ENTRY e = remove_head(&ext->pending);
        KeReleaseSpinLock(&ext->lock, old);
        PIRP irp = (PIRP)((char *)e - offsetof(IRP, Tail.Overlay.ListEntry));
        complete(irp, STATUS_CANCELLED, 0);
    }
}

static NTSTATUS DRVCALL dispatch_pnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    HELLO_EXT *ext = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    switch (sl->MinorFunction) {
    case IRP_MN_START_DEVICE:
        __atomic_add_fetch(&ext->pnp_starts, 1, __ATOMIC_SEQ_CST);
        return complete(Irp, STATUS_SUCCESS, 0);
    case IRP_MN_REMOVE_DEVICE: {
        __atomic_add_fetch(&ext->pnp_removes, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&ext->stop, 1, __ATOMIC_SEQ_CST);
        KeSetEvent(&ext->wake, 0, FALSE);
        if (ext->worker) {
            KeWaitForSingleObject(ext->worker, Executive, KernelMode, FALSE, NULL);
            ObfDereferenceObject(ext->worker);
            ext->worker = NULL;
        }
        /* Stop the timer, drop the queued DPC, and wait out one that is running,
         * before the extension (which the DPC uses) can be freed. */
        IoStopTimer(DeviceObject);
        KeRemoveQueueDpc(&ext->dpc);
        KeFlushQueuedDpcs();
        cancel_pending(ext);
        IoDeleteSymbolicLink(&g_link_name);
        IoDetachDevice(ext->pdo);
        NTSTATUS st = complete(Irp, STATUS_SUCCESS, 0);
        IoDeleteDevice(DeviceObject);
        return st;
    }
    default:
        /* Unknown minors complete with the status the PnP manager set (not supported). */
        return complete(Irp, Irp->IoStatus.Status, Irp->IoStatus.Information);
    }
}

static NTSTATUS DRVCALL dispatch_power(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    HELLO_EXT *ext = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    if (sl->MinorFunction == IRP_MN_SET_POWER && sl->Parameters.Power.Type == DevicePowerState) {
        ext->device_power = (ULONG)sl->Parameters.Power.State;
        if (sl->Parameters.Power.State == PowerDeviceD0) __atomic_add_fetch(&ext->power_d0, 1, __ATOMIC_SEQ_CST);
    }
    PoStartNextPowerIrp(Irp);
    return complete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS DRVCALL add_device(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT fdo = NULL;
    NTSTATUS st = IoCreateDevice(DriverObject, sizeof(HELLO_EXT), &g_dev_name,
                                 FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
    if (!NT_SUCCESS(st)) return st;
    HELLO_EXT *ext = fdo->DeviceExtension;
    ext->pdo = PhysicalDeviceObject;
    KeInitializeSpinLock(&ext->lock);
    init_list(&ext->pending);
    KeInitializeEvent(&ext->wake, SynchronizationEvent, FALSE);
    KeInitializeDpc(&ext->dpc, dpc_complete, ext);

    st = IoCreateSymbolicLink(&g_link_name, &g_dev_name);
    if (!NT_SUCCESS(st)) { IoDeleteDevice(fdo); return st; }

    ext->lower = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (!ext->lower) {
        IoDeleteSymbolicLink(&g_link_name);
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }
    fdo->Flags |= DO_BUFFERED_IO;

    HANDLE th = NULL;
    st = PsCreateSystemThread(&th, 0, NULL, NULL, NULL, worker_main, ext);
    if (!NT_SUCCESS(st)) {
        IoDetachDevice(PhysicalDeviceObject);
        IoDeleteSymbolicLink(&g_link_name);
        IoDeleteDevice(fdo);
        return st;
    }
    st = ObReferenceObjectByHandle(th, 0, *PsThreadType, KernelMode, &ext->worker, NULL);
    ZwClose(th);
    if (!NT_SUCCESS(st)) { ext->worker = NULL; }

    IoInitializeTimer(fdo, io_tick, ext);
    IoStartTimer(fdo);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    DbgPrint("HelloDrv: AddDevice fdo=%p over pdo=%p\n", fdo, PhysicalDeviceObject);
    return STATUS_SUCCESS;
}

static void DRVCALL driver_unload(PDRIVER_OBJECT DriverObject)
{
    DbgPrint("HelloDrv: DriverUnload %wZ\n", &DriverObject->DriverName);
}

DRVCALL NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    g_driver = DriverObject;
    RtlInitUnicodeString(&g_dev_name, (PCWSTR)u"\\Device\\HelloDrv0");
    RtlInitUnicodeString(&g_link_name, (PCWSTR)u"\\DosDevices\\HelloDrv");
    DbgPrint("HelloDrv: DriverEntry registry=%wZ\n", RegistryPath);

    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) DriverObject->MajorFunction[i] = NULL;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = dispatch_create;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = dispatch_close;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_ioctl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = dispatch_power;
    DriverObject->DriverUnload = driver_unload;
    DriverObject->DriverExtension->AddDevice = add_device;
    return STATUS_SUCCESS;
}
