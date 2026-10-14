/* driver_unit_test.c - unit tests for the executive, I/O, Rtl and debug surface
 * of the NT driver host.  No .sys image is loaded here; hello_drv covers that
 * in driver_host_test.c.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dh_test.h"
#include "drvhost_int.h"

/* ---- DbgPrint capture ---------------------------------------------------- */
static char g_cap[4096];
static size_t g_cap_len;

static void capture_sink(void *ctx, const char *text, SIZE_T len)
{
    (void)ctx;
    for (SIZE_T i = 0; i < len && g_cap_len + 1 < sizeof g_cap; i++) g_cap[g_cap_len++] = text[i];
    g_cap[g_cap_len] = 0;
}

static void cap_reset(void) { g_cap_len = 0; g_cap[0] = 0; }

/* ---- pool ---------------------------------------------------------------- */
static void test_pool(void)
{
    drvhost_stats_t a, b;
    drvhost_get_stats(&a);
    void *p = ExAllocatePoolWithTag(NonPagedPool, 100, 0x54534554u);
    CHECK(p != NULL);
    void *q = ExAllocatePool2(POOL_FLAG_PAGED, 4096, 0x54534554u);
    CHECK(q != NULL);
    drvhost_get_stats(&b);
    CHECK(b.pool_outstanding == a.pool_outstanding + 2);
    CHECK(b.pool_bytes == a.pool_bytes + 100 + 4096);
    /* The buffer is writable across its full length. */
    memset(q, 0xA5, 4096);
    CHECK(((unsigned char *)q)[4095] == 0xA5);
    ExFreePoolWithTag(p, 0x54534554u);
    ExFreePool(q);
    drvhost_get_stats(&b);
    CHECK(b.pool_outstanding == a.pool_outstanding);
    CHECK(b.pool_bytes == a.pool_bytes);
    CHECK(ExAllocatePoolWithTag(NonPagedPool, 0, 0x54534554u) == NULL);
}

/* ---- IRQL and spin locks ------------------------------------------------- */
static void test_irql_spinlock(void)
{
    KSPIN_LOCK lock;
    KIRQL old = 0xFF;
    KeInitializeSpinLock(&lock);
    CHECK(KeGetCurrentIrql() == 0);
    KeAcquireSpinLock(&lock, &old);
    CHECK(old == 0);
    CHECK(KeGetCurrentIrql() == 2);
    KeReleaseSpinLock(&lock, old);
    CHECK(KeGetCurrentIrql() == 0);
    CHECK(lock == 0);

    KIRQL prev = KeRaiseIrqlToDpcLevel();
    CHECK(prev == 0);
    KeAcquireSpinLockAtDpcLevel(&lock);
    KeReleaseSpinLockFromDpcLevel(&lock);
    KeLowerIrql(0);
    CHECK(KeGetCurrentIrql() == 0);

    FAST_MUTEX fm;
    ExInitializeFastMutex(&fm);
    ExAcquireFastMutex(&fm);
    CHECK(KeGetCurrentIrql() == 1);
    CHECK(ExTryToAcquireFastMutex(&fm) == FALSE);
    ExReleaseFastMutex(&fm);
    CHECK(KeGetCurrentIrql() == 0);
    CHECK(ExTryToAcquireFastMutex(&fm) == TRUE);
    ExReleaseFastMutex(&fm);
}

