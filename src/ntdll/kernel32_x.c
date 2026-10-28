/*
 * kernel32_x.c - process, thread and system functions: TerminateProcess, ExitThread, process and thread times, thread
 * context, affinity, SYSTEM_INFO and memory status, processor features, versions, system directories and the computer
 * name, a fiber local storage built on TLS, FormatMessage, the lstr functions, MulDiv, the Interlocked exports,
 * RaiseException, OutputDebugString, error modes, GetStartupInfo and the code page functions.
 *
 * The rules are in k32_sysinfo_pure.h and k32_err_pure.h.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "k32_sysinfo_pure.h"
#include "k32_err_pure.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif

typedef PVOID LPVOID;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef UINT
#define UINT unsigned int
#endif

#define KX_STATUS_NO_YIELD_PERFORMED ((NTSTATUS)0x40000024L)
#define KX_ERROR_INVALID_HANDLE 6u
#define KX_ERROR_NOT_ENOUGH_MEMORY 8u
#define KX_ERROR_INVALID_PARAMETER 87u
#define KX_ERROR_INSUFFICIENT_BUFFER 122u
#define KX_ERROR_MR_MID_NOT_FOUND 317u
#define KX_ERROR_NO_MORE_ITEMS 259u
#define KX_ERROR_CALL_NOT_IMPLEMENTED 120u
#define KX_ERROR_ENVVAR_NOT_FOUND 203u

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) extern NTSTATUS NtTerminateProcess(HANDLE, NTSTATUS);
__declspec(dllimport) extern NTSTATUS NtTerminateThread(HANDLE, NTSTATUS);
__declspec(dllimport) extern NTSTATUS NtQueryInformationProcess(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtQueryInformationThread(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtSetInformationThread(HANDLE, ULONG, PVOID, ULONG);
__declspec(dllimport) extern NTSTATUS NtSetInformationProcess(HANDLE, ULONG, PVOID, ULONG);
__declspec(dllimport) extern NTSTATUS NtGetContextThread(HANDLE, PCONTEXT);
__declspec(dllimport) extern NTSTATUS NtSetContextThread(HANDLE, PCONTEXT);
__declspec(dllimport) extern NTSTATUS NtQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtYieldExecution(void);
__declspec(dllimport) extern NTSTATUS NtQueryPerformanceCounter(PLARGE_INTEGER, PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtQuerySystemTime(PLARGE_INTEGER);
__declspec(dllimport) extern void NTAPI RtlRaiseException(PEXCEPTION_RECORD);
__declspec(dllimport) extern NTSTATUS NTAPI RtlGetVersion(PVOID);

/* Defined in kernel32.c and its neighbours. */
void SetLastError(DWORD e);
DWORD GetLastError(void);
DWORD WINAPI TlsAlloc(void);
BOOL WINAPI TlsFree(DWORD);
LPVOID WINAPI TlsGetValue(DWORD);
BOOL WINAPI TlsSetValue(DWORD, LPVOID);
HANDLE WINAPI GetProcessHeap(void);
PVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, PVOID p);
HANDLE WINAPI LocalAlloc(UINT flags, SIZE_T bytes);
DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);

static PTEB kx_teb(void)
{
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    return teb;
}

static void kx_status(NTSTATUS s)
{
    SetLastError(NT_SUCCESS(s) ? 0 : RtlNtStatusToDosError(s));
}

/* ---- Processes and threads ---------------------------------------------------------------------------------- */

BOOL WINAPI TerminateProcess(HANDLE process, UINT code)
{
    NTSTATUS s = NtTerminateProcess(process, (NTSTATUS)code);
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return FALSE;
    }
    return TRUE;
}

void WINAPI ExitThread(DWORD code)
{
    NtTerminateThread((HANDLE)(intptr_t)-2, (NTSTATUS)code);
    for (;;)
        __asm__ volatile("hlt");
}

/* KERNEL_USER_TIMES (process class 4, thread class 1) */
typedef struct {
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
} KX_TIMES;

typedef struct { DWORD dwLowDateTime; DWORD dwHighDateTime; } KX_FILETIME;

static void kx_ft(const LARGE_INTEGER *t, KX_FILETIME *ft)
{
    ft->dwLowDateTime = (DWORD)(t->QuadPart & 0xFFFFFFFFu);
    ft->dwHighDateTime = (DWORD)((ULONGLONG)t->QuadPart >> 32);
}

BOOL WINAPI GetProcessTimes(HANDLE process, KX_FILETIME *create, KX_FILETIME *exit_time, KX_FILETIME *kernel, KX_FILETIME *user)
{
    KX_TIMES t;
    NTSTATUS s = NtQueryInformationProcess(process, 4 /* ProcessTimes */, &t, sizeof(t), NULL);
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return FALSE;
    }
    if (create)
        kx_ft(&t.CreateTime, create);
    if (exit_time)
        kx_ft(&t.ExitTime, exit_time);
    if (kernel)
        kx_ft(&t.KernelTime, kernel);
    if (user)
        kx_ft(&t.UserTime, user);
    return TRUE;
}

