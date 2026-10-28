/*
 * test_k32_sysinfo_pure.c - host tests for src/ntdll/k32_sysinfo_pure.h.
 */
#include "minitest.h"
#include "k32_sysinfo_pure.h"

#include <string.h>

static void system_info_fill(void)
{
    K32SI_SYSTEM_INFO si;
    K32SI_BASIC b;
    memset(&b, 0, sizeof b);
    b.PageSize = 4096;
    b.AllocationGranularity = 65536;
    b.MinimumUserModeAddress = 0x10000;
    b.MaximumUserModeAddress = 0x00007FFFFFFEFFFFull;
    b.NumberOfProcessors = 8;
    k32si_fill(&si, &b, 0, 6, 0x9A01);
    MT_EXPECT_EQ(si.wProcessorArchitecture, 9);
    MT_EXPECT_EQ((long)si.dwPageSize, 4096);
    MT_EXPECT_EQ((long)si.dwNumberOfProcessors, 8);
    MT_EXPECT_EQ((long)si.dwActiveProcessorMask, 0xFF);
    MT_EXPECT_EQ((long)si.dwProcessorType, 8664);
    MT_EXPECT_EQ((long)si.dwAllocationGranularity, 65536);
    MT_EXPECT(si.lpMaximumApplicationAddress == (PVOID)0x00007FFFFFFEFFFFull);
    MT_EXPECT(si.wProcessorLevel == 6 && si.wProcessorRevision == 0x9A01);
    /* a failed query falls back to the usual values */
    k32si_fill(&si, NULL, 0, 6, 0);
    MT_EXPECT_EQ((long)si.dwPageSize, 4096);
    MT_EXPECT_EQ((long)si.dwNumberOfProcessors, 1);
    MT_EXPECT(si.lpMinimumApplicationAddress == (PVOID)0x10000);
    /* an explicit processor count wins and is capped at 64 */
    k32si_fill(&si, &b, 200, 6, 0);
    MT_EXPECT_EQ((long)si.dwNumberOfProcessors, 64);
    MT_EXPECT(si.dwActiveProcessorMask == ~(ULONG_PTR)0);
}

static void cpuid_decoding(void)
{
    /* family 6, model 0x9A (extended model 9, model A), stepping 3: an Alder Lake mobile part */
    uint32_t eax = (0x9u << 16) | (0x6u << 8) | (0xAu << 4) | 0x3u;
    MT_EXPECT_EQ(k32si_cpu_level(eax), 6);
    MT_EXPECT_EQ(k32si_cpu_revision(eax), 0x9A03);
    /* family 0xF with an extended family of 8 is family 0x17 (Zen) */
    eax = (0x8u << 20) | (0xFu << 8) | (0x1u << 4) | 0x2u;
    MT_EXPECT_EQ(k32si_cpu_level(eax), 0x17);
}

static void versions_and_names(void)
{
    MT_EXPECT_EQ((long)k32si_packed_version(10, 0, 26200), (long)(10 | (0 << 8) | (26200u << 16)));
    MT_EXPECT_EQ((long)(k32si_packed_version(6, 1, 7601) >> 16), 7601);
    {
        WCHAR w[32];
        MT_EXPECT_EQ((long)k32si_copy_name(K32SI_WINDOWS_DIR, w, 32), 10);
        MT_EXPECT(w[0] == 'C' && w[3] == 'W' && w[10] == 0);
        MT_EXPECT_EQ((long)k32si_copy_name(K32SI_SYSTEM_DIR, w, 8), 20); /* too small: size needed */
    }
    {
        char n[32];
        MT_EXPECT_EQ((long)k32si_computer_name("my-host.example.com", n, 32), 7);
        MT_EXPECT(strcmp(n, "MY-HOST") == 0);
        MT_EXPECT_EQ((long)k32si_computer_name("averyveryverylonghostname", n, 32), 15);
        MT_EXPECT_EQ((long)k32si_computer_name("", n, 32), 6);
        MT_EXPECT(strcmp(n, "NTPERS") == 0);
    }
}

static void memory_status(void)
{
    K32SI_MEMORYSTATUSEX m;
    k32si_memory_status(&m, 1048576, 4096, 524288, 0x00007FFFFFFE0000ull);
    MT_EXPECT_EQ((long)m.dwLength, (long)sizeof(m));
    MT_EXPECT(m.ullTotalPhys == 4294967296ull && m.ullAvailPhys == 2147483648ull);
    MT_EXPECT_EQ((long)m.dwMemoryLoad, 50);
    MT_EXPECT(m.ullAvailPageFile <= m.ullTotalPageFile);
    MT_EXPECT(m.ullAvailVirtual <= m.ullTotalVirtual);
    /* more available than total is clamped */
    k32si_memory_status(&m, 100, 4096, 500, 1ull << 40);
    MT_EXPECT(m.ullAvailPhys == m.ullTotalPhys && m.dwMemoryLoad == 0);
}

static void processor_features(void)
{
    /* SSE2 (edx bit 26), SSE3 (ecx bit 0), AVX (ecx bit 28), AVX2 (ebx7 bit 5) */
    uint32_t ecx = 1u | (1u << 28) | (1u << 13), edx = (1u << 23) | (1u << 25) | (1u << 26), ebx7 = 1u << 5;
    MT_EXPECT(k32si_feature(10, ecx, edx, ebx7, 0));
    MT_EXPECT(k32si_feature(13, ecx, edx, ebx7, 0));
    MT_EXPECT(k32si_feature(39, ecx, edx, ebx7, 0));
    MT_EXPECT(k32si_feature(40, ecx, edx, ebx7, 0));
    MT_EXPECT(!k32si_feature(41, ecx, edx, ebx7, 0));
    MT_EXPECT(!k32si_feature(10, 0, 0, 0, 0));
    MT_EXPECT(k32si_feature(23, 0, 0, 0, 0)); /* fast fail is always there */
    MT_EXPECT(!k32si_feature(999, ecx, edx, ebx7, 0));
    MT_EXPECT(k32si_feature(14, ecx, edx, ebx7, 0)); /* cmpxchg16b */
}

static const mt_case_t cases[] = {
    MT_CASE(system_info_fill), MT_CASE(cpuid_decoding), MT_CASE(versions_and_names), MT_CASE(memory_status), MT_CASE(processor_features),
};

int main(void)
{
    return MT_RUN("k32_sysinfo_pure", cases);
}