/* ---- events and waits ---------------------------------------------------- */
static void test_events(void)
{
    KEVENT sync, notif;
    KeInitializeEvent(&sync, SynchronizationEvent, FALSE);
    KeInitializeEvent(&notif, NotificationEvent, FALSE);

    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    CHECK_STATUS(KeWaitForSingleObject(&sync, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);

    CHECK(KeSetEvent(&sync, 0, FALSE) == 0);
    CHECK(KeSetEvent(&sync, 0, FALSE) == 1);      /* already signalled */
    CHECK_STATUS(KeWaitForSingleObject(&sync, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    /* Auto-reset: the first wait consumed the signal. */
    CHECK_STATUS(KeWaitForSingleObject(&sync, Executive, KernelMode, FALSE, &zero), STATUS_TIMEOUT);

    KeSetEvent(&notif, 0, FALSE);
    CHECK_STATUS(KeWaitForSingleObject(&notif, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    /* Manual-reset: the signal persists across waits. */
    CHECK_STATUS(KeWaitForSingleObject(&notif, Executive, KernelMode, FALSE, &zero), STATUS_SUCCESS);
    CHECK(KeResetEvent(&notif) == 1);
    CHECK(KeReadStateEvent(&notif) == 0);

    /* A relative 20 ms wait on an unsignalled event times out after roughly that long. */
    LARGE_INTEGER rel;
    rel.QuadPart = -200000;   /* 20 ms in 100 ns units */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK_STATUS(KeWaitForSingleObject(&sync, Executive, KernelMode, FALSE, &rel), STATUS_TIMEOUT);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ms >= 15);
}

/* ---- DPCs, timers, system threads --------------------------------------- */
static DRVCALL void set_event_dpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    (void)dpc; (void)a1; (void)a2;
    KeSetEvent((PKEVENT)ctx, 0, FALSE);
}

static DRVCALL void thread_body(PVOID ctx)
{
    KeSetEvent((PKEVENT)ctx, 0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void test_dpc_timer_thread(void)
{
    KEVENT done;
    KeInitializeEvent(&done, SynchronizationEvent, FALSE);

    /* DPC queued and run by the worker; a second insert of a queued DPC fails. */
    KDPC dpc;
    KeInitializeDpc(&dpc, set_event_dpc, &done);
    CHECK(KeInsertQueueDpc(&dpc, NULL, NULL) == TRUE);
    LARGE_INTEGER wait;
    wait.QuadPart = -10 * 1000 * 1000 * 5LL; /* 5 s */
    CHECK_STATUS(KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, &wait), STATUS_SUCCESS);

    /* Removing a DPC that is not queued reports FALSE.  (Removing a queued one
     * races with the worker, so it is not asserted here.) */
    KDPC idle;
    KeInitializeDpc(&idle, set_event_dpc, &done);
    CHECK(KeRemoveQueueDpc(&idle) == FALSE);

    /* A timer with a DPC: due in 10 ms, the DPC signals the event. */
    KTIMER timer;
    KeInitializeTimer(&timer);
    KEVENT tfired;
    KeInitializeEvent(&tfired, SynchronizationEvent, FALSE);
    KDPC tdpc;
    KeInitializeDpc(&tdpc, set_event_dpc, &tfired);
    LARGE_INTEGER due;
    due.QuadPart = -100000;  /* 10 ms relative */
    CHECK(KeSetTimer(&timer, due, &tdpc) == FALSE);
    CHECK(KeSetTimer(&timer, due, &tdpc) == TRUE);  /* re-arm reports it was pending */
    CHECK_STATUS(KeWaitForSingleObject(&tfired, Executive, KernelMode, FALSE, &wait), STATUS_SUCCESS);

    /* Cancel a timer that is armed for later. */
    LARGE_INTEGER later;
    later.QuadPart = -10 * 1000 * 1000 * 60LL;
    KeSetTimer(&timer, later, NULL);
    CHECK(KeCancelTimer(&timer) == TRUE);
    CHECK(KeCancelTimer(&timer) == FALSE);

    /* A system thread runs, signals, and exits; its thread object is waitable. */
    KEVENT tdone;
    KeInitializeEvent(&tdone, NotificationEvent, FALSE);
    HANDLE th = NULL;
    CHECK_STATUS(PsCreateSystemThread(&th, 0, NULL, NULL, NULL, thread_body, &tdone), STATUS_SUCCESS);
    CHECK_STATUS(KeWaitForSingleObject(&tdone, Executive, KernelMode, FALSE, &wait), STATUS_SUCCESS);
    PVOID tobj = NULL;
    CHECK_STATUS(ObReferenceObjectByHandle(th, 0, *PsThreadType, KernelMode, &tobj, NULL), STATUS_SUCCESS);
    CHECK(tobj != NULL);
    CHECK_STATUS(KeWaitForSingleObject(tobj, Executive, KernelMode, FALSE, &wait), STATUS_SUCCESS);
    ObfDereferenceObject(tobj);
    CHECK_STATUS(ZwClose(th), STATUS_SUCCESS);
    CHECK_STATUS(ZwClose(th), STATUS_INVALID_HANDLE);
}

/* ---- Rtl ----------------------------------------------------------------- */
static void test_rtl(void)
{
    UNICODE_STRING a, b, c;
    RtlInitUnicodeString(&a, (PCWSTR)u"HelloDrv");
    RtlInitUnicodeString(&b, (PCWSTR)u"hellodrv");
    CHECK(a.Length == 16);
    CHECK(a.MaximumLength == 18);
    CHECK(RtlEqualUnicodeString(&a, &b, FALSE) == FALSE);
    CHECK(RtlEqualUnicodeString(&a, &b, TRUE) == TRUE);
    CHECK(RtlCompareUnicodeString(&a, &b, TRUE) == 0);
    CHECK(RtlCompareUnicodeString(&a, &b, FALSE) < 0);

    WCHAR buf[8];
    c.Buffer = buf;
    c.Length = 0;
    c.MaximumLength = sizeof buf;
    CHECK_STATUS(RtlCopyUnicodeString(&c, &a), STATUS_SUCCESS);
    CHECK(c.Length == 16);   /* truncated to MaximumLength (8 chars) */
    CHECK(c.Buffer[7] == 'v');
    CHECK_STATUS(RtlAppendUnicodeStringToString(&c, &a), STATUS_BUFFER_TOO_SMALL);

    ANSI_STRING ansi;
    UNICODE_STRING back;
    RtlInitAnsiString(&ansi, "Abc");
    CHECK_STATUS(RtlAnsiStringToUnicodeString(&back, &ansi, TRUE), STATUS_SUCCESS);
    CHECK(back.Length == 6);
    CHECK(back.Buffer[0] == 'A' && back.Buffer[2] == 'c');
    RtlFreeUnicodeString(&back);
    CHECK(back.Buffer == NULL);

    RTL_OSVERSIONINFOW vi;
    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    CHECK_STATUS(RtlGetVersion(&vi), STATUS_SUCCESS);
    CHECK(vi.dwMajorVersion == 10);
    CHECK(vi.dwBuildNumber == 26200);
}

/* ---- DbgPrint formatting ------------------------------------------------- */
static void test_dbgprint(void)
{
    drvhost_set_dbg_sink(capture_sink, NULL);
    UNICODE_STRING w;
    RtlInitUnicodeString(&w, (PCWSTR)u"hi");
    ANSI_STRING a;
    RtlInitAnsiString(&a, "ansi");

    cap_reset();
    DbgPrint("n=%d s=%s x=%x w=%wZ\n", -5, "abc", 0xbeef, &w);
    CHECK(strcmp(g_cap, "n=-5 s=abc x=beef w=hi\n") == 0);

    cap_reset();
    DbgPrintEx(0, 0, "a=%u %Z %c%%\n", 42u, &a, 'Z');
    CHECK(strcmp(g_cap, "a=42 ansi Z%\n") == 0);

    cap_reset();
    DbgPrint("[%5d][%-4s][%08x][%I64x][%lu]\n", 7, "ab", 0x1234u, (unsigned long long)0xFEEDFACEULL, 9ul);
    CHECK(strcmp(g_cap, "[    7][ab  ][00001234][feedface][9]\n") == 0);

    cap_reset();
    DbgPrint("%S|%p\n", (const WCHAR *)w.Buffer, (void *)0x1000);
    CHECK(strncmp(g_cap, "hi|0x", 5) == 0 && strstr(g_cap, "1000") != NULL);

    drvhost_set_dbg_sink(NULL, NULL);
    CHECK(strcmp(drvhost_status_name(STATUS_SUCCESS), "STATUS_SUCCESS") == 0);
}

/* ---- IRP stack model: two levels, completion routine ---------------------- */
static DEVICE_OBJECT *g_seen_lower;
static DEVICE_OBJECT *g_seen_completion_dev;
static int g_completion_calls;

static DRVCALL NTSTATUS lower_dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    g_seen_lower = dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 42;
    IoCompleteRequest(irp, 0);
    return STATUS_SUCCESS;
}

static DRVCALL NTSTATUS completion_fn(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    (void)irp; (void)ctx;
    g_seen_completion_dev = dev;
    g_completion_calls++;
    return STATUS_SUCCESS;
}

static DEVICE_OBJECT *g_lower_of_upper;

/* The WDK forwarding pattern: copy the current location down, set a completion
 * routine on the lower location, then IoCallDriver (which moves down itself). */
static DRVCALL NTSTATUS upper_dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    PIO_STACK_LOCATION cur = IoGetCurrentIrpStackLocation(irp);
    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(irp);
    *next = *cur;
    next->Control = 0;
    IoSetCompletionRoutine(irp, completion_fn, NULL, TRUE, TRUE, TRUE);
    return IoCallDriver(g_lower_of_upper, irp);
}

static void test_irp_stack(void)
{
    /* Dispatch is per driver object, so each layer needs its own. */
    DRIVER_OBJECT drv_upper, drv_lower;
    memset(&drv_upper, 0, sizeof drv_upper);
    memset(&drv_lower, 0, sizeof drv_lower);
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        drv_upper.MajorFunction[i] = lower_dispatch;
        drv_lower.MajorFunction[i] = lower_dispatch;
    }
    drv_upper.MajorFunction[IRP_MJ_DEVICE_CONTROL] = upper_dispatch;

    DEVICE_OBJECT *upper = NULL, *lower = NULL;
    CHECK_STATUS(IoCreateDevice(&drv_upper, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &upper), STATUS_SUCCESS);
    CHECK_STATUS(IoCreateDevice(&drv_lower, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &lower), STATUS_SUCCESS);
    CHECK(IoAttachDeviceToDeviceStack(upper, lower) == lower);
    CHECK(upper->StackSize == 2);
    g_lower_of_upper = lower;

    PIRP irp = IoAllocateIrp(2, FALSE);
    CHECK(irp != NULL);
    PIO_STACK_LOCATION top = IoGetNextIrpStackLocation(irp);
    top->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    top->DeviceObject = upper;
    g_seen_lower = NULL;
    g_seen_completion_dev = NULL;
    g_completion_calls = 0;
    NTSTATUS st = IoCallDriver(upper, irp);
    CHECK_STATUS(st, STATUS_SUCCESS);
    CHECK(g_seen_lower == lower);
    CHECK(g_completion_calls == 1);
    CHECK(g_seen_completion_dev == upper);   /* routine receives the upper device */
    CHECK(irp->IoStatus.Information == 42);
    IoFreeIrp(irp);

    IoDetachDevice(lower);
    IoDeleteDevice(upper);
    IoDeleteDevice(lower);
}

/* ---- symbolic links and device names ------------------------------------ */
static void test_symlinks(void)
{
    DRIVER_OBJECT drv;
    memset(&drv, 0, sizeof drv);
    PDEVICE_OBJECT dev = NULL;
    UNICODE_STRING dn, ln;
    RtlInitUnicodeString(&dn, (PCWSTR)u"\\Device\\UnitDev");
    RtlInitUnicodeString(&ln, (PCWSTR)u"\\DosDevices\\UnitDev");
    CHECK_STATUS(IoCreateDevice(&drv, 16, &dn, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev), STATUS_SUCCESS);
    CHECK(dev->DeviceExtension != NULL);
    CHECK_STATUS(IoCreateDevice(&drv, 0, &dn, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev), STATUS_OBJECT_NAME_COLLISION);
    CHECK_STATUS(IoCreateSymbolicLink(&ln, &dn), STATUS_SUCCESS);
    CHECK_STATUS(IoCreateSymbolicLink(&ln, &dn), STATUS_OBJECT_NAME_COLLISION);
    char target[256];
    CHECK(dh_symlink_lookup("\\DosDevices\\UnitDev", target, sizeof target) == 1);
    CHECK(strcmp(target, "\\Device\\UnitDev") == 0);
    CHECK_STATUS(IoDeleteSymbolicLink(&ln), STATUS_SUCCESS);
    CHECK_STATUS(IoDeleteSymbolicLink(&ln), STATUS_OBJECT_NAME_NOT_FOUND);
    IoDeleteDevice(dev);
}

/* ---- exports and MmGetSystemRoutineAddress ------------------------------- */
static void test_exports(void)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, (PCWSTR)u"IoCreateDevice");
    CHECK(MmGetSystemRoutineAddress(&n) == (PVOID)&IoCreateDevice);
    RtlInitUnicodeString(&n, (PCWSTR)u"NoSuchRoutineAnywhere");
    CHECK(MmGetSystemRoutineAddress(&n) == NULL);
    CHECK(dh_host_export_module("hal.dll", "HalDisplayString", 16) == (void *)&HalDisplayString);
    CHECK(dh_host_export_module("ntoskrnl.exe", "HalDisplayString", 16) == NULL);
    CHECK(MmIsAddressValid((PVOID)&IoCreateDevice) == TRUE);
    CHECK(MmIsAddressValid(NULL) == FALSE);
}

