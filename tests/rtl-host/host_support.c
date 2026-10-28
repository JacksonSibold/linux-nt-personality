/*
 * host_support.c - the parts of ntdll and of Wine's test framework that the Rtl host tests need but the ported sources
 * do not bring: a malloc-backed process heap, stubs for the few system calls the sources reach, the lookup that stands
 * in for GetProcAddress, and the counters behind ok() and skip().
 *
 * Built only by tests/rtl-host/Makefile. Original code of this repository.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdint.h>
#include <sys/mman.h>

#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ntstatus.h"
#include "wine/test.h"
#include "winnls.h"

/* ---- Process heap -------------------------------------------------------------------------------------------- */

void *WINAPI RtlAllocateHeap(HANDLE heap, ULONG flags, SIZE_T size)
{
    (void)heap;
    return (flags & HEAP_ZERO_MEMORY) ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
}

BOOLEAN WINAPI RtlFreeHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap; (void)flags;
    free(ptr);
    return TRUE;
}

PVOID WINAPI RtlReAllocateHeap(HANDLE heap, ULONG flags, PVOID ptr, SIZE_T size)
{
    (void)heap; (void)flags;
    return realloc(ptr, size ? size : 1);
}

/* ---- System calls the sources reach ------------------------------------------------------------------------------ */

NTSTATUS WINAPI NtQuerySystemTime(LARGE_INTEGER *time)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time->QuadPart = ((LONGLONG)ts.tv_sec + 11644473600LL) * 10000000LL + ts.tv_nsec / 100;
    return STATUS_SUCCESS;
}

/* The kernel probes user pointers and reports STATUS_ACCESS_VIOLATION; mincore tells whether the page is mapped. */
static int bad_ptr(const void *p)
{
    unsigned char vec;
    return !p || mincore((void *)((uintptr_t)p & ~(uintptr_t)4095), 1, &vec) != 0;
}

NTSTATUS WINAPI NtQueryPerformanceCounter(LARGE_INTEGER *counter, LARGE_INTEGER *frequency)
{
    struct timespec ts;
    if (bad_ptr(counter) || (frequency && bad_ptr(frequency))) return STATUS_ACCESS_VIOLATION;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (counter) counter->QuadPart = (LONGLONG)ts.tv_sec * 10000000LL + ts.tv_nsec / 100;
    if (frequency) frequency->QuadPart = 10000000LL;
    return STATUS_SUCCESS;
}

DWORD WINAPI GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *counter)
{
    NtQueryPerformanceCounter(counter, NULL);
    return TRUE;
}

unsigned long long __rdtsc(void)
{
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

/* A timer with the usual 15.625 ms tick and 0.5 ms limit; the host has no kernel timer to set. */
static ULONG timer_res = 156250;

NTSTATUS WINAPI NtQueryTimerResolution(ULONG *min_res, ULONG *max_res, ULONG *current_res)
{
    *min_res = 156250;
    *max_res = 5000;
    *current_res = timer_res;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtSetTimerResolution(ULONG res, BOOLEAN set, ULONG *current_res)
{
    if (set) timer_res = res < 5000 ? 5000 : (res > 156250 ? 156250 : res);
    else timer_res = 156250;
    *current_res = timer_res;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQuerySystemInformation(SYSTEM_INFORMATION_CLASS cls, void *info, ULONG len, ULONG *retlen)
{
    (void)cls; (void)info; (void)len; (void)retlen;
    return STATUS_NOT_IMPLEMENTED;
}

/* ---- Win32 functions the unit tests call -------------------------------------------------------------------------- */

void WINAPI RtlGetDefaultCodePage(USHORT *ansi, USHORT *oem);

static __thread DWORD last_error;

DWORD WINAPI GetLastError(void) { return last_error; }
void WINAPI SetLastError(DWORD err) { last_error = err; }
UINT WINAPI GetACP(void) { USHORT ansi, oem; RtlGetDefaultCodePage(&ansi, &oem); return ansi; }
UINT WINAPI GetOEMCP(void) { USHORT ansi, oem; RtlGetDefaultCodePage(&ansi, &oem); return oem; }

HANDLE WINAPI GetProcessHeap(void) { return NULL; }
LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) { return RtlAllocateHeap(heap, flags, size); }
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID ptr) { return RtlFreeHeap(heap, flags, ptr); }

/* The debug helpers run on the arguments of ok() even when the test passes, and a test may pass a buffer that its own
 * check shows to be unterminated. Reading past it is what Wine does, so these two stay out of AddressSanitizer. */
#define NO_ASAN __attribute__((no_sanitize("address")))

NO_ASAN int WINAPI lstrlenW(LPCWSTR s) { int n = 0; while (s[n]) n++; return n; }

int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b)
{
    while (*a && *a == *b) a++, b++;
    return (int)*a - (int)*b;
}

