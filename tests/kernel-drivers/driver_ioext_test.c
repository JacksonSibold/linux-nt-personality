/* driver_ioext_test.c - unit tests for drvhost_ioext.c and the Rtl time and string additions: remove locks, the cancel
 * protocol, StartIo queueing, IRP re-initialization, named events, shutdown notification, processes and time fields. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dh_test.h"
#include "drvhost_int.h"

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- remove lock --------------------------------------------------------- */
static IO_REMOVE_LOCK g_rl;
static volatile int g_rl_done;

static void *rl_wait_thread(void *arg)
{
    (void)arg;
    IoReleaseRemoveLockAndWaitEx(&g_rl, (PVOID)1, sizeof g_rl);
    g_rl_done = 1;
    return NULL;
}

static void test_remove_lock(void)
{
    IoInitializeRemoveLockEx(&g_rl, 0x74736574, 0, 0, sizeof g_rl);
    CHECK(g_rl.Common.IoCount == 1);
    CHECK(!g_rl.Common.Removed);
    CHECK_STATUS(IoAcquireRemoveLockEx(&g_rl, (PVOID)1, "t", 1, sizeof g_rl), STATUS_SUCCESS);   /* the caller's */
    CHECK_STATUS(IoAcquireRemoveLockEx(&g_rl, (PVOID)2, "t", 2, sizeof g_rl), STATUS_SUCCESS);   /* an I/O in flight */
    CHECK(g_rl.Common.IoCount == 3);

    pthread_t th;
    g_rl_done = 0;
    pthread_create(&th, NULL, rl_wait_thread, NULL);
    sleep_ms(50);
    CHECK(g_rl.Common.Removed);
    CHECK(!g_rl_done);                                           /* the in-flight reference holds it */
    CHECK_STATUS(IoAcquireRemoveLockEx(&g_rl, (PVOID)3, "t", 3, sizeof g_rl), STATUS_DELETE_PENDING);
    CHECK(g_rl.Common.IoCount == 1);                             /* a refused acquire leaves no reference */
    IoReleaseRemoveLockEx(&g_rl, (PVOID)2, sizeof g_rl);
    pthread_join(th, NULL);
    CHECK(g_rl_done);
    CHECK(g_rl.Common.IoCount == 0);
    CHECK(KeReadStateEvent(&g_rl.Common.RemoveEvent) == 1);
}

/* ---- cancel and StartIo -------------------------------------------------- */
static int g_cancel_calls;
static KIRQL g_cancel_irql_seen;
static PIRP g_cancel_irp;

static DRVCALL void cancel_plain(PDEVICE_OBJECT dev, PIRP irp)
{
    g_cancel_calls++;
    g_cancel_irp = irp;
    g_cancel_irql_seen = KeGetCurrentIrql();
    IoReleaseCancelSpinLock(irp->CancelIrql);
}

static DRVCALL void cancel_dequeue(PDEVICE_OBJECT dev, PIRP irp)
{
    g_cancel_calls++;
    KeRemoveEntryDeviceQueue(&dev->DeviceQueue, &irp->Tail.Overlay.DeviceQueueEntry);
    IoReleaseCancelSpinLock(irp->CancelIrql);
    irp->IoStatus.Status = STATUS_CANCELLED;
}

static void test_cancel(void)
{
    PIRP irp = IoAllocateIrp(1, FALSE);
    CHECK(!IoCancelIrp(irp));                                    /* no routine */
    CHECK(irp->Cancel);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);                  /* the cancel lock was released */

    irp->Cancel = FALSE;
    CHECK(IoSetCancelRoutine(irp, cancel_plain) == NULL);
    g_cancel_calls = 0;
    CHECK(IoCancelIrp(irp));
    CHECK(g_cancel_calls == 1 && g_cancel_irp == irp);
    CHECK(g_cancel_irql_seen == DISPATCH_LEVEL);                 /* called with the lock held */
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    CHECK(irp->CancelRoutine == NULL);                           /* taken, so it cannot run twice */
    CHECK(!IoCancelIrp(irp));
    CHECK(g_cancel_calls == 1);
    IoFreeIrp(irp);
}

