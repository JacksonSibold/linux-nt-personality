# 0074: Per-process address spaces for the memory manager

Status: Accepted. Implemented in 021e475 (mm.c, mm.h, mm_test.c, object.c, object.h).
Finding: none in `docs/review/`. Gap found while reading `docs/architecture/memory.md`: "One address space: `ProcessHandle` is ignored by the section and allocation paths, which use `g_nt_mm`. Every process shares one VAD list." This record is the prerequisite for per-process isolation in `docs/architecture/process-creation.md`.
Date: 2026-10-12

## Context

`src/kernel/nt/mm.c` keeps one VAD list, `g_nt_mm`. Every service body starts with `struct nt_mm *mm = &g_nt_mm;` and ignores the process argument. `NtFlushVirtualMemory`, `NtReadVirtualMemory`, `NtWriteVirtualMemory` and `NtAllocateVirtualMemoryEx` return `STATUS_NOT_SUPPORTED` for any handle other than the pseudo-handle `-1`. `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtProtectVirtualMemory`, `NtQueryVirtualMemory`, the lock services and the section map and unmap services accept the handle and drop it. So a second process object created with `ObCreateProcessObject` has no memory of its own, and process destruction has nothing to free.

The process-creation milestones need a separate space per process: the loader maps an image into the child, and `NtReadVirtualMemory` must reach the child's pages rather than the parent's.

The kernel build maps into the real process address space through `vm_mmap` and `mmap_lock`. Only the current task's `mm_struct` is reachable there, so a foreign process cannot be given a real Linux address space in this tree.

## Options

1. Keep one global space, and have every service ignore `ProcessHandle` as now. Tradeoff: no change, but a child process cannot own memory, and cross-process access is impossible. This is the status quo.
2. A space per `nt_eprocess`, for both builds, with each foreign process's space mapped into real Linux memory of its own. Tradeoff: the kernel build needs a second `mm_struct` per process (`mm_alloc`, `switch_mm`, page tables). That is a large change in the kernel tree, which cannot be built or run on this machine.
3. A space per `nt_eprocess` as a `struct nt_mm`. The existing global space becomes the space of the current process (`g_nt_mm`). Each service resolves `ProcessHandle` through `ObReferenceObjectByHandle` with the access the service needs. In the userspace shim, a non-current process's space is backed by its own host arena: a single PROT_NONE reservation, committed piecewise, that its VAD list carves up. Cross-process `Read`/`WriteVirtualMemory` copy between arenas through `ntmm_transfer`. In the kernel build, the real mm stays bound to the current process only, and a foreign handle returns `STATUS_NOT_SUPPORTED`.

## Recommendation

Option 3. It gives each process its own VAD map and lock, and lets the host tests check isolation, cross-process copies, and teardown. It costs no kernel-side change beyond the refusal path.

Design:

- `struct nt_eprocess` gains `struct nt_mm *aspace` (NULL until a foreign space is first needed) and `void (*aspace_free)(struct nt_mm *)`. The callback is set together with `aspace`. `ObDestroyProcessObject` calls it when it is set. The object layer does not link against `mm.c`, because several host test binaries link `object.o` without `mm.o`.
- `struct nt_mm` gains `low_addr`, `high_addr` (the bounds the hole search and region query use; `NTMM_LOW_ADDRESS` and `NTMM_HIGH_ADDRESS` for the global space) and `arena_base`, `arena_size` (zero for the global space).
- `ntmm_space_acquire(ProcessHandle, access, &ref)` resolves the space:
  - `-1` (`NtCurrentProcess`), `NULL` and any handle to the current process resolve to `&g_nt_mm`. The pseudo-handle takes no object reference and no access check, so its behaviour is unchanged.
  - Any other handle is looked up with `ObReferenceObjectByHandle(current, h, &nt_process_type, KernelMode, access, ...)`, which checks the granted access. The process comes from `ObFindProcessByHeader`. Access is `PROCESS_VM_OPERATION` for allocate, free, protect, lock, unlock, map and unmap; `PROCESS_VM_READ` for read; `PROCESS_VM_WRITE` for write; `PROCESS_QUERY_INFORMATION` for query; and `PROCESS_VM_OPERATION` for flush. The reference is held until `ntmm_space_release`.
  - A foreign process gets its space created on first use. In the userspace shim, the space reserves a host arena of `NTMM_ARENA_SIZE` (64 MiB) and sets `low_addr` and `high_addr` to the 64 KiB-aligned interior. The kernel build returns `STATUS_NOT_SUPPORTED` for any foreign process.
- Service bodies take the resolved `struct nt_mm *`. Each public service resolves, calls its body, and releases. The body is unchanged apart from the space parameter.
- Host arenas do not reserve each region with `MAP_FIXED_NOREPLACE`, because the arena is already committed PROT_NONE. Commits go through `kshim_mmap_commit` (which falls back to a fixed map) and decommits through `kshim_munmap`. Teardown releases the whole arena with one `kshim_munmap`.
- Section views map into the target's space at a hole in that space, so `NtMapViewOfSection(section, foreignHandle, ...)` places the view inside the target arena. The section object's reference is held by the view, as now.
- `ntmm_destroy` on an arena frees all VADs (dropping section references) and then releases the arena. The process destructor calls `aspace_free` after the handle table is destroyed, so the handle table's references to section objects are gone before the space is torn down.
- Kernel build: the real mm is kept only for the current process. `ntmm_space_acquire` returns `STATUS_NOT_SUPPORTED` for any other process, with the path forward documented in `docs/modules/mm.md`: a per-process `mm_struct` (`mm_alloc` and `switch_mm` on the target) is the next step, to be taken with process-creation milestone M6.

## Test plan

Host tests in `src/kernel/nt/mm_test.c`, run by `make check-quick`:

- Allocate, query, protect and free in a second process object, using its handle. The allocation sits in that process's arena, and the global space has no VAD for it.
- Access rights: a handle with only `PROCESS_VM_READ` can read but cannot allocate or write (`STATUS_ACCESS_DENIED`).
- Cross-process `NtWriteVirtualMemory` then `NtReadVirtualMemory` round trip; the pseudo-handle view of the same address shows nothing.
- Section mapped into the second process's space; cross-process read sees section data; unmap through the handle.
- Destroying the process object frees its whole space: the leak counter is unchanged after `ObDestroyProcessObject`.
- The pseudo-handle semantics are unchanged: `-1` allocates in the global space and existing tests stay as they are.
- A forged or closed handle is `STATUS_INVALID_HANDLE`.

## Consequences

- The NULL handle now resolves to the global space, as the pseudo-handle does. The old code ignored every handle, so this keeps the existing behaviour for NULL.
- A handle that names no process (for example a forged value) is STATUS_INVALID_HANDLE. Four existing assertions in `mm_test.c` pinned STATUS_NOT_SUPPORTED for such a handle, and now pin STATUS_INVALID_HANDLE.
- The global space can hand out a host address that an arena occupies. Such a global allocation fails with STATUS_NO_MEMORY. Moving the arena out of the global range would need a shared reservation map, which is not part of this change.

## Not verified

The kernel build (`vm/kbuild.sh`) and the `arch/x86/nt/mm.c` copy are not built. `arch/x86/nt/mm.c` is not changed by this record, as required. The kernel branch is compiled only through the userspace shim's syntax check; its foreign-process refusal path is not exercised on a Linux kernel.
