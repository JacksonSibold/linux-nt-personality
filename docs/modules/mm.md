# Module: mm (NT virtual-memory manager, ntmm)

Sources: `src/kernel/nt/mm.c`, `src/kernel/nt/mm.h`
Related: `proc_thread.md` (process address space setup), `object.md` (section objects are object-manager objects), `dispatch.md` (`nt_svc_NtAllocateVirtualMemory` and related wrappers live there).

## Purpose

Implements the NT virtual-memory model over the host address space: reserve versus commit, 64 KiB allocation granularity, a Virtual Address Descriptor (VAD) map of every region, `PAGE_GUARD` semantics, `NtQueryVirtualMemory` region reporting, and sections with create, map and unmap. Sections are object-manager objects of type `Section`, and their handles come from the handle table (ADR 0001). The header describes the kernel build as a red-black tree of VADs and the userspace shim as a sorted list.

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
- `NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, ULONG MemoryInformationClass, PVOID MemoryInformation, SIZE_T MemoryInformationLength, PSIZE_T ReturnLength)` - `MemoryBasicInformation` (class 0) is defined. The query returns the region that holds `BaseAddress` (`mm.c:1406-1446`).
- `NtFlushVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, PIO_STATUS_BLOCK IoStatus)` (`mm.c:1600`) - accepts only the current-process pseudo-handle (`(HANDLE)-1`). Any other process returns `STATUS_NOT_SUPPORTED`.
- `NtAllocateVirtualMemoryEx(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG AllocationType, ULONG PageProtection, PVOID ExtendedParameters, ULONG ExtendedParameterCount)` (`mm.c:1639`) - current process only. A non-zero `ExtendedParameterCount` returns `STATUS_NOT_SUPPORTED`. With none, it calls `NtAllocateVirtualMemory`.

Sections (object model, the sections block of `mm.c`):

- `NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection, ULONG AllocationAttributes, HANDLE FileHandle)` (`mm.c:1881`). `SEC_IMAGE` returns `STATUS_NOT_IMPLEMENTED` (`mm.c:1894`), with or without a file. A non-NULL `FileHandle` builds a file-backed data section (`nt_create_file_section`, `mm.c:1817`). It needs `SEC_COMMIT`. The handle must be an open file object with `FILE_WRITE_DATA` for `PAGE_READWRITE` or `FILE_READ_DATA` otherwise. Errors: `STATUS_INVALID_HANDLE` (not open), `STATUS_OBJECT_TYPE_MISMATCH` (not a file), `STATUS_ACCESS_DENIED` (missing access). The default `MaximumSize` is the file size. An empty file returns `STATUS_MAPPED_FILE_SIZE_ZERO`. A read-only or copy-on-write section larger than its file returns `STATUS_SECTION_TOO_BIG`. A read-write section larger than its file extends the file. A non-NULL `MaximumSize` is required for anonymous sections, otherwise `STATUS_INVALID_PARAMETER`. The object is made with `ObCreateObject` on `nt_section_type` and the handle is made with `ObInsertObject`.
- `NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID *BaseAddress, ULONG_PTR ZeroBits, SIZE_T CommitSize, PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize, ULONG InheritDisposition, ULONG AllocationType, ULONG Win32Protect)` (`mm.c:1989`). It looks the handle up with `ObReferenceObjectByHandle` against `nt_section_type`. The helper `nt_map_view_of_section` (`mm.c:1924`) does the mapping. File views need a 64 KiB aligned offset (`STATUS_MAPPED_ALIGNMENT`). A shared writable view needs a writable section (`STATUS_SECTION_PROTECTION`).
- `NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress)` (`mm.c:2013`). On the host it writes shared writable file views back before it unmaps them.

Section services in the syscall table (`docs/status/SERVICES.md`): `NtCreateSection`, `NtCreateSectionEx`, `NtOpenSection`, `NtMapViewOfSection`, `NtMapViewOfSectionEx`, `NtUnmapViewOfSection`, `NtUnmapViewOfSectionEx`, `NtExtendSection` and `NtQuerySection` have no `nt_svc_` definition, so their dispatch slots are NULL. The C functions above are reached only by C callers and by `mm_test.c`. The memory services `NtLockVirtualMemory` and `NtUnlockVirtualMemory` are also missing. The eight memory services that are implemented (allocate, allocate-Ex, free, protect, query, read, write and flush) have wrappers in `dispatch.c` (`dispatch.c:160-348`).

Guard pages:

- `NTSTATUS NtMmHandleGuardFault(PVOID Address)` - simulates first access to a guarded page, returns `STATUS_GUARD_PAGE_VIOLATION` and clears the guard bit, or returns `STATUS_ACCESS_VIOLATION` for an uncommitted address.

