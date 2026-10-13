# Module: mm (NT virtual-memory manager, ntmm)

Sources: `src/kernel/nt/mm.c`, `src/kernel/nt/mm.h`. Image layout and relocation helpers: `src/kernel/nt/image_plan.h`, `src/kernel/nt/image_reloc.h` (see [image_plan](image_plan.md) and [image_reloc](image_reloc.md)).
Related: `proc_thread.md` (process address space setup), `object.md` (section objects are object-manager objects), `handle.md` (section handles are handle-table indices), `dispatch.md` (service table).

## Purpose

Implements the NT virtual-memory model over the host address space: reserve versus commit, 64 KiB allocation granularity, a Virtual Address Descriptor (VAD) map of every region, `PAGE_GUARD` semantics, `NtQueryVirtualMemory` region reporting, and sections. Sections come in three kinds: anonymous, file-backed data (`SEC_COMMIT` with a file), and image (`SEC_IMAGE`, userspace build only). Section handles come from the handle table (ADR 0001).

The file is built twice. The userspace build (`#ifndef __KERNEL__`, the host shim) keeps VADs in a sorted list. The kernel build keeps them in a red-black tree. Most service bodies are shared; the `nt_svc_*` wrappers and the image-section code are not.

## Public API (mm.h)

Manager state and maintenance:

- `extern struct nt_mm g_nt_mm` and `extern int g_nt_mm_initialized`.
- `NTSTATUS ntmm_init(struct nt_mm *mm)` (`mm.c:749`) / `void ntmm_destroy(struct nt_mm *mm)` / `void ntmm_reset(void)`.
- `NTSTATUS ntmm_add_region(ULONG_PTR base, SIZE_T size, ULONG protect, ULONG type, ULONG_PTR alloc_base)` (`mm.c:2361`) - insert a VAD for a region the loader already mapped.
- `int ntmm_check_invariants(struct nt_mm *mm)` - test-facing VAD consistency check.

Memory services (native-style signatures, bodies in `mm.c`):

- `NtAllocateVirtualMemory` (`mm.c:839`), `NtFreeVirtualMemory` (`:1146`), `NtProtectVirtualMemory` (`:1288`), `NtQueryVirtualMemory` (`:1402`), `NtReadVirtualMemory` and `NtWriteVirtualMemory` (`:1583`, `:1596`) through the copy helper `ntmm_transfer` (`:1545`), `NtFlushVirtualMemory` (`:1634`) and `NtAllocateVirtualMemoryEx` (`:1683`).
- `NtFlushVirtualMemory` accepts only the current-process pseudo-handle. Any other process returns `STATUS_NOT_SUPPORTED`.
- `NtAllocateVirtualMemoryEx` is current-process only. A non-zero `ExtendedParameterCount` returns `STATUS_NOT_SUPPORTED`. With none, it calls `NtAllocateVirtualMemory`.
- `NtMmHandleGuardFault(PVOID Address)` (`mm.c:1698`) - simulation hook for first access to a guarded page. It returns `STATUS_GUARD_PAGE_VIOLATION` and clears the guard bit, or `STATUS_ACCESS_VIOLATION` for an uncommitted address. It has no caller in `src/` or `arch/` except `mm_test.c`.

Sections:

- `NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection, ULONG AllocationAttributes, HANDLE FileHandle)` (`mm.c:2007`). It dispatches on the flags:
  - `SEC_IMAGE` goes to `nt_create_image_section` (`mm.c:1939` in the userspace build). Without a file it returns `STATUS_INVALID_PARAMETER`. The kernel build returns `STATUS_NOT_IMPLEMENTED` (the kernel stub is at `mm.c:1893`).
  - A non-NULL `FileHandle` goes to `nt_create_file_section` (`mm.c:1821`). It needs `SEC_COMMIT`, otherwise `STATUS_NOT_IMPLEMENTED`. The handle must be an open file object with `FILE_WRITE_DATA` for `PAGE_READWRITE` or `FILE_READ_DATA` otherwise. Errors: `STATUS_INVALID_HANDLE` (not open), `STATUS_OBJECT_TYPE_MISMATCH` (not a file), `STATUS_ACCESS_DENIED` (missing access). The default `MaximumSize` is the file size. An empty file returns `STATUS_MAPPED_FILE_SIZE_ZERO`. A read-only or copy-on-write section larger than its file returns `STATUS_SECTION_TOO_BIG`. A read-write section larger than its file extends the file. Execute protections and `SEC_RESERVE` return `STATUS_NOT_IMPLEMENTED` (`nt_file_section_protect_ok`, `mm.c:1785`).
  - Otherwise an anonymous section is created. A non-NULL `MaximumSize` is required, otherwise `STATUS_INVALID_PARAMETER`.
- `NtOpenSection` (`nt_open_section`, `mm.c:2390`).
- `NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID *BaseAddress, ULONG_PTR ZeroBits, SIZE_T CommitSize, PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize, ULONG InheritDisposition, ULONG AllocationType, ULONG Win32Protect)` (`mm.c:2283`). It looks the handle up against `nt_section_type` and calls `nt_map_view_of_section` (`mm.c:2213`). File views need a 64 KiB aligned offset (`STATUS_MAPPED_ALIGNMENT`). A shared writable view needs a writable section (`STATUS_SECTION_PROTECTION`).
- `NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress)` (`mm.c:2307`). On the host it writes shared writable file views back before it unmaps them.
- `NtQuerySection` (`nt_query_section_core`, `mm.c:2571`). `SectionBasicInformation` is answered. `SectionImageInformation` (class 1) returns `STATUS_NOT_IMPLEMENTED` (`mm.c:2579`).
- `NtExtendSection` returns `STATUS_NOT_IMPLEMENTED` in both builds (`mm.c:2610` and `:2646`).

Service wrappers. Each service has a typed definition in the userspace branch and a raw-argument definition (`unsigned long a0, ...`) in the kernel branch, which is the form `dispatch.c` calls. The userspace typed wrappers are at `mm.c:2407` (`NtCreateSection`), `:2415` (`NtOpenSection`), `:2495` (`NtMapViewOfSection`), `:2504` (`NtUnmapViewOfSection`) and `:2603` (`NtQuerySection`). The kernel raw wrappers are at `:2425`, `:2463`, `:2514`, `:2556`, `:2619` and `:2646`. Those are the dispatch slots. `docs/status/SERVICES.md` lists them as IMPLEMENTED, except `NtExtendSection` (STUB in both builds).

Not present in the service table: `NtCreateSectionEx`, `NtMapViewOfSectionEx`, `NtUnmapViewOfSectionEx`, `NtLockVirtualMemory` and `NtUnlockVirtualMemory` (dispatch slot NULL, so `STATUS_NOT_IMPLEMENTED`).

Image sections (`SEC_IMAGE`, userspace build). The file is read whole (up to 64 MiB, `NT_IMAGE_MAX_FILE`, else `STATUS_SECTION_TOO_BIG`) and planned by `image_plan.h`, which reuses `src/loader/pe_parse.c` for validation. Status mapping (`ip_status_to_nt`, `mm.c:1906`): a non-PE or malformed image is `STATUS_INVALID_IMAGE_FORMAT`, a zero-size file is `STATUS_MAPPED_FILE_SIZE_ZERO`, and SectionAlignment below a page is `STATUS_NOT_IMPLEMENTED`. A 32-bit image is refused as `STATUS_INVALID_IMAGE_FORMAT`, because `STATUS_INVALID_IMAGE_WIN_32` is not defined in this tree.