BOOL WINAPI GetThreadTimes(HANDLE thread, KX_FILETIME *create, KX_FILETIME *exit_time, KX_FILETIME *kernel, KX_FILETIME *user)
{
    KX_TIMES t;
    NTSTATUS s = NtQueryInformationThread(thread, 1 /* ThreadTimes */, &t, sizeof(t), NULL);
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return FALSE;
    }
    if (create)
        kx_ft(&t.CreateTime, create);
    if (exit_time)
        kx_ft(&t.ExitTime, exit_time);
    if (kernel)
        kx_ft(&t.KernelTime, kernel);
    if (user)
        kx_ft(&t.UserTime, user);
    return TRUE;
}

/* the first fields of THREAD_BASIC_INFORMATION and PROCESS_BASIC_INFORMATION */
typedef struct {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
} KX_TBI;

typedef struct {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} KX_PBI;

DWORD WINAPI GetThreadId(HANDLE thread)
{
    KX_TBI b;
    NTSTATUS s;
    if (thread == (HANDLE)(intptr_t)-2) {
        PTEB teb = kx_teb();
        return teb ? (DWORD)(ULONG_PTR)teb->ClientId.UniqueThread : 0;
    }
    s = NtQueryInformationThread(thread, 0, &b, sizeof(b), NULL);
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return 0;
    }
    return (DWORD)(ULONG_PTR)b.UniqueThread;
}

DWORD WINAPI GetProcessIdOfThread(HANDLE thread)
{
    KX_TBI b;
    NTSTATUS s = NtQueryInformationThread(thread, 0, &b, sizeof(b), NULL);
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return 0;
    }
    return (DWORD)(ULONG_PTR)b.UniqueProcess;
}

DWORD WINAPI GetProcessId(HANDLE process)
{
    KX_PBI b;
    NTSTATUS s;
    if (process == (HANDLE)(intptr_t)-1) {
        PTEB teb = kx_teb();
        return teb ? (DWORD)(ULONG_PTR)teb->ClientId.UniqueProcess : 0;
    }
    s = NtQueryInformationProcess(process, 0, &b, sizeof(b), NULL);
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return 0;
    }
    return (DWORD)b.UniqueProcessId;
}

BOOL WINAPI ProcessIdToSessionId(DWORD pid, DWORD *session)
{
    (void)pid;
    if (!session) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *session = 1;
    return TRUE;
}

BOOL WINAPI SwitchToThread(void)
{
    return NtYieldExecution() != KX_STATUS_NO_YIELD_PERFORMED;
}

BOOL WINAPI GetThreadContext(HANDLE thread, PCONTEXT ctx)
{
    NTSTATUS s = NtGetContextThread(thread, ctx);
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI SetThreadContext(HANDLE thread, const CONTEXT *ctx)
{
    NTSTATUS s = NtSetContextThread(thread, (PCONTEXT)ctx);
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return FALSE;
    }
    return TRUE;
}

ULONG_PTR WINAPI SetThreadAffinityMask(HANDLE thread, ULONG_PTR mask)
{
    KX_TBI b;
    NTSTATUS s = NtQueryInformationThread(thread, 0, &b, sizeof(b), NULL);
    ULONG_PTR old;
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return 0;
    }
    old = b.AffinityMask;
    s = NtSetInformationThread(thread, 4 /* ThreadAffinityMask */, &mask, sizeof(mask));
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return 0;
    }
    return old ? old : 1;
}

BOOL WINAPI GetProcessAffinityMask(HANDLE process, ULONG_PTR *proc_mask, ULONG_PTR *sys_mask)
{
    KX_PBI b;
    NTSTATUS s = NtQueryInformationProcess(process, 0, &b, sizeof(b), NULL);
    ULONG_PTR mask;
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return FALSE;
    }
    mask = b.AffinityMask ? b.AffinityMask : 1;
    if (proc_mask)
        *proc_mask = mask;
    if (sys_mask)
        *sys_mask = mask;
    return TRUE;
}

BOOL WINAPI SetProcessAffinityMask(HANDLE process, ULONG_PTR mask)
{
    NTSTATUS s = NtSetInformationProcess(process, 21 /* ProcessAffinityMask */, &mask, sizeof(mask));
    if (!NT_SUCCESS(s)) {
        kx_status(s);
        return FALSE;
    }
    return TRUE;
}

DWORD WINAPI SetThreadIdealProcessor(HANDLE thread, DWORD ideal)
{
    (void)thread;
    (void)ideal;
    return 0;
}

DWORD WINAPI GetCurrentProcessorNumber(void)
{
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return aux & 0xFFFu; /* the processor number is in the low bits of TSC_AUX */
}

BOOL WINAPI SetThreadPriorityBoost(HANDLE thread, BOOL disable)
{
    (void)thread;
    (void)disable;
    return TRUE;
}