/* The ANSI page and UTF-8 only: the tests that need other pages skip themselves or use the Rtl functions. */
int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen, LPWSTR dst, int dstlen)
{
    DWORD out;
    if (srclen < 0) srclen = strlen(src) + 1;
    if (cp == CP_UTF8) {
        NTSTATUS status = RtlUTF8ToUnicodeN(dstlen ? dst : NULL, dstlen * sizeof(WCHAR), &out, src, srclen);
        if (status == STATUS_SOME_NOT_MAPPED && (flags & MB_ERR_INVALID_CHARS)) { last_error = ERROR_NO_UNICODE_TRANSLATION; return 0; }
        return out / sizeof(WCHAR);
    }
    if (!dstlen) { RtlMultiByteToUnicodeSize(&out, src, srclen); return out / sizeof(WCHAR); }
    RtlMultiByteToUnicodeN(dst, dstlen * sizeof(WCHAR), &out, src, srclen);
    (void)cp; (void)flags;
    return out / sizeof(WCHAR);
}

int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srclen, LPSTR dst, int dstlen, LPCSTR def, BOOL *used)
{
    DWORD out;
    if (srclen < 0) srclen = lstrlenW(src) + 1;
    if (cp == CP_UTF8) {
        NTSTATUS status = RtlUnicodeToUTF8N(dstlen ? dst : NULL, dstlen, &out, src, srclen * sizeof(WCHAR));
        if (status == STATUS_SOME_NOT_MAPPED && (flags & WC_ERR_INVALID_CHARS)) { last_error = ERROR_NO_UNICODE_TRANSLATION; return 0; }
        return out;
    }
    if (!dstlen) { RtlUnicodeToMultiByteSize(&out, src, srclen * sizeof(WCHAR)); return out; }
    RtlUnicodeToMultiByteN(dst, dstlen, &out, src, srclen * sizeof(WCHAR));
    (void)cp; (void)flags; (void)def;
    if (used) *used = FALSE;
    return out;
}

int WINAPI lstrcmpA(LPCSTR a, LPCSTR b) { return strcmp(a, b); }
int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b) { return strcasecmp(a, b); }
int WINAPI lstrlenA(LPCSTR s) { return (int)strlen(s); }

int WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b)
{
    while (*a && RtlUpcaseUnicodeChar(*a) == RtlUpcaseUnicodeChar(*b)) a++, b++;
    return (int)RtlUpcaseUnicodeChar(*a) - (int)RtlUpcaseUnicodeChar(*b);
}

/* The per-thread error value lives in the TEB in ntdll.dll; the host has no TEB. */
void WINAPI RtlSetLastWin32Error(DWORD err) { last_error = err; }
DWORD WINAPI RtlGetLastWin32Error(void) { return last_error; }
void WINAPI RtlSetLastWin32ErrorAndNtStatusFromNtStatus(NTSTATUS status) { last_error = RtlNtStatusToDosError(status); }

LPWSTR WINAPI lstrcpyW(LPWSTR dst, LPCWSTR src)
{
    LPWSTR d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}

/* ---- Debug string helpers: quoted strings in a ring of buffers, as Wine prints them ------------------------------ */

static char *dbg_buf(size_t n)
{
    static char ring[16][1024];
    static unsigned int next;
    (void)n;
    return ring[next++ % 16];
}

NO_ASAN static const char *dbg_str(const void *s, int n, int wide)
{
    char *out = dbg_buf(0), *p = out;
    int i;

    if (!s) return "(null)";
    *p++ = wide ? 'L' : ' ';
    if (!wide) p--;
    *p++ = '"';
    for (i = 0; i < n && p < out + 1000; i++)
    {
        unsigned int c = wide ? ((const WCHAR *)s)[i] : ((const unsigned char *)s)[i];
        if (c == '\\' || c == '"') { *p++ = '\\'; *p++ = (char)c; }
        else if (c >= 0x20 && c < 0x7f) *p++ = (char)c;
        else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
        else if (c == '\t') { *p++ = '\\'; *p++ = 't'; }
        else p += sprintf(p, wide ? "\\x%04x" : "\\x%02x", c);
    }
    *p++ = '"';
    *p = 0;
    return out;
}

const char *wine_dbgstr_an(const char *s, int n) { return dbg_str(s, n, 0); }
NO_ASAN const char *wine_dbgstr_a(const char *s) { return s ? dbg_str(s, (int)strlen(s), 0) : "(null)"; }
const char *wine_dbgstr_wn(const WCHAR *s, int n) { return dbg_str(s, n, 1); }
NO_ASAN const char *wine_dbgstr_w(const WCHAR *s) { return s ? dbg_str(s, lstrlenW(s), 1) : "(null)"; }
const char *wine_dbgstr_us(const UNICODE_STRING *us) { return us ? dbg_str(us->Buffer, us->Length / sizeof(WCHAR), 1) : "<null>"; }

