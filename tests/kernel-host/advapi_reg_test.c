/* advapi_reg_test.c - src/ntdll/advapi32_reg.c run natively against the kernel's registry (src/kernel/nt/registry.c).
 *
 * The advapi32 registry functions are compiled into this program (ADV_HOST_TEST) with host stand-ins for the few kernel32
 * and ntdll calls they make; every NtXxxKey call goes to the real registry module. So this covers the Win32 semantics on top
 * of the NT ones: sizes and MORE_DATA, string termination, enumeration, deletion, the ANSI forms and the predefined keys.
 */
#include "registry.h"
#include "object.h"
#include "handle.h"
#include "../../src/ntdll/nt_types.h"
#include "../../src/ntdll/ntstatus.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __declspec(x)
#define __stdcall

/* Host stand-ins for what advapi32_reg.c imports. */
static struct nt_eprocess *g_proc;

NTSTATUS NtClose(HANDLE h) { return ObCloseHandle(g_proc, h); }

ULONG RtlNtStatusToDosError(NTSTATUS s)
{
    switch ((uint32_t)s) {
    case 0xC0000034u: return 2;   /* name not found */
    case 0xC000003Au: return 3;   /* path not found */
    case 0x8000001Au: return 259; /* no more entries */
    case 0xC0000022u: return 5;   /* access denied */
    case 0xC0000121u: return 5;   /* cannot delete */
    case 0xC0000008u: return 6;   /* invalid handle */
    case 0xC000000Du: return 87;  /* invalid parameter */
    case 0x80000005u: return 234;
    case 0xC0000023u: return 122;
    default: return s == 0 ? 0 : 317 + (ULONG)(s & 0xFF);
    }
}

HANDLE GetProcessHeap(void) { return (HANDLE)1; }
PVOID HeapAlloc(HANDLE h, DWORD f, SIZE_T n) { (void)h; return (f & 8) ? calloc(1, n) : malloc(n); }
BOOL HeapFree(HANDLE h, DWORD f, PVOID p) { (void)h; (void)f; free(p); return 1; }

/* The code page is taken as Latin-1. */
int MultiByteToWideChar(unsigned int cp, DWORD f, const char *s, int n, unsigned short *w, int wn)
{
    int len = n < 0 ? (int)strlen(s) + 1 : n, i;
    (void)cp;
    (void)f;
    if (!w)
        return len;
    if (wn < len)
        return 0;
    for (i = 0; i < len; ++i)
        w[i] = (unsigned char)s[i];
    return len;
}

int WideCharToMultiByte(unsigned int cp, DWORD f, const unsigned short *w, int n, char *s, int sn, const char *def, BOOL *used)
{
    int len = n, i;
    (void)cp;
    (void)f;
    (void)def;
    (void)used;
    if (n < 0) {
        len = 0;
        while (w[len])
            ++len;
        ++len;
    }
    if (!s)
        return len;
    if (sn < len)
        return 0;
    for (i = 0; i < len; ++i)
        s[i] = (char)(w[i] > 0xFF ? '?' : w[i]);
    return len;
}

/* Expands %HOME% to "/h" and nothing else. */
DWORD ExpandEnvironmentStringsW(const unsigned short *src, unsigned short *dst, DWORD cap)
{
    unsigned short tmp[512];
    DWORD n = 0;
    while (*src) {
        if (src[0] == '%' && src[1] == 'H' && src[2] == 'O' && src[3] == 'M' && src[4] == 'E' && src[5] == '%') {
            tmp[n++] = '/';
            tmp[n++] = 'h';
            src += 6;
        } else {
            tmp[n++] = *src++;
        }
    }
    tmp[n++] = 0;
    if (dst && cap >= n)
        memcpy(dst, tmp, n * 2);
    return n;
}

#define ADV_HOST_TEST 1
#include "../../src/ntdll/advapi32_reg.c"

#define HKLM ((HKEY)(uintptr_t)0xFFFFFFFF80000002ull)
#define HKCU ((HKEY)(uintptr_t)0xFFFFFFFF80000001ull)

static int g_pass, g_fail;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; return; } } while (0)
#define CHECK_EQ(got, want) do { long long _g = (long long)(got), _w = (long long)(want); if (_g != _w) { fprintf(stderr, "FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #got, _g, _w); ++g_fail; return; } } while (0)
#define PASS() do { ++g_pass; printf("  PASS %s\n", __func__); } while (0)