BOOL WINAPI GetThreadPriorityBoost(HANDLE thread, BOOL *disabled)
{
    (void)thread;
    if (disabled)
        *disabled = FALSE;
    return TRUE;
}

BOOL WINAPI SetThreadStackGuarantee(ULONG *size)
{
    (void)size;
    return TRUE;
}

BOOL WINAPI FlushInstructionCache(HANDLE process, const void *base, SIZE_T size)
{
    (void)process;
    (void)base;
    (void)size;
    return TRUE; /* x86 keeps the instruction cache coherent */
}

LONG WINAPI SetThreadDescription(HANDLE thread, const WCHAR *desc)
{
    (void)thread;
    (void)desc;
    return 0; /* S_OK; the name is not kept */
}

LONG WINAPI GetThreadDescription(HANDLE thread, WCHAR **desc)
{
    WCHAR *p = (WCHAR *)LocalAlloc(0x40 /* LPTR */, sizeof(WCHAR));
    (void)thread;
    if (!p)
        return (LONG)0x8007000E; /* E_OUTOFMEMORY */
    *desc = p;
    return 0;
}

/* ---- Fiber local storage, on thread local storage -------------------------------------------------------------- */

typedef void(WINAPI *KX_FLS_CALLBACK)(void *);
static KX_FLS_CALLBACK g_kx_fls_callbacks[64];

DWORD WINAPI FlsAlloc(KX_FLS_CALLBACK callback)
{
    DWORD idx = TlsAlloc();
    if (idx == 0xFFFFFFFFu)
        return idx; /* the last error is set by TlsAlloc */
    if (idx < 64)
        g_kx_fls_callbacks[idx] = callback;
    return idx;
}

BOOL WINAPI FlsFree(DWORD idx)
{
    LPVOID v = TlsGetValue(idx);
    if (idx < 64 && g_kx_fls_callbacks[idx] && v) {
        KX_FLS_CALLBACK cb = g_kx_fls_callbacks[idx];
        g_kx_fls_callbacks[idx] = NULL;
        TlsSetValue(idx, NULL);
        cb(v); /* only the calling thread's value is released here */
    }
    return TlsFree(idx);
}

PVOID WINAPI FlsGetValue(DWORD idx)
{
    PVOID v = TlsGetValue(idx);
    SetLastError(0);
    return v;
}

BOOL WINAPI FlsSetValue(DWORD idx, PVOID value)
{
    return TlsSetValue(idx, value);
}

/* ---- System information --------------------------------------------------------------------------------------- */

static void kx_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "0"(leaf), "2"(sub));
}

static void kx_fill_system_info(K32SI_SYSTEM_INFO *si)
{
    K32SI_BASIC basic;
    uint32_t a, b, c, d;
    NTSTATUS s;
    memset(&basic, 0, sizeof(basic));
    {
        BYTE buf[64];
        memset(buf, 0, sizeof(buf));
        s = NtQuerySystemInformation(0, buf, sizeof(buf), NULL);
        if (NT_SUCCESS(s))
            memcpy(&basic, buf, sizeof(basic) < sizeof(buf) ? sizeof(basic) : sizeof(buf));
    }
    kx_cpuid(1, 0, &a, &b, &c, &d);
    k32si_fill(si, NT_SUCCESS(s) ? &basic : NULL, 0, k32si_cpu_level(a), k32si_cpu_revision(a));
}

void WINAPI GetSystemInfo(K32SI_SYSTEM_INFO *si)
{
    if (si)
        kx_fill_system_info(si);
}

void WINAPI GetNativeSystemInfo(K32SI_SYSTEM_INFO *si)
{
    if (si)
        kx_fill_system_info(si);
}

BOOL WINAPI GlobalMemoryStatusEx(K32SI_MEMORYSTATUSEX *m)
{
    K32SI_SYSTEM_INFO si;
    BYTE buf[64];
    ULONG pages = 1048576, page = 4096;
    if (!m || m->dwLength < sizeof(*m)) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(buf, 0, sizeof(buf));
    if (NT_SUCCESS(NtQuerySystemInformation(0, buf, sizeof(buf), NULL))) {
        K32SI_BASIC *b = (K32SI_BASIC *)buf;
        if (b->NumberOfPhysicalPages)
            pages = b->NumberOfPhysicalPages;
        if (b->PageSize)
            page = b->PageSize;
    }
    kx_fill_system_info(&si);
    k32si_memory_status(m, pages, page, pages / 2, (ULONGLONG)(ULONG_PTR)si.lpMaximumApplicationAddress);
    return TRUE;
}

typedef struct {
    DWORD dwLength, dwMemoryLoad;
    SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} KX_MEMORYSTATUS;

