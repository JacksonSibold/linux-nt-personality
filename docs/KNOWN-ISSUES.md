# Known issues and gaps

This page collects the known gaps recorded in the module reference
([docs/modules/](modules/INDEX.md)) and the highest-priority findings from the
static review ([docs/review/INDEX.md](review/INDEX.md)). It is a summary.
Each linked page has the details and line references. The project is an
incomplete research prototype (see the top-level [README](../README.md)).
Fix status below comes from the "Fix status" sections and per-finding status
lines in the review reports, as of 2026-10-12. The fixes were not built in a
kernel tree and were not run. On 2026-10-12 the unit suite passed (<!-- counts:unit_tests:begin -->489<!-- counts:unit_tests:end --> `test_` functions in <!-- counts:unit_modules:begin -->25<!-- counts:unit_modules:end --> modules),
and the syntax check passed 32 of 37 files
([tests/syntax/README.md](../tests/syntax/README.md)). Decision records and their
status are indexed in [docs/decisions/INDEX.md](decisions/INDEX.md).

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
- Closed slots go to the tail of a FIFO free list and are reused only after more
  than `HANDLE_QUARANTINE` other slots have been freed (`d7adc94`, record 0003
  option 1). Handle values still have no generation
  counter (option 2 is open), so a stale handle can refer to a reused slot.
  The header comment at `handle.c:22` still describes the old LIFO free list.

### io ([modules/io.md](modules/io.md))

- The header says async work runs on a workqueue. The code has no workqueue
  calls, so completion appears to run inline.
- Stdin is not backed in the shim build and returns `STATUS_END_OF_FILE`.
- Generic file mapping is "simplified".
- `nt_svc_NtClose` has four definitions in `src/kernel/nt`: `io.c:2467` (weak,
  kernel), `io.c:2506` and `nt_obj.c:762` (both non-kernel and strong), and
  `nt_obj.c:1086` (kernel). Two strong definitions in one non-kernel link would
  conflict. `tests/kernel-host` works around this with `objcopy`. The build that
  links each one was not traced.
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

## Kernel flags and syntax-check gaps

- `TIF_NTABI` and `TIF_NTSUSPEND` are not defined by any source in this
  repository's userspace model, and their values must come from the kernel
  patches. `src/kernel/patches/0001-nt-personality-dispatch.patch` defines
  `TIF_NTABI` as 29 in `arch/x86/include/asm/thread_info.h`. No patch under
  `src/kernel/patches/` defines `TIF_NTSUSPEND`. The only value in the repo is
  30, a fallback `#define` in `src/kernel/nt/proc_thread.c` and
  `arch/x86/nt/proc_thread.c`, and `vm/rebuild_kdir.sh` adds it to the kernel
  header. Set both from the patches. Do not take the 32/33 values in
  `tests/syntax/shim/linux/thread_info.h`: those are syntax-check placeholders.
  See the header comment in `src/kernel/fs/kshim.h`.
- `src/kernel/fs/binfmt_pe_test.c` does not compile yet. It calls loader
  helpers that were never written in any commit (`pe_build_command_line`,
  `pe_build_environment`, `pe_utf8_to_utf16`, `pe_prot_to_mmap`), uses
  `PPEB`, `PTEB`, `RTL_USER_PROCESS_PARAMETERS` and `pe_load_state.pe`, none of
  which are defined. Record
  [0028](decisions/0028-binfmt-pe-test-missing-helpers.md) (status Proposed)
  lists the options. `binfmt_pe.c` reads `inode->i_mode` and calls
  `i_uid_read()`; the syntax-check shim `tests/syntax/shim/linux/fs.h` now
  declares both (`5304c09`), so that part no longer blocks the check. The
  userspace model's kernel API signatures are fixed; the rest needs loader or
  stub changes.
- `src/kernel/nt/kshim.c` depends on glibc `st_atim`. The file now uses
  `KSHIM_STAT_*` macros, which fall back to `st_atime` on non-Linux builds.
  The Linux branch is not compiled on this machine.

