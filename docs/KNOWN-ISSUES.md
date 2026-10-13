# Known issues and gaps

This page collects the known gaps recorded in the module reference
([docs/modules/](modules/INDEX.md)), the open items from the static review
([docs/review/INDEX.md](review/INDEX.md)) and the open questions from the
commit audit ([docs/review/audit-fixes.md](review/audit-fixes.md)). It is a
summary. Each linked page has the details and line references. Items that were
fixed are removed; the fix is recorded in the review report or the decision
record. Decisions the owner must make are listed in
[docs/OWNER-DECISIONS.md](OWNER-DECISIONS.md). The project is an incomplete
research prototype (see the top-level [README](../README.md)).

Status as of 2026-10-12. The fixes were not built in a kernel tree and were not
run there. On 2026-10-12 the unit suite passed (<!-- counts:unit_tests:begin -->546<!-- counts:unit_tests:end --> `test_` functions in <!-- counts:unit_modules:begin -->26<!-- counts:unit_modules:end --> modules).
The syntax check passed 36 of 41 files at the last audit pass
([tests/syntax/README.md](../tests/syntax/README.md)). Decision records and their
status are indexed in [docs/decisions/INDEX.md](decisions/INDEX.md).

The review found <!-- counts:review_findings:begin -->220<!-- counts:review_findings:end --> findings
(<!-- counts:review_high:begin -->50<!-- counts:review_high:end --> high,
<!-- counts:review_medium:begin -->72<!-- counts:review_medium:end --> medium,
<!-- counts:review_low:begin -->84<!-- counts:review_low:end --> low and
<!-- counts:review_info:begin -->14<!-- counts:review_info:end --> info).
Of these, <!-- counts:fix_fixed:begin -->113<!-- counts:fix_fixed:end --> are fixed,
<!-- counts:fix_partly:begin -->11<!-- counts:fix_partly:end --> are partly fixed,
<!-- counts:fix_blocked:begin -->41<!-- counts:fix_blocked:end --> are blocked on an owner decision,
<!-- counts:fix_no_change:begin -->7<!-- counts:fix_no_change:end --> were checked and are not defects, and
<!-- counts:fix_open:begin -->48<!-- counts:fix_open:end --> are open.

## Gaps by module

### dispatch ([modules/dispatch.md](modules/dispatch.md))

- Six hard-coded SSNs call `pr_debug` on every call (`dispatch.c:114`). The
  calls are silent unless dynamic debug is enabled, but they stay in the hot path.
- `nt_can_sysret` is a stub that always returns `true`.
- The table covers SSNs up to 0x1db, but only 171 entries are listed. The rest
  return `STATUS_NOT_IMPLEMENTED`.
- 27 of the 171 listed services have no `nt_svc_` definition (for example
  `NtCreateSectionEx`, `NtMapViewOfSectionEx`, and the port, debug and job-object
  services). See [status/SERVICES.md](status/SERVICES.md).
- The virtual-memory wrappers are defined in `dispatch.c` and declared `extern`
  there. There is no `dispatch.h`.

### handle ([modules/handle.md](modules/handle.md))

- `NT_CURRENT_THREAD_HANDLE` in `handle.h` (line 35) has unbalanced parentheses.
  It is unused, so the build is not affected.
- Closed slots go to the tail of a FIFO free list and are reused only after more
  than `HANDLE_QUARANTINE` other slots have been freed (`d7adc94`, record 0003,
  option 1). Handle values still have no generation counter (option 2 is open),
  so a stale handle can refer to a reused slot.
- The `handle.c` header comments at lines 22 and 60 still describe the old LIFO
  free list.
- The table has no growth limit beyond its 2^24 index space, although the header
  describes memory-based growth.
- Inheritance is all or nothing (`ObInheritHandleTable`). The shim calls it at
  process creation; the kernel build does not call any inheritance path yet.

### io ([modules/io.md](modules/io.md))

- The header says async work runs on a workqueue. The code has no workqueue
  calls, so completion appears to run inline.
- Stdin is not backed in the shim build and returns `STATUS_END_OF_FILE`.
- `nt_svc_NtClose` has four definitions: `io.c:2980` (weak, raw-argument form),
  `io.c:3019`, `nt_obj.c:1061` and `nt_obj.c:1727`. Two strong `HANDLE`-form
  definitions in one link would conflict. `tests/kernel-host` works around the
  duplicate with `objcopy`. The link for each build was not traced.
