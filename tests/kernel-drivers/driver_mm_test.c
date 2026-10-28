/* driver_mm_test.c - unit tests for the MDL and memory-mapping surface of the NT driver host (drvhost_mm.c). */
#include <stdio.h>
#include <string.h>

#include "dh_test.h"
#include "drvhost_int.h"

static void test_mdl_basic(void)
{
    char *buf = MmAllocateContiguousMemory(3 * PAGE_SIZE, ~0ull);
    CHECK(buf != NULL);
    CHECK(((ULONG_PTR)buf & (PAGE_SIZE - 1)) == 0);
    CHECK(MmGetPhysicalAddress(buf) == (ULONGLONG)(ULONG_PTR)buf);
    memset(buf, 0xab, 3 * PAGE_SIZE);   /* the block is usable and writable end to end */

    /* An MDL for an unaligned range spanning 3 pages. */
    PMDL mdl = IoAllocateMdl(buf + 0x10, 2 * PAGE_SIZE + 0x20, FALSE, FALSE, NULL);
    CHECK(mdl != NULL);
    CHECK(mdl->StartVa == buf);
    CHECK(mdl->ByteOffset == 0x10);
    CHECK(mdl->ByteCount == 2 * PAGE_SIZE + 0x20);
    CHECK(MmGetMdlVirtualAddress(mdl) == buf + 0x10);
    CHECK(ADDRESS_AND_SIZE_TO_SPAN_PAGES(buf + 0x10, 2 * PAGE_SIZE + 0x20) == 3);
    CHECK(mdl->Size == (CSHORT)(sizeof(MDL) + 3 * sizeof(PFN_NUMBER)));

    MmProbeAndLockPages(mdl, KernelMode, 2 /* IoModifyAccess */);
    CHECK(mdl->MdlFlags & MDL_PAGES_LOCKED);
    CHECK(mdl->MdlFlags & MDL_WRITE_OPERATION);
    PPFN_NUMBER pfn = MmGetMdlPfnArray(mdl);
    CHECK(pfn[0] == (ULONG_PTR)buf >> PAGE_SHIFT);
    CHECK(pfn[1] == pfn[0] + 1);
    CHECK(pfn[2] == pfn[0] + 2);

    PVOID va = MmMapLockedPagesSpecifyCache(mdl, KernelMode, 1, NULL, FALSE, 16);
    CHECK(va == buf + 0x10);
    CHECK(mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    CHECK(MmMapLockedPages(mdl, KernelMode) == va);   /* already mapped: same address */
    MmUnmapLockedPages(va, mdl);
    CHECK(!(mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA));
    CHECK(mdl->MappedSystemVa == NULL);

    MmUnlockPages(mdl);
    CHECK(!(mdl->MdlFlags & MDL_PAGES_LOCKED));
    IoFreeMdl(mdl);
    IoFreeMdl(NULL);
    MmFreeContiguousMemory(buf);
    MmFreeContiguousMemory(NULL);
}

static void test_mdl_nonpaged(void)
{
    static char pool[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
    PMDL mdl = IoAllocateMdl(pool + 8, 100, FALSE, FALSE, NULL);
    CHECK(mdl != NULL);
    MmBuildMdlForNonPagedPool(mdl);
    CHECK(mdl->MdlFlags & MDL_SOURCE_IS_NONPAGED_POOL);
    CHECK(mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    CHECK(mdl->MappedSystemVa == pool + 8);
    CHECK(MmGetMdlPfnArray(mdl)[0] == (ULONG_PTR)pool >> PAGE_SHIFT);
    /* Unmapping a nonpaged-pool MDL leaves its mapping in place. */
    MmUnmapLockedPages(mdl->MappedSystemVa, mdl);
    CHECK(mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    IoFreeMdl(mdl);
}

static void test_mdl_irp_chain(void)
{
    static char b1[64], b2[64], b3[64];
    IRP irp;
    memset(&irp, 0, sizeof irp);
    PMDL m1 = IoAllocateMdl(b1, sizeof b1, FALSE, FALSE, &irp);
    CHECK(irp.MdlAddress == m1);
    PMDL m2 = IoAllocateMdl(b2, sizeof b2, TRUE, FALSE, &irp);
    PMDL m3 = IoAllocateMdl(b3, sizeof b3, TRUE, FALSE, &irp);
    CHECK(irp.MdlAddress == m1);
    CHECK(m1->Next == m2);
    CHECK(m2->Next == m3);
    CHECK(m3->Next == NULL);
    IoFreeMdl(m3); IoFreeMdl(m2); IoFreeMdl(m1);
}

static void test_contiguous(void)
{
    CHECK(MmAllocateContiguousMemory(0, ~0ull) == NULL);
    char *a = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE + 1, 0, ~0ull, 0, 0 /* MmNonCached */);
    CHECK(a != NULL);
    CHECK(((ULONG_PTR)a & (PAGE_SIZE - 1)) == 0);
    a[PAGE_SIZE] = 1;                   /* second page exists */
    MmFreeContiguousMemory(a);
}

int main(void)
{
    CHECK_STATUS(drvhost_init(), 0);
    test_mdl_basic();
    test_mdl_nonpaged();
    test_mdl_irp_chain();
    test_contiguous();
    drvhost_shutdown();
    DH_TEST_SUMMARY("driver_mm_test");
}
