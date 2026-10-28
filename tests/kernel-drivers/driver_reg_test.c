/* driver_reg_test.c - the registry services of the NT driver host (drvhost_zwreg.c) against the personality registry:
 * the Zw key and value functions, and RtlQueryRegistryValues with its flags. */
#include <stdio.h>
#include <string.h>
#include <uchar.h>

#include "dh_test.h"
#include "drvhost_int.h"

void test_registry_boot(void);

#define W(s) ((PCWSTR)(s))
#define ROOT u"\\Registry\\Machine\\SOFTWARE\\NtPersTest"

static UNICODE_STRING us(const char16_t *s)
{
    UNICODE_STRING u;
    RtlInitUnicodeString(&u, (PCWSTR)s);
    return u;
}

static HANDLE open_abs(const char16_t *path, ACCESS_MASK acc, int create, ULONG *disp)
{
    UNICODE_STRING n = us(path);
    OBJECT_ATTRIBUTES oa = { sizeof oa, NULL, &n, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL };
    HANDLE h = NULL;
    NTSTATUS st = create ? ZwCreateKey(&h, acc, &oa, 0, NULL, 0, disp) : ZwOpenKey(&h, acc, &oa);
    return NT_SUCCESS(st) ? h : NULL;
}

static void set_sz(HANDLE k, const char16_t *name, const char16_t *v, ULONG type)
{
    UNICODE_STRING n = us(name);
    size_t bytes = (drv_wcslen((PCWSTR)v) + 1) * 2;
    CHECK_STATUS(ZwSetValueKey(k, &n, 0, type, (PVOID)v, (ULONG)bytes), STATUS_SUCCESS);
}

static void set_dword(HANDLE k, const char16_t *name, ULONG v)
{
    UNICODE_STRING n = us(name);
    CHECK_STATUS(ZwSetValueKey(k, &n, 0, REG_DWORD, &v, 4), STATUS_SUCCESS);
}