- Share enforcement exists only in the kernel. Non-NT opens are not blocked.
- `ntio_resolve_path` (the `\??` and `\Device` mapping) was not traced in full.
- Directory enumeration in the kernel build returns `STATUS_NOT_SUPPORTED`.
- `NtLockFile` ignores `FailImmediately`, and a conflict always returns
  `STATUS_LOCK_NOT_GRANTED`.
- Three four-KiB stack path buffers remain in `NtQueryAttributesFile`,
  `NtDeleteFile` and `NtQueryFullAttributesFile` (audit pass 3, item 7).
- The kernel wrappers target a specific kernel version (6.18 API changes), and
  NFS delegation is not handled.

### iocp ([modules/iocp.md](modules/iocp.md))

- `ntio_iocp_release_concurrency` is marked unused, so nothing decrements the
  active count through it (kernel-nt IOCP-1, Medium, open: record 0009).
- The APC list is one global list in the shim. Per-thread queues are only in the
  planned kernel design.
- `NtRemoveIoCompletion` and `NtRemoveIoCompletionEx` each have two definitions,
  one per build branch.
- There is no `iocp_test.c`.

### mm ([modules/mm.md](modules/mm.md))

- Section services have `nt_svc_` wrappers now. `NtCreateSectionEx`,
  `NtMapViewOfSectionEx` and `NtUnmapViewOfSectionEx` are not in the service table.
  `NtLockVirtualMemory` and `NtUnlockVirtualMemory` are implemented (host page lock in
  the userspace build, `STATUS_NOT_SUPPORTED` in the kernel build).
- `SEC_IMAGE` sections are built in the userspace build only. The kernel build
  returns `STATUS_NOT_IMPLEMENTED` (process-creation milestone M6, kernel side).
- `NtQuerySection(SectionImageInformation)` returns `STATUS_NOT_IMPLEMENTED`.
- `NtMmHandleGuardFault` is a simulation hook, not real page-fault handling.
- `kshim_mprotect` finds the unexported `do_mprotect_pkey` through
  `kallsyms_lookup_name` and falls back to a hand-written path.
- The shim VAD list and the kernel rb-tree are separate code paths. Only
  `ntmm_check_invariants` is shared.
- The arch copy of `mm.c` still returns the object address as the section handle
  (arch M-2, open: record 0021).

### object ([modules/object.md](modules/object.md))

- `ObCreateObjectType` is declared but has no definition. Only
  `ObRegisterObjectType` with a static type works.
- `OBJ_PERMANENT` has no lifetime effect. `is_permanent` is set and never read
  (audit pass 4, item 3, open).
- A symbolic link in a middle path component is not followed (`object.c:434`).
- `ObpRemoveFromDirectory` reads `parent_dir` before it takes the directory
  lock (audit pass 4, item 15, open; kernel-nt C-2 is the related insert race).
- Case-insensitive comparison is ASCII only.
- The security descriptor is not applied at object creation (`ObCreateObject`
  does not read `OBJECT_ATTRIBUTES.SecurityDescriptor`).
- `OBJECT_ATTRIBUTES.RootDirectory` is not used by `ObInsertOrOpenNamedObject`,
  so a relative name fails (audit pass 4, item 2, open).

### proc_thread ([modules/proc_thread.md](modules/proc_thread.md))

- In the kernel build, `NtCreateProcessEx`, `NtCreateProcess` and the process
  part of `NtCreateUserProcess` return `STATUS_NOT_IMPLEMENTED`. `NtCreateUserProcess`
  checks its arguments first. Only the shim creates processes.
- `NtGetContextThread` and `NtSetContextThread` are not implemented.
- `arch/x86/nt/proc_thread.c` still returns `STATUS_INVALID_INFO_CLASS` from
  `NtSetInformationProcess` and `NtSetInformationThread`. The class
  implementation is in `src/kernel/nt/proc_thread.c` only.
- The kernel build copies `PreviousSuspendCount` to user memory while holding
  `thr->lock` (`proc_thread.c:1500` and `:1541`).
- Any caller can open any process or thread by ID (see OD-01).
- The shim has no copy-on-write isolation, no separate VAD and no fork pid, and
  it synthesises the image name.
- The trampoline page is RWX-style, which the design note itself flags.

### registry ([modules/registry.md](modules/registry.md))

