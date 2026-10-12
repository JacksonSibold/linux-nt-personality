# Known issues and gaps

This page collects the known gaps recorded in the module reference
([docs/modules/](modules/INDEX.md)) and the highest-priority findings from the
static review ([docs/review/INDEX.md](review/INDEX.md)). It is a summary.
Each linked page has the details and line references. The project is an
incomplete research prototype (see the top-level [README](../README.md)).
Fix status below comes from the "Fix status" sections and per-finding status
lines in the review reports, as of 2026-10-12. Nothing was compiled, run, or
re-tested to confirm those fixes.

## Gaps by module

### dispatch ([modules/dispatch.md](modules/dispatch.md))

- Debug logging is in the hot path: six hard-coded SSNs call `pr_info` on every
  call, with no debug gate.
- `nt_can_sysret` is a stub that always returns `true`.
- The table covers SSNs up to 0x1db, but only 171 entries are listed. The rest
  return `STATUS_NOT_IMPLEMENTED`.
- Some listed services have no `nt_svc_` definition (for example
  `NtAllocateVirtualMemoryEx`, `NtAlertResumeThread`, `NtWaitForAlertByThreadId`).
  Only three were checked.
- The virtual-memory wrappers are defined in `dispatch.c` and are also declared
  `extern`. There is no `dispatch.h`.

### handle ([modules/handle.md](modules/handle.md))

- `NT_CURRENT_THREAD_HANDLE` in `handle.h` has unbalanced parentheses. It is
  unused, so the build is not affected.
- There is no dedicated handle test. Coverage comes from `object_test.c` and
  `i2_test.c`.
- The table has no growth limit beyond its 2^24 index space, even though the
  header describes memory-based growth.

### io ([modules/io.md](modules/io.md))

- The header says async work runs on a workqueue. The code has no workqueue
  calls, so completion appears to run inline.
- Stdin is not backed in the shim build and returns `STATUS_END_OF_FILE`.
- Generic file mapping is "simplified".
- `nt_svc_NtClose` is defined three times. Which definition links depends on
  the build, and that was not traced.
- Share enforcement exists only in the kernel. Non-NT opens are not blocked.
- The NT-to-Linux path translation (`\??`, `\Device`) was not traced in full.
- The kernel wrappers target a specific kernel version (6.18 API changes), and
  NFS delegation is not handled.

### iocp ([modules/iocp.md](modules/iocp.md))

- `nt_svc_NtQueryIoCompletion` validates the handle, then returns
  `STATUS_NOT_IMPLEMENTED` without filling the buffers.
- `ntio_iocp_release_concurrency` is marked unused, so nothing decrements the
  active count through it.
- The APC list is one global list in the shim. Per-thread queues are only in
  the planned kernel design.
- `NtRemoveIoCompletion` and `NtRemoveIoCompletionEx` each have two definitions,
  one per build branch.
- There is no `iocp_test.c`.

### mm ([modules/mm.md](modules/mm.md))

- A comment in the kernel free path (`mm.c:1189-1192`) describes a call that is
  commented out. The active call is `kshim_munmap`.
- `kshim_mprotect` finds the unexported `do_mprotect_pkey` through
  `kallsyms_lookup_name` and falls back to a hand-written path. This depends on
  kernel internals.
- `NtMmHandleGuardFault` is a simulation hook, not real page-fault handling.
- `mm.c:147` uses `0x10000` as a default address. The build branch was not traced.
- The shim VAD list and the kernel rb-tree are separate code paths. Only
  `ntmm_check_invariants` is shared.

### object ([modules/object.md](modules/object.md))

- `ObCreateObjectType` is declared but has no definition. Only
  `ObRegisterObjectType` with a static type works.
- Case-insensitive comparison is ASCII only.
- A symbolic link in a middle path component is not followed
  (`object.c:426-429`).
- Only `registry.c` sets the `parse` hook.
- Security descriptors are stored, but no access check is enforced here.

### proc_thread ([modules/proc_thread.md](modules/proc_thread.md))

- In the kernel build, `NtCreateProcessEx`, `NtCreateProcess` and
  `NtCreateUserProcess` return `STATUS_NOT_IMPLEMENTED`. Only the shim creates
  processes.
- `NtGetContextThread` and `NtSetContextThread` are not implemented.
- In the shim, `NtSetInformationProcess` and `NtSetInformationThread` return
  `STATUS_INVALID_INFO_CLASS` for every class, so sets always fail there.
- The shim has no copy-on-write isolation, no separate VAD, no fork pid, and
  synthesises the image name.
- The trampoline page is RWX-style, which the design note itself flags.

### registry ([modules/registry.md](modules/registry.md))

- `NtFlushKey` writes only a magic value and a count that is always 0, so the
  snapshot holds no key or value data.
