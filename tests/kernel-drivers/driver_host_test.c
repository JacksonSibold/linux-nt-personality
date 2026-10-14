/* driver_host_test.c - end-to-end test: loads hello_drv.sys through the service
 * path, runs the PnP lifecycle, drives every IOCTL method through the NT and
 * Win32 entry points, then unloads.  It cycles load/unload twice to prove the
 * unload path leaves no devices, IRPs or images behind.  Also checks the three
 * loader failures: missing service, missing file, and a malformed image.
 *
 * usage: driver_host_test <sysroot>   (sysroot/drivers/ holds the test images)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>
#include <time.h>

#include "dh_test.h"
#include "drvhost.h"

/* Mirrors hello_drv/hello_drv.c; kept as literals so the test is independent of
 * the driver's own header. */
#define IOCTL_HELLO_PING    0x80002000u
#define IOCTL_HELLO_ECHO    0x80002004u
#define IOCTL_HELLO_ADD     0x80002008u
#define IOCTL_HELLO_DEFER   0x8000200Cu
#define IOCTL_HELLO_STATS   0x80002010u
#define IOCTL_HELLO_NEITHER 0x80002017u
#define IOCTL_HELLO_UNKNOWN 0x800023FCu

struct hello_stats_mirror {
    ULONG magic, opens, closes, pnp_starts, power_d0, dpc_runs, deferred_done, io_ticks, device_power, pnp_removes;
};

void test_registry_boot(void);   /* reg_boot.c */

static char g_cap[16384];
static size_t g_cap_len;

static void capture_sink(void *ctx, const char *text, SIZE_T len)
{
    (void)ctx;
    for (SIZE_T i = 0; i < len && g_cap_len + 1 < sizeof g_cap; i++) g_cap[g_cap_len++] = text[i];
    g_cap[g_cap_len] = 0;
}

static void write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(data, f);
    fclose(f);
}

static UNICODE_STRING ustr(const char16_t *lit)
{
    UNICODE_STRING u;
    RtlInitUnicodeString(&u, (PCWSTR)lit);
    return u;
}

