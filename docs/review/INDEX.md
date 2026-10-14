# Review index: src/loader, src/kernel/fs, arch/x86/nt, src/kernel/nt, src/ntdll

Date: 2026-10-12. Written on branch `haiku/review` (worktree `ntpers-review`, base `5eb8026`). Recounted on 2026-10-12 on `haiku/docs5`: the per-report counts, the decision-record count and the audit-pass counts below were rechecked against the reports, the finding headings and status lines, and [audit-fixes.md](audit-fixes.md).
Type: static review. The review itself modified no source file and ran no build, sanitizer, or test. Fixes made after it are recorded in each report's status lines and summarised under [Fix status](#fix-status). Fixes are not compiled or run against a kernel tree.

## Reports

Counts are the finding headings (`### ID [Severity] Title`) in each report, by severity.

| Area | File | High | Medium | Low | Info |
|---|---|---|---|---|---|
| PE parser and protection mapping | [loader.md](loader.md) | 2 | 5 | 9 | 1 |
| PE exec loader and test shim | [kernel-fs.md](kernel-fs.md) | 5 | 7 | 9 | 3 |
| NT personality (dispatch, VM, I/O, processes, services) | [arch-x86-nt.md](arch-x86-nt.md) | 14 | 24 | 24 | 1 |
| NT kernel objects, handles, registry, sync, security, VM, I/O, IOCP, processes, dispatch (`src/kernel/nt`) | [kernel-nt.md](kernel-nt.md) | 9 | 12 | 20 | 1 |
| NT user-mode runtime: loader, startup, unwind, exception dispatch, C runtime, Win32 shims, NTSTATUS mapping (`src/ntdll`, base `7e75069`) | [ntdll.md](ntdll.md) | 20 | 25 | 22 | 8 |
| **Total** | | **50** | **73** | **84** | **14** |

The Summary table inside [kernel-nt.md](kernel-nt.md) (8, 9, 19, 1) was written before the C-, T-, K- and some other findings were added. It disagrees with the 42 headings in that report (9, 12, 20, 1). The headings are counted here. Correcting the report's own summary table is left to the author of that report.

## Fix status

Each finding is classified from its recorded status: the `**Status:**` line under the finding, a status marker in its heading (`(fixed <hash>)`, `FIXED <hash>`), or the Medium and Low status table in [ntdll.md](ntdll.md). `Fixed` means a commit fixed the finding. `Partly` means some part is fixed and the rest is recorded as open or as needing an owner. `Blocked` means the status says blocked or needs an owner decision, and no part is fixed. `No change` means the finding was checked and is not a defect (or was already correct, or is stale). `Open` means no fix is recorded and no owner decision is named. A decision record that is only proposed counts as `Open`. The finding IDs are local to each report, so the same defect can appear in two reports (see [decisions index](../decisions/INDEX.md)).

| Report | Findings | Fixed | Partly | Blocked | No change | Open |
|---|---|---|---|---|---|---|
| [loader.md](loader.md) | 17 | 11 | 0 | 1 | 2 | 3 |
| [kernel-fs.md](kernel-fs.md) | 24 | 16 | 1 | 4 | 0 | 3 |
| [arch-x86-nt.md](arch-x86-nt.md) | 63 | 33 | 3 | 17 | 3 | 7 |
| [kernel-nt.md](kernel-nt.md) | 42 | 11 | 2 | 4 | 2 | 23 |
| [ntdll.md](ntdll.md) | 75 | 43 | 5 | 15 | 0 | 12 |
| **Total** | **221** | **114** | **11** | **41** | **7** | **48** |

The rows add to the severity total above (221). Per report, the `Fixed`, `Partly`, `Blocked`, `No change` and `Open` columns add to the findings column.

Where the status and the decision record disagree, the report is the source for this table, and the disagreement is listed in [OWNER-DECISIONS.md](../OWNER-DECISIONS.md). One case: the arch report marks `B-1` as blocked, while [decision record 0027](../decisions/0027-arch-kernel-stubs-file.md) is `Fixed` in `974db57`.

Decision records: [docs/decisions/INDEX.md](../decisions/INDEX.md) lists the 73 records (15 fixed, 16 open, 41 blocked, 1 no-change) in its 2026-10-13 snapshot. Each record's status line was checked against the index; none was missing from it.

## Audit passes

[audit-fixes.md](audit-fixes.md) is a commit audit, not a finding list. It has no severity. Its verdicts are counted per commit in each pass. The per-commit tables agree with the Results tables in every pass.

| Pass | Commits audited | OK | SUSPECT | BROKEN |
|---|---|---|---|---|
| 1 (`5eb8026` to HEAD, NT personality paths) | 96 | 90 | 5 | 1 |
| 2 (`974db57` to `fd26716`) | 115 | 102 | 10 | 3 |
| 3 (after `fd26716`) | 64 | 48 | 7 | 9 |
| 4 (after `2393db8`) | 98 | 74 | 15 | 9 |

The pass-3 SUSPECT list has eight items. One of them, `b9661c2`, is also counted as BROKEN (fixed in `6b45e31` and `7c6f281`), so the table has seven SUSPECT rows. The pass-4 list has seventeen items: fifteen SUSPECT rows and two BROKEN rows that also carry a SUSPECT note (`bc82d49`, `22f2688`).

Items in the resolution sections that still need an owner are listed in [OWNER-DECISIONS.md](../OWNER-DECISIONS.md).

## Top findings