- The regf-compatible hive writer is not implemented.
- The Machine hive is protected by a path check, not by the security module.
- `REG_OPTION_CREATE_LINK` and `REG_OPTION_OPEN_LINK` have no effect.
- Per-key directory persistence from the design note is not implemented.

### security ([modules/security.md](modules/security.md))

- `se_access_check`, `token_create_primary`, `sd_create_absolute` and
  `se_inherit_dacl` have no call sites outside the module and its tests. The
  object, handle, registry, io and mm paths do not enforce access.
- The ACL capacity table holds 256 entries and silently drops later ones.
- The module has no synchronisation.

### sync ([modules/sync.md](modules/sync.md))

- The timer thread is described as "simplified". It sleeps in one-second steps
  and recomputes the remaining time approximately.
- Two sort helpers exist, and which one each call site uses was not traced.
- Keyed events use a global waiter list, not per-object state.
- The userspace build maps primitives to pthreads. The kernel wait-queue path is
  not exercised by `sync_test.c`.

### ntstatus_kernel ([modules/ntstatus_kernel.md](modules/ntstatus_kernel.md))

- Resolved: `ntstatus_table.inc` is tracked (520 entries) and generated by
  `make -f build/Makefile ntstatus-table`. See
  [DEVELOPMENT.md](DEVELOPMENT.md#generated-file-ntstatus_tableinc). The unit
  test for the generator checks that the tracked file matches it.
- Open: the table order is not checked. The current table is sorted by signed
  value, but the generator does not enforce that, and `ntstatus_kernel.c`
  binary-searches it, so a misordered table gives wrong answers without an error.
- Open: statuses with bit 28 set are normalised only when the top nibble is 0xD.

## Top findings from the static review

The review ([review/INDEX.md](review/INDEX.md)) covers four areas: the PE
parser (`src/loader`), the kernel PE loader (`src/kernel/fs`), the NT
personality (`arch/x86/nt`), and the NT kernel objects (`src/kernel/nt`). It
found 28 high, 43 medium, 60 low and 6 info findings. Nothing was compiled or
run. The most important ones, with their fix status, are:

1. **Out-of-bounds read on a crafted PE.** The `e_lfanew` bounds check in
   `src/kernel/fs/binfmt_pe.c` wrapped in u32 (lines 181 and 639). Fixed in
   b8930c9 (exe path) and 4ffc5e5 (ntdll path). See
   [review/kernel-fs.md](review/kernel-fs.md) (K-1).
2. **Kernel pointer used as a section handle.** `NtCreateSection` returns the
   object address to user space, and `NtMapViewOfSection` dereferences the
   value it is given (`arch/x86/nt/mm.c:1540`, `:1549`). Open: it needs a real
   object-table handle and a type-checked lookup. See
   [review/arch-x86-nt.md](review/arch-x86-nt.md) (M-2) and
   [review/kernel-nt.md](review/kernel-nt.md) (M-1).
3. **Heap over-read in rename.** `FileNameLength` was not checked, so a rename
   could copy up to about 64 KiB past its buffer (`arch/x86/nt/nt_io.c:2272`).
   Fixed in 79d1de2. See [review/arch-x86-nt.md](review/arch-x86-nt.md) (IO-1).

Other high-priority items from the same review:

- Uninitialised kernel heap returned to user space by `NtQueryVirtualMemory`
  (`dispatch.c:258-264`, arch D-1). Fixed in 16af7b7 (`kzalloc`). The length
  cap is not done.
- `NtReadFile` returned success without reading (`nt_io.c:1894-1904`, arch
  IO-2). Fixed in 6da43d2.
- Any process can open and terminate any other by ID, with no access check
  (`proc_thread.c:1589-1592`, arch P-1). Open: needs an access policy.
- Sections can be writable and executable with no `mprotect` (`binfmt_pe.c:438`,
  `713`, kernel-fs K-3). Open.
- Exec returns errors after the point of no return (`binfmt_pe.c:906-942`,
  kernel-fs K-4). Open.

Cross-cutting problems the review flags:

- The kernel loader does not use the hardened parser in
  `src/loader/pe_parse.c` (loader L-0). Open.
- The build may not be reproducible. `arch/x86/nt/kernel_stubs.c` is not C
  (arch B-1), and the two `mm.c` copies differ (arch B-3). The arch-to-src
  port is tracked in [analysis/tree-divergence.md](analysis/tree-divergence.md).
  Open for `mm.c` and `kernel_stubs.c`.
- There is no access control on handles and processes (arch IO-3, IO-4, P-1;
  kernel-nt IO-2, P-1). Open.

The review's suggested order of work is in
[review/INDEX.md](review/INDEX.md#suggested-order).