static void run_cycle(const char *sysroot, int cycle)
{
    (void)sysroot;
    UNICODE_STRING key = ustr(u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\HelloDrv");

    g_cap_len = 0;
    g_cap[0] = 0;
    CHECK_STATUS(NtLoadDriver(&key), STATUS_SUCCESS);
    CHECK(strstr(g_cap, "HelloDrv: DriverEntry registry=\\Registry\\Machine\\System\\CurrentControlSet\\Services\\HelloDrv") != NULL);

    PDEVICE_OBJECT pdo = NULL;
    CHECK_STATUS(drvhost_pnp_enumerate("HelloDrv", "HelloPdo0", &pdo), STATUS_SUCCESS);
    CHECK(pdo != NULL);

    HANDLE h = CreateFileA("\\\\.\\HelloDrv", 0xC0000000u, 0, NULL, 3, 0, NULL);
    CHECK(h != (HANDLE)(intptr_t)-1);

    unsigned char out[256];
    ULONG ret = 0;

    /* PING: magic and version. */
    memset(out, 0, sizeof out);
    CHECK(DeviceIoControl(h, IOCTL_HELLO_PING, NULL, 0, out, 8, &ret, NULL) == 1);
    CHECK(ret == 8);
    CHECK(memcmp(out, "HELO", 4) == 0);
    CHECK(out[4] == 0 && out[5] == 0 && out[6] == 1 && out[7] == 0);

    /* ECHO: round trip of a payload. */
    const char payload[] = "hello world";
    memset(out, 0, sizeof out);
    CHECK(DeviceIoControl(h, IOCTL_HELLO_ECHO, (void *)payload, 11, out, 11, &ret, NULL) == 1);
    CHECK(ret == 11);
    CHECK(memcmp(out, payload, 11) == 0);

    /* ADD: u32 + u32 into u64. */
    ULONG addin[2] = { 7, 35 };
    unsigned long long sum = 0;
    CHECK(DeviceIoControl(h, IOCTL_HELLO_ADD, addin, 8, &sum, 8, &ret, NULL) == 1);
    CHECK(sum == 42);
    CHECK(ret == 8);

    /* ADD with a wrong input size fails with STATUS_INVALID_PARAMETER. */
    IO_STATUS_BLOCK iosb;
    CHECK_STATUS(NtDeviceIoControlFile(h, NULL, NULL, NULL, &iosb, IOCTL_HELLO_ADD, addin, 4, &sum, 8),
                 STATUS_INVALID_PARAMETER);

    /* DEFER: the IRP pends until the system thread queues a DPC that completes it. */
    ULONG seq = 0;
    CHECK(DeviceIoControl(h, IOCTL_HELLO_DEFER, NULL, 0, &seq, 4, &ret, NULL) == 1);
    CHECK(seq == 1);
    CHECK(ret == 4);
    CHECK(DeviceIoControl(h, IOCTL_HELLO_DEFER, NULL, 0, &seq, 4, &ret, NULL) == 1);
    CHECK(seq == 2);

    /* NEITHER: the driver reads the caller's buffers directly. */
    const char nei[] = "abcdef";
    memset(out, 0, sizeof out);
    CHECK(DeviceIoControl(h, IOCTL_HELLO_NEITHER, (void *)nei, 6, out, 6, &ret, NULL) == 1);
    CHECK(ret == 6);
    CHECK(memcmp(out, "abcdef", 6) == 0);

    /* An unknown code completes with STATUS_INVALID_DEVICE_REQUEST. */
    CHECK_STATUS(NtDeviceIoControlFile(h, NULL, NULL, NULL, &iosb, IOCTL_HELLO_UNKNOWN, NULL, 0, NULL, 0),
                 STATUS_INVALID_DEVICE_REQUEST);

    /* STATS: counters reflect the work above. */
    struct hello_stats_mirror s;
    memset(&s, 0, sizeof s);
    CHECK(DeviceIoControl(h, IOCTL_HELLO_STATS, NULL, 0, &s, sizeof s, &ret, NULL) == 1);
    CHECK(ret == sizeof s);
    CHECK(s.magic == 0x4F4C4548u);
    CHECK(s.opens >= 1);
    CHECK(s.pnp_starts == 1);
    CHECK(s.power_d0 == 1);
    CHECK(s.device_power == 1);
    CHECK(s.deferred_done == 2);
    CHECK(s.dpc_runs >= 2);

    /* The IoTimer fires once per second; wait up to 3 s for the first tick. */
    if (cycle == 0) {
        for (int i = 0; i < 150; i++) {
            if (DeviceIoControl(h, IOCTL_HELLO_STATS, NULL, 0, &s, sizeof s, &ret, NULL) != 1) break;
            if (s.io_ticks >= 1) break;
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        CHECK(s.io_ticks >= 1);
    }

    CHECK(CloseHandle(h) == 1);
    CHECK_STATUS(NtDeviceIoControlFile(h, NULL, NULL, NULL, &iosb, IOCTL_HELLO_PING, NULL, 0, NULL, 0),
                 STATUS_INVALID_HANDLE);

    /* PnP remove, then unload. */
    CHECK_STATUS(drvhost_pnp_remove("HelloDrv", pdo), STATUS_SUCCESS);
    drvhost_stats_t mid;
    drvhost_get_stats(&mid);
    CHECK(mid.devices_live == 0);
    CHECK_STATUS(NtUnloadDriver(&key), STATUS_SUCCESS);
    drvhost_stats_t after;
    drvhost_get_stats(&after);
    CHECK(after.devices_live == 0);
    CHECK(after.irps_live == 0);
    CHECK(after.images_loaded == 0);
    CHECK(after.pool_outstanding == 0);
}

int main(int argc, char **argv)
{
    const char *sysroot = argc > 1 ? argv[1] : "build/sysroot";
    test_registry_boot();
    CHECK_STATUS(drvhost_init(), 0);
    drvhost_set_system_root(sysroot);
    drvhost_set_dbg_sink(capture_sink, NULL);

    CHECK_STATUS(drvhost_service_add("HelloDrv", "\\SystemRoot\\drivers\\hello_drv.sys", 1, 3), 0);
    CHECK_STATUS(drvhost_service_add("BadImport", "\\SystemRoot\\drivers\\bad_import.sys", 1, 3), 0);
    CHECK_STATUS(drvhost_service_add("Junk", "\\SystemRoot\\drivers\\junk.txt", 1, 3), 0);
    CHECK_STATUS(drvhost_service_add("Gone", "\\SystemRoot\\drivers\\gone.sys", 1, 3), 0);

    /* Malformed image: a text file is not a PE. */
    char junk[512];
    snprintf(junk, sizeof junk, "%s/drivers/junk.txt", sysroot);
    write_file(junk, "this is not a portable executable\n");

    run_cycle(sysroot, 0);
    run_cycle(sysroot, 1);

    /* Loader failure modes. */
    UNICODE_STRING key_bad = ustr(u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\BadImport");
    g_cap_len = 0;
    g_cap[0] = 0;
    CHECK_STATUS(NtLoadDriver(&key_bad), STATUS_PROCEDURE_NOT_FOUND);
    CHECK(strstr(g_cap, "unresolved import ntoskrnl.exe!NoSuchFunctionXyz") != NULL);

    UNICODE_STRING key_junk = ustr(u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Junk");
    CHECK_STATUS(NtLoadDriver(&key_junk), STATUS_INVALID_IMAGE_FORMAT);

    UNICODE_STRING key_gone = ustr(u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Gone");
    CHECK_STATUS(NtLoadDriver(&key_gone), STATUS_NO_SUCH_FILE);

    UNICODE_STRING key_none = ustr(u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\NotThere");
    CHECK_STATUS(NtLoadDriver(&key_none), STATUS_OBJECT_NAME_NOT_FOUND);

    drvhost_set_dbg_sink(NULL, NULL);
    drvhost_shutdown();
    DH_TEST_SUMMARY("driver_host_test");
}