- The regf-compatible hive writer is not implemented.
- The Machine hive is protected by a path check, not by the security module.
- `REG_OPTION_CREATE_LINK` is refused, and `REG_OPTION_OPEN_LINK` has no effect.
- `REG_OPTION_VOLATILE` and `REG_OPTION_BACKUP_RESTORE` are ignored.
- Per-key directory persistence from the design note is not implemented.

### security ([modules/security.md](modules/security.md))

- `se_access_check` is called by the token and descriptor services. The object,
  handle, registry, io and mm open and create paths do not call it, so they do
  not enforce descriptors.
- `NtSetSecurityObject` refuses an object with no descriptor
  (`STATUS_ACCESS_DENIED`), so the owner cannot attach one (audit pass 4, item 4).
- The ACL capacity table holds 256 entries and silently drops later ones
  (kernel-nt SEC-1, record 0035, blocked).
- `ntsec_sr_length` adds the `ULONG` offset and size after the 64-bit comparison.
  The result is fail-closed (audit pass 4, item 11).
- `NtAccessCheckAndAuditAlarm` writes no audit record.
- The module has no locks apart from the token reference count and the token ID counter.

### sync ([modules/sync.md](modules/sync.md))

- Timer APC routines are not implemented. `NtSetTimer` returns
  `STATUS_NOT_IMPLEMENTED` when a routine is given (`sync.c:2603`).
- The header comment names `kernel_sort_by_addr`, which does not exist. The only
  sort is `sort_by_addr`.
- Keyed events use a global waiter list, not per-object state.
- The userspace build maps primitives to pthreads. The kernel wait-queue path is
  not exercised by `sync_test.c`.

### ntstatus_kernel ([modules/ntstatus_kernel.md](modules/ntstatus_kernel.md))

- Open: the table order is not checked. The current table is sorted by signed
  value, but the generator does not enforce that, and `ntstatus_kernel.c`
  binary-searches it, so a misordered table gives wrong answers without an error.
- Open: statuses with bit 28 set are normalised only when the top nibble is 0xD.

### Modules added in this sweep

- `image_plan`, `image_reloc`: userspace `SEC_IMAGE` only. The planner refuses
  32-bit images. The relocation applier refuses HIGHADJ and reserved types.
  See [modules/image_plan.md](modules/image_plan.md) and
  [modules/image_reloc.md](modules/image_reloc.md).
- `process_params`: not called by any service (milestone M1, open).
- `proc_create_args`: the `NtCreateUserProcess` stubs call it, then refuse
  (milestone M5, partial).

## Open audit questions

From the resolution sections of [audit-fixes.md](review/audit-fixes.md). Each
needs a decision before the code changes. They are listed as owner decisions in
[OWNER-DECISIONS.md](OWNER-DECISIONS.md).

- `1f334ce`: `sprintf` and `vsprintf` cut output silently at 1023 bytes. A
  caller that formats 1024 or more bytes gets short output with no error.
- `fd86cb3`: does Windows `VirtualAlloc` accept protection 0 with `MEM_RESERVE`?
  If it does, the commit makes a valid call fail. There is no Windows reference
  on this host.
- `cc755fa`: a duplicate created with `OBJ_PROTECT_FROM_CLOSE` cannot be undone,
  so the handle and its reference leak on the undo path.
- `a90862b`: `NtQueryAttributesFile`, `NtDeleteFile` and `NtQueryFullAttributesFile`
  still use 4 KiB stack path buffers.
- `7a676a1`: `RootDirectory` is not honoured by `ObInsertOrOpenNamedObject`.
- `6b86fe8`: an anonymous `OBJ_PERMANENT` object does not keep a reference.
- `3477776`: `NtSetSecurityObject` refuses an object with no descriptor.
- `0520a92`: `snap_walk_key` recurses once per key level with no depth bound.
- `ecc7f83`: `ObpRemoveFromDirectory` reads `parent_dir` before taking the lock.
- `2393db8`: the 255-character create is skipped on hosts where `PATH_MAX` is
  260. The skip is documented and kept.
- Audit pass 4 observed that `proc_thread_test` prints `leaks 34` at exit and
  still returns 0.

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
- `src/kernel/fs/binfmt_pe_test.c` does not compile. It calls loader helpers
  that were never written (`pe_build_command_line`, `pe_build_environment`,
  `pe_utf8_to_utf16`, `pe_prot_to_mmap`), and uses `PPEB`, `PTEB`,
  `RTL_USER_PROCESS_PARAMETERS` and `pe_load_state.pe`, none of which are
  defined. Record [0028](decisions/0028-binfmt-pe-test-missing-helpers.md)
  (Proposed) lists the options.