void WINAPI GlobalMemoryStatus(KX_MEMORYSTATUS *m)
{
    K32SI_MEMORYSTATUSEX ex;
    ex.dwLength = sizeof(ex);
    if (!m || !GlobalMemoryStatusEx(&ex))
        return;
    m->dwLength = sizeof(*m);
    m->dwMemoryLoad = ex.dwMemoryLoad;
    m->dwTotalPhys = (SIZE_T)ex.ullTotalPhys;
    m->dwAvailPhys = (SIZE_T)ex.ullAvailPhys;
    m->dwTotalPageFile = (SIZE_T)ex.ullTotalPageFile;
    m->dwAvailPageFile = (SIZE_T)ex.ullAvailPageFile;
    m->dwTotalVirtual = (SIZE_T)ex.ullTotalVirtual;
    m->dwAvailVirtual = (SIZE_T)ex.ullAvailVirtual;
}

BOOL WINAPI GetPhysicallyInstalledSystemMemory(ULONGLONG *kb)
{
    K32SI_MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    if (!kb || !GlobalMemoryStatusEx(&m)) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *kb = m.ullTotalPhys / 1024;
    return TRUE;
}

SIZE_T WINAPI GetLargePageMinimum(void)
{
    return 2u * 1024u * 1024u;
}

BOOL WINAPI IsProcessorFeaturePresent(DWORD feature)
{
    uint32_t a, b, c1, d1, c, d, b7 = 0, d_ext = 0, max;
    kx_cpuid(0, 0, &max, &b, &c, &d);
    kx_cpuid(1, 0, &a, &b, &c1, &d1);
    if (max >= 7) {
        kx_cpuid(7, 0, &a, &b7, &c, &d);
    }
    kx_cpuid(0x80000001u, 0, &a, &b, &c, &d_ext);
    return k32si_feature(feature, c1, d1, b7, d_ext) ? TRUE : FALSE;
}

DWORD WINAPI GetActiveProcessorCount(WORD group)
{
    K32SI_SYSTEM_INFO si;
    (void)group;
    kx_fill_system_info(&si);
    return si.dwNumberOfProcessors;
}

WORD WINAPI GetActiveProcessorGroupCount(void)
{
    return 1;
}

DWORD WINAPI GetMaximumProcessorCount(WORD group)
{
    return GetActiveProcessorCount(group);
}

BOOL WINAPI GetSystemTimes(KX_FILETIME *idle, KX_FILETIME *kernel, KX_FILETIME *user)
{
    LARGE_INTEGER now;
    NtQuerySystemTime(&now);
    /* the times are not tracked per system here; report time since the epoch of the clock as user time */
    if (idle) {
        idle->dwLowDateTime = idle->dwHighDateTime = 0;
    }
    if (kernel) {
        kernel->dwLowDateTime = kernel->dwHighDateTime = 0;
    }
    if (user)
        kx_ft(&now, user);
    return TRUE;
}

BOOL WINAPI QueryUnbiasedInterruptTime(ULONGLONG *t)
{
    LARGE_INTEGER c, f;
    if (!t) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!NT_SUCCESS(NtQueryPerformanceCounter(&c, &f)) || f.QuadPart == 0)
        return FALSE;
    *t = (ULONGLONG)c.QuadPart / (ULONGLONG)f.QuadPart * 10000000ull + (ULONGLONG)c.QuadPart % (ULONGLONG)f.QuadPart * 10000000ull / (ULONGLONG)f.QuadPart;
    return TRUE;
}

void WINAPI QueryInterruptTime(ULONGLONG *t)
{
    QueryUnbiasedInterruptTime(t);
}

/* ---- Versions, directories and names ------------------------------------------------------------------------- */

typedef struct {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    WCHAR szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} KX_OSVERSIONINFOEXW;

