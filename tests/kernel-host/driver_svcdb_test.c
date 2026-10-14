/* driver_svcdb_test.c - the driver host's service database (drvhost_svcdb.c)
 * against the personality registry.  Covers register, lookup, overwrite,
 * malformed values, invalid names, removal, and a service key created by hand
 * with the wrong value type.
 *
 * Built and run by tests/kernel-host/Makefile (userspace kshim, no kernel boot).
 */
#include "registry.h"
#include "object.h"
#include "handle.h"
#include "kshim.h"
#include "../../src/kernel/nt/driver/drvhost_svcdb.h"
#include "../../src/ntdll/nt_types.h"
#include "../../src/ntdll/ntstatus.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; return; \
    } \
} while (0)

#define ASSERT_STATUS(expr, expect) do { \
    long _s = (long)(expr); \
    if (_s != (long)(expect)) { \
        fprintf(stderr, "FAIL %s:%d: expect 0x%08lx got 0x%08lx for %s\n", \
                __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_s, #expr); \
        g_fail++; return; \
    } \
} while (0)

#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while (0)

static void reset_state(void)
{
    ntreg_cleanup();
    ObCleanupObjectManager();
    NTSTATUS st = ObInitObjectManager();
    if (st != STATUS_SUCCESS) { fprintf(stderr, "ObInitObjectManager failed\n"); g_fail++; }
    ntreg_init();
    /* The registry calls use the current process for handles. */
    struct nt_eprocess *p = NULL;
    if (ObCreateProcessObject(&p, 4242, "driver_svcdb_test") == STATUS_SUCCESS && p)
        kshim_set_current_process(p);
}

static void reg_set_string(const char *service, const char *value, const char *s)
{
    /* Writes a REG_SZ under the service key without the svcdb helpers, to build
     * a record the helpers would never write. */
    char path[256];
    snprintf(path, sizeof path, "%s%s", DRVHOST_SVC_KEY_PREFIX, service);
    WCHAR pbuf[320], vbuf[32], dbuf[256];
    size_t n = strlen(path);
    for (size_t i = 0; i < n; i++) pbuf[i] = (WCHAR)(unsigned char)path[i];
    pbuf[n] = 0;
    UNICODE_STRING pu = { (USHORT)(n * 2), (USHORT)((n + 1) * 2), pbuf };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.ObjectName = &pu;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    HANDLE h = NULL;
    ULONG disp = 0;
    ntreg_open_key_trusted(&pu, KEY_ALL_ACCESS, 1, &h);
    size_t vn_len = strlen(value), dn = strlen(s);
    for (size_t i = 0; i < vn_len; i++) vbuf[i] = (WCHAR)(unsigned char)value[i];
    vbuf[vn_len] = 0;
    for (size_t i = 0; i < dn; i++) dbuf[i] = (WCHAR)(unsigned char)s[i];
    dbuf[dn] = 0;
    UNICODE_STRING vn = { (USHORT)(vn_len * 2), (USHORT)((vn_len + 1) * 2), vbuf };
    NtSetValueKey(h, &vn, 0, REG_SZ, dbuf, (ULONG)((dn + 1) * 2));
    ObCloseHandle(kshim_current_process(), h);
}

static void test_register_and_lookup(void)
{
    reset_state();
    ASSERT_STATUS(drvhost_svcdb_register("HelloDrv", "\\SystemRoot\\drivers\\hello_drv.sys", 1, 3), STATUS_SUCCESS);
    drvhost_svc_record_t r;
    memset(&r, 0, sizeof r);
    ASSERT_STATUS(drvhost_svcdb_lookup("HelloDrv", &r), STATUS_SUCCESS);
    ASSERT(strcmp(r.image_path, "\\SystemRoot\\drivers\\hello_drv.sys") == 0);
    ASSERT(r.type == 1);
    ASSERT(r.start == 3);
    /* Names are case-insensitive, as registry keys are. */
    memset(&r, 0, sizeof r);
    ASSERT_STATUS(drvhost_svcdb_lookup("helloDRV", &r), STATUS_SUCCESS);
    ASSERT(r.start == 3);
    PASS();
}

static void test_missing_service(void)
{
    reset_state();
    drvhost_svc_record_t r;
    ASSERT_STATUS(drvhost_svcdb_lookup("NoSuchService", &r), STATUS_OBJECT_NAME_NOT_FOUND);
    PASS();
}