`nt_map_image_view` (`mm.c:2093`) maps the whole SizeOfImage at the preferred base if it is free, else at the first free hole. It returns `STATUS_IMAGE_NOT_AT_BASE` (informational) for a relocatable image placed elsewhere, and `STATUS_CONFLICTING_ADDRESSES` for a non-relocatable one. A `RELOCS_STRIPPED` image is non-relocatable even with a directory present. For a relocatable image placed elsewhere, the base relocations are applied to the private copy by `nt_image_relocate` (`mm.c:2068`, which calls `ir_apply` from `image_reloc.h`) before any segment gets its final protection. A malformed directory fails the map with `STATUS_INVALID_IMAGE_FORMAT`, leaves no VAD, and frees everything the map allocated.

Order of work inside the map: the whole range is reserved and committed read-write, each segment's file bytes are copied in with `pread` and its zero tail is cleared, relocations are applied if the base moved, then each segment takes its final protection and the gaps between segments are set to `PROT_NONE`. Each segment has its own `MEM_IMAGE` VAD. The header is read-only, `.text` is `PAGE_EXECUTE_READ`, and a writable section is `PAGE_WRITECOPY` (or `PAGE_EXECUTE_WRITECOPY`). The bytes are copied at map time, so a write never reaches the file. The view takes no offset, and its size is 0 or at least SizeOfImage.

Guard pages: see `NtMmHandleGuardFault` above.

Constants in the header: `NTMM_ALLOC_GRANULARITY` (0x10000), `NTMM_PAGE_SIZE` (4096), `NTMM_LOW_ADDRESS` (0x10000), `NTMM_HIGH_ADDRESS` (0x00007FFFFFF00000), and section flags such as `SEC_NOCACHE` and `SEC_LARGE_PAGES`. The image planner has its own constants (`IP_USER_LIMIT`, `IP_MAX_SECTIONS`).

## Key data structures

- `struct nt_vad` - an `rb_node` in the kernel build, or `list_next`/`list_prev` in the shim. Fields: `start` (inclusive), `end` (exclusive), `state` (`MEM_RESERVE` or `MEM_COMMIT`), `protect`, `alloc_protect`, `alloc_base`, `type` (`MEM_PRIVATE`, `MEM_MAPPED`, `MEM_IMAGE`), `section` and `section_offset` for views, plus guard state.
- `struct nt_mm` - `vad_root` (rb_root), a lock (`spinlock_t` in the shim, `rw_semaphore` in the kernel build), `vad_head` (shim list head), and `highest_vad`.
- `struct nt_section` (`mm.h`) - the body of a `Section` object: `attributes` (SEC_* flags, plus `SEC_FILE` for a file section and `SEC_IMAGE` for an image one), `size`, `page_protection`, `is_file`, the backing (`struct kshim_section_backing`: `fd`, `mem`, `size`, and `file` in the kernel build), and `image`, a `struct ip_plan *` that only an image section sets. The kernel build always leaves `image` NULL.
- The object type is `nt_section_type` (`mm.c:1749`, named `"Section"`). Its delete callback `nt_section_delete` closes the backing: the host fd, or the kernel `struct file` reference (`fput`), and frees the plan. The type is registered by the first create or open that needs it (`ObRegisterObjectType` at `mm.c:1863`, `:1981`, `:2028`), not at object-manager init.
- A view that a VAD maps holds one object reference (`nt_section_ref` and `nt_section_unref`).
- The global `g_nt_mm` is the process-wide manager instance used by the services.

## Invariants and locking (as implemented)