DWORD WINAPI GetVersion(void)
{
    KX_OSVERSIONINFOEXW v;
    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    RtlGetVersion(&v);
    return k32si_packed_version(v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}

BOOL WINAPI GetVersionExW(KX_OSVERSIONINFOEXW *v)
{
    DWORD size;
    KX_OSVERSIONINFOEXW tmp;
    if (!v) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    size = v->dwOSVersionInfoSize;
    if (size != 148 /* OSVERSIONINFOW */ && size != sizeof(KX_OSVERSIONINFOEXW)) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.dwOSVersionInfoSize = sizeof(tmp);
    RtlGetVersion(&tmp);
    v->dwMajorVersion = tmp.dwMajorVersion;
    v->dwMinorVersion = tmp.dwMinorVersion;
    v->dwBuildNumber = tmp.dwBuildNumber;
    v->dwPlatformId = tmp.dwPlatformId;
    memset(v->szCSDVersion, 0, sizeof(v->szCSDVersion));
    if (size == sizeof(KX_OSVERSIONINFOEXW)) {
        v->wServicePackMajor = 0;
        v->wServicePackMinor = 0;
        v->wSuiteMask = 0;
        v->wProductType = tmp.wProductType;
    }
    return TRUE;
}

typedef struct {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    char szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} KX_OSVERSIONINFOEXA;

BOOL WINAPI GetVersionExA(KX_OSVERSIONINFOEXA *v)
{
    KX_OSVERSIONINFOEXW w;
    DWORD size;
    if (!v) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    size = v->dwOSVersionInfoSize;
    if (size != 148 && size != sizeof(KX_OSVERSIONINFOEXA)) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(&w, 0, sizeof(w));
    w.dwOSVersionInfoSize = sizeof(w);
    if (!GetVersionExW(&w))
        return FALSE;
    v->dwMajorVersion = w.dwMajorVersion;
    v->dwMinorVersion = w.dwMinorVersion;
    v->dwBuildNumber = w.dwBuildNumber;
    v->dwPlatformId = w.dwPlatformId;
    memset(v->szCSDVersion, 0, sizeof(v->szCSDVersion));
    if (size == sizeof(KX_OSVERSIONINFOEXA))
        v->wProductType = w.wProductType;
    return TRUE;
}

BOOL WINAPI IsWow64Process(HANDLE process, BOOL *wow64)
{
    (void)process;
    if (!wow64) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *wow64 = FALSE;
    return TRUE;
}

BOOL WINAPI IsWow64Process2(HANDLE process, USHORT *process_machine, USHORT *native_machine)
{
    (void)process;
    if (process_machine)
        *process_machine = 0; /* IMAGE_FILE_MACHINE_UNKNOWN: not a WOW64 process */
    if (native_machine)
        *native_machine = 0x8664;
    return TRUE;
}

UINT WINAPI GetSystemDirectoryW(WCHAR *buf, UINT cap)
{
    return k32si_copy_name(K32SI_SYSTEM_DIR, buf, cap);
}

UINT WINAPI GetSystemDirectoryA(char *buf, UINT cap)
{
    DWORD n = (DWORD)sizeof(K32SI_SYSTEM_DIR) - 1;
    if (n + 1 > cap)
        return n + 1;
    memcpy(buf, K32SI_SYSTEM_DIR, n + 1);
    return n;
}

UINT WINAPI GetWindowsDirectoryW(WCHAR *buf, UINT cap)
{
    return k32si_copy_name(K32SI_WINDOWS_DIR, buf, cap);
}

UINT WINAPI GetWindowsDirectoryA(char *buf, UINT cap)
{
    DWORD n = (DWORD)sizeof(K32SI_WINDOWS_DIR) - 1;
    if (n + 1 > cap)
        return n + 1;
    memcpy(buf, K32SI_WINDOWS_DIR, n + 1);
    return n;
}

UINT WINAPI GetSystemWindowsDirectoryW(WCHAR *buf, UINT cap)
{
    return GetWindowsDirectoryW(buf, cap);
}

UINT WINAPI GetSystemWindowsDirectoryA(char *buf, UINT cap)
{
    return GetWindowsDirectoryA(buf, cap);
}

UINT WINAPI GetSystemWow64DirectoryW(WCHAR *buf, UINT cap)
{
    (void)buf;
    (void)cap;
    SetLastError(KX_ERROR_CALL_NOT_IMPLEMENTED); /* no WOW64 here */
    return 0;
}

BOOL WINAPI GetComputerNameW(WCHAR *buf, DWORD *size)
{
    WCHAR host[64];
    char narrow[64], name[32];
    DWORD n = 0, len;
    static const WCHAR var[] = {'C', 'O', 'M', 'P', 'U', 'T', 'E', 'R', 'N', 'A', 'M', 'E', 0};
    if (!buf || !size) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    len = GetEnvironmentVariableW(var, host, 64);
    if (len > 0 && len < 64) {
        for (; n < len && n < 63; ++n)
            narrow[n] = host[n] < 0x80 ? (char)host[n] : '_';
    }
    narrow[n] = 0;
    len = k32si_computer_name(narrow, name, sizeof(name));
    if (len + 1 > *size) {
        *size = len + 1;
        SetLastError(234u); /* ERROR_MORE_DATA */
        return FALSE;
    }
    for (DWORD i = 0; i <= len; ++i)
        buf[i] = (WCHAR)(unsigned char)name[i];
    *size = len;
    return TRUE;
}

BOOL WINAPI GetComputerNameA(char *buf, DWORD *size)
{
    WCHAR w[32];
    DWORD n = 32;
    if (!buf || !size) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!GetComputerNameW(w, &n))
        return FALSE;
    if (n + 1 > *size) {
        *size = n + 1;
        SetLastError(234u);
        return FALSE;
    }
    for (DWORD i = 0; i <= n; ++i)
        buf[i] = (char)w[i];
    *size = n;
    return TRUE;
}

/* ---- String helpers and arithmetic ---------------------------------------------------------------------------- */

int WINAPI lstrlenA(const char *s)
{
    int n = 0;
    if (!s)
        return 0;
    while (s[n])
        ++n;
    return n;
}

int WINAPI lstrlenW(const WCHAR *s)
{
    int n = 0;
    if (!s)
        return 0;
    while (s[n])
        ++n;
    return n;
}

char *WINAPI lstrcpyA(char *d, const char *s)
{
    char *r = d;
    if (!d || !s)
        return NULL;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}

