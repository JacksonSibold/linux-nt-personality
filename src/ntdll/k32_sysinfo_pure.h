/*
 * k32_sysinfo_pure.h - pure rules for the system information functions (kernel32_x.c): the GetSystemInfo fields built from
 * the NT basic and processor information, the packed GetVersion value, memory status figures, processor feature flags
 * from CPUID bits, and the Windows directory names.
 * Host tests: tests/ntdll-host/test_k32_sysinfo_pure.c.
 */
#ifndef NTDLL_K32_SYSINFO_PURE_H
#define NTDLL_K32_SYSINFO_PURE_H

#include "nt_types.h"
#include <stddef.h>
#include <stdint.h>

/* SYSTEM_INFO (x64) */
typedef struct {
    WORD wProcessorArchitecture;
    WORD wReserved;
    DWORD dwPageSize;
    PVOID lpMinimumApplicationAddress;
    PVOID lpMaximumApplicationAddress;
    ULONG_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} K32SI_SYSTEM_INFO;

#define K32SI_PROCESSOR_ARCHITECTURE_AMD64 9u
#define K32SI_PROCESSOR_AMD_X8664 8664u

/* What the kernel reports in SYSTEM_BASIC_INFORMATION (class 0), the leading part. */
typedef struct {
    ULONG Reserved;
    ULONG TimerResolution;
    ULONG PageSize;
    ULONG NumberOfPhysicalPages;
    ULONG LowestPhysicalPageNumber;
    ULONG HighestPhysicalPageNumber;
    ULONG AllocationGranularity;
    ULONG_PTR MinimumUserModeAddress;
    ULONG_PTR MaximumUserModeAddress;
    ULONG_PTR ActiveProcessorsAffinityMask;
    signed char NumberOfProcessors;
} K32SI_BASIC;

/* Fills SYSTEM_INFO. basic may be NULL (the query failed): then the usual x64 values are used. cpu_family and cpu_model are
 * the CPUID family and model, for wProcessorLevel and wProcessorRevision (model << 8 | stepping). */
static inline void k32si_fill(K32SI_SYSTEM_INFO *si, const K32SI_BASIC *basic, DWORD cpus, WORD cpu_level, WORD cpu_revision)
{
    si->wProcessorArchitecture = (WORD)K32SI_PROCESSOR_ARCHITECTURE_AMD64;
    si->wReserved = 0;
    si->dwPageSize = basic && basic->PageSize ? basic->PageSize : 4096u;
    si->lpMinimumApplicationAddress = (PVOID)(basic && basic->MinimumUserModeAddress ? basic->MinimumUserModeAddress : 0x10000u);
    si->lpMaximumApplicationAddress = (PVOID)(basic && basic->MaximumUserModeAddress ? basic->MaximumUserModeAddress : 0x00007FFFFFFEFFFFull);
    si->dwActiveProcessorMask = 0;
    si->dwNumberOfProcessors = cpus ? cpus : (basic && basic->NumberOfProcessors > 0 ? (DWORD)basic->NumberOfProcessors : 1u);
    if (si->dwNumberOfProcessors > 64)
        si->dwNumberOfProcessors = 64;
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~(ULONG_PTR)0 : (((ULONG_PTR)1 << si->dwNumberOfProcessors) - 1);
    si->dwProcessorType = K32SI_PROCESSOR_AMD_X8664;
    si->dwAllocationGranularity = basic && basic->AllocationGranularity ? basic->AllocationGranularity : 65536u;
    si->wProcessorLevel = cpu_level;
    si->wProcessorRevision = cpu_revision;
}

/* CPUID leaf 1 EAX: the family (with the extended family for 0xF) and the revision word. */
static inline WORD k32si_cpu_level(uint32_t eax)
{
    uint32_t family = (eax >> 8) & 0xF;
    if (family == 0xF)
        family += (eax >> 20) & 0xFF;
    return (WORD)family;
}

static inline WORD k32si_cpu_revision(uint32_t eax)
{
    uint32_t family = (eax >> 8) & 0xF, model = (eax >> 4) & 0xF, stepping = eax & 0xF;
    if (family == 0x6 || family == 0xF)
        model |= ((eax >> 16) & 0xF) << 4;
    return (WORD)((model << 8) | stepping);
}

/* GetVersion's packed value: major and minor in the low word, the build number in the high word (the top bit is set
 * on a non-NT platform only). */
static inline DWORD k32si_packed_version(DWORD major, DWORD minor, DWORD build)
{
    return (major & 0xFF) | ((minor & 0xFF) << 8) | ((build & 0x7FFF) << 16);
}

/* MEMORYSTATUSEX figures from the physical page count: the available part is what is not assumed to be in use. The
 * commit limit is physical memory plus the page file, taken as equal to it; the virtual address space is the user
 * range. */
typedef struct {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} K32SI_MEMORYSTATUSEX;