static DRIVER_OBJECT g_drv;
static PIRP g_started[8];
static int g_nstarted;

static DRVCALL void start_io(PDEVICE_OBJECT dev, PIRP irp)
{
    g_started[g_nstarted++] = irp;
}

static void test_start_packet(void)
{
    memset(&g_drv, 0, sizeof g_drv);
    g_drv.DriverStartIo = start_io;
    PDEVICE_OBJECT dev;
    CHECK_STATUS(IoCreateDevice(&g_drv, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev), STATUS_SUCCESS);

    PIRP a = IoAllocateIrp(1, FALSE), b = IoAllocateIrp(1, FALSE), c = IoAllocateIrp(1, FALSE);
    g_nstarted = 0;
    IoStartPacket(dev, a, NULL, NULL);
    CHECK(g_nstarted == 1 && g_started[0] == a);
    CHECK(dev->CurrentIrp == a);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);

    IoStartPacket(dev, b, NULL, NULL);                           /* busy: queued */
    IoStartPacket(dev, c, NULL, NULL);
    CHECK(g_nstarted == 1);

    IoStartNextPacket(dev, FALSE);
    CHECK(g_nstarted == 2 && g_started[1] == b && dev->CurrentIrp == b);
    IoStartNextPacket(dev, FALSE);
    CHECK(g_nstarted == 3 && g_started[2] == c && dev->CurrentIrp == c);
    IoStartNextPacket(dev, FALSE);                               /* empty: the device goes idle */
    CHECK(g_nstarted == 3 && dev->CurrentIrp == NULL);

    /* A cancelable packet that sits in the queue can be taken out by its cancel routine. */
    g_nstarted = 0; g_cancel_calls = 0;
    IoStartPacket(dev, a, NULL, NULL);
    ULONG k1 = 7;
    b->CurrentLocation--;                                        /* b is now "in" the device's stack location */
    b->Tail.Overlay.CurrentStackLocation--;
    b->Tail.Overlay.CurrentStackLocation->DeviceObject = dev;
    IoStartPacket(dev, b, &k1, cancel_dequeue);
    CHECK(g_nstarted == 1 && b->CancelRoutine == cancel_dequeue);
    CHECK(IoCancelIrp(b));
    CHECK(g_cancel_calls == 1);
    CHECK(KeGetCurrentIrql() == PASSIVE_LEVEL);
    IoStartNextPacket(dev, TRUE);                                /* nothing left */
    CHECK(dev->CurrentIrp == NULL && g_nstarted == 1);

    /* Keyed queue: the packet with the smallest key not below the requested one runs next. */
    g_nstarted = 0;
    ULONG ka = 5, kb = 3, kc = 9;
    IoStartPacket(dev, a, &ka, NULL);                            /* idle: starts at once */
    IoStartPacket(dev, b, &kb, NULL);
    IoStartPacket(dev, c, &kc, NULL);
    IoStartNextPacketByKey(dev, FALSE, 4);
    CHECK(g_nstarted == 2 && g_started[1] == c);                 /* b (3) is below 4; c (9) is the first at or above */
    IoStartNextPacketByKey(dev, FALSE, 4);
    CHECK(g_nstarted == 3 && g_started[2] == b);                 /* wraps to the lowest */
    IoStartNextPacket(dev, FALSE);
    CHECK(dev->CurrentIrp == NULL);

    IoFreeIrp(a); IoFreeIrp(b); IoFreeIrp(c);
    IoDeleteDevice(dev);
}