- `src/kernel/nt/kshim.c` depends on glibc `st_atim`. The file now uses
  `KSHIM_STAT_*` macros, which fall back to `st_atime` on non-Linux builds. The
  Linux branch is not compiled on this machine.
- The syntax check fails on five files: `src/kernel/nt/dispatch.c`,
  `src/kernel/nt/kshim.c`, `src/kernel/fs/binfmt_pe.c`,
  `src/kernel/fs/binfmt_pe_test.c` and `arch/x86/nt/dispatch.c` (audit passes 2
  to 4).

## Host unit-test harness (tests/kernel-host)

Status from a clean rebuild on 2026-10-12, recorded in
[tests/kernel-host/README.md](../tests/kernel-host/README.md#status):

- The build succeeds. Earlier notes that it "does not build" are stale.
- `mman_test` (`shim/mman_test.c`, host shim self-test) is intermittent: 2 of
  14 runs failed. Not root-caused. This is a shim issue
  (`tests/kernel-host/shim/mman_win.c`), not a `src/` issue.
- `io_test` `path_parsing` (mixed-case lookup) failed once in 13 runs. Not
  root-caused.
- `proc_thread_test` prints `leaks 34` (audit pass 4) before teardown. That is
  the live count, and the at-exit count is 0.

## Top findings from the static review

The review ([review/INDEX.md](review/INDEX.md)) covers five areas: the PE parser
(`src/loader`), the kernel PE loader (`src/kernel/fs`), the NT personality
(`arch/x86/nt`), the NT kernel objects (`src/kernel/nt`), and the user-mode
runtime (`src/ntdll`). Nothing was compiled or run. The most important items
are listed below with their fix status. Line numbers are from the review
snapshot (`5eb8026`) and have moved after later fixes.

1. **Kernel pointer used as a section handle (arch copy).** The `src/kernel/nt`
   copy is fixed: section handles are handle-table indices (record
   [0001](decisions/0001-section-handle-object.md), `c8f479e`). The arch copy
   still stores the object address (`arch/x86/nt/mm.c`) and dereferences the
   value it is given. Open: record [0021](decisions/0021-arch-section-handle-pointer.md).
   See [review/arch-x86-nt.md](review/arch-x86-nt.md) (M-2).

Other high-priority items:

- Any process can open and terminate any other by ID, with no access check
  (`proc_thread.c`, arch P-1). Open: records [0002](decisions/0002-process-open-access-check.md)
  and [0025](decisions/0025-arch-process-thread-open-access.md) (OD-01).
- Sections can be writable and executable with no `mprotect` (kernel-fs K-3).
  Blocked: record [0011](decisions/0011-kernel-wx-sections.md) (OD-03).
- The file handle table is global, and handles are shared across processes
  (arch IO-3). Open: records [0022](decisions/0022-arch-global-file-handle-table.md)
  and [0007](decisions/0007-file-handle-ownership.md) (OD-04).
- `FILE_SUPERSEDE` deletes the existing file before the open can succeed (arch
  IO-5). Blocked: record [0024](decisions/0024-arch-supersede-unlink-order.md) (OD-06).
- The MinGW build compiles only loader stubs (ntdll LDR-1). Open: record
  [0029](decisions/0029-loader-mingw-build-selection.md) (OD-17).
- DllMain and TLS callbacks are never called (ntdll LDR-3). Open: record
  [0031](decisions/0031-loader-dllmain-tls-order.md) (OD-18).
- `RtlUnwindEx` runs no unwind handlers and does not transfer control (ntdll
  UNW-2). Open; needs the second-pass design (OD-19).

Cross-cutting problems the review flags:

- The kernel loader does not use the hardened parser in `src/loader/pe_parse.c`
  (loader L-0). Open.
- The build may not be reproducible. `arch/x86/nt/kernel_stubs.c` now defines
  `nt_banner_init()` (record [0027](decisions/0027-arch-kernel-stubs-file.md),
  `974db57`), and `vm/kbuild.sh` still writes the same banner from a heredoc.
  The two `mm.c` copies still differ (arch B-3, needs owner; OD-12).
- The arch-to-src port is tracked in [analysis/tree-divergence.md](analysis/tree-divergence.md).

The review's suggested order of work is in
[review/INDEX.md](review/INDEX.md#suggested-order).
