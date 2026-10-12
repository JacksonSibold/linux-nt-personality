# Review index: src/loader, src/kernel/fs, arch/x86/nt, src/kernel/nt, src/ntdll

Date: 2026-10-12. Written on branch `haiku/review` (worktree `ntpers-review`, base `5eb8026`).
Type: static review. The review itself modified no source file and ran no build, sanitizer, or test. Fixes made after it are recorded in each report's status lines and summarised under [Fix status](#fix-status). Fixes are not compiled or run against a kernel tree.

## Reports

| Area | File | High | Medium | Low | Info |
|---|---|---|---|---|---|
| PE parser and protection mapping | [loader.md](loader.md) | 2 | 3 | 9 | 1 |
| PE exec loader and test shim | [kernel-fs.md](kernel-fs.md) | 5 | 7 | 9 | 3 |
| NT personality (dispatch, VM, I/O, processes, services) | [arch-x86-nt.md](arch-x86-nt.md) | 14 | 24 | 24 | 1 |
| NT kernel objects, handles, registry, sync, security, VM, I/O, IOCP, processes, dispatch (`src/kernel/nt`) | [kernel-nt.md](kernel-nt.md) | 7 | 9 | 18 | 1 |
| NT user-mode runtime: loader, startup, unwind, exception dispatch, C runtime, Win32 shims, NTSTATUS mapping (`src/ntdll`, base `7e75069`) | [ntdll.md](ntdll.md) | 20 | 24 | 22 | 8 |
| **Total** | | **48** | **67** | **82** | **14** |

## Fix status

Counts are per report and were recounted on 2026-10-12 from each finding's recorded status: the `**Status:**` line under the finding, a status marker in its heading, or the status table in the report (ntdll's Medium and Low table, and the prose passes in kernel-nt). `Fixed` means a commit fixed the finding; `Partly` means some part is fixed; `Blocked` means the status says blocked or needs an owner decision; `No change` means the finding was checked and is not a defect (or was already correct); `Open` means no fix is recorded and no owner decision is named. The finding IDs are local to each report, so the same defect can appear in two reports (see [decisions index](../decisions/INDEX.md)).

| Report | Findings | Fixed | Partly | Blocked | No change | Open |
|---|---|---|---|---|---|---|
| [loader.md](loader.md) | 15 | 9 | 0 | 1 | 2 | 3 |
| [kernel-fs.md](kernel-fs.md) | 24 | 16 | 1 | 4 | 0 | 3 |
| [arch-x86-nt.md](arch-x86-nt.md) | 63 | 33 | 2 | 17 | 2 | 9 |
| [kernel-nt.md](kernel-nt.md) | 35 | 21 | 1 | 4 | 2 | 7 |
| [ntdll.md](ntdll.md) | 74 | 42 | 5 | 15 | 0 | 12 |
| **Total** | **211** | **121** | **9** | **41** | **6** | **34** |

Totals match the severity table above (211 findings in both tables; the per-report rows sum to the total row in each). Open findings by severity are not split out here; each report lists them. The per-report `Fix status` sections name the commits.

Decision records: [docs/decisions/INDEX.md](../decisions/INDEX.md) lists the 35 records with status (11 fixed, 16 open, 7 blocked, 1 no-change).

The per-finding status values are in each report (headings, status lines, and the ntdll status table). The per-report counts above were rechecked against them; the earlier per-finding list in this paragraph was incomplete and has been removed.

## Top findings

1. **Out-of-bounds kernel read on a crafted PE** (`src/kernel/fs/binfmt_pe.c:181`, also `:639`). The check `nt_off + 6 > (u32)n` wraps for `e_lfanew` values near 4 GiB, so the header read goes far past the buffer. Any user who can execute a file reaches this. See kernel-fs K-1. **Fixed** in b8930c9 (exe path) and 4ffc5e5 (ntdll path).
2. **Kernel pointer used as a section handle** (`arch/x86/nt/mm.c:1540`, `:1549`). `NtCreateSection` returns the object address to user space, and `NtMapViewOfSection` dereferences whatever integer it is given. This is both an address disclosure and a forgery path. See arch M-2. Reachability depends on the `nt_svc_*` wrapper, which is outside this directory. **Open** (decision records 0001 and 0021).
3. **Kernel heap read leaks into a filename** (`arch/x86/nt/nt_io.c:2272`). `FileNameLength` is not checked against the buffer, so a rename reads up to about 64 KiB past its allocation, and the bytes become part of the target name. See arch IO-1. **Fixed** in 79d1de2.