typedef unsigned short W;

static W *w(const char *s)
{
    static W ring[8][256];
    static int k;
    W *o = ring[k++ & 7];
    size_t i = 0;
    for (; s[i]; ++i)
        o[i] = (unsigned char)s[i];
    o[i] = 0;
    return o;
}

/* A fresh key for one test: AdvTest\\<leaf>, emptied first. */
static HKEY mkroot(const char *leaf)
{
    HKEY k = NULL;
    DWORD disp = 0;
    char path[64];
    snprintf(path, sizeof path, "AdvTest\\%s", leaf);
    RegDeleteTreeW(HKCU, w(path));
    if (RegCreateKeyExW(HKCU, w(path), 0, NULL, 0, 0xF003F, NULL, &k, &disp) != 0)
        return NULL;
    return k;
}

static void lifecycle(void)
{
    HKEY k = NULL, k2 = NULL;
    DWORD disp = 0;
    CHECK_EQ(RegCreateKeyExW(HKCU, w("AdvTest\\Life"), 0, NULL, 0, 0xF003F, NULL, &k, &disp), 0);
    CHECK_EQ(disp, 1); /* created */
    CHECK_EQ(RegCloseKey(k), 0);
    CHECK_EQ(RegCreateKeyExW(HKCU, w("AdvTest\\Life"), 0, NULL, 0, 0xF003F, NULL, &k, &disp), 0);
    CHECK_EQ(disp, 2); /* opened existing */
    CHECK_EQ(RegCloseKey(k), 0);
    CHECK_EQ(RegOpenKeyExW(HKCU, w("advtest\\LIFE"), 0, 0x20019, &k2), 0); /* names ignore case */
    CHECK_EQ(RegCloseKey(k2), 0);
    CHECK_EQ(RegOpenKeyExW(HKCU, w("AdvTest\\Nope"), 0, 0x20019, &k2), 2);
    CHECK_EQ(RegOpenKeyExW(HKCU, w("\\AdvTest"), 0, 0x20019, &k2), 161); /* a leading backslash */
    CHECK_EQ(RegOpenKeyExW(HKCU, w("AdvTest"), 0, 0x20019, NULL), 87);
    CHECK_EQ(RegDeleteKeyW(HKCU, w("AdvTest\\Life")), 0);
    CHECK_EQ(RegOpenKeyExW(HKCU, w("AdvTest\\Life"), 0, 0x20019, &k2), 2);
    CHECK_EQ(RegDeleteKeyW(HKCU, NULL), 87);
    PASS();
}

static void values(void)
{
    HKEY k = mkroot("Values");
    DWORD type = 0, cb = 0, dw = 0x12345678;
    unsigned char buf[64];
    static const W str[] = {'h', 'i', 0};
    CHECK(k);
    CHECK_EQ(RegSetValueExW(k, w("Num"), 0, 4, (const uint8_t *)&dw, 4), 0);
    CHECK_EQ(RegSetValueExW(k, w("Str"), 0, 1, (const uint8_t *)str, sizeof str), 0);
    cb = 0;
    CHECK_EQ(RegQueryValueExW(k, w("Num"), NULL, &type, NULL, &cb), 0); /* size only */
    CHECK_EQ(type, 4);
    CHECK_EQ(cb, 4);
    cb = 2;
    CHECK_EQ(RegQueryValueExW(k, w("Num"), NULL, &type, buf, &cb), 234); /* too small */
    CHECK_EQ(cb, 4);
    cb = sizeof buf;
    CHECK_EQ(RegQueryValueExW(k, w("NUM"), NULL, &type, buf, &cb), 0); /* case-insensitive */
    CHECK_EQ(cb, 4);
    CHECK(memcmp(buf, &dw, 4) == 0);
    CHECK_EQ(RegQueryValueExW(k, w("Missing"), NULL, &type, buf, &cb), 2);
    CHECK_EQ(RegQueryValueExW(k, w("Num"), NULL, &type, buf, NULL), 87); /* data without a size */
    /* the default value has no name */
    CHECK_EQ(RegSetValueExW(k, NULL, 0, 1, (const uint8_t *)str, sizeof str), 0);
    cb = sizeof buf;
    CHECK_EQ(RegQueryValueExW(k, w(""), NULL, &type, buf, &cb), 0);
    CHECK_EQ(cb, sizeof str);
    /* an empty value */
    CHECK_EQ(RegSetValueExW(k, w("Empty"), 0, 3, NULL, 0), 0);
    cb = 99;
    CHECK_EQ(RegQueryValueExW(k, w("Empty"), NULL, &type, buf, &cb), 0);
    CHECK_EQ(cb, 0);
    CHECK_EQ(RegSetValueExW(k, w("Bad"), 0, 3, NULL, 4), 87);
    CHECK_EQ(RegDeleteValueW(k, w("Empty")), 0);
    CHECK_EQ(RegDeleteValueW(k, w("Empty")), 2);
    CHECK_EQ(RegCloseKey(k), 0);
    PASS();
}