static void test_overwrite_updates_record(void)
{
    reset_state();
    ASSERT_STATUS(drvhost_svcdb_register("Svc", "\\SystemRoot\\drivers\\a.sys", 1, 3), STATUS_SUCCESS);
    ASSERT_STATUS(drvhost_svcdb_register("Svc", "\\SystemRoot\\drivers\\bb.sys", 2, 0), STATUS_SUCCESS);
    drvhost_svc_record_t r;
    ASSERT_STATUS(drvhost_svcdb_lookup("Svc", &r), STATUS_SUCCESS);
    ASSERT(strcmp(r.image_path, "\\SystemRoot\\drivers\\bb.sys") == 0);
    ASSERT(r.type == 2);
    ASSERT(r.start == 0);
    PASS();
}

static void test_invalid_arguments(void)
{
    reset_state();
    ASSERT_STATUS(drvhost_svcdb_register("Bad\\Name", "x.sys", 1, 3), STATUS_OBJECT_NAME_INVALID);
    ASSERT_STATUS(drvhost_svcdb_register("", "x.sys", 1, 3), STATUS_OBJECT_NAME_INVALID);
    char longpath[300];
    memset(longpath, 'p', sizeof longpath - 1);
    longpath[sizeof longpath - 1] = 0;
    ASSERT_STATUS(drvhost_svcdb_register("Long", longpath, 1, 3), STATUS_INVALID_PARAMETER);
    drvhost_svc_record_t r;
    ASSERT_STATUS(drvhost_svcdb_lookup(NULL, &r), STATUS_INVALID_PARAMETER);
    PASS();
}

static void test_wrong_value_type_rejected(void)
{
    reset_state();
    /* Type stored as a string where a DWORD is required. */
    ASSERT_STATUS(drvhost_svcdb_register("Typed", "\\SystemRoot\\drivers\\t.sys", 1, 3), STATUS_SUCCESS);
    reg_set_string("Typed", "Start", "3");
    drvhost_svc_record_t r;
    ASSERT_STATUS(drvhost_svcdb_lookup("Typed", &r), STATUS_OBJECT_NAME_INVALID);
    PASS();
}

static void test_missing_image_path_rejected(void)
{
    reset_state();
    /* A key with only Type and Start: ImagePath is required. */
    char path[256];
    snprintf(path, sizeof path, "%s%s", DRVHOST_SVC_KEY_PREFIX, "NoImage");
    WCHAR pbuf[320];
    size_t n = strlen(path);
    for (size_t i = 0; i < n; i++) pbuf[i] = (WCHAR)(unsigned char)path[i];
    pbuf[n] = 0;
    UNICODE_STRING pu = { (USHORT)(n * 2), (USHORT)((n + 1) * 2), pbuf };
    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.ObjectName = &pu;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    HANDLE h = NULL;
    ULONG disp = 0;
    ntreg_open_key_trusted(&pu, KEY_ALL_ACCESS, 1, &h);
    ObCloseHandle(kshim_current_process(), h);
    drvhost_svc_record_t r;
    ASSERT_STATUS(drvhost_svcdb_lookup("NoImage", &r), STATUS_OBJECT_NAME_INVALID);
    PASS();
}

static void test_remove(void)
{
    reset_state();
    ASSERT_STATUS(drvhost_svcdb_register("Gone", "\\SystemRoot\\drivers\\g.sys", 1, 3), STATUS_SUCCESS);
    ASSERT_STATUS(drvhost_svcdb_remove("Gone"), STATUS_SUCCESS);
    drvhost_svc_record_t r;
    ASSERT_STATUS(drvhost_svcdb_lookup("Gone", &r), STATUS_OBJECT_NAME_NOT_FOUND);
    ASSERT_STATUS(drvhost_svcdb_remove("Gone"), STATUS_OBJECT_NAME_NOT_FOUND);
    PASS();
}

int main(void)
{
    printf("driver service database tests starting\n");
    test_register_and_lookup();
    test_missing_service();
    test_overwrite_updates_record();
    test_invalid_arguments();
    test_wrong_value_type_rejected();
    test_missing_image_path_rejected();
    test_remove();
    printf("driver service database tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
