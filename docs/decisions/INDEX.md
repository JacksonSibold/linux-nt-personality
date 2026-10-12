# Decision records: index and status

Each record is one finding from the static review ([docs/review/INDEX.md](../review/INDEX.md)) with the options considered. The status below is taken from the `Status:` line of each record, and the commit named there is checked against `git log`. Nothing was compiled or run to confirm the fixes.

Status values:

- **fixed**: the record is accepted and names the commit that implements it.
- **blocked**: the record needs an owner decision before it can be implemented (its status line says so).
- **no-change**: the record is accepted and no code change is needed.
- **open**: the record is proposed and not implemented.

Snapshot: 2026-10-12, `HEAD` at the time this index was written. 32 records: 10 fixed, 16 open, 5 blocked, 1 no-change.

| Record | Title | Status | Finding | Commit |
|---|---|---|---|---|
| [0001](0001-section-handle-object.md) | Section handles come from the object table | open | kernel-nt M-1 (High) | |
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
| [0027](0027-arch-kernel-stubs-file.md) | kernel_stubs.c is not C and breaks the build | blocked | arch B-1 (High) | |
| [0028](0028-binfmt-pe-test-missing-helpers.md) | binfmt_pe_test.c calls helpers that are not defined anywhere | open | kernel-fs T-1, T-2 (Medium) | |
| [0029](0029-loader-mingw-build-selection.md) | MinGW build compiles only loader stubs | open | ntdll LDR-1 (High) | |
| [0030](0030-loader-iat-rva-patch.md) | IAT slot writes use a file offset, not an RVA | fixed | ntdll LDR-2 (High) | a18c03e |
| [0031](0031-loader-dllmain-tls-order.md) | DllMain and TLS callbacks are never called | open | ntdll LDR-3 (High) | |
| [0032](0032-loader-tls-callback-table-rva.md) | TLS callback table read at its preferred-base address | fixed | ntdll LDR-4 (High) | bef4d79 |

## Notes

- Records 0020 to 0027 use the arch-tree finding IDs. Several describe the same defect as a kernel-tree record (for example arch M-2 and kernel-nt M-1 both concern the section handle, and 0021 is to be decided together with 0001).
- The `Commit` column lists only the commit named in the record's status line. Other commits that touch the same finding are listed in the review reports.
- Record 0028 has no implementation commit. Its test file does not compile until one of its options is chosen.