WCHAR *WINAPI lstrcpyW(WCHAR *d, const WCHAR *s)
{
    WCHAR *r = d;
    if (!d || !s)
        return NULL;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}

char *WINAPI lstrcpynA(char *d, const char *s, int n)
{
    int i = 0;
    if (!d || !s || n <= 0)
        return d;
    for (; i < n - 1 && s[i]; ++i)
        d[i] = s[i];
    d[i] = 0;
    return d;
}

WCHAR *WINAPI lstrcpynW(WCHAR *d, const WCHAR *s, int n)
{
    int i = 0;
    if (!d || !s || n <= 0)
        return d;
    for (; i < n - 1 && s[i]; ++i)
        d[i] = s[i];
    d[i] = 0;
    return d;
}

char *WINAPI lstrcatA(char *d, const char *s)
{
    char *r = d;
    if (!d || !s)
        return NULL;
    while (*d)
        ++d;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}

WCHAR *WINAPI lstrcatW(WCHAR *d, const WCHAR *s)
{
    WCHAR *r = d;
    if (!d || !s)
        return NULL;
    while (*d)
        ++d;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}

static int kx_fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int WINAPI lstrcmpA(const char *a, const char *b)
{
    if (!a || !b)
        return a == b ? 0 : (a ? 1 : -1);
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a < (unsigned char)*b ? -1 : (unsigned char)*a > (unsigned char)*b ? 1 : 0;
}

int WINAPI lstrcmpiA(const char *a, const char *b)
{
    if (!a || !b)
        return a == b ? 0 : (a ? 1 : -1);
    while (*a && kx_fold((unsigned char)*a) == kx_fold((unsigned char)*b)) {
        ++a;
        ++b;
    }
    {
        int x = kx_fold((unsigned char)*a), y = kx_fold((unsigned char)*b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
}

int WINAPI lstrcmpW(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b)
        return a == b ? 0 : (a ? 1 : -1);
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a < *b ? -1 : *a > *b ? 1 : 0;
}

int WINAPI lstrcmpiW(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b)
        return a == b ? 0 : (a ? 1 : -1);
    while (*a && kx_fold(*a) == kx_fold(*b)) {
        ++a;
        ++b;
    }
    {
        int x = kx_fold(*a), y = kx_fold(*b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
}

/* MulDiv: a*b/c rounded to nearest (halves away from zero); -1 for a zero divisor or a result that does not fit. */
int WINAPI MulDiv(int a, int b, int c)
{
    long long p, half, r;
    if (c == 0)
        return -1;
    p = (long long)a * b;
    half = c < 0 ? -(long long)c / 2 : (long long)c / 2;
    if ((p < 0) != (c < 0))
        r = (p - half) / c;
    else
        r = (p + half) / c;
    if (r > 2147483647ll || r < -2147483648ll)
        return -1;
    return (int)r;
}

LONG WINAPI InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
LONG WINAPI InterlockedDecrement(volatile LONG *p) { return __sync_sub_and_fetch(p, 1); }
LONG WINAPI InterlockedExchange(volatile LONG *p, LONG v) { return __sync_lock_test_and_set(p, v); }
LONG WINAPI InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __sync_fetch_and_add(p, v); }
LONG WINAPI InterlockedCompareExchange(volatile LONG *p, LONG exch, LONG comp) { return __sync_val_compare_and_swap(p, comp, exch); }
LONGLONG WINAPI InterlockedIncrement64(volatile LONGLONG *p) { return __sync_add_and_fetch(p, 1); }
LONGLONG WINAPI InterlockedDecrement64(volatile LONGLONG *p) { return __sync_sub_and_fetch(p, 1); }
LONGLONG WINAPI InterlockedExchange64(volatile LONGLONG *p, LONGLONG v) { return __sync_lock_test_and_set(p, v); }
LONGLONG WINAPI InterlockedExchangeAdd64(volatile LONGLONG *p, LONGLONG v) { return __sync_fetch_and_add(p, v); }
LONGLONG WINAPI InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG exch, LONGLONG comp) { return __sync_val_compare_and_swap(p, comp, exch); }

/* ---- Exceptions, debugging, error mode -------------------------------------------------------------------------- */

void WINAPI RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags & 1u; /* only EXCEPTION_NONCONTINUABLE is kept */
    if (nargs > 15)
        nargs = 15;
    rec.NumberParameters = args ? nargs : 0;
    for (DWORD i = 0; args && i < nargs; ++i)
        rec.ExceptionInformation[i] = args[i];
    rec.ExceptionAddress = __builtin_return_address(0);
    RtlRaiseException(&rec);
}

void WINAPI DebugBreak(void)
{
    __asm__ volatile("int3");
}

void WINAPI OutputDebugStringA(const char *s)
{
    (void)s; /* no debugger channel here: the text is dropped */
}

void WINAPI OutputDebugStringW(const WCHAR *s)
{
    (void)s;
}

