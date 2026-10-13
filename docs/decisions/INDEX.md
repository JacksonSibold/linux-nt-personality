# Decision records: index and status

Each record is one finding from the static review ([docs/review/INDEX.md](../review/INDEX.md)) with the options considered. The status below is taken from the `Status:` line of each record, and the commit named there is checked against `git log`. Nothing was compiled or run to confirm the fixes.

Status values:

- **fixed**: the record is accepted and names the commit that implements it.
- **blocked**: the record needs an owner decision before it can be implemented (its status line says so).
- **no-change**: the record is accepted and no code change is needed.
- **open**: the record is proposed and not implemented.

Snapshot: 2026-10-12, `HEAD` at the time this index was written. 73 records: 15 fixed, 16 open, 41 blocked, 1 no-change.

| Record | Title | Status | Finding | Commit |
|---|---|---|---|---|
| [0001](0001-section-handle-object.md) | Section handles come from the object table | fixed | kernel-nt M-1 (High) | c8f479e |
| [0002](0002-process-open-access-check.md) | Access check and reference counting for NtOpenProcess / NtOpenThread | open | kernel-nt P-1 (High) | |
| [0003](0003-handle-slot-reuse.md) | Handle slot reuse and index exhaustion | fixed (option 1; option 2 still open) | kernel-nt H-2 (Medium) | d7adc94 |
| [0004](0004-section-object-lifetime.md) | Section object lifetime | open | kernel-nt M-3 (Medium) | |
| [0005](0005-vad-commit-rollback.md) | VAD state rollback on failed commit or decommit | blocked | kernel-nt M-4 (Medium) | |
| [0006](0006-io-write-any-of-access.md) | NtWriteFile requires FILE_WRITE_DATA or FILE_APPEND_DATA | fixed | kernel-nt IO-1, write half (Medium) | 80b6fea |
| [0007](0007-file-handle-ownership.md) | File handle table is global with no owner | open | kernel-nt IO-2 (Medium) | |
| [0008](0008-teb-address-disclosure.md) | TEB address returned by NtQueryInformationThread | open | kernel-nt PT-1 (Medium) | |
| [0009](0009-iocp-concurrency-release.md) | Concurrency slot is never released | open | kernel-nt IOCP-1 (Medium) | |
| [0010](0010-iocp-queue-quota.md) | Completion queue quota | fixed | kernel-nt IOCP-2 (Medium) | 0b7921b |
| [0011](0011-kernel-wx-sections.md) | Sections are mapped writable and executable | blocked | kernel-fs K-3 (High) | |
| [0012](0012-kernel-post-commit-exec-failure.md) | Error returns after the point of no return in PE exec | fixed | kernel-fs K-4 (High) | 8eac32c |
| [0013](0013-kernel-ntdll-trusted-path.md) | ntdll is loaded from a world-writable path | fixed | kernel-fs K-5 (High) | 13a4357 |
| [0014](0014-kernel-ntmm-add-region-results.md) | ntmm_add_region() results are ignored in the exec path | fixed | kernel-fs K-10 (Medium) | 7729288 |
| [0015](0015-kernel-weak-terminate-process.md) | Weak NtTerminateProcess fallback kills the caller | fixed | kernel-fs K-12 (Low) | 83e3349 |
| [0016](0016-kernel-clear-user-return.md) | clear_user() return values are ignored in the exec path | fixed | kernel-fs K-13 (Low) | 419f675 |
| [0017](0017-loader-public-unchecked-readers.md) | Public unchecked PE readers | open | loader L-9 (Low) | |
| [0018](0018-loader-diagnostic-buffer.md) | Shared diagnostic buffer and kernel log flooding | open | loader L-10 (Low) | |
| [0019](0019-loader-aligned-highest-wrap.md) | aligned_highest u32 wrap is not reachable | no-change | loader L-14 (Info) | |
| [0020](0020-arch-global-vad-table.md) | One global VAD table for every process | open | arch M-1 (High) | |
| [0021](0021-arch-section-handle-pointer.md) | Section handle is a kernel pointer | open | arch M-2 (High) | |
| [0022](0022-arch-global-file-handle-table.md) | Handle table is global; handles are shared across processes | open | arch IO-3 (High) | |
| [0023](0023-arch-file-access-rights.md) | No access rights enforced on file operations | blocked | arch IO-4 (High) | |
| [0024](0024-arch-supersede-unlink-order.md) | FILE_SUPERSEDE deletes the file before the open can succeed | blocked | arch IO-5 (High) | |
| [0025](0025-arch-process-thread-open-access.md) | Process and thread open without access checks | open | arch P-1 (High) | |
| [0026](0026-arch-teb-kernel-heap.md) | TEB is kernel heap memory and is used as the GS base | open | arch P-2 (High) | |
| [0027](0027-arch-kernel-stubs-file.md) | kernel_stubs.c is not C and breaks the build | fixed | arch B-1 (High) | 974db57 |
| [0028](0028-binfmt-pe-test-missing-helpers.md) | binfmt_pe_test.c calls helpers that are not defined anywhere | open | kernel-fs T-1, T-2 (Medium) | |
| [0029](0029-loader-mingw-build-selection.md) | MinGW build compiles only loader stubs | fixed | ntdll LDR-1 (High) | 10f42c8 |
| [0030](0030-loader-iat-rva-patch.md) | IAT slot writes use a file offset, not an RVA | fixed | ntdll LDR-2 (High) | a18c03e |
| [0031](0031-loader-dllmain-tls-order.md) | DllMain and TLS callbacks are never called | open | ntdll LDR-3 (High) | 10f42c8 (partly) |
| [0032](0032-loader-tls-callback-table-rva.md) | TLS callback table read at its preferred-base address | fixed | ntdll LDR-4 (High) | bef4d79 |
| [0033](0033-kernel-exclusive-open-atomicity.md) | Exclusive-open check is not atomic with handle creation | blocked | kernel-nt O-2 (Low) | |
| [0034](0034-kernel-timeout-pointer-provenance.md) | timeout_to_jiffies dereferences non-user pointers | blocked | kernel-nt SYNC-2 (Low) | |
| [0035](0035-kernel-acl-capacity-table.md) | ACL capacity table silently drops registrations | blocked | kernel-nt SEC-1 (Low) | |
| [0036](0036-loader-import-walk-budget.md) | Import walk has no total bound across descriptors | fixed | loader L-16 (Medium) | 7f7e5d1 |
| [0037](0037-process-creation.md) | How NtCreateUserProcess and CreateProcess create a PE process | open | design (no review finding) | |
| [0038](0038-arch-io-root-resolve-beneath.md) | Path mapping is a string prefix; symlinks escape the root; default root is the host | blocked | arch IO-11 (Medium) | |
| [0039](0039-arch-io-non-ascii-names.md) | Non-ASCII NT names collapse to '?' | blocked | arch IO-15 (Low) | |
| [0040](0040-arch-trampoline-rwx.md) | Synthetic trampoline page is RWX and never unmapped | blocked | arch P-5 (Medium) | |
| [0041](0041-loader-wx-policy.md) | Protection mapping has no W^X policy | blocked | loader L-12 (Low) | |
| [0042](0042-kernel-fs-ntdll-fallback.md) | Exec continues without ntdll when the ntdll image fails | blocked | kernel-fs K-18 (Low) | |
| [0043](0043-ntdll-gettempfilename.md) | GetTempFileNameA uses a fixed path and an unbounded copy | blocked | ntdll K32-9 (Medium) | |
| [0044](0044-ntdll-sprintf-contract.md) | sprintf and vsprintf write without a size | blocked | ntdll CRT-1 (High) | |
| [0045](0045-arch-build-tree-warnings.md) | Canonical copy of the NT sources and warning policy | blocked | arch B-2, B-3 (Medium) | |
| [0046](0046-arch-weak-fallbacks.md) | Weak fallbacks hide missing strong symbols | blocked | arch B-4 (Low) | |
| [0047](0047-kernel-fs-kshim-guard.md) | kshim.h has no guard against kernel inclusion | blocked | kernel-fs S-1 (Low) | |
| [0048](0048-arch-thread-object-leak.md) | Thread object is leaked when there is no ethread | blocked | arch D-5 (Low) | |
| [0049](0049-arch-noinstr-logging.md) | noinstr function calls instrumented code | blocked | arch D-7 (Low) | |
| [0050](0050-arch-output-params-on-failure.md) | Output parameters are written back when the service fails | blocked | arch D-8 (Low) | |
| [0051](0051-arch-vad-protect-order.md) | VAD protection changed before the page protection | blocked | arch M-5 (Medium) | |
| [0052](0052-arch-vad-publish-window.md) | VAD is published before its mapping exists | blocked | arch M-11 (Medium) | |
| [0053](0053-arch-file-section-stub.md) | Kernel stub for file-backed sections drops the file | blocked | arch M-14 (Low) | |
| [0054](0054-arch-rename-root-ref.md) | Reference leak when renaming relative to a root directory | blocked | arch IO-8 (Medium) | |
| [0055](0055-arch-casefold-scan.md) | Case-insensitive resolution scans whole directories under a spinlock | blocked | arch IO-12 (Medium) | |
| [0056](0056-arch-replace-if-exists.md) | ReplaceIfExists is ignored on rename | blocked | arch IO-13 (Low) | |
| [0057](0057-arch-stack-path-buffers.md) | Large stack buffers in the file path | blocked | arch IO-16 (Low) | |
| [0058](0058-arch-eof-limit.md) | EndOfFile is not range-checked | blocked | arch IO-18 (Low) | |
| [0059](0059-kernel-nt-exclusive-open.md) | Exclusive-open check is not atomic with handle creation | blocked | kernel-nt O-2 (Low) | |
| [0060](0060-kernel-nt-timeout-pointer.md) | timeout_to_jiffies dereferences a pointer that may be a user address | blocked | kernel-nt SYNC-2 (Low) | |
| [0061](0061-kernel-nt-acl-capacity.md) | ACL capacity table silently drops registrations | blocked | kernel-nt SEC-1 (Low) | |
| [0062](0062-ntdll-unwind-bounds.md) | UNWIND_INFO and chain reads are not bounds-checked | blocked | ntdll UNW-5 (Low) | |
| [0063](0063-ntdll-second-pass.md) | No second pass for exception unwinding | blocked | ntdll EXC-3 (Medium) | |
| [0064](0064-ntdll-dispatch-state-lock.md) | Dispatch state is global and unsynchronised; lock primitive is not chosen | blocked | ntdll EXC-4, EXC-5 (Low), K32-14 (Medium) | |
| [0065](0065-ntdll-unsupported-formats.md) | printf-family stubs and float formats fail silently | blocked | ntdll RT-3 (Low), CRT-5 (Medium) | |
| [0066](0066-ntdll-errno-abi.md) | errno definition clashes with MinGW's _errno | blocked | ntdll RT-4 (Low) | |
| [0067](0067-ntdll-exit-terminate.md) | _exit spins on hlt and discards the status | blocked | ntdll RT-6 (Low) | |
| [0068](0068-ntdll-import-failure.md) | Import failures and thunk overflow do not stop process start | blocked | ntdll MAIN-3 (Medium), MAIN-6 (Low) | |
| [0069](0069-ntdll-win32-file-ops.md) | Win32 file operations report false success or always fail | blocked | ntdll K32-8 (Medium) | |
| [0070](0070-ntdll-file-mapping.md) | File mappings return a fake handle | blocked | ntdll K32-12 (Medium) | |
| [0071](0071-ntdll-registry-backend.md) | Registry functions always report "not found" | blocked | ntdll ADV-1 (Low) | |
| [0072](0072-kernel-nt-lpc-ports.md) | LPC ports (NtCreatePort family) are not implemented | fixed | none (service gap) | fa370b1 |
| [0073](0073-per-process-address-space.md) | Per-process address spaces for the memory manager | fixed | none (design gap) | 021e475 |

## Notes

- Records 0020 to 0027 use the arch-tree finding IDs. Several describe the same defect as a kernel-tree record (for example arch M-2 and kernel-nt M-1 both concern the section handle, and 0021 is to be decided together with 0001).
- The `Commit` column lists only the commit named in the record's status line. Other commits that touch the same finding are listed in the review reports.
- Record 0028 has no implementation commit. Its test file does not compile until one of its options is chosen.
