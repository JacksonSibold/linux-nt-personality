# Module: mm (NT virtual-memory manager, ntmm)

Sources: `src/kernel/nt/mm.c`, `src/kernel/nt/mm.h`
Related: `proc_thread.md` (process address space setup), `object.md` (section objects are object-manager objects), `dispatch.md` (`nt_svc_NtAllocateVirtualMemory` and related wrappers live there).

## Purpose

Implements the NT virtual-memory model over the host address space: reserve versus commit, 64 KiB allocation granularity, a Virtual Address Descriptor (VAD) map of every region, `PAGE_GUARD` semantics, `NtQueryVirtualMemory` coalescing, and section objects with map and unmap. The header describes the kernel build as a red-black tree of VADs and the userspace shim as a sorted list.

## Public API (mm.h)

Manager state and maintenance:

- `extern struct nt_mm g_nt_mm` and `extern int g_nt_mm_initialized`.
- `NTSTATUS ntmm_init(struct nt_mm *mm)` / `void ntmm_destroy(struct nt_mm *mm)` / `void ntmm_reset(void)`.
- `NTSTATUS ntmm_add_region(ULONG_PTR base, SIZE_T size, ULONG protect, ULONG type, ULONG_PTR alloc_base)` - insert a VAD for an existing region, used for images and mapped regions.
- `int ntmm_check_invariants(struct nt_mm *mm)` - test-facing VAD consistency check.

Memory services (native-style signatures):

- `NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect)` - reserve, commit, or both.
- `NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType)` - release or decommit.
- `NtProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, PULONG OldProtect)`.
- `NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, ULONG MemoryInformationClass, PVOID MemoryInformation, SIZE_T MemoryInformationLength, PSIZE_T ReturnLength)` - `MemoryBasicInformation` (class 0) is defined.

Sections:

- `NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection, ULONG AllocationAttributes, HANDLE FileHandle)`.
- `NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID *BaseAddress, ULONG_PTR ZeroBits, SIZE_T CommitSize, PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize, ULONG InheritDisposition, ULONG AllocationType, ULONG Win32Protect)`.
- `NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress)`.

Guard pages:

- `NTSTATUS NtMmHandleGuardFault(PVOID Address)` - simulates first access to a guarded page, returns `STATUS_GUARD_PAGE_VIOLATION` and clears the guard bit, or returns `STATUS_ACCESS_VIOLATION` for an uncommitted address.

Constants in the header: `NTMM_ALLOC_GRANULARITY` (0x10000), `NTMM_PAGE_SIZE` (4096), `NTMM_LOW_ADDRESS` (0x10000), `NTMM_HIGH_ADDRESS` (0x00007FFFFFF00000), and section flags such as `SEC_NOCACHE` and `SEC_LARGE_PAGES`.

## Key data structures

- `struct nt_vad` - an `rb_node` in the kernel build, or `list_next`/`list_prev` in the shim. Fields: `start` (inclusive), `end` (exclusive), `state` (`MEM_RESERVE` or `MEM_COMMIT`), `protect`, `alloc_protect`, `alloc_base`, and `type` (`MEM_PRIVATE`, `MEM_MAPPED`, `MEM_IMAGE`), plus guard state.
- `struct nt_mm` - `vad_root` (rb_root), a lock (`spinlock_t` in the shim, `rw_semaphore` in the kernel build), `vad_head` (shim list head), and `highest_vad`.
- `struct nt_section` - forward-declared in `mm.h` and used by the section services.
- The global `g_nt_mm` is the process-wide manager instance used by the services.

## Invariants and locking (as implemented)

- Every mutating operation goes through `ntmm_lock`/`ntmm_unlock`. The macros map to `spin_lock`/`spin_unlock` in the shim and to `down_write`/`up_write` in the kernel build (`mm.c:171-175`).
- VADs are non-overlapping and kept split and coalesced on every change, so `NtQueryVirtualMemory` can report maximal runs by scanning adjacent nodes (`mm.h` VAD comment). `ntmm_check_invariants` checks the per-VAD constraints (`mm.c:758`). The comment at `mm.c:767-770` says adjacent VADs with identical attributes may remain separate and that coalescing is query-time only.
- Address searches clamp to `[NTMM_LOW_ADDRESS, NTMM_HIGH_ADDRESS)` and align to the allocation granularity (`mm.c:406-441`, `637-649`).
- A reserved-but-uncommitted range is represented by a VAD with `state == MEM_RESERVE` (`struct nt_vad` comment in `mm.h`).

## Known gaps (evident in code)

- The free path's unmap comment is stale. In the kernel branch (`mm.c:1165-1195`), the comment at `mm.c:1188-1192` says to call `vm_munmap` directly, and that call is commented out. The active call is `kshim_munmap` (`mm.c:1194`), which in the kernel build is a wrapper around `vm_munmap` (defined near the top of `mm.c`). Behaviour is the same, but the comment does not match the code.
- `kshim_mprotect` (`mm.c`, near the top of the kernel section) looks up the unexported symbol `do_mprotect_pkey` through `kallsyms_lookup_name` and falls back to a hand-written `mprotect_fixup` path if the lookup fails. This is fragile and depends on kernel internals.
- The `PAGE_GUARD` helper `NtMmHandleGuardFault` is a simulation hook (per its comment), not a real page-fault integration.
- `mm.c:147` substitutes `0x10000` as the target address when the caller supplies none. I did not trace which build branch this line is in.
- The userspace VAD list (`vad_head`) and the kernel rb tree are separate code paths. `ntmm_check_invariants` is the only shared check I found.
- `nt_svc_NtAllocateVirtualMemory`, `nt_svc_NtFreeVirtualMemory`, `nt_svc_NtProtectVirtualMemory` and `nt_svc_NtQueryVirtualMemory` are wrappers in `dispatch.c` (see `dispatch.md`), not in this file.
- Section mapping honours `SectionOffset` alignment as the code comment at `mm.c:1518` describes (64 KiB for images, page size for data). Image-specific behaviour beyond that was not traced.