/* ---- IoTimer: one-second period, fired by the worker --------------------- */
static volatile int g_io_timer_fired;
static DRVCALL void io_timer_fn(PDEVICE_OBJECT dev, PVOID ctx)
{
    (void)dev; (void)ctx;
    g_io_timer_fired++;
}

static void test_io_timer(void)
{
    DRIVER_OBJECT drv;
    memset(&drv, 0, sizeof drv);
    PDEVICE_OBJECT dev = NULL;
    CHECK_STATUS(IoCreateDevice(&drv, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev), STATUS_SUCCESS);
    IoInitializeTimer(dev, io_timer_fn, NULL);
    IoStartTimer(dev);
    for (int i = 0; i < 150 && g_io_timer_fired == 0; i++) usleep(20000); /* up to 3 s */
    IoStopTimer(dev);
    CHECK(g_io_timer_fired >= 1);
    IoDeleteDevice(dev);
}

/* ---- lifetime: flush waits for a running DPC; delete waits for an IoTimer ---- */
static volatile int g_slow_dpc_started, g_slow_dpc_done;
static DRVCALL void slow_dpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
    g_slow_dpc_started = 1;
    usleep(150000);
    g_slow_dpc_done = 1;
}

static void test_flush_waits_for_running_dpc(void)
{
    KDPC d;
    KeInitializeDpc(&d, slow_dpc, NULL);
    CHECK(KeInsertQueueDpc(&d, NULL, NULL) == TRUE);
    for (int i = 0; i < 500 && !g_slow_dpc_started; i++) usleep(1000);
    CHECK(g_slow_dpc_started == 1);
    KeFlushQueuedDpcs();
    CHECK(g_slow_dpc_done == 1);
}