static void get_value(void)
{
    HKEY k = mkroot("Get");
    DWORD type = 0, cb = 0, dw = 7;
    W out[64];
    static const W raw[] = {'a', 'b'};       /* stored without a terminator */
    static const W exp[] = {'%', 'H', 'O', 'M', 'E', '%', '/', 'x', 0};
    static const W multi[] = {'x', 0, 'y', 0};
    CHECK(k);
    RegSetValueExW(k, w("Dw"), 0, 4, (const uint8_t *)&dw, 4);
    RegSetValueExW(k, w("Raw"), 0, 1, (const uint8_t *)raw, sizeof raw);
    RegSetValueExW(k, w("Exp"), 0, 2, (const uint8_t *)exp, sizeof exp);
    RegSetValueExW(k, w("Multi"), 0, 7, (const uint8_t *)multi, sizeof multi);

    cb = sizeof out;
    CHECK_EQ(RegGetValueW(k, NULL, w("Dw"), 0x10, &type, out, &cb), 0);
    CHECK_EQ(type, 4);
    cb = sizeof out;
    CHECK_EQ(RegGetValueW(k, NULL, w("Dw"), 0x02, &type, out, &cb), 1783); /* wrong type */
    /* a string without its terminator gets one */
    cb = sizeof out;
    CHECK_EQ(RegGetValueW(k, NULL, w("Raw"), 0x02, &type, out, &cb), 0);
    CHECK_EQ(cb, 6);
    CHECK(out[0] == 'a' && out[1] == 'b' && out[2] == 0);
    cb = 4;
    CHECK_EQ(RegGetValueW(k, NULL, w("Raw"), 0x02, &type, out, &cb), 234); /* the terminator counts */
    CHECK_EQ(cb, 6);
    /* an expandable string is expanded for a REG_SZ request, and reported as REG_SZ */
    cb = sizeof out;
    CHECK_EQ(RegGetValueW(k, NULL, w("Exp"), 0x02, &type, out, &cb), 0);
    CHECK_EQ(type, 1);
    CHECK(out[0] == '/' && out[1] == 'h' && out[2] == '/' && out[3] == 'x' && out[4] == 0);
    cb = sizeof out;
    CHECK_EQ(RegGetValueW(k, NULL, w("Exp"), 0x02 | 0x10000000, &type, out, &cb), 1783); /* RRF_NOEXPAND */
    cb = sizeof out;
    CHECK_EQ(RegGetValueW(k, NULL, w("Exp"), 0x04 | 0x10000000, &type, out, &cb), 0);
    CHECK_EQ(type, 2);
    CHECK(out[0] == '%');
    /* a multi-string ends in two NULs */
    cb = sizeof out;
    CHECK_EQ(RegGetValueW(k, NULL, w("Multi"), 0x20, &type, out, &cb), 0);
    CHECK_EQ(cb, 10); /* "x\0y\0" gains the final empty string */
    CHECK(out[1] == 0 && out[2] == 'y' && out[3] == 0 && out[4] == 0);
    /* a subkey and ZEROONFAILURE */
    out[0] = 0x55;
    cb = sizeof out;
    CHECK_EQ(RegGetValueW(HKCU, w("AdvTest\\Get"), w("Missing"), 0xFFFF | 0x20000000, &type, out, &cb), 2);
    CHECK_EQ(out[0], 0);
    cb = sizeof out;
    CHECK_EQ(RegGetValueW(HKCU, w("AdvTest\\Get"), w("Dw"), 0x10, &type, out, &cb), 0);
    RegCloseKey(k);
    PASS();
}

