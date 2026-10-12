# 0026: TEB is kernel heap memory and is used as the GS base (arch P-2)

Status: Proposed. Arch source (`arch/x86/nt`). Recorded only in this batch, not implemented. Depends on the TEB mapping policy in 0008.
Finding: `docs/review/arch-x86-nt.md` P-2 (High).
Date: 2026-10-12

## Context

`arch/x86/nt/proc_thread.c:1007-1010` allocates the TEB with `kzalloc(2 * PAGE_SIZE)` and stores that kernel address in `thr->teb_base`. `proc_thread.c:1316` installs it as the child's GS base, so GS-relative access in user space faults. `proc_thread.c:1817` returns `teb_base` in `TebBaseAddress` to user space, which leaks a kernel heap address. `binfmt_pe.c:590` allocates the initial thread's TEB with `vm_mmap`, so the two paths disagree.

## Options

1. Allocate the TEB in user space with `vm_mmap` and copy the initial fields there, as `binfmt_pe.c` does. Tradeoff: user-space memory management per thread. Small to medium change.
2. Map kernel pages into user space (`remap_pfn_range` or `vm_insert_page`). Tradeoff: more complex, shares kernel memory with user space.
3. Keep the kernel heap and return 0 or hide the value. Tradeoff: GS-relative access still fails. Not acceptable.

## Recommendation

Option 1. It matches the exec path and the TEB policy in 0008 (kernel-nt PT-1).

## Patch sketch

- Replace the `kzalloc` at `1007-1010` with a `vm_mmap` of two pages in the child mm, then `copy_to_user` the TEB header.
- Set `teb_base` to the user address, so the GS base and `NtQueryInformationThread` both use it.
- Free the mapping at thread exit.

## Test plan

- Kernel test: a new thread reads its TEB self pointer through GS (`gs:[0x30]`) without a fault.
- Kernel test: `NtQueryInformationThread` returns an address below the user address limit.
- Host: not applicable.

## Not verified

Not built or run. Whether the child mm is active at the point of the `vm_mmap` was not checked.
