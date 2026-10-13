# 0037: How NtCreateUserProcess and CreateProcess create a PE process

Status: Proposed. Open. The owner chooses between options A, B and C below. Nothing is implemented.
Finding: not a review finding. The process-creation stubs are listed as a gap in [modules/proc_thread.md](../modules/proc_thread.md) and [KNOWN-ISSUES.md](../KNOWN-ISSUES.md).
Date: 2026-10-12

## Context

In the kernel build, `NtCreateProcessEx`, `NtCreateProcess` and `NtCreateUserProcess` return `STATUS_NOT_IMPLEMENTED` (`arch/x86/nt/proc_thread.c:1352-1354`, `src/kernel/nt/proc_thread.c:1416-1418`). `kernel32!CreateProcessW` is a stub that returns NULL (`src/ntdll/kernel32.c:399-402`). A PE process today starts only by a Linux `execve` of the PE file, handled by `binfmt_pe` (`src/kernel/fs/binfmt_pe.c:975`).

The full description is in [architecture/process-creation.md](../architecture/process-creation.md). Three points decide the options:

- `binfmt_pe` reads the PE from the file itself. It does not use a section, so a path-based spawn needs no file-backed sections.
- `NtCreateSection` refuses a file handle and `SEC_IMAGE` (`src/kernel/nt/mm.c:1742`, `:1745`). A `NtCreateUserProcess` that takes a section from a file handle needs that work first.
- The kernel has one global address space and one global VAD table (`src/kernel/nt/mm.c`, [0020](0020-arch-global-vad-table.md)). A child process needs its own mm, which is the largest piece of work.

Fork from a PE task is meant to be refused (research-2, line 196), but the code does not do this (`binfmt_pe.c:1142`). Any spawn primitive changes that policy, so the policy has to be decided with this record.

## Options

### A. Userspace-assisted CreateProcess in ntdll

`CreateProcessW` in ntdll writes the parameter block and the handle list into a memfd that is inherited across exec. It then calls a new kernel helper that spawns the child and execs the PE in it. The child's ntdll reads the block from the inherited fd. `NtCreateUserProcess` stays unsupported.

Tradeoffs: no NT create semantics in the kernel, and the existing `binfmt_pe` path is reused. No file-backed sections are needed. The spawn helper is still a kernel exception to the fork policy. Windows code that calls `NtCreateUserProcess` directly still fails. Handle and token inheritance live in a convention, not in kernel state.

### B. Kernel-side full NtCreateUserProcess

The kernel implements `NtCreateUserProcess` for a section or a path. It creates the child task with its own mm and VAD table, runs the PE load there, copies and relocates the parameter block, inherits handles and the token, creates a suspended initial thread, and returns `PS_CREATE_INFO`.

Tradeoffs: this matches the Windows contract, and `CreateProcessW` becomes a wrapper. It depends on file-backed `SEC_IMAGE` sections, per-process VAD tables, handle inheritance, and process identity at creation time. The kernel writes into another task's user memory at create time, which is a new write path. This is the largest change and the largest risk.

### C. Hybrid: one kernel spawn primitive, NT wrapper in the kernel

The kernel gets one primitive. It creates a child task, runs the PE exec in the child, takes the parameter block from a memfd or a checked user buffer, and leaves the initial thread suspended. `NtCreateUserProcess` maps its arguments onto that primitive. ntdll does the parameter marshalling. Handle inheritance uses `ObInheritHandles` after the child exists, gated by the caller's list.

Tradeoffs: one kernel primitive to review and test in the VM. `CreateProcessW` and `NtCreateUserProcess` share one path. File-handle sources can be added later, when file-backed sections exist, without changing the primitive. The kernel side still needs the fork exception and the child-mm work of B, so it is not cheaper than B for the kernel.

## Recommendation

Option C, built in this order: parameter block read by `binfmt_pe` from a memfd (no file-backed sections needed), then the spawn primitive with the child exec and a suspended start, then inheritance, then the `NtCreateUserProcess` wrapper for path sources, then file-handle sources after `SEC_IMAGE` sections exist.

Option A is the fallback if the per-process mm work (P3 in the architecture note) cannot start soon. It gives `CreateProcessW` a path with less kernel change, and it does not satisfy `NtCreateUserProcess`.

The fork policy (P8 in the architecture note) must be decided with this record. It is a policy question for the owner and not only a code change.

## Prerequisites

Each item has its own record or status line. The architecture note lists them in full.

- File-backed and `SEC_IMAGE` sections: `mm.c:1742`, `:1745`. Related [0004](0004-section-object-lifetime.md), [0011](0011-kernel-wx-sections.md).
- Handle inheritance and a global file-handle table: [0007](0007-file-handle-ownership.md), [0022](0022-arch-global-file-handle-table.md).
- Per-process VAD and address space: [0020](0020-arch-global-vad-table.md), [0005](0005-vad-commit-rollback.md).
- Process and thread open access checks: [0002](0002-process-open-access-check.md), [0025](0025-arch-process-thread-open-access.md).
- TEB location and disclosure: [0026](0026-arch-teb-kernel-heap.md), [0008](0008-teb-address-disclosure.md).
- Post-commit failure in a new task: [0012](0012-kernel-post-commit-exec-failure.md) (fixed; same pattern).
- DllMain and TLS for a child that runs: [0031](0031-loader-dllmain-tls-order.md).
- Binfmt test helpers before M1 tests can be added: [0028](0028-binfmt-pe-test-missing-helpers.md).

## Test plan

- Host: keep `src/kernel/nt/proc_thread_test.c` and add status checks for the kernel build. Add parameter-block parser tests with truncated, oversized and misaligned input. Add handle-inheritance tests that copy only `HANDLE_FLAG_INHERIT` entries.
- VM: a parent PE spawns a child PE and waits. Check the exit code, that the child is blocked before resume, and that a failed child exec kills the child. Check that an unsupported image source returns the documented status.
- Regression: `tools/check_syscall_tables.py` after any new service number.

## Not verified

- No build and no VM run. Every claim in this record is from reading the source.
- Whether `execve` resets the NT handle table of an existing task. The code shown destroys it on task exit (`src/kernel/nt/dispatch.c:401-405`). If execve does not reset it, option A passes too much to the child.