/* ---- IRP re-initialization ----------------------------------------------- */
static void test_irp_init(void)
{
    PIRP irp = IoAllocateIrp(3, FALSE);
    CHECK(irp->Size == sizeof(IRP) + 3 * sizeof(IO_STACK_LOCATION));
    CHECK(irp->StackCount == 3 && irp->CurrentLocation == 4);

    static unsigned char buf[sizeof(IRP) + 2 * sizeof(IO_STACK_LOCATION)] __attribute__((aligned(16)));
    memset(buf, 0xcc, sizeof buf);
    PIRP r = (PIRP)buf;
    IoInitializeIrp(r, sizeof buf, 2);
    CHECK(r->Type == 6 && r->Size == sizeof buf);
    CHECK(r->StackCount == 2 && r->CurrentLocation == 3);
    CHECK(r->Tail.Overlay.CurrentStackLocation == (PIO_STACK_LOCATION)(buf + sizeof buf));
    CHECK(r->IoStatus.Status == 0 && r->Cancel == FALSE && r->MdlAddress == NULL);
    CHECK(r->ThreadListEntry.Flink == &r->ThreadListEntry);

    r->AllocationFlags = 0x04;
    r->IoStatus.Information = 99;
    r->Cancel = TRUE;
    r->CurrentLocation = 1;
    IoReuseIrp(r, STATUS_PENDING);
    CHECK(r->IoStatus.Status == STATUS_PENDING && r->IoStatus.Information == 0);
    CHECK(r->AllocationFlags == 0x04);
    CHECK(r->StackCount == 2 && r->CurrentLocation == 3 && !r->Cancel);
    IoFreeIrp(irp);
}

/* ---- named events -------------------------------------------------------- */
static void test_named_events(void)
{
    UNICODE_STRING n1, n2, n3;
    RtlInitUnicodeString(&n1, (PCWSTR)u"\\BaseNamedObjects\\TestEvent");
    RtlInitUnicodeString(&n2, (PCWSTR)u"\\BASENAMEDOBJECTS\\testevent");
    RtlInitUnicodeString(&n3, (PCWSTR)u"\\BaseNamedObjects\\Other");
    /* RtlInitUnicodeString of a char16 literal needs the host's wide length; build by hand when it counts bytes. */
    HANDLE h1 = NULL, h2 = NULL, h3 = NULL;
    PKEVENT e1 = IoCreateNotificationEvent(&n1, &h1);
    PKEVENT e2 = IoCreateNotificationEvent(&n2, &h2);
    PKEVENT e3 = IoCreateSynchronizationEvent(&n3, &h3);
    CHECK(e1 && e2 && e3);
    CHECK(e1 == e2);                                              /* same name, any case */
    CHECK(e3 != e1);
    CHECK(h1 && h2 && h3 && h1 != h2);                            /* a new handle per call */
    CHECK(e1->Header.Type == NotificationEvent);
    CHECK(e3->Header.Type == SynchronizationEvent);
    CHECK(KeReadStateEvent(e1) == 1);                             /* created signalled */
    KeResetEvent(e1);
    CHECK(KeReadStateEvent(e2) == 0);
    CHECK_STATUS(ZwClose(h1), STATUS_SUCCESS);
    CHECK_STATUS(ZwClose(h2), STATUS_SUCCESS);
    CHECK_STATUS(ZwClose(h3), STATUS_SUCCESS);
    CHECK(IoCreateNotificationEvent(NULL, &h1) == NULL);
}

/* ---- shutdown notification ----------------------------------------------- */
static int g_shutdowns;
static DRVCALL NTSTATUS shutdown_dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    g_shutdowns++;
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, 0);
    return STATUS_SUCCESS;
}

static void test_shutdown(void)
{
    DRIVER_OBJECT drv;
    memset(&drv, 0, sizeof drv);
    drv.MajorFunction[IRP_MJ_SHUTDOWN] = shutdown_dispatch;
    PDEVICE_OBJECT d1, d2;
    CHECK_STATUS(IoCreateDevice(&drv, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &d1), STATUS_SUCCESS);
    CHECK_STATUS(IoCreateDevice(&drv, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &d2), STATUS_SUCCESS);
    CHECK_STATUS(IoRegisterShutdownNotification(d1), STATUS_SUCCESS);
    g_shutdowns = 0;
    dh_io_send_shutdown();
    CHECK(g_shutdowns == 1);                                      /* only the registered device */
    IoUnregisterShutdownNotification(d1);
    dh_io_send_shutdown();
    CHECK(g_shutdowns == 1);
    IoDeleteDevice(d1);
    IoDeleteDevice(d2);
}

