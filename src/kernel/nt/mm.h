/*
 * mm.h - NT virtual-memory manager (ntmm) for the ntpers kernel personality.
 *
 * Implements the reserve/commit distinction, 64 KiB allocation granularity,
 * VAD tree, VirtualQuery coalescing, PAGE_GUARD semantics and section objects.
 *
 * In the kernel this file is included from nt/mm.c and wired into the NT
 * dispatch table.  For unit testing it is compiled in userspace with kshim.h.
 */

#ifndef NT_MM_H
#define NT_MM_H

#ifdef __KERNEL__
#include <linux/mm.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#undef STATUS_INVALID_SYSTEM_SERVICE
#define STATUS_INVALID_SYSTEM_SERVICE ((NTSTATUS)0xC000001BL)
#else
#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#endif

/* -------------------------------------------------------------------------- */
/* NT memory constants                                                        */
/* -------------------------------------------------------------------------- */

#define MEM_COMMIT                      0x00001000UL
#define MEM_RESERVE                     0x00002000UL
#define MEM_DECOMMIT                    0x00004000UL
#define MEM_RELEASE                     0x00008000UL
#define MEM_FREE                        0x00010000UL
#define MEM_PRIVATE                     0x00020000UL
#define MEM_MAPPED                      0x00040000UL
#define MEM_IMAGE                       0x01000000UL
#define MEM_TOP_DOWN                    0x00100000UL
#define MEM_WRITE_WATCH                 0x00200000UL
#define MEM_RESET                       0x00080000UL
#define MEM_LARGE_PAGES                 0x20000000UL
#define MEM_PHYSICAL                    0x00400000UL

#ifdef PAGE_NOACCESS
#undef PAGE_NOACCESS
#endif
#define PAGE_NOACCESS                   0x01UL
#ifdef PAGE_READONLY
#undef PAGE_READONLY
#endif
#define PAGE_READONLY                   0x02UL
#ifdef PAGE_READWRITE
#undef PAGE_READWRITE
#endif
#define PAGE_READWRITE                  0x04UL
#ifdef PAGE_WRITECOPY
#undef PAGE_WRITECOPY
#endif
#define PAGE_WRITECOPY                  0x08UL
#ifdef PAGE_EXECUTE
#undef PAGE_EXECUTE
#endif
#define PAGE_EXECUTE                    0x10UL
#ifdef PAGE_EXECUTE_READ
#undef PAGE_EXECUTE_READ
#endif
#define PAGE_EXECUTE_READ               0x20UL
#ifdef PAGE_EXECUTE_READWRITE
#undef PAGE_EXECUTE_READWRITE
#endif
#define PAGE_EXECUTE_READWRITE          0x40UL
#ifdef PAGE_EXECUTE_WRITECOPY
#undef PAGE_EXECUTE_WRITECOPY
#endif
#define PAGE_EXECUTE_WRITECOPY          0x80UL
#ifdef PAGE_GUARD
#undef PAGE_GUARD
#endif
#define PAGE_GUARD                      0x100UL
#ifdef PAGE_NOCACHE
#undef PAGE_NOCACHE
#endif
#define PAGE_NOCACHE                    0x200UL
#ifdef PAGE_WRITECOMBINE
#undef PAGE_WRITECOMBINE
#endif
#define PAGE_WRITECOMBINE               0x400UL

#define SEC_FILE                        0x00800000UL
#define SEC_IMAGE                       0x01000000UL
#define SEC_RESERVE                     0x04000000UL
#define SEC_COMMIT                      0x08000000UL
#define SEC_NOCACHE                     0x10000000UL
#define SEC_LARGE_PAGES                 0x80000000UL

#define MemoryBasicInformation          0UL
#define MemoryWorkingSetInformation     1UL
#define MemoryMappedFilenameInformation 2UL
#define MemoryRegionInformation         3UL
#define MemoryWorkingSetExInformation   4UL

/* MEMORY_REGION_INFORMATION.RegionType bits */
#define MEM_REGION_PRIVATE              0x00000001UL
#define MEM_REGION_MAPPED_DATA_FILE     0x00000002UL
#define MEM_REGION_MAPPED_IMAGE         0x00000004UL
#define MEM_REGION_MAPPED_PAGEFILE      0x00000008UL

#define NTMM_ALLOC_GRANULARITY          0x10000UL
#define NTMM_ALLOC_GRANULARITY_SHIFT    16
#define NTMM_PAGE_SIZE                  4096UL
#define NTMM_PAGE_SHIFT                 12
#define NTMM_LOW_ADDRESS                0x10000UL
#define NTMM_HIGH_ADDRESS               0x00007FFFFFF00000ULL

/* -------------------------------------------------------------------------- */
/* NT structures                                                              */
/* -------------------------------------------------------------------------- */

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID   BaseAddress;
    PVOID   AllocationBase;
    ULONG   AllocationProtect;
    SIZE_T  RegionSize;
    ULONG   State;
    ULONG   Protect;
    ULONG   Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

/* Layout of the Windows MEMORY_REGION_INFORMATION (64-bit). */
typedef struct _MEMORY_REGION_INFORMATION {
    PVOID   AllocationBase;
    ULONG   AllocationProtect;
    ULONG   RegionType;         /* MEM_REGION_* bits */
    SIZE_T  RegionSize;
    SIZE_T  CommitSize;
} MEMORY_REGION_INFORMATION, *PMEMORY_REGION_INFORMATION;

typedef struct _SECTION_BASIC_INFORMATION {
    ULONG_PTR Unknown;
    ULONG     SectionAttributes;
    LARGE_INTEGER SectionSize;
} SECTION_BASIC_INFORMATION, *PSECTION_BASIC_INFORMATION;