static UINT g_kx_error_mode;

UINT WINAPI SetErrorMode(UINT mode)
{
    UINT old = g_kx_error_mode;
    g_kx_error_mode = mode;
    return old;
}

UINT WINAPI GetErrorMode(void)
{
    return g_kx_error_mode;
}

BOOL WINAPI SetThreadErrorMode(DWORD mode, DWORD *old)
{
    if (old)
        *old = g_kx_error_mode;
    g_kx_error_mode = mode;
    return TRUE;
}

DWORD WINAPI GetThreadErrorMode(void)
{
    return g_kx_error_mode;
}

BOOL WINAPI IsBadReadPtr(const void *p, ULONG_PTR n)
{
    volatile const char *q = (volatile const char *)p;
    (void)q;
    return (!p && n) ? TRUE : FALSE; /* no fault handling to probe with: only a null pointer is judged bad */
}

BOOL WINAPI IsBadWritePtr(void *p, ULONG_PTR n)
{
    return (!p && n) ? TRUE : FALSE;
}

/* ---- GetStartupInfo ---------------------------------------------------------------------------------------------- */

typedef struct {
    DWORD cb;
    WCHAR *lpReserved;
    WCHAR *lpDesktop;
    WCHAR *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} KX_STARTUPINFOW;

void WINAPI GetStartupInfoW(KX_STARTUPINFOW *si)
{
    PTEB teb = kx_teb();
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    RTL_USER_PROCESS_PARAMETERS *pp = peb ? peb->ProcessParameters : NULL;
    if (!si)
        return;
    memset(si, 0, sizeof(*si));
    si->cb = sizeof(*si);
    if (pp) {
        si->lpDesktop = pp->DesktopInfo.Buffer;
        si->lpTitle = pp->WindowTitle.Buffer;
        si->dwX = pp->StartingX;
        si->dwY = pp->StartingY;
        si->dwXSize = pp->CountX;
        si->dwYSize = pp->CountY;
        si->dwXCountChars = pp->CountCharsX;
        si->dwYCountChars = pp->CountCharsY;
        si->dwFillAttribute = pp->FillAttribute;
        si->dwFlags = pp->WindowFlags;
        si->wShowWindow = (WORD)pp->ShowWindowFlags;
        si->hStdInput = pp->StandardInput;
        si->hStdOutput = pp->StandardOutput;
        si->hStdError = pp->StandardError;
    }
}

/* ---- Code pages -------------------------------------------------------------------------------------------------- */

UINT WINAPI GetACP(void)
{
    return 1252;
}

UINT WINAPI GetOEMCP(void)
{
    return 437;
}

typedef struct {
    UINT MaxCharSize;
    BYTE DefaultChar[2];
    BYTE LeadByte[12];
} KX_CPINFO;