/* ---- processes, threads and odds and ends -------------------------------- */
static void test_ps_misc(void)
{
    PVOID proc = NULL;
    CHECK(PsGetCurrentThread() == KeGetCurrentThread());
    CHECK(IoGetCurrentProcess() == PsGetCurrentProcess());
    CHECK_STATUS(PsLookupProcessByProcessId(PsGetCurrentProcessId(), &proc), STATUS_SUCCESS);
    CHECK(proc == PsGetCurrentProcess());
    CHECK(PsGetProcessId(proc) == PsGetCurrentProcessId());
    CHECK_STATUS(PsLookupProcessByProcessId((HANDLE)0x7ffffff0, &proc), STATUS_INVALID_CID);

    PIRP irp = IoAllocateIrp(1, FALSE);
    CHECK(IoGetRequestorProcess(irp) == NULL);
    irp->Tail.Overlay.Thread = KeGetCurrentThread();
    CHECK(IoGetRequestorProcess(irp) == PsGetCurrentProcess());
    CHECK(IoGetRequestorProcessId(irp) == PsGetCurrentProcessId());
    IoFreeIrp(irp);

    CHECK(IoIsWdmVersionAvailable(1, 0x30));
    CHECK(IoIsWdmVersionAvailable(6, 0x30));
    CHECK(!IoIsWdmVersionAvailable(6, 0x31));
    CHECK(!IoIsWdmVersionAvailable(7, 0));

    UUID a, b;
    CHECK_STATUS(ExUuidCreate(&a), STATUS_SUCCESS);
    CHECK_STATUS(ExUuidCreate(&b), STATUS_SUCCESS);
    CHECK(memcmp(&a, &b, sizeof a) != 0);
    CHECK((a.Data3 >> 12) == 4 && (a.Data4[0] & 0xc0) == 0x80);

    CHECK(KeQueryActiveProcessorCountEx(0) >= 1);
    CHECK(KeQueryActiveProcessorCountEx(1) == 0);
    KAFFINITY aff = 0;
    ULONG n = KeQueryActiveProcessorCount(&aff);
    CHECK(n >= 1 && (aff & 1));
    PROCESSOR_NUMBER pn;
    ULONG idx = KeGetCurrentProcessorNumberEx(&pn);
    CHECK(pn.Group == 0 && pn.Number == (UCHAR)idx);

    DRIVER_OBJECT drv;
    memset(&drv, 0, sizeof drv);
    PDEVICE_OBJECT dev;
    CHECK_STATUS(IoCreateDeviceSecure(&drv, 8, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, NULL, NULL, &dev), STATUS_SUCCESS);
    CHECK(dev->DeviceExtension != NULL);
    CHECK(IoGetLowerDeviceObject(dev) == NULL);
    IoDeleteDevice(dev);

    PVOID log = IoAllocateErrorLogEntry(&drv, 0x40);
    CHECK(log != NULL);
    IoWriteErrorLogEntry(log);
    CHECK(IoAllocateErrorLogEntry(&drv, 0x10) == NULL);           /* smaller than the packet header */
}

