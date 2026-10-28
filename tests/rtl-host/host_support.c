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
#include <malloc.h>

#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ntstatus.h"
#include "winioctl.h"
#include "../../src/ntdll/os_identity.h"
#include "wine/test.h"
#include "winnls.h"

/* ---- Process heap -------------------------------------------------------------------------------------------- */

/* Each block carries its requested size in a 16-byte header, which is what HeapSize reports. */
struct host_block { SIZE_T size; SIZE_T pad; };

void *WINAPI RtlAllocateHeap(HANDLE heap, ULONG flags, SIZE_T size)
{
    struct host_block *b;
    (void)heap;
    b = malloc(sizeof(*b) + (size ? size : 1));
    if (!b) return NULL;
    b->size = size;
    if (flags & HEAP_ZERO_MEMORY) memset(b + 1, 0, size);
    return b + 1;
}

BOOLEAN WINAPI RtlFreeHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap; (void)flags;
    if (ptr) free((struct host_block *)ptr - 1);
    return TRUE;
}

PVOID WINAPI RtlReAllocateHeap(HANDLE heap, ULONG flags, PVOID ptr, SIZE_T size)
{
    struct host_block *b;
    (void)heap;
    if (!ptr) return RtlAllocateHeap(heap, flags, size);
    b = realloc((struct host_block *)ptr - 1, sizeof(*b) + (size ? size : 1));
    if (!b) return NULL;
    if ((flags & HEAP_ZERO_MEMORY) && size > b->size) memset((char *)(b + 1) + b->size, 0, size - b->size);
    b->size = size;
    return b + 1;
}

SIZE_T WINAPI RtlSizeHeap(HANDLE heap, ULONG flags, const void *ptr)
{
    (void)heap; (void)flags;
    return ((const struct host_block *)ptr - 1)->size;
}

/* ---- A TEB and a PEB: the environment functions and the last error are kept there ----------------------------------- */

static RTL_USER_PROCESS_PARAMETERS *host_params_ptr; /* on the heap: the tests ask for its size */
static PEB host_peb;
static TEB host_teb;
static RTL_CRITICAL_SECTION host_peb_lock;
extern char **environ;

TEB *WINAPI NtCurrentTeb(void)
{
    if (!host_teb.Peb) {
        static const WCHAR image[] = L"C:\\test.exe", dir[] = L"C:\\";
        RTL_USER_PROCESS_PARAMETERS *params;
        UNICODE_STRING *strs[7];
        WCHAR *env, *pos;
        unsigned int i;

        host_peb.FastPebLock = &host_peb_lock;
        host_teb.Peb = &host_peb;
        host_teb.Tib.Self = &host_teb.Tib;

        /* The initial parameters of a process, as the loader builds them: the strings follow the structure one after the
         * other without alignment, and the environment and the current directory are separate blocks. */
        params = RtlAllocateHeap(NULL, HEAP_ZERO_MEMORY, sizeof(*params) + (2 * (lstrlenW(image) + 1) + 5) * sizeof(WCHAR));
        strs[0] = &params->DllPath;
        strs[1] = &params->ImagePathName;
        strs[2] = &params->CommandLine;
        strs[3] = &params->WindowTitle;
        strs[4] = &params->Desktop;
        strs[5] = &params->ShellInfo;
        strs[6] = &params->RuntimeInfo;
        pos = (WCHAR *)(params + 1);
        for (i = 0; i < 7; i++)
        {
            const WCHAR *src = (i == 1 || i == 2) ? image : L"";
            unsigned int len = lstrlenW(src) * sizeof(WCHAR);
            strs[i]->Length = len;
            strs[i]->MaximumLength = len + sizeof(WCHAR);
            strs[i]->Buffer = pos;
            memcpy(pos, src, len + sizeof(WCHAR));
            pos += len / sizeof(WCHAR) + 1;
        }
        params->Size = (char *)pos - (char *)params;
        params->AllocationSize = params->Size;
        params->Flags = PROCESS_PARAMS_FLAG_NORMALIZED;
        params->CurrentDirectory.DosPath.Buffer = calloc(MAX_PATH, sizeof(WCHAR));
        params->CurrentDirectory.DosPath.MaximumLength = MAX_PATH * sizeof(WCHAR);
        memcpy(params->CurrentDirectory.DosPath.Buffer, dir, sizeof(dir));
        params->CurrentDirectory.DosPath.Length = sizeof(dir) - sizeof(WCHAR);
        RtlCreateEnvironment(FALSE, &env);
        params->Environment = env;
        params->EnvironmentSize = RtlSizeHeap(NULL, 0, env);
        host_params_ptr = params;
        host_peb.ProcessParameters = params;
    }
    return &host_teb;
}

/* ---- Heap, critical sections and virtual memory behind the Rtl functions ------------------------------------------- */