Constants in the header: `NTMM_ALLOC_GRANULARITY` (0x10000), `NTMM_PAGE_SIZE` (4096), `NTMM_LOW_ADDRESS` (0x10000), `NTMM_HIGH_ADDRESS` (0x00007FFFFFF00000), and section flags such as `SEC_NOCACHE` and `SEC_LARGE_PAGES`.

## Key data structures

- `struct nt_vad` - an `rb_node` in the kernel build, or `list_next`/`list_prev` in the shim. Fields: `start` (inclusive), `end` (exclusive), `state` (`MEM_RESERVE` or `MEM_COMMIT`), `protect`, `alloc_protect`, `alloc_base`, and `type` (`MEM_PRIVATE`, `MEM_MAPPED`, `MEM_IMAGE`), plus guard state.
- `struct nt_mm` - `vad_root` (rb_root), a lock (`spinlock_t` in the shim, `rw_semaphore` in the kernel build), `vad_head` (shim list head), and `highest_vad`.
- `struct nt_section` (`mm.h:215`) - the body of a `Section` object: `attributes` (SEC_* flags, plus `SEC_FILE` for a file section), `size`, `page_protection`, `is_file` and a `struct kshim_section_backing` (`fd`, `mem`, `size`, and `file` in the kernel build). The object type is `nt_section_type` (defined at `mm.c:1745`, delete callback `nt_section_delete` at `mm.c:1731`). The delete callback closes the backing: the host fd, or the kernel `struct file` reference (`fput`).
- A view that a VAD maps holds one object reference (`nt_section_ref` and `nt_section_unref`, `mm.c:270-275`).
- The global `g_nt_mm` is the process-wide manager instance used by the services.

## Invariants and locking (as implemented)

- Every mutating operation goes through `ntmm_lock`/`ntmm_unlock`. The macros map to `spin_lock`/`spin_unlock` in the shim and to `down_write`/`up_write` in the kernel build (`mm.c:173-181`).
- VADs are non-overlapping. The `mm.h` comment (`mm.h:168-173`) says splitting and coalescing happen on every mutating operation. The code does not do that. A commit does not coalesce (`mm.c:1116-1118`), and `NtProtectVirtualMemory` coalesces afterwards (`mm.c:1260`, `1328`, `nt_coalesce_all`). `NtQueryVirtualMemory` reports the VAD holding the address and scans adjacent VADs to find its region (`mm.c:1406-1446`).
- `ntmm_check_invariants` checks the per-VAD constraints (`mm.c:783`).
- Address searches clamp to `[NTMM_LOW_ADDRESS, NTMM_HIGH_ADDRESS)` and align to the allocation granularity (`mm.c:429-464`, `662-683`).
- A reserved-but-uncommitted range is represented by a VAD with `state == MEM_RESERVE` (`struct nt_vad` comment in `mm.h`).

## Known gaps (evident in code)

- The free path's unmap comment is stale. Around `mm.c:1215-1221`, the comment says to call `vm_munmap` directly, and that call (`mm.c:1220`) is commented out. The active call is `kshim_munmap` (`mm.c:1221`), which wraps `vm_munmap` (`mm.c:51`). Behaviour is the same, but the comment does not match the code.
- `kshim_mprotect` (`mm.c:55-65`) looks up the unexported symbol `do_mprotect_pkey` through `kallsyms_lookup_name` (`mm.c:58`) and falls back to a hand-written `mprotect_fixup` path if the lookup fails. This is fragile and depends on kernel internals.
- The `PAGE_GUARD` helper `NtMmHandleGuardFault` is a simulation hook (per its comment), not a real page-fault integration.
- `kshim_section_map` (`mm.c:150`) substitutes `0x10000` as the target address when the caller supplies none. I did not trace which build branch this helper is in.
- The userspace VAD list (`vad_head`) and the kernel rb tree are separate code paths. `ntmm_check_invariants` is the only shared check I found.
- The `SEC_IMAGE` branches in `nt_map_view_of_section` cannot run, because `NtCreateSection` refuses `SEC_IMAGE` (`mm.c:1894`, `STATUS_NOT_IMPLEMENTED`). Image sections are not reachable until process-creation M6.
- Anonymous section offsets must be page-aligned. File sections need 64 KiB, and the 64 KiB rule for `SEC_IMAGE` has no reachable case.
- The host shim cannot map a file (`mman_win.c` rejects fd-backed `mmap`), so a host file view is a copy filled at map time (`kshim_section_map_file`). Shared writable views are written back on flush and on unmap. Copy-on-write views are never written back. Two views of one section are therefore not coherent on the host until a flush. The kernel build maps the file's pages with `vm_mmap`, so its views share the page cache. Anonymous sections on the host have the same non-coherence.
- File sections do not support execute protections or `SEC_RESERVE` (`STATUS_NOT_IMPLEMENTED`). An anonymous section is still zero-filled memory, not file contents.
- No `nt_svc_` wrapper exists for the section services, so an application cannot reach them through the syscall table (see the list above).