Also high and worth prioritising: uninitialised kernel heap returned to user by `NtQueryVirtualMemory` (`dispatch.c:258-264`, arch D-1), **fixed** in 16af7b7; `NtReadFile` always returned success without reading (`nt_io.c:1894-1904`, arch IO-2), **fixed** in 6da43d2; any process can open and terminate any other by ID with no access check (`proc_thread.c:1589-1592`, arch P-1), **open** (records 0002 and 0025); sections left writable and executable with no `mprotect` (`binfmt_pe.c:438`, `713`, kernel-fs K-3), **blocked** (record 0011); and exec error returns after the point of no return (`binfmt_pe.c:906-942`, kernel-fs K-4), **fixed** in 8eac32c (record 0012).

## Cross-cutting themes

- **Build integrity.** `arch/x86/nt/kernel_stubs.c` is not C (arch B-1). `vm/kbuild.sh` and `vm/rebuild_kdir.sh` copy the `src/` and `arch/` trees over each other in opposite orders, and the two copies of `mm.c` differ by about 155 lines (arch B-3). The reviewed `mm.c` may not be the one that runs. `-w` hides all warnings (arch B-2).
- **The kernel loader does not use the hardened parser.** `src/loader/pe_parse.c` is only used by a userspace test. `binfmt_pe.c` re-parses the file with weaker checks (loader L-0, kernel-fs K-1 to K-7).
- **Uninitialised memory returned to user space.** `dispatch.c:258` (D-1), `nt_io.c:2144` (IO-9), `proc_thread.c:1649` and `1814` (P-4), `nt_services.c:325` (N-2).
- **No access control on handles and processes.** One global file-handle table (IO-3), required rights never checked (IO-4), process and thread open by ID with verbatim rights (P-1), thread handles always full-access (P-1).
- **Virtual-memory state can diverge from real mappings.** A single global VAD table for all processes (M-1), an empty rollback (M-4), VAD changed before the page protection (M-5), and a publish-before-map window (M-11). The loader's `ntmm_add_region` results are ignored (kernel-fs K-10).
- **Thread lifecycle.** A thread is started before its handle is validated (P-3), a TEB that is kernel heap is used as the GS base (P-2), and a trampoline page leaks per thread (P-5).

## Suggested order

1. Fix the out-of-bounds reads and wraps: kernel-fs K-1, K-2, K-14; arch IO-1; arch M-3.
2. Remove the kernel pointer handles and the uninitialised copies: arch M-2, D-1, IO-9, P-4, N-2.
3. Add access checks to process, thread, and file handles: arch P-1, IO-3, IO-4.
4. Fix the exec contract: kernel-fs K-3, K-4, K-5, K-6, K-9.
5. Fix the build: arch B-1, B-3, then B-2.
6. Fix the functional break in `NtReadFile` (arch IO-2) and the VM rollback and state divergence (arch M-4, M-5, M-11).
7. Work through the remaining Medium and Low items in each report.

## Coverage and limits

- **Loader:** all five files read in full.
- **Kernel-fs:** `binfmt_pe.c`, `kshim.h`, `kshim.c` read in full. `binfmt_pe_test.c` (819 lines) was read only for its include structure, its helper calls, its test list, and one test. Its test bodies were not reviewed.
- **Arch:** `dispatch.c`, `nt_services.c`, `weak.c`, `kernel_stubs.c`, `Makefile`, `nt_internal.h`, and `nt_service_list.h` read in full. `mm.c` read in full except for a few helpers. `proc_thread.c` kernel branch (lines 932 onward) read in full. The userspace shim branch of `proc_thread.c` (lines 1-931) was not reviewed. `nt_io.c` read in most places, with gaps listed in arch.md. `mm.h` (260 lines) was not read.
- **Out of tree:** `src/kernel/nt/object.c`, `sync.c`, `handle.c`, `iocp.c`, and the kernel's `fs/exec.c` were not read. Some findings depend on them (noted in the affected reports).
- **Not checked by tool:** nothing was compiled, linked, sanitized, fuzzed, or run.