static volatile int g_slow_timer_started, g_slow_timer_done;
static DRVCALL void slow_io_timer(PDEVICE_OBJECT dev, PVOID ctx)
{
    (void)dev; (void)ctx;
    g_slow_timer_started = 1;
    usleep(200000);
    g_slow_timer_done = 1;
}

static void test_delete_waits_for_io_timer(void)
{
    DRIVER_OBJECT drv;
    memset(&drv, 0, sizeof drv);
    PDEVICE_OBJECT dev = NULL;
    CHECK_STATUS(IoCreateDevice(&drv, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev), STATUS_SUCCESS);
    IoInitializeTimer(dev, slow_io_timer, NULL);
    IoStartTimer(dev);
    for (int i = 0; i < 300 && !g_slow_timer_started; i++) usleep(10000); /* up to 3 s */
    CHECK(g_slow_timer_started == 1);
    /* Delete while the routine is still running: it must return before the free. */
    IoDeleteDevice(dev);
    CHECK(g_slow_timer_done == 1);
}

int main(void)
{
    CHECK_STATUS(drvhost_init(), 0);
    test_pool();
    test_irql_spinlock();
    test_events();
    test_dpc_timer_thread();
    test_rtl();
    test_dbgprint();
    test_irp_stack();
    test_symlinks();
    test_exports();
    test_io_timer();
    test_flush_waits_for_running_dpc();
    test_delete_waits_for_io_timer();
    drvhost_shutdown();
    DH_TEST_SUMMARY("driver_unit_test");
}