BOOL WINAPI GetCPInfo(UINT cp, KX_CPINFO *info)
{
    if (!info) {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (cp == 0 || cp == 1 || cp == 2 || cp == 3 || cp == 437 || cp == 1252 || cp == 28591 || cp == 20127) {
        memset(info, 0, sizeof(*info));
        info->MaxCharSize = 1;
        info->DefaultChar[0] = '?';
        return TRUE;
    }
    if (cp == 65001) {
        memset(info, 0, sizeof(*info));
        info->MaxCharSize = 4;
        info->DefaultChar[0] = '?';
        return TRUE;
    }
    SetLastError(KX_ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL WINAPI IsValidCodePage(UINT cp)
{
    return (cp == 437 || cp == 1252 || cp == 28591 || cp == 20127 || cp == 65001 || cp == 1200) ? TRUE : FALSE;
}

BOOL WINAPI IsDBCSLeadByte(BYTE b)
{
    (void)b;
    return FALSE;
}

/* ---- FormatMessage ------------------------------------------------------------------------------------------------ */

static const WCHAR *kx_widen_system(DWORD code, WCHAR *tmp, size_t cap)
{
    const char *t = k32e_lookup(code);
    size_t i = 0;
    if (!t)
        return NULL;
    for (; t[i] && i + 1 < cap; ++i)
        tmp[i] = (WCHAR)(unsigned char)t[i];
    tmp[i] = 0;
    return tmp;
}

/* The common core. The insert values come from an array or, when the caller gave a va_list, are read in order up to the
 * highest insert number the format uses. Returns the characters stored (without the NUL), or 0 with the last error. */
static DWORD kx_format_message(DWORD flags, const void *source, DWORD msg_id, void *buf, DWORD size, const ULONG_PTR *array, va_list *va, int narrow)
{
    WCHAR sysbuf[600];
    const WCHAR *fmt;
    WCHAR *wsource_copy = NULL;
    ULONG_PTR args[100];
    size_t nargs = 0;
    WCHAR out[4096];
    k32e_out o;
    int stopped = 0, add_break = 0;
    DWORD err;
    (void)wsource_copy;
    if (flags & K32E_FORMAT_MESSAGE_FROM_STRING) {
        if (!source) {
            SetLastError(KX_ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (narrow) {
            const char *s = (const char *)source;
            size_t n = 0;
            while (s[n] && n < 4000)
                ++n;
            wsource_copy = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));
            if (!wsource_copy) {
                SetLastError(KX_ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            for (size_t i = 0; i <= n; ++i)
                wsource_copy[i] = (WCHAR)(unsigned char)s[i];
            fmt = wsource_copy;
        } else {
            fmt = (const WCHAR *)source;
        }
    } else if (flags & K32E_FORMAT_MESSAGE_FROM_SYSTEM) {
        fmt = kx_widen_system(msg_id, sysbuf, sizeof(sysbuf) / sizeof(sysbuf[0]));
        if (!fmt) {
            SetLastError(KX_ERROR_MR_MID_NOT_FOUND);
            return 0;
        }
        add_break = 1;
    } else {
        SetLastError(KX_ERROR_INVALID_PARAMETER);
        return 0;
    }
    memset(args, 0, sizeof(args));
    if (!(flags & K32E_FORMAT_MESSAGE_IGNORE_INSERTS)) {
        if (array) {
            nargs = 99;
            for (size_t i = 0; i < nargs; ++i)
                args[i] = array[i];
        } else if (va) {
            /* the highest insert number used */
            size_t maxn = 0;
            for (const WCHAR *p = fmt; *p; ++p) {
                if (*p == '%' && p[1] >= '1' && p[1] <= '9') {
                    size_t v = 0;
                    const WCHAR *q = p + 1;
                    while (*q >= '0' && *q <= '9') {
                        v = v * 10 + (size_t)(*q - '0');
                        ++q;
                    }
                    if (v > maxn && v < 100)
                        maxn = v;
                }
            }
            for (size_t i = 0; i < maxn; ++i)
                args[i] = va_arg(*va, ULONG_PTR);
            nargs = maxn;
        } else {
            flags |= K32E_FORMAT_MESSAGE_IGNORE_INSERTS; /* no arguments were given: keep the sequences */
        }
    }
    o.buf = out;
    o.cap = sizeof(out) / sizeof(out[0]) - 3;
    o.n = 0;
    o.overflow = 0;
    o.line = 0;
    err = k32e_format(&o, fmt, flags, args, nargs, narrow, &stopped);
    if (err) {
        SetLastError(err);
        if (wsource_copy)
            HeapFree(GetProcessHeap(), 0, wsource_copy);
        return 0;
    }
    if (add_break && !stopped) {
        k32e_put(&o, '\r');
        k32e_put(&o, '\n');
    }
    if (wsource_copy)
        HeapFree(GetProcessHeap(), 0, wsource_copy);
    if (o.overflow) {
        SetLastError(KX_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (flags & K32E_FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        size_t unit = narrow ? 1 : sizeof(WCHAR);
        void *p = LocalAlloc(0x40, (o.n + 1 > size ? o.n + 1 : size) * unit);
        if (!p) {
            SetLastError(KX_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        if (narrow) {
            char *c = (char *)p;
            for (size_t i = 0; i < o.n; ++i)
                c[i] = out[i] <= 0xFF ? (char)out[i] : '?';
            c[o.n] = 0;
        } else {
            memcpy(p, out, o.n * sizeof(WCHAR));
            ((WCHAR *)p)[o.n] = 0;
        }
        *(void **)buf = p;
    } else {
        if (!buf || o.n + 1 > size) {
            SetLastError(KX_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (narrow) {
            char *c = (char *)buf;
            for (size_t i = 0; i < o.n; ++i)
                c[i] = out[i] <= 0xFF ? (char)out[i] : '?';
            c[o.n] = 0;
        } else {
            memcpy(buf, out, o.n * sizeof(WCHAR));
            ((WCHAR *)buf)[o.n] = 0;
        }
    }
    SetLastError(0);
    return (DWORD)o.n;
}

DWORD WINAPI FormatMessageW(DWORD flags, const void *source, DWORD id, DWORD lang, WCHAR *buf, DWORD size, va_list *args)
{
    (void)lang;
    return kx_format_message(flags, source, id, buf, size, (flags & K32E_FORMAT_MESSAGE_ARGUMENT_ARRAY) ? (const ULONG_PTR *)args : NULL,
                             (flags & K32E_FORMAT_MESSAGE_ARGUMENT_ARRAY) ? NULL : args, 0);
}

DWORD WINAPI FormatMessageA(DWORD flags, const void *source, DWORD id, DWORD lang, char *buf, DWORD size, va_list *args)
{
    (void)lang;
    return kx_format_message(flags, source, id, buf, size, (flags & K32E_FORMAT_MESSAGE_ARGUMENT_ARRAY) ? (const ULONG_PTR *)args : NULL,
                             (flags & K32E_FORMAT_MESSAGE_ARGUMENT_ARRAY) ? NULL : args, 1);
}

#endif /* __MINGW32__ */