1. **Out-of-bounds kernel read on a crafted PE** (`src/kernel/fs/binfmt_pe.c:181`, also `:639`). The check `nt_off + 6 > (u32)n` wraps for `e_lfanew` values near 4 GiB, so the header read goes far past the buffer. Any user who can execute a file reaches this. See kernel-fs K-1. **Fixed** in b8930c9 (exe path) and 4ffc5e5 (ntdll path).
2. **Kernel pointer used as a section handle.** The `src/kernel/nt` copy is fixed: section handles are handle-table indices (`mm.c`, record [0001](../decisions/0001-section-handle-object.md), `c8f479e`). The arch copy (`arch/x86/nt/mm.c`, `NtCreateSection` returns the object address and `NtMapViewOfSection` dereferences what it is given) is **open** (record [0021](../decisions/0021-arch-section-handle-pointer.md), arch M-2).
3. **Kernel heap read leaks into a filename** (`arch/x86/nt/nt_io.c:2272`). `FileNameLength` is not checked against the buffer, so a rename reads up to about 64 KiB past its allocation, and the bytes become part of the target name. See arch IO-1. **Fixed** in 79d1de2.

Also high and worth prioritising: uninitialised kernel heap returned to user by `NtQueryVirtualMemory` (`dispatch.c:258-264`, arch D-1), **fixed** in 16af7b7; `NtReadFile` always returned success without reading (`nt_io.c:1894-1904`, arch IO-2), **fixed** in 6da43d2; any process can open and terminate any other by ID with no access check (`proc_thread.c:1589-1592`, arch P-1), **open** (records 0002 and 0025); sections left writable and executable with no `mprotect` (`binfmt_pe.c:438`, `713`, kernel-fs K-3), **blocked** (record 0011); and exec error returns after the point of no return (`binfmt_pe.c:906-942`, kernel-fs K-4), **fixed** in 8eac32c (record 0012).

## Cross-cutting themes

- **Build integrity.** `arch/x86/nt/kernel_stubs.c` is not C (arch B-1), and it is now fixed by record 0027 (`974db57`). `vm/kbuild.sh` and `vm/rebuild_kdir.sh` copy the `src/` and `arch/` trees over each other in opposite orders, and the two copies of `mm.c` differ (arch B-3, open). The reviewed `mm.c` may not be the one that runs. `-w` hides all warnings (arch B-2).
- **The kernel loader does not use the hardened parser.** `src/loader/pe_parse.c` is only used by userspace tests and by the image planner. `binfmt_pe.c` re-parses the file with weaker checks (loader L-0, kernel-fs K-1 to K-7).
- **Uninitialised memory returned to user space.** `dispatch.c:258` (D-1), `nt_io.c:2144` (IO-9), `proc_thread.c:1649` and `1814` (P-4), `nt_services.c:325` (N-2). The arch copies of D-1, IO-9 and P-4 are fixed; the review's line numbers are from the snapshot.
- **No access control on handles and processes.** One global file-handle table (IO-3), required rights not checked (IO-4), process and thread open by ID with verbatim rights (P-1).
- **Virtual-memory state can diverge from real mappings.** A single global VAD table for all processes (M-1), an empty rollback (M-4), VAD changed before the page protection (M-5), and a publish-before-map window (M-11). The loader's `ntmm_add_region` results are ignored (kernel-fs K-10, fixed in `7729288`).
- **Thread lifecycle.** A thread is started before its handle is validated (P-3, fixed), a TEB that is kernel heap is used as the GS base (P-2, open), and a trampoline page leaks per thread (P-5, needs owner).

## Suggested order

1. Fix the out-of-bounds reads and wraps: kernel-fs K-1, K-2, K-14; arch IO-1; arch M-3. (All fixed.)
2. Remove the kernel pointer handles and the uninitialised copies: arch M-2, D-1, IO-9, P-4, N-2. (M-2 open; the others fixed or closed.)
3. Add access checks to process, thread, and file handles: arch P-1, IO-3, IO-4 (see records 0022, 0023, 0025).
4. Fix the exec contract: kernel-fs K-3 (blocked), K-4 (fixed), K-5, K-6, K-9.
5. Fix the build: arch B-1 (fixed by 0027; the status in the arch report is stale), B-3 (needs owner), then B-2.
6. Fix the functional break in `NtReadFile` (arch IO-2, fixed) and the VM rollback and state divergence (arch M-4, M-5, M-11).
7. Work through the remaining Medium and Low items in each report.

## Coverage and limits

- **Loader:** all five files read in full.
- **Kernel-fs:** `binfmt_pe.c`, `kshim.h`, `kshim.c` read in full. `binfmt_pe_test.c` (819 lines) was read only for its include structure, its helper calls, its test list, and one test. Its test bodies were not reviewed.
- **Arch:** `dispatch.c`, `nt_services.c`, `weak.c`, `kernel_stubs.c`, `Makefile`, `nt_internal.h`, and `nt_service_list.h` read in full. `mm.c` read in full except for a few helpers. `proc_thread.c` kernel branch (lines 932 onward) read in full. The userspace shim branch of `proc_thread.c` (lines 1-931) was not reviewed. `nt_io.c` read in most places, with gaps listed in arch.md. `mm.h` (260 lines) was not read.
- **Out of tree:** `src/kernel/nt/object.c`, `sync.c`, `handle.c`, `iocp.c`, and the kernel's `fs/exec.c` were not read in the review. Some findings depend on them (noted in the affected reports).
- **Not checked by tool:** nothing was compiled, linked, sanitized, fuzzed, or run in the review. The later fixes were tested on the host only where the report says so.