/* ---- Rtl additions ------------------------------------------------------- */
static void test_rtl(void)
{
    TIME_FIELDS tf;
    LARGE_INTEGER t;

    t.QuadPart = 0;
    RtlTimeToTimeFields(&t, &tf);
    CHECK(tf.Year == 1601 && tf.Month == 1 && tf.Day == 1 && tf.Hour == 0 && tf.Weekday == 1);

    t.QuadPart = 116444736000000000ll;                            /* the Unix epoch */
    RtlTimeToTimeFields(&t, &tf);
    CHECK(tf.Year == 1970 && tf.Month == 1 && tf.Day == 1 && tf.Weekday == 4);

    tf.Year = 2000; tf.Month = 2; tf.Day = 29; tf.Hour = 12; tf.Minute = 34; tf.Second = 56; tf.Milliseconds = 789;
    tf.Weekday = 0;
    CHECK(RtlTimeFieldsToTime(&tf, &t));
    TIME_FIELDS back;
    RtlTimeToTimeFields(&t, &back);
    CHECK(back.Year == 2000 && back.Month == 2 && back.Day == 29);
    CHECK(back.Hour == 12 && back.Minute == 34 && back.Second == 56 && back.Milliseconds == 789);
    CHECK(back.Weekday == 2);                                     /* Tuesday */

    tf.Year = 2100; tf.Month = 2; tf.Day = 29;                    /* 2100 is not a leap year */
    CHECK(!RtlTimeFieldsToTime(&tf, &t));
    tf.Year = 2000; tf.Month = 13; tf.Day = 1;
    CHECK(!RtlTimeFieldsToTime(&tf, &t));
    tf.Month = 4; tf.Day = 31;
    CHECK(!RtlTimeFieldsToTime(&tf, &t));
    tf.Day = 30; tf.Year = 1600;
    CHECK(!RtlTimeFieldsToTime(&tf, &t));

    /* Every day of two years round-trips through both conversions. */
    int bad = 0;
    for (int y = 1999; y <= 2001 && !bad; y++)
        for (int m = 1; m <= 12; m++)
            for (int d = 1; d <= 31; d++) {
                tf.Year = y; tf.Month = m; tf.Day = d; tf.Hour = 23; tf.Minute = 59; tf.Second = 59; tf.Milliseconds = 999;
                if (!RtlTimeFieldsToTime(&tf, &t)) continue;
                RtlTimeToTimeFields(&t, &back);
                if (back.Year != y || back.Month != m || back.Day != d || back.Second != 59) bad++;
            }
    CHECK(bad == 0);

    LARGE_INTEGER utc = { .QuadPart = 123456789 }, loc, utc2;
    ExSystemTimeToLocalTime(&utc, &loc);
    ExLocalTimeToSystemTime(&loc, &utc2);
    CHECK(utc2.QuadPart == utc.QuadPart);

    CHECK(RtlCompareMemory("abcdef", "abcxyz", 6) == 3);
    CHECK(RtlCompareMemory("abc", "abc", 3) == 3);
    CHECK(RtlCompareMemory("abc", "xbc", 0) == 0);

    CHECK(RtlUpcaseUnicodeChar('a') == 'A' && RtlUpcaseUnicodeChar('Z') == 'Z' && RtlUpcaseUnicodeChar('1') == '1');
    CHECK(RtlUpcaseUnicodeChar(0xe9) == 0xc9 && RtlUpcaseUnicodeChar(0xf7) == 0xf7);
    CHECK(RtlUpcaseUnicodeChar(0x3b1) == 0x391 && RtlUpcaseUnicodeChar(0x3c2) == 0x3a3);
    CHECK(RtlUpcaseUnicodeChar(0x430) == 0x410 && RtlUpcaseUnicodeChar(0x451) == 0x401);

    WCHAR buf[16];
    UNICODE_STRING us = { 0, sizeof buf, (PWSTR)buf };
    CHECK_STATUS(RtlAppendUnicodeToString(&us, (PCWSTR)u"Dev"), STATUS_SUCCESS);
    CHECK_STATUS(RtlAppendUnicodeToString(&us, (PCWSTR)u"ice"), STATUS_SUCCESS);
    CHECK(us.Length == 12 && buf[0] == 'D' && buf[5] == 'e' && buf[6] == 0);
    CHECK_STATUS(RtlAppendUnicodeToString(&us, (PCWSTR)u"01234567890"), STATUS_BUFFER_TOO_SMALL);
    CHECK(us.Length == 12);
}

int main(void)
{
    CHECK_STATUS(drvhost_init(), 0);
    test_remove_lock();
    test_cancel();
    test_start_packet();
    test_irp_init();
    test_named_events();
    test_shutdown();
    test_ps_misc();
    test_rtl();
    drvhost_shutdown();
    DH_TEST_SUMMARY("driver_ioext_test");
}
