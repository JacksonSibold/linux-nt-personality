/* drvhost_mm.c - memory descriptor lists and the memory manager services built on them.
 *
 * The host runs drivers in the address space of its process, so a "system virtual address" of a locked buffer is the
 * buffer's own address and a page frame number is the page number of its virtual address. That is enough for drivers
 * that only walk an MDL, map it and pass it on. A driver that programs a DMA engine with the frame numbers needs the
 * kernel build, where the numbers come from the page tables.
 */

#include "drvhost_int.h"

#define DH_MAX_MDL_PAGES 0x100000u       /* 4 GB per MDL */

DRVCALL PMDL IoAllocateMdl(PVOID VirtualAddress, ULONG Length, BOOLEAN SecondaryBuffer, BOOLEAN ChargeQuota, PIRP Irp)
{
    (void)ChargeQuota;
    ULONG pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(VirtualAddress, Length);
    if (pages > DH_MAX_MDL_PAGES) return NULL;
    size_t bytes = sizeof(MDL) + (size_t)pages * sizeof(PFN_NUMBER);
    PMDL mdl = dh_alloc(bytes);
    if (!mdl) return NULL;
    mdl->Size = (CSHORT)(bytes > 0x7fff ? 0x7fff : bytes);
    mdl->StartVa = PAGE_ALIGN(VirtualAddress);
    mdl->ByteOffset = BYTE_OFFSET(VirtualAddress);
    mdl->ByteCount = Length;
    if (Irp) {
        if (!SecondaryBuffer) {
            Irp->MdlAddress = mdl;
        } else {
            PMDL last = Irp->MdlAddress;
            if (!last) Irp->MdlAddress = mdl;
            else { while (last->Next) last = last->Next; last->Next = mdl; }
        }
    }
    return mdl;
}

DRVCALL void IoFreeMdl(PMDL Mdl)
{
    if (!Mdl) return;
    dh_free(Mdl);
}

static void mdl_fill_pfns(PMDL Mdl)
{
    ULONG pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(Mdl), Mdl->ByteCount);
    PPFN_NUMBER pfn = MmGetMdlPfnArray(Mdl);
    ULONG_PTR first = (ULONG_PTR)Mdl->StartVa >> PAGE_SHIFT;
    for (ULONG i = 0; i < pages; i++) pfn[i] = first + i;
}

DRVCALL void MmBuildMdlForNonPagedPool(PMDL Mdl)
{
    mdl_fill_pfns(Mdl);
    Mdl->MdlFlags |= MDL_SOURCE_IS_NONPAGED_POOL | MDL_MAPPED_TO_SYSTEM_VA;
    Mdl->MappedSystemVa = MmGetMdlVirtualAddress(Mdl);
}

DRVCALL void MmProbeAndLockPages(PMDL Mdl, KPROCESSOR_MODE AccessMode, ULONG Operation)
{
    (void)AccessMode;
    char *va = (char *)Mdl->StartVa;
    ULONG pages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(Mdl), Mdl->ByteCount);
    for (ULONG i = 0; i < pages; i++) {
        if (!MmIsAddressValid(va + (size_t)i * PAGE_SIZE))
            dh_bugcheck(STATUS_ACCESS_VIOLATION, (ULONG_PTR)Mdl, (ULONG_PTR)va, 0, 0);
    }
    mdl_fill_pfns(Mdl);
    Mdl->MdlFlags |= MDL_PAGES_LOCKED;
    if (Operation != 0 /* IoReadAccess */) Mdl->MdlFlags |= MDL_WRITE_OPERATION;
}

DRVCALL void MmUnlockPages(PMDL Mdl)
{
    if (!(Mdl->MdlFlags & MDL_PAGES_LOCKED)) dh_bugcheck(DH_BUGCHECK_BAD_HOST_STATE, 0x20, (ULONG_PTR)Mdl, 0, 0);
    Mdl->MdlFlags &= (CSHORT)~MDL_PAGES_LOCKED;
}

DRVCALL PVOID MmMapLockedPagesSpecifyCache(PMDL Mdl, KPROCESSOR_MODE AccessMode, ULONG CacheType,
                                           PVOID RequestedAddress, ULONG BugCheckOnFailure, ULONG Priority)
{
    (void)AccessMode; (void)CacheType; (void)RequestedAddress; (void)BugCheckOnFailure; (void)Priority;
    if (Mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) return Mdl->MappedSystemVa;
    Mdl->MappedSystemVa = MmGetMdlVirtualAddress(Mdl);
    Mdl->MdlFlags |= MDL_MAPPED_TO_SYSTEM_VA;
    return Mdl->MappedSystemVa;
}

DRVCALL PVOID MmMapLockedPages(PMDL Mdl, KPROCESSOR_MODE AccessMode)
{
    return MmMapLockedPagesSpecifyCache(Mdl, AccessMode, 1, NULL, 1, 16);
}

DRVCALL void MmUnmapLockedPages(PVOID BaseAddress, PMDL Mdl)
{
    (void)BaseAddress;
    if (!(Mdl->MdlFlags & MDL_SOURCE_IS_NONPAGED_POOL)) {
        Mdl->MdlFlags &= (CSHORT)~MDL_MAPPED_TO_SYSTEM_VA;
        Mdl->MappedSystemVa = NULL;
    }
}

/* Contiguous memory: page aligned, from the platform's page mapper. A header page in front records the size, so that
 * MmFreeContiguousMemory can unmap it. The "physical" address of a block is its address. */
DRVCALL PVOID MmAllocateContiguousMemory(SIZE_T NumberOfBytes, ULONGLONG HighestAcceptableAddress)
{
    (void)HighestAcceptableAddress;
    if (NumberOfBytes == 0) return NULL;
    size_t total = ((NumberOfBytes + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1)) + PAGE_SIZE;
    char *base = dh_map_image(total);
    if (!base) return NULL;
    *(size_t *)base = total;
    return base + PAGE_SIZE;
}

DRVCALL PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T NumberOfBytes, ULONGLONG LowestAcceptableAddress,
                                                     ULONGLONG HighestAcceptableAddress,
                                                     ULONGLONG BoundaryAddressMultiple, ULONG CacheType)
{
    (void)LowestAcceptableAddress; (void)BoundaryAddressMultiple; (void)CacheType;
    return MmAllocateContiguousMemory(NumberOfBytes, HighestAcceptableAddress);
}

DRVCALL void MmFreeContiguousMemory(PVOID BaseAddress)
{
    if (!BaseAddress) return;
    char *base = (char *)BaseAddress - PAGE_SIZE;
    dh_unmap_image(base, *(size_t *)base);
}

DRVCALL ULONGLONG MmGetPhysicalAddress(PVOID BaseAddress)
{
    return (ULONGLONG)(ULONG_PTR)BaseAddress;
}