typedef struct _SECTION_IMAGE_INFORMATION {
    PVOID EntryPoint;
    ULONG StackReserve;
    ULONG StackCommit;
    ULONG SubSystemType;
    USHORT SubSystemMinorVersion;
    USHORT SubSystemMajorVersion;
    ULONG OperatingSystemVersion;
    USHORT ImageCharacteristics;
    USHORT DllCharacteristics;
    USHORT Machine;
    BOOLEAN ImageContainsCode;
    UCHAR ImageFlags;
    ULONG LoaderFlags;
    ULONG ImageFileSize;
    ULONG CheckSum;
} SECTION_IMAGE_INFORMATION, *PSECTION_IMAGE_INFORMATION;

/* -------------------------------------------------------------------------- */
/* VAD (Virtual Address Descriptor)                                           */
/* -------------------------------------------------------------------------- */
/*
 * Each VAD node describes one contiguous run of pages with identical NT
 * attributes.  Runs are non-overlapping and stored in a red-black tree keyed
 * by start address (kernel) or sorted list (userspace shim).  Splitting and
 * coalescing happen on every mutating operation so that VirtualQuery can report
 * maximal runs by simply scanning adjacent nodes.
 */

struct nt_section;

struct nt_vad {
    struct rb_node rb;
#ifdef __KERNEL__
    /* kernel uses rb tree */
#else
    struct nt_vad *list_next;
    struct nt_vad *list_prev;
#endif
    ULONG_PTR start;            /* inclusive */
    ULONG_PTR end;              /* exclusive */

    ULONG     state;            /* MEM_RESERVE or MEM_COMMIT */
    ULONG     protect;          /* current protection (commit) or allocation protect (reserve) */
    ULONG     alloc_protect;    /* protection specified at allocation/reservation time */
    ULONG_PTR alloc_base;       /* base of the original allocation */
    ULONG     type;             /* MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE */

    int       guard;            /* PAGE_GUARD active on committed pages */
    int       write_watch;      /* MEM_WRITE_WATCH marker (tracked, not enforced) */

    struct nt_section *section; /* non-NULL for mapped/image views */
    SIZE_T    section_offset;   /* byte offset into section for this run */
};

#ifdef __KERNEL__
#ifndef KSHIM_SECTION_BACKING_DEFINED
#define KSHIM_SECTION_BACKING_DEFINED
struct kshim_section_backing { int fd; size_t size; void *mem; };
#endif
#endif

/* -------------------------------------------------------------------------- */
/* Section object                                                             */
/* -------------------------------------------------------------------------- */

struct nt_section {
    ULONG                   attributes;   /* SEC_* */
    SIZE_T                  size;
    ULONG                   page_protection;
    int                     is_file;
    struct kshim_section_backing backing;
};

/* -------------------------------------------------------------------------- */
/* Per-process address space                                                  */
/* -------------------------------------------------------------------------- */

struct nt_mm {
    struct rb_root  vad_root;
#ifdef __KERNEL__
    struct rw_semaphore lock;
#else
    spinlock_t      lock;
    struct nt_vad  *vad_head; /* shim list head; vad_root unused in userspace */
#endif
    ULONG_PTR       highest_vad;
};

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

extern struct nt_mm g_nt_mm;
extern int g_nt_mm_initialized;
NTSTATUS ntmm_init(struct nt_mm *mm);
void     ntmm_destroy(struct nt_mm *mm);
void     ntmm_reset(void);
NTSTATUS ntmm_add_region(ULONG_PTR base, SIZE_T size, ULONG protect, ULONG type, ULONG_PTR alloc_base);

NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                                 ULONG_PTR ZeroBits, PSIZE_T RegionSize,
                                 ULONG AllocationType, ULONG Protect);
NTSTATUS NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                             PSIZE_T RegionSize, ULONG FreeType);
NTSTATUS NtProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                                PSIZE_T RegionSize, ULONG NewProtect,
                                PULONG OldProtect);
NTSTATUS NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                              ULONG MemoryInformationClass,
                              PVOID MemoryInformation, SIZE_T MemoryInformationLength,
                              PSIZE_T ReturnLength);

NTSTATUS NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                         POBJECT_ATTRIBUTES ObjectAttributes,
                         PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection,
                         ULONG AllocationAttributes, HANDLE FileHandle);
NTSTATUS NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle,
                            PVOID *BaseAddress, ULONG_PTR ZeroBits,
                            SIZE_T CommitSize, PLARGE_INTEGER SectionOffset,
                            PSIZE_T ViewSize, ULONG InheritDisposition,
                            ULONG AllocationType, ULONG Win32Protect);
NTSTATUS NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress);
NTSTATUS NtReadVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                             PVOID Buffer, SIZE_T BufferSize,
                             PSIZE_T NumberOfBytesRead);
NTSTATUS NtWriteVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                              PVOID Buffer, SIZE_T BufferSize,
                              PSIZE_T NumberOfBytesWritten);
NTSTATUS NtFlushVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                              PSIZE_T RegionSize, PIO_STATUS_BLOCK IoStatus);

/* Guard fault helper: simulate access to addr. Returns STATUS_GUARD_PAGE_VIOLATION
 * on first access to a guarded page and clears the guard bit. Returns STATUS_SUCCESS
 * if no guard, or STATUS_ACCESS_VIOLATION if not committed. */
NTSTATUS NtMmHandleGuardFault(PVOID Address);

/* Exposed for tests: VAD invariant checker */
int ntmm_check_invariants(struct nt_mm *mm);

#endif /* NT_MM_H */