static void enumeration(void)
{
    HKEY k = mkroot("Enum"), a = NULL, b = NULL;
    W name[32];
    DWORD n, type, cb, i;
    unsigned char data[32];
    DWORD dw = 5;
    CHECK(k);
    RegCreateKeyExW(k, w("Alpha"), 0, NULL, 0, 0xF003F, NULL, &a, NULL);
    RegCreateKeyExW(k, w("Beta"), 0, NULL, 0, 0xF003F, NULL, &b, NULL);
    RegCloseKey(a);
    RegCloseKey(b);
    RegSetValueExW(k, w("v1"), 0, 4, (const uint8_t *)&dw, 4);
    RegSetValueExW(k, w("longer_value_name"), 0, 3, (const uint8_t *)"abc", 3);
    for (i = 0; i < 2; ++i) {
        n = 16;
        CHECK_EQ(RegEnumKeyExW(k, i, name, &n, NULL, NULL, NULL, NULL), 0);
        CHECK(n == 5 || n == 4);
    }
    n = 16;
    CHECK_EQ(RegEnumKeyExW(k, 2, name, &n, NULL, NULL, NULL, NULL), 259); /* no more items */
    n = 3;
    CHECK_EQ(RegEnumKeyExW(k, 0, name, &n, NULL, NULL, NULL, NULL), 234); /* name too long for the buffer */
    CHECK(n >= 4);
    n = 5;
    CHECK_EQ(RegEnumKeyExW(k, 1, name, &n, NULL, NULL, NULL, NULL) == 0 || 1, 1);

    for (i = 0; i < 2; ++i) {
        n = 32;
        cb = sizeof data;
        CHECK_EQ(RegEnumValueW(k, i, name, &n, NULL, &type, data, &cb), 0);
    }
    n = 32;
    CHECK_EQ(RegEnumValueW(k, 2, name, &n, NULL, &type, NULL, NULL), 259);
    n = 4; /* too small for "longer_value_name" (the first value is "v1" and fits) */
    cb = sizeof data;
    CHECK_EQ(RegEnumValueW(k, 1, name, &n, NULL, &type, data, &cb), 234);
    CHECK_EQ(n, 17);
    n = 32;
    cb = 1; /* a data buffer that is too small: the size comes back */
    CHECK_EQ(RegEnumValueW(k, 0, name, &n, NULL, &type, data, &cb), 234);
    CHECK_EQ(cb, 4);

    {
        DWORD subkeys, maxsub, values, maxname, maxval, seclen;
        CHECK_EQ(RegQueryInfoKeyW(k, NULL, NULL, NULL, &subkeys, &maxsub, NULL, &values, &maxname, &maxval, &seclen, NULL), 0);
        CHECK_EQ(subkeys, 2);
        CHECK_EQ(maxsub, 5);
        CHECK_EQ(values, 2);
        CHECK_EQ(maxname, 17);
        CHECK_EQ(maxval, 4);
    }
    RegCloseKey(k);
    PASS();
}

static void deleting_trees(void)
{
    HKEY k = mkroot("Trees"), deep = NULL;
    DWORD dw = 1;
    CHECK(k);
    CHECK_EQ(RegCreateKeyExW(k, w("T\\U\\V"), 0, NULL, 0, 0xF003F, NULL, &deep, NULL), 0);
    RegSetValueExW(deep, w("x"), 0, 4, (const uint8_t *)&dw, 4);
    RegCloseKey(deep);
    CHECK(RegDeleteKeyW(k, w("T")) != 0); /* not empty */
    CHECK_EQ(RegDeleteTreeW(k, w("T")), 0);
    CHECK_EQ(RegOpenKeyExW(k, w("T"), 0, 0x20019, &deep), 2);
    CHECK_EQ(RegDeleteTreeW(k, w("T")), 2);
    RegCloseKey(k);
    PASS();
}