/* The host tests are single threaded: a critical section only counts. */
NTSTATUS WINAPI RtlInitializeCriticalSection(RTL_CRITICAL_SECTION *crit) { memset(crit, 0, sizeof(*crit)); crit->LockCount = -1; return STATUS_SUCCESS; }
NTSTATUS WINAPI RtlDeleteCriticalSection(RTL_CRITICAL_SECTION *crit) { memset(crit, 0, sizeof(*crit)); return STATUS_SUCCESS; }
NTSTATUS WINAPI RtlEnterCriticalSection(RTL_CRITICAL_SECTION *crit) { crit->RecursionCount++; return STATUS_SUCCESS; }
NTSTATUS WINAPI RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *crit) { crit->RecursionCount--; return STATUS_SUCCESS; }

NTSTATUS WINAPI NtAllocateVirtualMemory(HANDLE process, PVOID *ret, ULONG_PTR zero_bits, SIZE_T *size_ptr, ULONG type, ULONG protect)
{
    void *p;
    (void)process; (void)zero_bits; (void)type; (void)protect;
    p = mmap(NULL, *size_ptr, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return STATUS_NO_MEMORY;
    *ret = p;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtFreeVirtualMemory(HANDLE process, PVOID *addr, SIZE_T *size_ptr, ULONG type)
{
    (void)process; (void)type;
    munmap(*addr, *size_ptr ? *size_ptr : 4096);
    return STATUS_SUCCESS;
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

/* A tiny NT namespace: the directories below exist, nothing else does. Enough for the path functions that check that a
 * directory is there (RtlSetCurrentDirectory_U, RtlDoesFileExists_U). */
static const WCHAR *const fake_dirs[] = { L"\\??\\C:", L"\\??\\C:\\windows", L"\\??\\C:\\windows\\system32", L"\\??\\C:\\temp" };

static int fake_dir_index(const UNICODE_STRING *name)
{
    unsigned int i, len = name->Length / sizeof(WCHAR);
    while (len && name->Buffer[len - 1] == '\\') len--;
    for (i = 0; i < sizeof(fake_dirs) / sizeof(fake_dirs[0]); i++)
    {
        UNICODE_STRING dir;
        RtlInitUnicodeString(&dir, fake_dirs[i]);
        if (dir.Length / sizeof(WCHAR) == len && !RtlCompareUnicodeStrings(dir.Buffer, len, name->Buffer, len, TRUE)) return i;
    }
    return -1;
}

NTSTATUS WINAPI NtOpenFile(PHANDLE handle, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr, IO_STATUS_BLOCK *io, ULONG share, ULONG options)
{
    int i = fake_dir_index(attr->ObjectName);
    (void)access; (void)io; (void)share; (void)options;
    if (i < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    *handle = (HANDLE)(uintptr_t)(i + 1);
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQueryAttributesFile(const OBJECT_ATTRIBUTES *attr, FILE_BASIC_INFORMATION *info)
{
    if (fake_dir_index(attr->ObjectName) < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    memset(info, 0, sizeof(*info));
    info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQueryVolumeInformationFile(HANDLE handle, IO_STATUS_BLOCK *io, void *buffer, ULONG len, FS_INFORMATION_CLASS cls)
{
    (void)handle; (void)len; (void)cls;
    memset(buffer, 0, sizeof(FILE_FS_DEVICE_INFORMATION));
    ((FILE_FS_DEVICE_INFORMATION *)buffer)->DeviceType = FILE_DEVICE_DISK;
    io->Information = sizeof(FILE_FS_DEVICE_INFORMATION);
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtClose(HANDLE handle) { (void)handle; return STATUS_SUCCESS; }

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

SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, LPCVOID ptr) { return RtlSizeHeap(heap, flags, ptr); }
LPVOID WINAPI HeapReAlloc(HANDLE heap, DWORD flags, LPVOID ptr, SIZE_T size) { return RtlReAllocateHeap(heap, flags, ptr, size); }

/* Win32 environment calls over the Rtl ones, with the ANSI page, as kernel32 does them. */
DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    WCHAR wname[256], wvalue[4096];
    UNICODE_STRING nameU, valueU;
    NTSTATUS status;
    DWORD len;

    MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 256);
    RtlInitUnicodeString(&nameU, wname);
    valueU.Buffer = wvalue;
    valueU.Length = 0;
    valueU.MaximumLength = sizeof(wvalue);
    status = RtlQueryEnvironmentVariable_U(NULL, &nameU, &valueU);
    if (status == STATUS_VARIABLE_NOT_FOUND) { last_error = 203; return 0; }
    if (status) { last_error = RtlNtStatusToDosError(status); return 0; }
    len = WideCharToMultiByte(CP_ACP, 0, wvalue, valueU.Length / sizeof(WCHAR), NULL, 0, NULL, NULL);
    if (len + 1 > size) return len + 1;
    WideCharToMultiByte(CP_ACP, 0, wvalue, valueU.Length / sizeof(WCHAR), buf, size, NULL, NULL);
    buf[len] = 0;
    return len;
}

BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    WCHAR wname[256], wvalue[4096];
    UNICODE_STRING nameU, valueU;
    NTSTATUS status;

    MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 256);
    RtlInitUnicodeString(&nameU, wname);
    if (value)
    {
        MultiByteToWideChar(CP_ACP, 0, value, -1, wvalue, 4096);
        RtlInitUnicodeString(&valueU, wvalue);
        status = RtlSetEnvironmentVariable(NULL, &nameU, &valueU);
    }
    else status = RtlSetEnvironmentVariable(NULL, &nameU, NULL);
    if (status) { last_error = RtlNtStatusToDosError(status); return FALSE; }
    return TRUE;
}


/* The current directory is the one in the process parameters. */
DWORD WINAPI GetCurrentDirectoryW(DWORD size, LPWSTR buf)
{
    return RtlGetCurrentDirectory_U(size * sizeof(WCHAR), buf) / sizeof(WCHAR);
}

DWORD WINAPI GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
    WCHAR wbuf[MAX_PATH];
    DWORD len = GetCurrentDirectoryW(MAX_PATH, wbuf);
    if (len + 1 > size) return len + 1;
    return WideCharToMultiByte(CP_ACP, 0, wbuf, len + 1, buf, size, NULL, NULL) - 1;
}

BOOL WINAPI SetCurrentDirectoryW(LPCWSTR dir)
{
    UNICODE_STRING us;
    NTSTATUS status;
    RtlInitUnicodeString(&us, dir);
    status = RtlSetCurrentDirectory_U(&us);
    if (status) { last_error = RtlNtStatusToDosError(status); return FALSE; }
    return TRUE;
}

BOOL WINAPI SetCurrentDirectoryA(LPCSTR dir)
{
    WCHAR wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, MAX_PATH);
    return SetCurrentDirectoryW(wdir);
}

HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(LONG_PTR)-1; }
void WINAPI RtlRaiseException(EXCEPTION_RECORD *rec) { (void)rec; abort(); }
BOOL WINAPI InitializeAcl(PACL acl, DWORD size, DWORD revision)
{
    NTSTATUS status = RtlCreateAcl(acl, size, revision);
    if (status) { last_error = RtlNtStatusToDosError(status); return FALSE; }
    return TRUE;
}