/* ---- Zw functions -------------------------------------------------------- */
static void test_zw(void)
{
    ULONG disp = 0;
    HANDLE k = open_abs(ROOT u"\\Zw", KEY_ALL_ACCESS, 1, &disp);
    CHECK(k != NULL);
    CHECK(disp == REG_CREATED_NEW_KEY);
    HANDLE k2 = open_abs(ROOT u"\\zw", KEY_READ, 1, &disp);        /* same key, any case */
    CHECK(k2 != NULL && disp == REG_OPENED_EXISTING_KEY);
    CHECK(k != k2);
    CHECK_STATUS(ZwClose(k2), STATUS_SUCCESS);
    CHECK(open_abs(ROOT u"\\NoSuchKey", KEY_READ, 0, NULL) == NULL);

    set_sz(k, u"Name", u"Disk", REG_SZ);
    set_dword(k, u"Start", 3);
    set_sz(k, u"Path", u"%SystemRoot%\\x.sys", REG_EXPAND_SZ);

    ULONG rl = 0;
    UNICODE_STRING n = us(u"Start");
    union { KEY_VALUE_PARTIAL_INFORMATION p; char raw[128]; } buf;
    CHECK_STATUS(ZwQueryValueKey(k, &n, KeyValuePartialInformation, &buf, sizeof buf, &rl), STATUS_SUCCESS);
    CHECK(buf.p.Type == REG_DWORD && buf.p.DataLength == 4 && *(ULONG *)buf.p.Data == 3);
    CHECK(rl == offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + 4);

    n = us(u"Name");
    union { KEY_VALUE_FULL_INFORMATION f; char raw[128]; } full;
    CHECK_STATUS(ZwQueryValueKey(k, &n, KeyValueFullInformation, &full, sizeof full, &rl), STATUS_SUCCESS);
    CHECK(full.f.Type == REG_SZ && full.f.NameLength == 8 && full.f.DataLength == 10);
    CHECK(memcmp((char *)&full + full.f.DataOffset, u"Disk", 10) == 0);

    n = us(u"Name");
    CHECK_STATUS(ZwQueryValueKey(k, &n, KeyValueFullInformation, &full, 8, &rl), STATUS_BUFFER_TOO_SMALL);
    n = us(u"Missing");
    CHECK_STATUS(ZwQueryValueKey(k, &n, KeyValuePartialInformation, &buf, sizeof buf, &rl), STATUS_OBJECT_NAME_NOT_FOUND);

    /* Enumeration reaches every value once, then ends. */
    int seen = 0;
    for (ULONG i = 0;; i++) {
        NTSTATUS st = ZwEnumerateValueKey(k, i, KeyValueBasicInformation, &buf, sizeof buf, &rl);
        if (st == STATUS_NO_MORE_ENTRIES) break;
        CHECK_STATUS(st, STATUS_SUCCESS);
        seen++;
        if (seen > 10) break;
    }
    CHECK(seen == 3);

    /* Subkeys, opened relative to a key handle. */
    HANDLE sub = open_abs(ROOT u"\\Zw\\Sub1", KEY_ALL_ACCESS, 1, NULL);
    CHECK(sub != NULL);
    ZwClose(sub);
    UNICODE_STRING rel = us(u"Sub2");
    OBJECT_ATTRIBUTES oa = { sizeof oa, k, &rel, OBJ_CASE_INSENSITIVE, NULL, NULL };
    HANDLE s2 = NULL;
    CHECK_STATUS(ZwCreateKey(&s2, KEY_ALL_ACCESS, &oa, 0, NULL, 0, &disp), STATUS_SUCCESS);
    CHECK(disp == REG_CREATED_NEW_KEY);
    set_dword(s2, u"V", 7);
    ZwClose(s2);
    CHECK(open_abs(ROOT u"\\Zw\\Sub2", KEY_READ, 0, NULL) != NULL);   /* the relative create landed where expected */

    int subs = 0;
    for (ULONG i = 0; i < 8; i++) {
        union { KEY_BASIC_INFORMATION b; char raw[128]; } kb;
        NTSTATUS st = ZwEnumerateKey(k, i, KeyBasicInformation, &kb, sizeof kb, &rl);
        if (st == STATUS_NO_MORE_ENTRIES) break;
        CHECK_STATUS(st, STATUS_SUCCESS);
        CHECK(kb.b.NameLength == 8);
        subs++;
    }
    CHECK(subs == 2);

    union { KEY_FULL_INFORMATION f; char raw[128]; } kf;
    CHECK_STATUS(ZwQueryKey(k, KeyFullInformation, &kf, sizeof kf, &rl), STATUS_SUCCESS);
    CHECK(kf.f.SubKeys == 2 && kf.f.Values == 3);

    n = us(u"Name");
    CHECK_STATUS(ZwDeleteValueKey(k, &n), STATUS_SUCCESS);
    CHECK_STATUS(ZwQueryValueKey(k, &n, KeyValuePartialInformation, &buf, sizeof buf, &rl), STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_STATUS(ZwFlushKey(k), STATUS_SUCCESS);

    CHECK_STATUS(ZwClose(k), STATUS_SUCCESS);
    CHECK_STATUS(ZwClose(k), STATUS_INVALID_HANDLE);              /* a closed handle is gone */
    CHECK_STATUS(ZwQueryValueKey(k, &n, KeyValuePartialInformation, &buf, sizeof buf, &rl), STATUS_INVALID_HANDLE);
    CHECK_STATUS(ZwFlushKey((HANDLE)0x1234), STATUS_INVALID_HANDLE);
}

/* ---- Rtl helpers --------------------------------------------------------- */
static void test_rtl_helpers(void)
{
    CHECK_STATUS(RtlCheckRegistryKey(RTL_REGISTRY_ABSOLUTE, (PWSTR)W(ROOT u"\\Helpers")), STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_STATUS(RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, (PWSTR)W(ROOT u"\\Helpers")), STATUS_SUCCESS);
    CHECK_STATUS(RtlCheckRegistryKey(RTL_REGISTRY_ABSOLUTE, (PWSTR)W(ROOT u"\\Helpers")), STATUS_SUCCESS);

    ULONG v = 0x1234;
    CHECK_STATUS(RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, W(ROOT u"\\Helpers"), W(u"Level"), REG_DWORD, &v, 4),
                 STATUS_SUCCESS);
    /* A relative path may start with a backslash. */
    CHECK_STATUS(RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, W(u"\\NtPersDrv\\Parameters"), W(u"Mode"), REG_DWORD, &v, 4),
                 STATUS_SUCCESS);
    CHECK_STATUS(RtlCheckRegistryKey(RTL_REGISTRY_ABSOLUTE,
                     (PWSTR)W(u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\NtPersDrv\\Parameters")),
                 STATUS_SUCCESS);
    CHECK_STATUS(RtlCheckRegistryKey(RTL_REGISTRY_CONTROL, (PWSTR)W(u"NoSuchControlKey")), STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK_STATUS(RtlCheckRegistryKey(77, (PWSTR)W(u"x")), STATUS_INVALID_PARAMETER);

    CHECK_STATUS(RtlDeleteRegistryValue(RTL_REGISTRY_ABSOLUTE, W(ROOT u"\\Helpers"), W(u"Level")), STATUS_SUCCESS);
    CHECK_STATUS(RtlDeleteRegistryValue(RTL_REGISTRY_ABSOLUTE, W(ROOT u"\\Helpers"), W(u"Level")),
                 STATUS_OBJECT_NAME_NOT_FOUND);
}

/* ---- RtlQueryRegistryValues ---------------------------------------------- */
typedef struct { int calls; WCHAR name[32]; ULONG type; ULONG len; char data[256]; PVOID ectx; } cb_rec_t;
static cb_rec_t g_cb[16];
static int g_ncb;

static DRVCALL NTSTATUS record_cb(PCWSTR name, ULONG type, PVOID data, ULONG len, PVOID ctx, PVOID ectx)
{
    cb_rec_t *r = &g_cb[g_ncb++ % 16];
    r->calls++;
    r->name[0] = 0;
    if (name) { size_t n = drv_wcslen(name); if (n > 31) n = 31; memcpy(r->name, name, n * 2); r->name[n] = 0; }
    r->type = type;
    r->len = len;
    if (data && len <= sizeof r->data) memcpy(r->data, data, len);
    r->ectx = ectx;
    return STATUS_SUCCESS;
}

static int name_is(const cb_rec_t *r, const char16_t *s)
{
    return drv_wcslen(r->name) == drv_wcslen((PCWSTR)s) && memcmp(r->name, s, drv_wcslen(r->name) * 2) == 0;
}

static void test_query(void)
{
    HANDLE k = open_abs(ROOT u"\\Q", KEY_ALL_ACCESS, 1, NULL);
    set_sz(k, u"Str", u"hello", REG_SZ);
    set_dword(k, u"Num", 42);
    set_sz(k, u"Exp", u"%Root%\\drv", REG_EXPAND_SZ);
    UNICODE_STRING mn = us(u"Multi");
    static const char16_t multi[] = u"one\0two\0three\0";            /* the literal's own NUL ends the list */
    CHECK_STATUS(ZwSetValueKey(k, &mn, 0, REG_MULTI_SZ, (PVOID)multi, sizeof multi), STATUS_SUCCESS);
    UNICODE_STRING bn = us(u"Bin");
    UCHAR bin[5] = { 1, 2, 3, 4, 5 };
    CHECK_STATUS(ZwSetValueKey(k, &bn, 0, REG_BINARY, bin, 5), STATUS_SUCCESS);
    HANDLE sk = open_abs(ROOT u"\\Q\\Sub", KEY_ALL_ACCESS, 1, NULL);
    set_dword(sk, u"A", 1);
    set_dword(sk, u"B", 2);
    ZwClose(sk);
    ZwClose(k);

    PCWSTR path = W(ROOT u"\\Q");

    /* Callback per named value, with the value's own name, type and data. */
    {
        RTL_QUERY_REGISTRY_TABLE t[3];
        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb; t[0].Name = (PWSTR)W(u"Str");
        t[1].QueryRoutine = record_cb; t[1].Name = (PWSTR)W(u"Num"); t[1].EntryContext = (PVOID)0x77;
        g_ncb = 0; memset(g_cb, 0, sizeof g_cb);
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 2);
        CHECK(name_is(&g_cb[0], u"Str") && g_cb[0].type == REG_SZ && g_cb[0].len == 12);
        CHECK(memcmp(g_cb[0].data, u"hello", 12) == 0);
        CHECK(name_is(&g_cb[1], u"Num") && g_cb[1].type == REG_DWORD && g_cb[1].len == 4);
        CHECK(*(ULONG *)g_cb[1].data == 42 && g_cb[1].ectx == (PVOID)0x77);
    }

    /* A missing value: its default goes to the callback; with REQUIRED the call fails instead; with no default nothing
     * happens. */
    {
        RTL_QUERY_REGISTRY_TABLE t[2];
        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb; t[0].Name = (PWSTR)W(u"Gone");
        t[0].DefaultType = REG_SZ; t[0].DefaultData = (PVOID)W(u"dflt"); t[0].DefaultLength = 0;
        g_ncb = 0; memset(g_cb, 0, sizeof g_cb);
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 1 && g_cb[0].type == REG_SZ && g_cb[0].len == 10 && memcmp(g_cb[0].data, u"dflt", 10) == 0);

        t[0].DefaultType = REG_NONE;
        g_ncb = 0;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 0);
        t[0].Flags = RTL_QUERY_REGISTRY_REQUIRED;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_OBJECT_NAME_NOT_FOUND);
    }

    /* DIRECT: a string into a UNICODE_STRING (allocated when it has no buffer), a number into a ULONG. */
    {
        UNICODE_STRING s = { 0, 0, NULL };
        ULONG num = 0;
        WCHAR fixed[3];
        UNICODE_STRING small = { 0, sizeof fixed, fixed };
        RTL_QUERY_REGISTRY_TABLE t[3];
        memset(t, 0, sizeof t);
        t[0].Flags = RTL_QUERY_REGISTRY_DIRECT; t[0].Name = (PWSTR)W(u"Str"); t[0].EntryContext = &s;
        t[1].Flags = RTL_QUERY_REGISTRY_DIRECT; t[1].Name = (PWSTR)W(u"Num"); t[1].EntryContext = &num;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(s.Buffer && s.Length == 10 && s.MaximumLength == 12 && memcmp(s.Buffer, u"hello", 10) == 0);
        CHECK(num == 42);
        RtlFreeUnicodeString(&s);

        t[0].EntryContext = &small;                                 /* a buffer that cannot hold it */
        t[1].Name = NULL;
        t[1].Flags = 0;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(small.Length == 0);                                   /* BUFFER_TOO_SMALL from a direct store is reported as success, nothing stored */

        t[0].Flags = RTL_QUERY_REGISTRY_DIRECT; t[0].Name = (PWSTR)W(u"Num"); t[0].QueryRoutine = record_cb;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_INVALID_PARAMETER);
    }

    /* DIRECT binary and the sized form: with a positive size the buffer gets {size, type, data}. */
    {
        struct { ULONG size; ULONG type; UCHAR data[8]; } sized = { 16, 0, { 0 } };
        RTL_QUERY_REGISTRY_TABLE t[2];
        memset(t, 0, sizeof t);
        t[0].Flags = RTL_QUERY_REGISTRY_DIRECT; t[0].Name = (PWSTR)W(u"Bin"); t[0].EntryContext = &sized;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(memcmp(&sized, (UCHAR[]){ 1, 2, 3, 4, 5 }, 5) == 0);   /* REG_BINARY is stored raw */

        struct { LONG size; UCHAR data[8]; } raw = { -8, { 0 } };
        t[0].EntryContext = &raw;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(raw.size == 0x04030201);                                /* the raw bytes start at the buffer's first byte */
    }

    /* Expansion: REG_EXPAND_SZ reaches the routine as REG_SZ with the environment applied; NOEXPAND leaves it. */
    {
        static const char16_t env[] = u"Root=C:\\Windows\0Other=x\0";
        RTL_QUERY_REGISTRY_TABLE t[2];
        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb; t[0].Name = (PWSTR)W(u"Exp");
        g_ncb = 0; memset(g_cb, 0, sizeof g_cb);
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, (PVOID)env), STATUS_SUCCESS);
        CHECK(g_cb[0].type == REG_SZ);
        CHECK(memcmp(g_cb[0].data, u"C:\\Windows\\drv", 28) == 0 && g_cb[0].len == 30);

        g_ncb = 0;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_cb[0].type == REG_SZ && memcmp(g_cb[0].data, u"%Root%\\drv", 20) == 0);   /* no block: left as written */

        t[0].Flags = RTL_QUERY_REGISTRY_NOEXPAND;
        g_ncb = 0;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, (PVOID)env), STATUS_SUCCESS);
        CHECK(g_cb[0].type == REG_EXPAND_SZ && memcmp(g_cb[0].data, u"%Root%\\drv", 20) == 0);
    }

    /* A multi-string is reported one string at a time as REG_SZ, unless NOEXPAND keeps it whole. */
    {
        RTL_QUERY_REGISTRY_TABLE t[2];
        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb; t[0].Name = (PWSTR)W(u"Multi");
        g_ncb = 0; memset(g_cb, 0, sizeof g_cb);
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 3);
        CHECK(g_cb[0].type == REG_SZ && memcmp(g_cb[0].data, u"one", 8) == 0 && g_cb[0].len == 8);
        CHECK(memcmp(g_cb[1].data, u"two", 8) == 0 && memcmp(g_cb[2].data, u"three", 12) == 0);

        UNICODE_STRING strs[3] = { { 0, 0, NULL }, { 0, 0, NULL }, { 0, 0, NULL } };
        t[0].QueryRoutine = NULL; t[0].Flags = RTL_QUERY_REGISTRY_DIRECT; t[0].EntryContext = strs;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(strs[0].Buffer && strs[0].Length == 6 && strs[2].Buffer && strs[2].Length == 10);
        for (int i = 0; i < 3; i++) RtlFreeUnicodeString(&strs[i]);

        t[0].QueryRoutine = record_cb; t[0].Flags = RTL_QUERY_REGISTRY_NOEXPAND; t[0].EntryContext = NULL;
        g_ncb = 0;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 1 && g_cb[0].type == REG_MULTI_SZ && g_cb[0].len == sizeof multi);
    }

    /* A nameless entry enumerates every value of the key; a SUBKEY entry moves to a subkey for the entries after it. */
    {
        RTL_QUERY_REGISTRY_TABLE t[4];
        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb;                            /* Name NULL: all values */
        g_ncb = 0; memset(g_cb, 0, sizeof g_cb);
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 7);

        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb; t[0].Flags = RTL_QUERY_REGISTRY_SUBKEY; t[0].Name = (PWSTR)W(u"Sub");
        g_ncb = 0; memset(g_cb, 0, sizeof g_cb);
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 2);                                          /* A and B */

        memset(t, 0, sizeof t);
        t[0].Flags = RTL_QUERY_REGISTRY_SUBKEY; t[0].Name = (PWSTR)W(u"Sub");
        t[1].QueryRoutine = record_cb; t[1].Name = (PWSTR)W(u"B");
        ULONG b = 0;
        t[2].Flags = RTL_QUERY_REGISTRY_TOPKEY | RTL_QUERY_REGISTRY_DIRECT; t[2].Name = (PWSTR)W(u"Num"); t[2].EntryContext = &b;
        g_ncb = 0; memset(g_cb, 0, sizeof g_cb);
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 1 && name_is(&g_cb[0], u"B") && *(ULONG *)g_cb[0].data == 2);
        CHECK(b == 42);                                             /* TOPKEY went back to the key the call started with */

        memset(t, 0, sizeof t);
        t[0].Flags = RTL_QUERY_REGISTRY_SUBKEY; t[0].Name = (PWSTR)W(u"NoSuchSub");
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_OBJECT_NAME_NOT_FOUND);
    }

    /* NOVALUE calls the routine with no value; DELETE removes what it delivered. */
    {
        RTL_QUERY_REGISTRY_TABLE t[3];
        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb; t[0].Flags = RTL_QUERY_REGISTRY_NOVALUE; t[0].EntryContext = (PVOID)5;
        g_ncb = 0; memset(g_cb, 0, sizeof g_cb);
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(g_ncb == 1 && g_cb[0].type == REG_NONE && g_cb[0].len == 0 && g_cb[0].ectx == (PVOID)5);

        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb; t[0].Flags = RTL_QUERY_REGISTRY_DELETE; t[0].Name = (PWSTR)W(u"Str");
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_SUCCESS);
        memset(t, 0, sizeof t);
        t[0].QueryRoutine = record_cb; t[0].Flags = RTL_QUERY_REGISTRY_REQUIRED; t[0].Name = (PWSTR)W(u"Str");
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL), STATUS_OBJECT_NAME_NOT_FOUND);
    }

    /* A key handle in place of a path (RTL_REGISTRY_HANDLE); the handle stays open. */
    {
        HANDLE h = open_abs(ROOT u"\\Q", KEY_READ, 0, NULL);
        ULONG num = 0;
        RTL_QUERY_REGISTRY_TABLE t[2];
        memset(t, 0, sizeof t);
        t[0].Flags = RTL_QUERY_REGISTRY_DIRECT; t[0].Name = (PWSTR)W(u"Num"); t[0].EntryContext = &num;
        CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_HANDLE, (PCWSTR)h, t, NULL, NULL), STATUS_SUCCESS);
        CHECK(num == 42);
        CHECK_STATUS(ZwClose(h), STATUS_SUCCESS);
    }

    CHECK_STATUS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, W(ROOT u"\\NoSuchQ"), (RTL_QUERY_REGISTRY_TABLE[]){ { 0 } },
                                        NULL, NULL), STATUS_OBJECT_NAME_NOT_FOUND);
}

int main(void)
{
    test_registry_boot();
    CHECK_STATUS(drvhost_init(), 0);
    test_zw();
    test_rtl_helpers();
    test_query();
    drvhost_shutdown();
    DH_TEST_SUMMARY("driver_reg_test");
}