- Every mutating operation goes through `ntmm_lock`/`ntmm_unlock`. The macros map to `spin_lock`/`spin_unlock` in the shim and to `down_write`/`up_write` in the kernel build.
- VADs are non-overlapping. The `mm.h` comment says splitting and coalescing happen on every mutating operation. The code does not do that. A commit does not coalesce, and `NtProtectVirtualMemory` coalesces afterwards (`nt_coalesce_all`). `NtQueryVirtualMemory` reports the VAD holding the address and scans adjacent VADs to find its region.
- `ntmm_check_invariants` checks the per-VAD constraints.
- Address searches clamp to `[NTMM_LOW_ADDRESS, NTMM_HIGH_ADDRESS)` and align to the allocation granularity.
- A reserved-but-uncommitted range is a VAD with `state == MEM_RESERVE`.
- Range arithmetic is checked for wrap before page rounding. `NtProtectVirtualMemory`, the decommit path of `NtFreeVirtualMemory`, and `NtFlushVirtualMemory` refuse a range that wraps, including a base in the top page (audit-fixes passes 2 and 3: `993c3ee`, `e1486e9`, `bfd5d93`). `NtAllocateVirtualMemoryEx` releases the region when a copy-out of the base or size faults (`f583b05`, `165bca4`).
- `NtMapViewOfSection` compares the requested view size with the section size minus the offset before rounding, and an oversize request returns `STATUS_INVALID_VIEW_SIZE` (`mm.c:2234-2241`; audit-fixes pass 1, `f942cb9`).
- `ntmm_transfer` copies through a heap bounce buffer, not a stack buffer (`19291fe`).

## Known gaps (evident in code)

- Two copies of this module exist. `arch/x86/nt/mm.c` is not the same code as `src/kernel/nt/mm.c`. The audit fixes were applied to both where the defect existed. See [tree-divergence](../analysis/tree-divergence.md).
- The arch copy still returns the object address as the section handle (`arch/x86/nt/mm.c`, review arch M-2, record 0021). The `src/kernel/nt` copy uses handle-table indices (record 0001).
- The image-section path is userspace only. The kernel build refuses `SEC_IMAGE` (`mm.c:1893`). Its mapping would need the helpers that `binfmt_pe` uses at exec time (process-creation milestone M6).
- `NtQuerySection(SectionImageInformation)` returns `STATUS_NOT_IMPLEMENTED`. Image views ignore `Win32Protect` (the protections come from the sections), and `SEC_IMAGE` sections ignore `MaximumSize`.
- Image views take offset 0 only (`STATUS_INVALID_PARAMETER` otherwise), so the 64 KiB rule for `SEC_IMAGE` is not reachable.
- The 4 KiB stack path buffers in `NtQueryAttributesFile`, `NtDeleteFile` and `NtQueryFullAttributesFile` (`io.c`) are not in this module. `ntmm_transfer` no longer has one.
- The free path's comment (`mm.c:1227-1230`) says the mapping is dropped with `vm_munmap` directly. The code calls `kshim_munmap` (`mm.c:1203`), which wraps `vm_munmap` (`mm.c:55`). The behaviour matches; the comment does not describe the call that runs.
- `kshim_mprotect` looks up the unexported `do_mprotect_pkey` through `kallsyms_lookup_name` and falls back to a hand-written path. This depends on kernel internals.
- `NtMmHandleGuardFault` is a simulation hook, not real page-fault handling. `kshim_install_guard_handler` is a no-op.
- The userspace VAD list and the kernel rb-tree are separate code paths. `ntmm_check_invariants` is the only shared check.
- The host shim cannot map a file, so a host file view is a copy filled at map time (`kshim_section_map_file`). Shared writable views are written back on flush and on unmap. Copy-on-write views are never written back, so two views of one section are not coherent on the host until a flush. The kernel build maps the file's pages with `vm_mmap`, so its views share the page cache.
- File sections do not support execute protections or `SEC_RESERVE`. An anonymous section is zero-filled memory, not file contents.
- `NtCreateSectionEx`, `NtMapViewOfSectionEx`, `NtUnmapViewOfSectionEx`, `NtLockVirtualMemory` and `NtUnlockVirtualMemory` are not in the service table.
- One address space: `ProcessHandle` is ignored by the section and allocation paths, which use `g_nt_mm`. Every process shares one VAD list.
