/*
 * kernel32_y.c - pointer encoding, version verification, processor topology (logical processor information, NUMA and
 * processor groups), power status and a few process settings.
 *
 * The rules are in k32_y_pure.h and are host-tested; this file reads CPUID and the NT information classes and calls them.
 * There is one processor group of at most 64 processors and one NUMA node.
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
#include "k32_y_pure.h"
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
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define KY_ERROR_INVALID_PARAMETER 87u
#define KY_ERROR_INSUFFICIENT_BUFFER 122u
#define KY_ERROR_INVALID_HANDLE 6u
#define KY_APPMODEL_ERROR_NO_PACKAGE 15700u

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) extern NTSTATUS NtQuerySystemTime(PLARGE_INTEGER);
__declspec(dllimport) extern NTSTATUS NtSetInformationObject(HANDLE, ULONG, PVOID, ULONG);
__declspec(dllimport) extern NTSTATUS NtSetInformationProcess(HANDLE, ULONG, PVOID, ULONG);
__declspec(dllimport) extern NTSTATUS NtQueryInformationProcess(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NTAPI RtlGetVersion(PVOID);

void SetLastError(DWORD e);
DWORD WINAPI GetActiveProcessorCount(WORD group);
DWORD WINAPI GetCurrentProcessorNumber(void);
ULONG_PTR WINAPI SetThreadAffinityMask(HANDLE thread, ULONG_PTR mask);
HANDLE WINAPI GetCurrentThread(void);
HANDLE WINAPI GetCurrentProcess(void);

static void ky_status(NTSTATUS s)
{
    SetLastError(NT_SUCCESS(s) ? 0 : RtlNtStatusToDosError(s));
}

/* ---- Pointer encoding ---------------------------------------------------------------------------------------------------- */

/* The process cookie: unpredictable per process, fixed afterwards. Without a kernel-provided one, it comes from the
 * system time and the addresses of the TEB and of this variable. */
static uint64_t g_ky_cookie;

static uint64_t ky_cookie(void)
{
    if (!g_ky_cookie) {
        LARGE_INTEGER t;
        PTEB teb;
        uint64_t c;
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
        NtQuerySystemTime(&t);
        c = (uint64_t)t.QuadPart * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)teb ^ ((uint64_t)(uintptr_t)&g_ky_cookie << 17);
        if (!c)
            c = 0x4E54505F434B4945ull;
        g_ky_cookie = c;
    }
    return g_ky_cookie;
}

PVOID WINAPI EncodePointer(PVOID p) { return (PVOID)(uintptr_t)k32y_encode_pointer((uint64_t)(uintptr_t)p, ky_cookie()); }
PVOID WINAPI DecodePointer(PVOID p) { return (PVOID)(uintptr_t)k32y_decode_pointer((uint64_t)(uintptr_t)p, ky_cookie()); }
PVOID WINAPI EncodeSystemPointer(PVOID p) { return EncodePointer(p); }
PVOID WINAPI DecodeSystemPointer(PVOID p) { return DecodePointer(p); }

/* ---- Version verification ------------------------------------------------------------------------------------------------- */

typedef struct {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    WCHAR szCSDVersion[128];
    WORD wServicePackMajor, wServicePackMinor, wSuiteMask;
    BYTE wProductType, wReserved;
} KY_OSVERSIONINFOEXW;

ULONGLONG WINAPI VerSetConditionMask(ULONGLONG mask, DWORD type_mask, BYTE condition)
{
    return k32y_set_condition(mask, type_mask, condition);
}

BOOL WINAPI VerifyVersionInfoW(KY_OSVERSIONINFOEXW *want, DWORD type_mask, ULONGLONG mask)
{
    KY_OSVERSIONINFOEXW have;
    k32y_version a, w;
    int r;
    if (!want || !type_mask || want->dwOSVersionInfoSize < sizeof(KY_OSVERSIONINFOEXW) - 0) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(&have, 0, sizeof have);
    have.dwOSVersionInfoSize = sizeof have;
    RtlGetVersion(&have);
    a.major = have.dwMajorVersion;
    a.minor = have.dwMinorVersion;
    a.build = have.dwBuildNumber;
    a.platform = have.dwPlatformId;
    a.sp_major = have.wServicePackMajor;
    a.sp_minor = have.wServicePackMinor;
    a.suite = have.wSuiteMask;
    a.product_type = have.wProductType;
    w.major = want->dwMajorVersion;
    w.minor = want->dwMinorVersion;
    w.build = want->dwBuildNumber;
    w.platform = want->dwPlatformId;
    w.sp_major = want->wServicePackMajor;
    w.sp_minor = want->wServicePackMinor;
    w.suite = want->wSuiteMask;
    w.product_type = want->wProductType;
    r = k32y_verify(&a, &w, type_mask, mask);
    if (r < 0) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!r)
        SetLastError(1150u); /* ERROR_OLD_WIN_VERSION */
    return r ? TRUE : FALSE;
}