BOOL WINAPI ConvertSidToStringSidW(PSID sid, LPWSTR *str)
{
    UNICODE_STRING us;
    NTSTATUS status;
    WCHAR *buf;

    us.Buffer = NULL; us.Length = us.MaximumLength = 0;
    status = RtlConvertSidToUnicodeString(&us, sid, TRUE);
    if (status) { last_error = RtlNtStatusToDosError(status); return FALSE; }
    buf = RtlAllocateHeap(NULL, 0, us.Length + sizeof(WCHAR));
    memcpy(buf, us.Buffer, us.Length);
    buf[us.Length / sizeof(WCHAR)] = 0;
    RtlFreeUnicodeString(&us);
    *str = buf;
    return TRUE;
}

/* ntdll_main.c has the real RtlGetVersion (os_identity.h); the host build does not compile that file. */
NTSTATUS WINAPI RtlGetVersion(RTL_OSVERSIONINFOEXW *info)
{
    info->dwMajorVersion = NT_OS_MAJOR_VERSION;
    info->dwMinorVersion = NT_OS_MINOR_VERSION;
    info->dwBuildNumber = NT_OS_BUILD_NUMBER;
    info->dwPlatformId = NT_OS_PLATFORM_ID;
    memset(info->szCSDVersion, 0, sizeof(info->szCSDVersion));
    if (info->dwOSVersionInfoSize == sizeof(RTL_OSVERSIONINFOEXW)) {
        info->wServicePackMajor = info->wServicePackMinor = info->wSuiteMask = 0;
        info->wProductType = NT_OS_PRODUCT_TYPE;
    }
    return STATUS_SUCCESS;
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

/* The other system DLLs the tests look at load, but export nothing: the tests skip what they cannot find. */
static char other_module;

HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
    HMODULE ntdll = GetModuleHandleA(name);
    return ntdll ? ntdll : (HMODULE)&other_module;
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
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (d) winetest_debug = atoi(d);
    RTL_TEST_FUNC();
    printf("%s: %ld successes, %ld failures, %ld skipped, %ld todo\n", RTL_TEST_NAME, winetest_successes,
           winetest_failures, winetest_skipped, winetest_todo_successes);
    return winetest_failures || winetest_todo_failures ? 1 : 0;
}