static inline void k32si_memory_status(K32SI_MEMORYSTATUSEX *m, ULONGLONG total_pages, ULONGLONG page_size, ULONGLONG avail_pages, ULONGLONG user_space)
{
    ULONGLONG total = total_pages * page_size, avail = avail_pages * page_size;
    if (avail > total)
        avail = total;
    m->dwLength = (DWORD)sizeof(*m);
    m->dwMemoryLoad = total ? (DWORD)((total - avail) * 100 / total) : 0;
    m->ullTotalPhys = total;
    m->ullAvailPhys = avail;
    m->ullTotalPageFile = total * 2;
    m->ullAvailPageFile = avail * 2 > total * 2 ? total * 2 : avail + total;
    m->ullTotalVirtual = user_space;
    m->ullAvailVirtual = user_space > total ? user_space - total / 2 : user_space / 2;
    m->ullAvailExtendedVirtual = 0;
}

/* IsProcessorFeaturePresent: the PF_ constants this layer answers, from CPUID leaf 1 (ecx, edx), leaf 7 (ebx) and the
 * extended leaf 0x80000001 (edx). Unknown features are absent. */
static inline int k32si_feature(DWORD feature, uint32_t ecx1, uint32_t edx1, uint32_t ebx7, uint32_t edx_ext)
{
    switch (feature) {
    case 0: return 0;                      /* PF_FLOATING_POINT_PRECISION_ERRATA */
    case 1: return 0;                      /* PF_FLOATING_POINT_EMULATED */
    case 2: return 1;                      /* PF_COMPARE_EXCHANGE_DOUBLE */
    case 3: return (edx1 >> 23) & 1;       /* PF_MMX_INSTRUCTIONS_AVAILABLE */
    case 6: return (edx1 >> 25) & 1;       /* PF_XMMI_INSTRUCTIONS_AVAILABLE */
    case 7: return (edx_ext >> 31) & 1;    /* PF_3DNOW_INSTRUCTIONS_AVAILABLE */
    case 8: return 1;                      /* PF_RDTSC_INSTRUCTION_AVAILABLE */
    case 9: return 1;                      /* PF_PAE_ENABLED */
    case 10: return (edx1 >> 26) & 1;      /* PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2) */
    case 12: return 1;                     /* PF_NX_ENABLED */
    case 13: return ecx1 & 1;              /* PF_SSE3_INSTRUCTIONS_AVAILABLE */
    case 14: return (ecx1 >> 13) & 1;      /* PF_COMPARE_EXCHANGE128 */
    case 15: return (ecx1 >> 13) & 1;      /* PF_COMPARE64_EXCHANGE128 */
    case 17: return 1;                     /* PF_CHANNELS_ENABLED */
    case 18: return 1;                     /* PF_XSAVE_ENABLED */
    case 20: return 1;                     /* PF_VIRT_FIRMWARE_ENABLED */
    case 23: return 1;                     /* PF_FASTFAIL_AVAILABLE */
    case 24: return 0;                     /* PF_ARM_VFP_32_REGISTERS_AVAILABLE */
    case 36: return (ecx1 >> 9) & 1;       /* PF_SSSE3_INSTRUCTIONS_AVAILABLE */
    case 37: return (ecx1 >> 19) & 1;      /* PF_SSE4_1_INSTRUCTIONS_AVAILABLE */
    case 38: return (ecx1 >> 20) & 1;      /* PF_SSE4_2_INSTRUCTIONS_AVAILABLE */
    case 39: return (ecx1 >> 28) & 1;      /* PF_AVX_INSTRUCTIONS_AVAILABLE */
    case 40: return (ebx7 >> 5) & 1;       /* PF_AVX2_INSTRUCTIONS_AVAILABLE */
    case 41: return (ebx7 >> 16) & 1;      /* PF_AVX512F_INSTRUCTIONS_AVAILABLE */
    default: return 0;
    }
}

/* The fixed directory names of the Windows installation, as wide strings. Writes into out (cap characters); returns the
 * length without the NUL, or the size needed including it when it does not fit (the Get...Directory convention). */
static inline DWORD k32si_copy_name(const char *ascii, WCHAR *out, DWORD cap)
{
    DWORD n = 0;
    while (ascii[n])
        ++n;
    if (n + 1 > cap)
        return n + 1;
    for (DWORD i = 0; i <= n; ++i)
        out[i] = (WCHAR)(unsigned char)ascii[i];
    return n;
}

#define K32SI_WINDOWS_DIR "C:\\Windows"
#define K32SI_SYSTEM_DIR "C:\\Windows\\System32"

/* ComputerName rules: 1..15 characters (NetBIOS), upper case from the host name, dots end the first label. */
static inline DWORD k32si_computer_name(const char *host, char *out, DWORD cap)
{
    DWORD n = 0;
    while (host[n] && host[n] != '.' && n < 15) {
        char c = host[n];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if (n + 1 < cap)
            out[n] = c;
        ++n;
    }
    if (n == 0) {
        static const char fallback[] = "NTPERS";
        for (DWORD i = 0; fallback[i] && i + 1 < cap; ++i)
            out[i] = fallback[i];
        n = 6;
    }
    if (n < cap)
        out[n] = 0;
    return n;
}

#endif /* NTDLL_K32_SYSINFO_PURE_H */