const char *wine_dbgstr_guid(const GUID *g)
{
    char *out = dbg_buf(0);
    if (!g) return "(null)";
    sprintf(out, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
            g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return out;
}

const char *wine_dbgstr_longlong(unsigned long long v)
{
    char *out = dbg_buf(0);
    sprintf(out, "%llx", v);
    return out;
}

/* ---- GetProcAddress ------------------------------------------------------------------------------------------ */

struct rtl_proc { const char *name; void *addr; };
extern const struct rtl_proc rtl_proc_table[];   /* proctab.c, written by gen_proctab.py */

HMODULE WINAPI GetModuleHandleA(LPCSTR name)
{
    if (name && (!strcmp(name, "ntdll.dll") || !strcmp(name, "ntdll"))) return (HMODULE)&rtl_proc_table;
    return NULL;
}

HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
    return GetModuleHandleA(name);
}

FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name)
{
    const struct rtl_proc *p;
    if (module != (HMODULE)&rtl_proc_table || !name) return NULL;
    for (p = rtl_proc_table; p->name; p++)
        if (!strcmp(p->name, name)) return (FARPROC)p->addr;
    return NULL;
}

/* ---- Test framework ------------------------------------------------------------------------------------------ */

int winetest_debug = 1;
int winetest_platform_is_wine = 1;
long winetest_successes, winetest_failures, winetest_skipped, winetest_todo_successes, winetest_todo_failures;
const char *winetest_cur_file = "";
int winetest_cur_line;
unsigned int winetest_todo_level, winetest_flaky_level;
static char context[8][128];
static unsigned int context_count;

void winetest_set_location(const char *file, int line)
{
    const char *p = strrchr(file, '/');
    winetest_cur_file = p ? p + 1 : file;
    winetest_cur_line = line;
}

static void print_location(const char *kind)
{
    unsigned int i;
    printf("%s:%d: %s", winetest_cur_file, winetest_cur_line, kind);
    for (i = 0; i < context_count; i++) printf("%s: ", context[i]);
}

void winetest_ok(int condition, const char *msg, ...)
{
    va_list args;

    va_start(args, msg);
    if (winetest_todo_level) {
        if (condition) {
            print_location("Test succeeded inside todo block: ");
            vprintf(msg, args);
            winetest_todo_failures++;
        } else {
            winetest_todo_successes++;
        }
    } else if (!condition) {
        print_location(winetest_flaky_level ? "Test marked flaky: " : "Test failed: ");
        vprintf(msg, args);
        if (!winetest_flaky_level) winetest_failures++;
    } else {
        winetest_successes++;
    }
    va_end(args);
}

void winetest_skip(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    if (winetest_debug > 0) { print_location("Tests skipped: "); vprintf(msg, args); }
    winetest_skipped++;
    va_end(args);
}

void winetest_win_skip(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    if (winetest_debug > 0) { print_location("Tests skipped: "); vprintf(msg, args); }
    winetest_skipped++;
    va_end(args);
}

void winetest_trace(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    if (winetest_debug > 1) { print_location(""); vprintf(msg, args); }
    va_end(args);
}

void winetest_subtest(const char *name)
{
    if (winetest_debug > 1) { print_location("Subtest "); printf("%s\n", name); }
}

void winetest_push_context(const char *fmt, ...)
{
    va_list args;
    if (context_count >= 8) return;
    va_start(args, fmt);
    vsnprintf(context[context_count++], sizeof(context[0]), fmt, args);
    va_end(args);
}

void winetest_pop_context(void)
{
    if (context_count) context_count--;
}

static int todo_pending, flaky_pending;

void winetest_start_todo(int is_todo)
{
    todo_pending = 1;
    if (is_todo) winetest_todo_level++;
}

int winetest_loop_todo(void)
{
    int r = todo_pending;
    todo_pending = 0;
    return r;
}

void winetest_end_todo(void)
{
    if (winetest_todo_level) winetest_todo_level--;
}

void winetest_start_flaky(int is_flaky)
{
    flaky_pending = 1;
    if (is_flaky) winetest_flaky_level++;
}

int winetest_loop_flaky(void)
{
    int r = flaky_pending;
    flaky_pending = 0;
    return r;
}

void winetest_end_flaky(void)
{
    if (winetest_flaky_level) winetest_flaky_level--;
}

/* The runner: main() calls func_<name> once; RTL_TEST_FUNC names it on the command line of the compiler. */
extern void RTL_TEST_FUNC(void);

int main(void)
{
    const char *d = getenv("WINETEST_DEBUG");
    if (d) winetest_debug = atoi(d);
    RTL_TEST_FUNC();
    printf("%s: %ld successes, %ld failures, %ld skipped, %ld todo\n", RTL_TEST_NAME, winetest_successes,
           winetest_failures, winetest_skipped, winetest_todo_successes);
    return winetest_failures || winetest_todo_failures ? 1 : 0;
}