/* ---- Processor topology ----------------------------------------------------------------------------------------------------- */

static void ky_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

#define KY_MAX_CACHES 8

/* The topology of this machine: the logical processor count from the system, SMT and the caches from CPUID leaf 4 (Intel
 * and AMD both report it); a CPU without leaf 4 reports no caches. */
static unsigned ky_topology(k32y_topology *t, k32y_cache *caches)
{
    uint32_t r[4];
    unsigned n = 0, logical = GetActiveProcessorCount(0), max_leaf;
    ky_cpuid(0, 0, r);
    max_leaf = r[0];
    t->logical = logical > 64 ? 64 : (logical ? logical : 1);
    ky_cpuid(1, 0, r);
    t->smt = (r[3] & (1u << 28)) && t->logical >= 2 && ((r[1] >> 16) & 0xFF) >= 2 ? 2u : 1u;
    if (max_leaf >= 4) {
        for (unsigned sub = 0; sub < 16 && n < KY_MAX_CACHES; ++sub) {
            unsigned type, level, ways, parts, line, sets, sharing;
            ky_cpuid(4, sub, r);
            type = r[0] & 31u;
            if (!type)
                break;
            level = (r[0] >> 5) & 7u;
            sharing = ((r[0] >> 14) & 0xFFFu) + 1;
            ways = (r[1] >> 22) + 1;
            parts = ((r[1] >> 12) & 0x3FFu) + 1;
            line = (r[1] & 0xFFFu) + 1;
            sets = r[2] + 1;
            caches[n].level = (uint8_t)level;
            caches[n].associativity = (uint8_t)(ways > 255 ? 255 : ways);
            caches[n].line_size = (uint16_t)line;
            caches[n].size = ways * parts * line * sets;
            caches[n].type = type == 1 ? K32Y_CACHE_DATA : type == 2 ? K32Y_CACHE_INSTRUCTION : K32Y_CACHE_UNIFIED;
            caches[n].shared_logical = sharing > t->logical ? t->logical : sharing;
            ++n;
        }
    }
    t->caches = caches;
    t->ncaches = n;
    return n;
}