## Host unit-test harness (tests/kernel-host)

Status from a clean rebuild on 2026-10-12, recorded in
[tests/kernel-host/README.md](../tests/kernel-host/README.md#status):

- The build succeeds. Earlier notes that it "does not build" are stale.
- `mman_test` (`shim/mman_test.c`, host shim self-test) is intermittent: 2 of
  14 runs failed. One failure was the `NOREPLACE over hole + free` check (line
  57, EEXIST) followed by a segfault; the other was a segfault alone. Not
  root-caused. This is a shim issue (`tests/kernel-host/shim/mman_win.c`), not
  a `src/` issue.
- `io_test` `path_parsing` (io_test.c:558, mixed-case lookup) failed once in
  13 runs. Not root-caused.
- `registry_test` ran and passed on this run (10/10). The Windows Application
  Control block mentioned in earlier notes was not hit.
- `proc_thread_test` prints `leaks 27` before teardown; that is the live count,
  and the at-exit count is 0.

## Top findings from the static review

The review ([review/INDEX.md](review/INDEX.md)) covers four areas: the PE
parser (`src/loader`), the kernel PE loader (`src/kernel/fs`), the NT
personality (`arch/x86/nt`), and the NT kernel objects (`src/kernel/nt`). It
found 28 high, 43 medium, 60 low and 6 info findings. Nothing was compiled or
run. The most important ones are listed below with their fix status. Line
numbers are from the review snapshot (`5eb8026`) and have moved after later
fixes; the review reports have the current references:

1. **Out-of-bounds read on a crafted PE.** The `e_lfanew` bounds check in
   `src/kernel/fs/binfmt_pe.c` wrapped in u32 (lines 181 and 639). Fixed in
   b8930c9 (exe path) and 4ffc5e5 (ntdll path). See
   [review/kernel-fs.md](review/kernel-fs.md) (K-1).
2. **Kernel pointer used as a section handle.** `NtCreateSection` returns the
   object address to user space, and `NtMapViewOfSection` dereferences the
   value it is given (`arch/x86/nt/mm.c:1540`, `:1549`). Open (records 0001 and
   0021): it needs a real object-table handle and a type-checked lookup. See
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
  `713`, kernel-fs K-3). Blocked: record [0011](decisions/0011-kernel-wx-sections.md)
  needs an owner decision.
- Exec returned errors after the point of no return (`binfmt_pe.c:906-942`,
  kernel-fs K-4). Fixed in 8eac32c (record 0012). Failures before
  `begin_new_exec` are unchanged.

Cross-cutting problems the review flags:

- The kernel loader does not use the hardened parser in
  `src/loader/pe_parse.c` (loader L-0). Open.
- The build may not be reproducible. `arch/x86/nt/kernel_stubs.c` was not C
  and broke the `CONFIG_NT_PERSONALITY` build. Fixed in 974db57 (arch B-1,
  record [0027](decisions/0027-arch-kernel-stubs-file.md)): the file now
  defines `nt_banner_init()`, the same as the vm scripts write. The vm scripts'
  heredocs still exist, so the banner text is in three places. The two `mm.c`
  copies still differ in about 120 changed lines (arch B-3, open). The arch-to-src port is tracked in
  [analysis/tree-divergence.md](analysis/tree-divergence.md).
- There is no access control on handles and processes (arch IO-3, IO-4, P-1;
  kernel-nt IO-2, P-1). Open. `NtReadFile` and `NtWriteFile` now check
  `FILE_READ_DATA` and `FILE_WRITE_DATA` (or `FILE_APPEND_DATA`) in
  `src/kernel/nt` (kernel-nt IO-1, fixed in 1d160e8 and 80b6fea). The arch
  copy of those checks is not covered by that fix.

The review's suggested order of work is in
[review/INDEX.md](review/INDEX.md#suggested-order).