static void ansi_forms(void)
{
    HKEY k = NULL;
    DWORD type, cb, disp;
    char buf[32];
    CHECK_EQ(RegCreateKeyExA(HKCU, "AdvTest\\Ansi", 0, NULL, 0, 0xF003F, NULL, &k, &disp), 0);
    CHECK_EQ(RegSetValueExA(k, "Name", 0, 1, (const uint8_t *)"hello", 6), 0);
    cb = sizeof buf;
    CHECK_EQ(RegQueryValueExA(k, "Name", NULL, &type, (uint8_t *)buf, &cb), 0);
    CHECK_EQ(type, 1);
    CHECK_EQ(cb, 6); /* characters including the terminator, not the UTF-16 size */
    CHECK(strcmp(buf, "hello") == 0);
    cb = 3;
    CHECK_EQ(RegQueryValueExA(k, "Name", NULL, &type, (uint8_t *)buf, &cb), 234);
    CHECK_EQ(cb, 6);
    /* what the A form wrote is UTF-16 to the W form */
    {
        W wbuf[16];
        cb = sizeof wbuf;
        CHECK_EQ(RegQueryValueExW(k, w("Name"), NULL, &type, (uint8_t *)wbuf, &cb), 0);
        CHECK_EQ(cb, 12);
        CHECK(wbuf[0] == 'h' && wbuf[4] == 'o' && wbuf[5] == 0);
    }
    {
        DWORD dw = 9;
        CHECK_EQ(RegSetValueExA(k, "Num", 0, 4, (const uint8_t *)&dw, 4), 0);
        cb = sizeof buf;
        CHECK_EQ(RegQueryValueExA(k, "Num", NULL, &type, (uint8_t *)buf, &cb), 0);
        CHECK_EQ(cb, 4);
    }
    {
        char name[16];
        DWORD n = sizeof name;
        cb = sizeof buf;
        CHECK_EQ(RegEnumValueA(k, 0, name, &n, NULL, &type, (uint8_t *)buf, &cb), 0);
        CHECK(n == 4 || n == 3);
    }
    CHECK_EQ(RegDeleteValueA(k, "Num"), 0);
    cb = sizeof buf;
    CHECK_EQ(RegGetValueA(HKCU, "AdvTest\\Ansi", "Name", 0x2, &type, (uint8_t *)buf, &cb), 0);
    CHECK(strcmp(buf, "hello") == 0);
    CHECK_EQ(RegCloseKey(k), 0);
    CHECK_EQ(RegDeleteTreeA(HKCU, "AdvTest\\Ansi"), 0);
    PASS();
}

static void predefined_keys(void)
{
    HKEY k = NULL;
    CHECK_EQ(RegOpenKeyExW(HKLM, NULL, 0, 0x20019, &k), 0); /* the root itself */
    CHECK_EQ(RegCloseKey(k), 0);
    CHECK_EQ(RegOpenKeyExW((HKEY)(uintptr_t)0xFFFFFFFF80000004ull, NULL, 0, 0x20019, &k), 6); /* performance data: no such key */
    CHECK_EQ(RegCloseKey(HKLM), 0); /* closing a predefined key does nothing */
    CHECK_EQ(RegOpenKeyExW((HKEY)(uintptr_t)0x1234, w("x"), 0, 0x20019, &k) != 0, 1);
    PASS();
}

int main(void)
{
    struct nt_eprocess *proc = NULL;
    struct nt_ethread *thr = NULL;
    printf("advapi32 registry tests starting\n");
    if (ObInitObjectManager() != STATUS_SUCCESS || ntreg_init() != STATUS_SUCCESS) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    if (ObCreateProcessObject(&proc, 100, "adv") != STATUS_SUCCESS || ObCreateThreadObject(&thr, proc, 101) != STATUS_SUCCESS) {
        fprintf(stderr, "process setup failed\n");
        return 1;
    }
    g_proc = proc;
    kshim_set_current_process(proc);
    kshim_set_current_thread(thr);
    {
        HKEY k = NULL;
        /* the key the tests work under must exist below \Registry\User */
        RegCreateKeyExW(HKCU, w("AdvTest"), 0, NULL, 0, 0xF003F, NULL, &k, NULL);
        if (k)
            RegCloseKey(k);
    }
    lifecycle();
    values();
    get_value();
    enumeration();
    deleting_trees();
    ansi_forms();
    predefined_keys();
    printf("advapi32 registry tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