BOOL WINAPI GetLogicalProcessorInformation(k32y_slpi *buf, DWORD *len)
{
    k32y_topology t;
    k32y_cache caches[KY_MAX_CACHES];
    size_t n;
    if (!len) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ky_topology(&t, caches);
    n = k32y_build_legacy(&t, NULL, 0);
    if (!buf || *len < n * sizeof(k32y_slpi)) {
        *len = (DWORD)(n * sizeof(k32y_slpi));
        SetLastError(KY_ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    k32y_build_legacy(&t, buf, n);
    *len = (DWORD)(n * sizeof(k32y_slpi));
    return TRUE;
}

BOOL WINAPI GetLogicalProcessorInformationEx(DWORD relationship, PVOID buf, DWORD *len)
{
    k32y_topology t;
    k32y_cache caches[KY_MAX_CACHES];
    size_t need;
    unsigned records;
    if (!len || (relationship > K32Y_RELATION_GROUP && relationship != K32Y_RELATION_ALL)) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ky_topology(&t, caches);
    need = k32y_build_ex(&t, relationship, NULL, 0, &records);
    if (!buf || *len < need) {
        *len = (DWORD)need;
        SetLastError(KY_ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    k32y_build_ex(&t, relationship, (uint8_t *)buf, *len, &records);
    *len = (DWORD)need;
    return TRUE;
}

WORD WINAPI GetMaximumProcessorGroupCount(void) { return 1; }

BOOL WINAPI GetNumaHighestNodeNumber(ULONG *highest)
{
    if (!highest) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *highest = 0;
    return TRUE;
}

static ULONGLONG ky_all_mask(void)
{
    DWORD n = GetActiveProcessorCount(0);
    return k32y_mask_of(0, n > 64 ? 64 : (n ? n : 1));
}

BOOL WINAPI GetNumaNodeProcessorMask(BYTE node, ULONGLONG *mask)
{
    if (!mask || node != 0) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *mask = ky_all_mask();
    return TRUE;
}

BOOL WINAPI GetNumaNodeProcessorMaskEx(WORD node, k32y_group_affinity *ga)
{
    if (!ga || node != 0) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(ga, 0, sizeof *ga);
    ga->mask = ky_all_mask();
    return TRUE;
}

BOOL WINAPI GetNumaProcessorNode(BYTE processor, BYTE *node)
{
    if (!node)
        return FALSE;
    *node = processor < GetActiveProcessorCount(0) ? 0 : 0xFF;
    return TRUE;
}

typedef struct {
    WORD Group;
    BYTE Number, Reserved;
} KY_PROCESSOR_NUMBER;

BOOL WINAPI GetNumaProcessorNodeEx(KY_PROCESSOR_NUMBER *p, WORD *node)
{
    if (!p || !node || p->Group != 0) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *node = p->Number < GetActiveProcessorCount(0) ? 0 : 0xFFFF;
    return TRUE;
}

void WINAPI GetCurrentProcessorNumberEx(KY_PROCESSOR_NUMBER *p)
{
    p->Group = 0;
    p->Number = (BYTE)GetCurrentProcessorNumber();
    p->Reserved = 0;
}

BOOL WINAPI GetThreadGroupAffinity(HANDLE thread, k32y_group_affinity *ga)
{
    ULONG_PTR old;
    if (!ga) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* the current mask is read by setting it to all processors and putting back what was there */
    old = SetThreadAffinityMask(thread, (ULONG_PTR)ky_all_mask());
    if (!old)
        return FALSE;
    SetThreadAffinityMask(thread, old);
    memset(ga, 0, sizeof *ga);
    ga->mask = old;
    return TRUE;
}

BOOL WINAPI SetThreadGroupAffinity(HANDLE thread, const k32y_group_affinity *ga, k32y_group_affinity *previous)
{
    ULONG_PTR old;
    if (!ga || ga->group != 0 || !ga->mask) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    old = SetThreadAffinityMask(thread, (ULONG_PTR)ga->mask);
    if (!old)
        return FALSE;
    if (previous) {
        memset(previous, 0, sizeof *previous);
        previous->mask = old;
    }
    return TRUE;
}

BOOL WINAPI GetProcessGroupAffinity(HANDLE process, WORD *count, WORD *groups)
{
    (void)process;
    if (!count || (*count < 1 && (SetLastError(KY_ERROR_INSUFFICIENT_BUFFER), *count = 1, 1)))
        return FALSE;
    if (groups)
        groups[0] = 0;
    *count = 1;
    return TRUE;
}

BOOL WINAPI GetThreadIdealProcessorEx(HANDLE thread, KY_PROCESSOR_NUMBER *p)
{
    (void)thread;
    if (!p) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    p->Group = 0;
    p->Number = 0;
    p->Reserved = 0;
    return TRUE;
}

BOOL WINAPI SetThreadIdealProcessorEx(HANDLE thread, KY_PROCESSOR_NUMBER *p, KY_PROCESSOR_NUMBER *previous)
{
    (void)thread;
    if (!p || p->Group != 0) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (previous) {
        previous->Group = 0;
        previous->Number = 0;
        previous->Reserved = 0;
    }
    return TRUE;
}

/* ---- Power and product ------------------------------------------------------------------------------------------------------ */

typedef struct {
    BYTE ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag;
    DWORD BatteryLifeTime, BatteryFullLifeTime;
} KY_SYSTEM_POWER_STATUS;

/* A desktop on mains power with no battery. */
BOOL WINAPI GetSystemPowerStatus(KY_SYSTEM_POWER_STATUS *s)
{
    if (!s) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    s->ACLineStatus = 1;
    s->BatteryFlag = 128; /* no system battery */
    s->BatteryLifePercent = 255;
    s->SystemStatusFlag = 0;
    s->BatteryLifeTime = 0xFFFFFFFFu;
    s->BatteryFullLifeTime = 0xFFFFFFFFu;
    return TRUE;
}

static DWORD g_ky_execution_state = 0x80000000u; /* ES_CONTINUOUS */

DWORD WINAPI SetThreadExecutionState(DWORD state)
{
    DWORD old = g_ky_execution_state;
    if (state & 0x80000000u)
        g_ky_execution_state = state;
    return old;
}

BOOL WINAPI GetProductInfo(DWORD major, DWORD minor, DWORD sp_major, DWORD sp_minor, DWORD *type)
{
    (void)major;
    (void)minor;
    (void)sp_major;
    (void)sp_minor;
    if (!type) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *type = 0x30; /* PRODUCT_PROFESSIONAL */
    return TRUE;
}

BOOL WINAPI GetFirmwareType(DWORD *type)
{
    if (!type) {
        SetLastError(KY_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *type = 1; /* FirmwareTypeBios */
    return TRUE;
}

/* ---- Handles and process settings --------------------------------------------------------------------------------------------- */

BOOL WINAPI SetHandleInformation(HANDLE h, DWORD mask, DWORD flags)
{
    struct {
        BYTE Inherit, ProtectFromClose;
    } hf;
    NTSTATUS s;
    DWORD cur = 0;
    BOOL GetHandleInformation(HANDLE, DWORD *);
    if (!GetHandleInformation(h, &cur))
        return FALSE;
    cur = (cur & ~mask) | (flags & mask);
    hf.Inherit = (cur & 1) ? 1 : 0;           /* HANDLE_FLAG_INHERIT */
    hf.ProtectFromClose = (cur & 2) ? 1 : 0;  /* HANDLE_FLAG_PROTECT_FROM_CLOSE */
    s = NtSetInformationObject(h, 4 /* ObjectHandleFlagInformation */, &hf, sizeof hf);
    ky_status(s);
    return NT_SUCCESS(s);
}

BOOL WINAPI SetPriorityClass(HANDLE process, DWORD cls)
{
    /* PROCESS_PRIORITY_CLASS: {BOOLEAN Foreground; UCHAR PriorityClass} with the NT numbering 1 idle .. 6 realtime */
    struct {
        BYTE Foreground, PriorityClass;
    } pc = {0, 2};
    NTSTATUS s;
    switch (cls) {
    case 0x40: pc.PriorityClass = 1; break;     /* IDLE_PRIORITY_CLASS */
    case 0x20: pc.PriorityClass = 2; break;     /* NORMAL */
    case 0x4000: pc.PriorityClass = 3; break;   /* BELOW_NORMAL */
    case 0x8000: pc.PriorityClass = 4; break;   /* ABOVE_NORMAL */
    case 0x80: pc.PriorityClass = 5; break;     /* HIGH */
    case 0x100: pc.PriorityClass = 6; break;    /* REALTIME */
    default: SetLastError(KY_ERROR_INVALID_PARAMETER); return FALSE;
    }
    s = NtSetInformationProcess(process, 18 /* ProcessPriorityClass */, &pc, sizeof pc);
    ky_status(s);
    return NT_SUCCESS(s);
}

/* Not a package: a classic desktop process. */
LONG WINAPI GetCurrentPackageId(uint32_t *len, BYTE *buf)
{
    (void)len;
    (void)buf;
    return (LONG)KY_APPMODEL_ERROR_NO_PACKAGE;
}

LONG WINAPI GetCurrentPackageFullName(uint32_t *len, WCHAR *name)
{
    (void)len;
    (void)name;
    return (LONG)KY_APPMODEL_ERROR_NO_PACKAGE;
}

LONG WINAPI GetCurrentPackageFamilyName(uint32_t *len, WCHAR *name)
{
    (void)len;
    (void)name;
    return (LONG)KY_APPMODEL_ERROR_NO_PACKAGE;
}

LONG WINAPI GetCurrentApplicationUserModelId(uint32_t *len, WCHAR *name)
{
    (void)len;
    (void)name;
    return 15703; /* APPMODEL_ERROR_NO_APPLICATION */
}

#endif /* __MINGW32__ */
