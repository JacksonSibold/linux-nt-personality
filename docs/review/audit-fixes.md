# NT personality fix audit (base 5eb8026)

Date: 2026-10-12. Branch `haiku/audit`, worktree `ntpers-audit`.
Type: read-only regression audit of the fix commits since base snapshot `5eb8026` that touch `arch/x86/nt`, `src/kernel/fs` or `src/kernel/nt`. The only source change is one fix commit (see [Fixes applied](#fixes-applied)).

## Method and limits

- Scope: the 96 non-merge commits returned by `git log --no-merges 5eb8026..HEAD -- arch/x86/nt src/kernel/fs src/kernel/nt`. Commits that also touch files outside those paths (build scripts, tests, tools) were audited for their in-scope hunks.
- Each diff was read in context (`git show -U8`). Each claim about a called symbol, its signature, `#ifdef` gating or return convention was checked with grep at HEAD.
- Nothing was compiled against a kernel tree. The target kernel is Linux 6.18 (`vm/*.sh`). Verification here is limited to `tests/syntax/check.sh` and `make -C tests/host test`. The host target builds only the pe_parse tests, and the syntax check does not define `__KERNEL__`, so neither compiles the kernel branches these fixes change.
- Location column: `file:line` is the first changed line in the commit's post-image, for the first in-scope file the commit touches. Lines quoted inside a reason are the ones checked during the audit.

## Results

| Verdict | Count |
|---|---|
| OK | 90 |
| SUSPECT | 5 |
| BROKEN | 1 (fixed in 7fba2cb) |
| Audited | 96 |

Verdict meanings: OK = behaviour on the success path preserved, unwind and error paths correct, APIs and signatures match the kernel, commit message matches the change. SUSPECT = a plausible defect or a message/behaviour mismatch that needs a human decision. BROKEN = a defect with an obviously correct minimal fix.

## Fixes applied

- **7fba2cb** `fix(arch-nt): drop stray IO-17 rewind that ran on every read chunk`, for 325b55c. In `ntio_do_read` (arch/x86/nt/nt_io.c) the commit left an unconditional `pos -= got;` after each successful chunk copy. Reads longer than `NTIO_BOUNCE_SIZE` (64 KiB) rewound the position and re-read the first chunk, so the caller got wrong data. The fix removes the stray three lines. The in-branch rewind on copy failure is kept.
- Re-check after the fix: `tests/syntax/check.sh` gives 32 passed and 5 failed, both before and after, with identical per-file results and 452 warnings in both runs. `make -C tests/host test` passes (pe_parse: 32 run, 0 failed).

## SUSPECT: resolution

Each item was re-checked against the code and the NT references in the repo on branch `haiku/suspects`. Hashes are on that branch.

- **d1f866c** (resolved in `8e7e190`, no table change): `0xC0000135` and `0xC0000139` are the correct header values. `STATUS_DLL_NOT_FOUND` and `STATUS_ENTRYPOINT_NOT_FOUND` already map to 126 and 127 through the aliased rows `STATUS_INVALID_IMAGE_WIN_16` and `STATUS_REGISTRY_CORRUPT`, so no entry was added. The `0xC0000228 -> 599` and `0xC0000233 -> 1908` rows are value-keyed rows from the DISCREPANCIES oracle (section 3). They are not the DLL or entrypoint codes, so their comments now say so. The conformance expectation for `STATUS_DLL_NOT_FOUND` moved from 599 to 126. The generated `ntstatus_table.inc` is unchanged, since the generator strips comments. The probe labels in `tests/differential/probes/probe_ntstatus*.c` still name 0xC0000228 and 0xC0000233 as DLL and entrypoint. Those labels are not changed here, since the probes run only on Windows.
- **d921888** and **40f6db0** (resolved in `f942cb9`): the raw `*ViewSize` is now compared with `sec->size - offset` before rounding, and a larger size returns `STATUS_INVALID_VIEW_SIZE`, the code the existing bounds test already uses. The same block is fixed in `src/kernel/nt/mm.c` and `arch/x86/nt/mm.c`. The regression test `test_view_size_round_up_wrap` in `src/kernel/nt/mm_test.c` failed before the change (status `STATUS_NO_MEMORY`) and passes now. The kernel-host `make check` passes.
- **38b1b0a** (not changed, decision): the order cannot be fixed safely here. The only order that avoids a copy-out after the count changes is copy-before-commit with a recheck loop, which changes the thread lock protocol. `arch/x86/nt/proc_thread.c` is kernel-only and is not built by any host target, so such a change could not be compiled or tested. The current behaviour is a known limit: a copy-out fault after the probe returns `STATUS_ACCESS_VIOLATION` with the suspend or resume already applied. Windows also writes the out-parameter after the state change. Separately, `src/kernel/nt/proc_thread.c` still calls `copy_to_user` under `thr->lock` at lines 1435 and 1476. That is the P-9 pattern this commit removed from the arch copy, and it was out of audit scope.
- **b3dcef5** (note only, history not rewritten): the commit message covers the IOSB probe only. The same commit also adds the IO-17 rewind in `ntio_do_read` (`arch/x86/nt/nt_io.c`, after the failed `copy_to_user` in the chunk loop, `pos -= got`). That rewind is correct, since the position must match the bytes not delivered. It is kept. The message does not mention it. Readers of the history should treat IO-17 as part of this commit.

## Per-commit audit

Newest first. Files: basenames of every file the commit touches.

| Commit | Files | Verdict | Location | Reason |
|---|---|---|---|---|
| `974db57` | kernel_stubs.c | OK | `arch/x86/nt/kernel_stubs.c:1` | kernel_stubs.c is now valid C and defines nt_banner_init; banner text and vmlinux symbol checks cannot be verified here |
| `3a8f7d1` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1141` | trampoline entered with sp+8 (16-byte aligned); direct StartRoutine fallback keeps the original sp |
| `10a9418` | mm.c | OK | `arch/x86/nt/mm.c:52` | do_mprotect_pkey kallsyms path removed; no remaining use of the removed len variable |
| `e8dd37b` | mm.c | OK | `arch/x86/nt/mm.c:18` | ntmm_init moved to a core_initcall in the kernel build; lazy checks kept for the shim |
| `075e040` | nt_io.c | OK | `arch/x86/nt/nt_io.c:549` | delete-on-close moved to ntio_file_release, so it runs on the last reference; NtClose no longer unlinks |
| `325b55c` | nt_io.c | BROKEN | `arch/x86/nt/nt_io.c:1663` | stray unconditional pos -= got after every successful chunk copy; reads over 64 KiB re-read the first chunk. Fixed in 7fba2cb |
| `b3dcef5` | nt_io.c | SUSPECT (note, see above) | `arch/x86/nt/nt_io.c:1525` | IOSB probe is correct, but the commit also adds the IO-17 in-branch rewind to ntio_do_read, which the message does not mention |
| `d730c42` | weak.c | OK | `arch/x86/nt/weak.c:15` | stray RtlUserThreadStart global removed; only comments refer to it now |
| `38b1b0a` | proc_thread.c | SUSPECT (decided: not changed) | `arch/x86/nt/proc_thread.c:1356` | probe, then count change, then copy-out after unlock: a fault after the count changed returns STATUS_ACCESS_VIOLATION with the suspend already applied (nt_put_prev_count) |
| `a4ddf86` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1391` | rcu_read_lock around find_vpid/pid_task in suspend, resume and terminate paths |
| `d6e5ab7` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1201` | cross-process thread creation refused with STATUS_NOT_SUPPORTED before any object is created |
| `e6d152f` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1118` | thr NULL check placed before the first dereference in nt_setup_child_regs |
| `0f85fea` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1110` | dead ntdll RtlUserThreadStart probe and its user-memory reads removed; no remaining references |
| `0b869fd` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1241` | thread creation and suspend/resume info logs demoted to pr_debug; NtResumeThread labels corrected |
| `bdae5f7` | nt_io.c | OK | `arch/x86/nt/nt_io.c:2245` | negative EndOfFile rejected with STATUS_INVALID_PARAMETER in ntio_set_eof |
| `2adcc11` | nt_io.c | OK | `arch/x86/nt/nt_io.c:36` | ntio locks and tables initialised by core_initcall; both init helpers are unguarded in the kernel build |
| `4a0a0e3` | nt_io.c | OK | `arch/x86/nt/nt_io.c:373` | output IOSB pointer probed before a file handle slot is reserved in ntio_handle_create |
| `649f184` | mm.c | OK | `arch/x86/nt/mm.c:1626` | private VADs refused; section views are MEM_MAPPED or MEM_IMAGE, so section unmap is unchanged |
| `e0e5989` | mm.c | OK | `arch/x86/nt/mm.c:876` | size arithmetic checked for wrap before rounding in NtAllocateVirtualMemory and NtProtectVirtualMemory |
| `5cced27` | mm.c | OK | `arch/x86/nt/mm.c:963` | reservation unmapped when the commit step fails in the commit-only path |
| `8f7d873` | nt_services.c | OK | `arch/x86/nt/nt_services.c:342` | NtQueryMutant buffer is 24 bytes, zeroed, and copied in full |
| `f63ca2d` | nt_services.c | OK | `arch/x86/nt/nt_services.c:159` | PreviousState/PreviousCount probed before the event, mutant or semaphore state changes |
| `35a1e46` | dispatch.c | OK | `arch/x86/nt/dispatch.c:382` | debugfs_lookup reference dropped with dput when a directory was found |
| `8ccc960` | dispatch.c | OK | `arch/x86/nt/dispatch.c:113` | per-call pr_info removed from the syscall hot path |
| `1fd3821` | dispatch.c | OK | `arch/x86/nt/dispatch.c:311` | missing-service log rate-limited |
| `ce0ec9a` | dispatch.c | OK | `arch/x86/nt/dispatch.c:119` | this_cpu_add used for the per-SSN counter on a preemptible path |
| `d022f12` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:876` | ntdll LdrInitializeThunk scan capped; thunk RVA checked for non-zero, in image, and in an executable section |
| `d4a33f2` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:55` | base-relocation directory capped at PE_MAX_RELOC_BYTES in exe and ntdll paths |
| `c75cda6` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:705` | page-multiple allocation with an exact-need read; short valid ntdll files accepted |
| `e9012fa` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:668` | ntdll header reads bounded to 24 bytes and optional header checked to [112,240] before field reads |
| `f2d8fd1` | io_test.c, posix_host.c | OK | `src/kernel/nt/io_test.c:868` | test-only: absolute scratch root and io_test cleanup; the posix_host.c change is outside the audit scope |
| `94cb2a9` | io_test.c | OK | `src/kernel/nt/io_test.c:487` | test-only precondition for trailing-dot checks |
| `a21ff94` | .gitignore, Makefile, kshim.c | OK | `src/kernel/nt/kshim.c:47` | KSHIM_POISON only when set in the environment; kzalloc untouched; test harness only |
| `eaac06b` | io.c | OK | `src/kernel/nt/io.c:448` | open handle lacking the right returns STATUS_ACCESS_DENIED; a handle that is not open stays STATUS_INVALID_HANDLE |
| `7f95ab4` | io_test.c | OK | `src/kernel/nt/io_test.c:431` | test-only: read or write through a missing right expects STATUS_ACCESS_DENIED |
| `847f6c1` | proc_thread_test.c | OK | `src/kernel/nt/proc_thread_test.c:92` | test-only: waits for sync.c allocations to return to baseline before teardown |
| `5b1fca3` | proc_thread.c | OK | `src/kernel/nt/proc_thread.c:206` | thread_sync_release frees the sync thread on every exit path; nt_thread_destroy still abandons owned mutants |
| `d490e92` | kshim.c, kshim.h, proc_thread_test.c | OK | `src/kernel/nt/kshim.c:29` | allocation-site tagging only under USERSPACE_KSHIM; kernel kmalloc/kzalloc unchanged |
| `213d333` | kshim.c | OK | `src/kernel/nt/kshim.c:399` | USERSPACE_KSHIM takes the st_atim branch |
| `d1f866c` | gen_ntstatus_table.py, ntstatus_table.inc | SUSPECT (resolved in 8e7e190) | `src/kernel/nt/gen_ntstatus_table.py:68` | parser change is sound, but ntstatus_table.inc now maps 0xC0000228 and 0xC0000233 where DLL_NOT_FOUND (0xC0000135) and ENTRYPOINT_NOT_FOUND (0xC0000139) belong (src/ntdll/ntstatus.h); not in the message |
| `da2531b` | io_test.c | OK | `src/kernel/nt/io_test.c:396` | test-only: roundtrip handle opened read-write so FILE_READ_DATA is granted |
| `ea3c20a` | mm.c | OK | `arch/x86/nt/mm.c:72` | KERN_EMERG trace demoted to pr_debug in the arch copy of kshim_mprotect |
| `a9bc215` | 34 files | OK | `src/kernel/nt/gen_ntstatus_table.py:?` | mode-only change (100644 to 100755) on scripts; no content change |
| `efae2d3` | mm.c | OK | `src/kernel/nt/mm.c:72` | KERN_EMERG trace demoted to pr_debug in the src copy; __KERNEL__ branch is not compiled here |
| `6efb8b7` | kshim.c | OK | `src/kernel/nt/kshim.c:396` | KSHIM_STAT_* macros use glibc nanosecond fields or a whole-second MinGW fallback |
| `3679688` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:30` | KSHIM_H guard skips the duplicate nt_enter_personality in the test build; kernel build unchanged |
| `5c1d29c` | kshim.h | OK | `src/kernel/fs/kshim.h:8` | kshim.h kernel_read model now matches the kernel signature; test-only model with TIF values documented |
| `f03a4d7` | binfmt_pe_test.c | OK | `src/kernel/fs/binfmt_pe_test.c:9` | include of src/loader/pe_prot.h resolves from src/kernel/fs |
| `7f11af9` | mm.c | OK | `arch/x86/nt/mm.c:1271` | debug printk removed from NtProtectVirtualMemory; the protect path does not depend on it |
| `419f675` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:430` | clear_user return checked at all four sites; -EFAULT routed through the post-commit or cleanup path |
| `7729288` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:415` | ntmm_add_region returns 0 on success, so the new if-checks are correct; ntdll region conflict made fatal |
| `8eac32c` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:19` | post-commit exec failures send SIGSEGV via force_sigsegv(int), the single-argument form in the 6.18 target; pre-commit paths unchanged |
| `13a4357` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:630` | ntdll loaded only from fixed root paths with O_NOFOLLOW and O_NONBLOCK; must be a root-owned regular file without group or world write |
| `83e3349` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:1067` | weak NtTerminateProcess no longer kills the caller; returns STATUS_NOT_IMPLEMENTED |
| `d7adc94` | handle.c, handle.h | OK | `src/kernel/nt/handle.c:150` | FIFO free list: tail append on close, head pop only above quarantine, last_free and free_count kept exact (read-only check) |
| `0b7921b` | iocp.c, iocp.h | OK | `src/kernel/nt/iocp.c:98` | queued packets capped at NTIO_IOCP_MAX_QUEUED under the port lock; packet freed on reject |
| `80b6fea` | io.c, io.h | OK | `src/kernel/nt/io.c:402` | NtWriteFile requires FILE_WRITE_DATA or FILE_APPEND_DATA through the any-of lookup |
| `1d160e8` | io.c | OK | `src/kernel/nt/io.c:1890` | NtReadFile requires FILE_READ_DATA; generic read rights map to it through ntio_map_generic_access |
| `30b200a` | object.c | OK | `src/kernel/nt/object.c:1177` | process and thread unregistered before free in the kernel build; kernel ob_unregister_proc and ObUnregisterThread exist |
| `6f58d04` | sync.c | OK | `src/kernel/nt/sync.c:2225` | wait accepts only event, mutant and semaphore types; references dropped on mismatch |
| `14f9fb8` | io.c | OK | `src/kernel/nt/io.c:2133` | kbuf declared only under __KERNEL__ in the two io functions; every use is kernel-gated |
| `b125094` | io.c | OK | `src/kernel/nt/io.c:1581` | LARGE_INTEGER off declared only under __KERNEL__; every use is kernel-gated |
| `0254b09` | registry.c | OK | `src/kernel/nt/registry.c:1403` | output buffers zeroed with kzalloc before copy-out in the registry query and enumerate wrappers |
| `68fe25b` | handle.c | OK | `src/kernel/nt/handle.c:97` | out-of-range handle index maps to 0, which every caller rejects |
| `40f6db0` | mm.c | SUSPECT (resolved in f942cb9) | `src/kernel/nt/mm.c:1524` | src port of d921888; same wrap in the ntmm_round_up(*ViewSize) step |
| `a3a84fe` | Makefile, ntstatus_table.inc, test_gen_ntstatus_table.py | OK | `src/kernel/nt/ntstatus_table.inc:1` | generated ntstatus_table.inc committed with its rule; later value change is in d1f866c |
| `ba78f96` | proc_thread.c | OK | `src/kernel/nt/proc_thread.c:1770` | port of 6f4e944: STATUS_NAME_TOO_LONG returned before any write |
| `c57c6ff` | proc_thread.c | OK | `src/kernel/nt/proc_thread.c:1665` | port of 182ee9f: basic-information structs zeroed before copy |
| `0d931bd` | proc_thread.c | OK | `src/kernel/nt/proc_thread.c:1603` | port of 3899c83: opened handle closed when copy-out fails |
| `c6f98dd` | proc_thread.c | OK | `src/kernel/nt/proc_thread.c:1480` | port of 34b3fa8: NULL current eprocess rejected in suspend and resume process |
| `ecf6a9e` | proc_thread.c | OK | `src/kernel/nt/proc_thread.c:1214` | port of 54baad9: thread handle probed before the thread is created |
| `ab11b7c` | dispatch.c | OK | `src/kernel/nt/dispatch.c:304` | nt_set_current_thread(NULL) after abandoning mutants in nt_exit_personality; declared through sync.h |
| `545b374` | dispatch.c | OK | `src/kernel/nt/dispatch.c:258` | port of 16af7b7: kzalloc for the NtQueryVirtualMemory bounce buffer |
| `ed17a53` | gen_ntstatus_table.py, test_gen_ntstatus_table.py | OK | `src/kernel/nt/gen_ntstatus_table.py:6` | generator parses strictly, counts every entry and emits LF output; committed .inc matches the generator at this commit |
| `9ce57ae` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:25` | duplicated CONFIG_NT_PERSONALITY #ifdef collapsed; active branches identical |
| `63008f4` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:854` | LdrInitializeThunk ordinal bounded against NumberOfFunctions |
| `db42c03` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:1048` | success path frees mapping nodes and headers_buf; pointers nulled and headers_buf unused afterwards |
| `833e002` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:1084` | register_binfmt() return checked, logged and returned from the initcall |
| `3cc5903` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:1042` | out-of-range TEB message printed with %p instead of %px |
| `dd6f4c8` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:542` | DIR64 relocation target checked against SizeOfImage in u64 before copy |
| `6f4e944` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1770` | image path length checked against USHORT before any write |
| `248eaa1` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:519` | relocation range end compared in u64 in exe and ntdll paths |
| `39de0fb` | nt_io.c | OK | `arch/x86/nt/nt_io.c:2139` | NtQueryInformationFile buffer zeroed with kzalloc |
| `539775a` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:722` | ntdll section VA and size bounded by SizeOfImage before vm_mmap |
| `b611a28` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:257` | SizeOfHeaders checked against SizeOfImage in exe and ntdll paths |
| `54baad9` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1214` | thread handle probed before wake_up_new_task |
| `b205768` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:185` | file-header and optional-header reads bounded; SizeOfOptionalHeader must be at least 112 (exe path only, as stated) |
| `34b3fa8` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1473` | NULL eprocess checked before ObReferenceObjectByHandle in suspend and resume process |
| `4ffc5e5` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:639` | e_lfanew check made wrap-safe on the ntdll path |
| `b8930c9` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:181` | e_lfanew check made wrap-safe on the exe path |
| `3899c83` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1594` | NtOpenProcess and NtOpenThread close the new handle when copy-out fails |
| `16af7b7` | dispatch.c | OK | `arch/x86/nt/dispatch.c:258` | NtQueryVirtualMemory bounce buffer zeroed with kzalloc |
| `182ee9f` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1650` | PROCESS and THREAD_BASIC_INFORMATION zeroed before copy-out |
| `d921888` | mm.c | SUSPECT (resolved in f942cb9) | `arch/x86/nt/mm.c:1566` | offset and size checked against the section, but the round-up of a *ViewSize above SIZE_MAX-4095 wraps to 0, which then passes the size check; check the raw size before rounding |
| `79d1de2` | nt_io.c | OK | `arch/x86/nt/nt_io.c:2266` | FileNameLength checked for even length and buffer bounds before the rename target is built |
| `6da43d2` | nt_io.c | OK | `arch/x86/nt/nt_io.c:1898` | early return removed so NtReadFile performs the read and fills the IOSB |


# Second pass: fix commits after 974db57

Date: 2026-10-12. Branch `haiku/audit2`, worktree `ntpers-audit2`. This pass repeats the audit for every non-merge commit after `974db57`, the newest row of the first pass, that touches `src`, `arch`, `build`, `tests` or `tools`. The range is `974db57..fd26716` (the HEAD at the start of this pass), which gives 120 commits. Five are docs-only (`c4d7615`, `eb600f7`, `7531c89`, `d0dba01`, `4471785`) and were skipped. The remaining 115 are in the table below.

## Method and limits

- Each diff was read in context (`git show -U8`). Called symbols, signatures and return conventions were checked by grep at the audited commit. The fix commits added in this pass are on top of the range, not part of it.
- Test changes were checked for removed assertions: `git log -p` over the test paths shows one removed `ASSERT_STATUS` (`667d788`, see SUSPECT) and no other removed `ASSERT`/`EXPECT` line.
- `build/generated_stubs.S`, `src/kernel/nt/ntstatus_table.inc` and `out/ntdll.def` were not regenerated. No Python interpreter works on this host (`python3` and `py` resolve to the Store stub), so the Python unit suite, `tests/unit`, `check_repo`, `update_doc_counts --check` and `check_syscall_tables.py` were not run. Commits that only add or change Python tools or their tests are marked "not run here".
- Build and test results, run on the fixed tree:
  - `bash tests/syntax/check.sh`: 32 passed, 5 failed, 452 warnings. The five failures are the same before and after this pass: `src/kernel/nt/dispatch.c`, `src/kernel/nt/kshim.c`, `src/kernel/fs/binfmt_pe.c`, `src/kernel/fs/binfmt_pe_test.c`, `arch/x86/nt/dispatch.c`.
  - `make -C tests/host test`: pass (pe_parse, real_pe and the other host suites).
  - `make -C tests/ntdll-host test`: all tests pass (`test_pe_bounds` build-only on this host).
  - `make -C tests/kernel-host check`: every program exits 0, including `mm_test` at 18/18.

## Results

| Verdict | Count |
|---|---|
| OK | 102 |
| SUSPECT | 10 |
| BROKEN | 3 (2 fixed in this pass, 1 fixed earlier in the range) |
| Audited | 115 |

## Fixes applied

One commit per audited hash, each referencing it:

- **6db83e7** `test(nt): fail when make_proc or make_thread returns NULL`, for `a275418`. The early bare `return` in `pseudo_handles` (`src/kernel/nt/object_test.c`) and `handle_table_lifetime` (`src/kernel/nt/i2_test.c`) counted neither a pass nor a failure, so a broken fixture was a silent pass. Both are now `ASSERT`.
- **993c3ee** `fix(nt/mm): reject sizes that wrap when page-rounded in protect and decommit`, for `d5728cf`. The end check ran after `ntmm_round_up`, so a size within one page of `SIZE_MAX` rounded to 0 and passed. Both `NtProtectVirtualMemory` and the decommit path of `NtFreeVirtualMemory` in `src/kernel/nt/mm.c` now refuse such a size before rounding, as the arch copy does since `e0e5989`. A new test in `src/kernel/nt/mm_test.c` fails before the change (protect returned `0xC0000045`, not `STATUS_INVALID_PARAMETER`) and passes after it.
- `325b55c` is BROKEN as committed (stray `pos -= got` on every chunk). `7fba2cb`, already in the range, removes it. No new change was needed.

## SUSPECT: for review

These need a decision. None of them was changed in this pass.

1. **`667d788`** (`src/kernel/nt/io_test.c:855`): the 300-character create case now accepts `STATUS_NAME_TOO_LONG` as well as `STATUS_OBJECT_NAME_INVALID`. The commit explains why, and it is still a rejection, but the expectation was relaxed. Confirm that is intended.
2. **`eef2786`** (`src/ntdll/ntdll.def`): this is a hand edit of a checked-in copy of generator output. The Makefile writes `out/ntdll.def`, and this copy is not regenerated. Re-run `build/gen_ntdll_def.py` where Python exists and diff.
3. **`1f334ce`** (`src/ntdll/msvcrt.c`, `ms_bounded_vsprintf`): `sprintf` output is now silently cut at 1023 characters (it was 8191 before). A caller buffer under 1024 bytes still overflows. The commit says so. This is a mitigation, not a fix.
4. **`27030e1`** (`build/gen_ntdll_def.py`, six `src/ntdll/*.def` copies): `_DllMainCRTStartup@12` was replaced by the undecorated name. The commit message says the generated output was not regenerated. Same follow-up as item 2.
5. **`fd86cb3`** (`src/ntdll/kernel32.c:193`): an unsupported protection now fails with `ERROR_INVALID_PARAMETER`. That includes protection 0 with `MEM_RESERVE`, which the code used to accept as `PAGE_READWRITE`. Confirm what Windows does for a reserve-only call with protection 0.
6. **`e6d152f`** (`arch/x86/nt/proc_thread.c:1118`): a NULL `thr` returns before the child registers are set, so the child would run the parent's return path. The caller ignores the result. The case is unreachable today, but a `return` with an error would be safer.
7. **`38b1b0a`** (`arch/x86/nt/proc_thread.c:1356`): unchanged from the first pass decision (count changed before the copy-out). See the resolution section above.
8. **`b3dcef5`** (`arch/x86/nt/nt_io.c:1525`): the IOSB probe is correct, but the message omits the IO-17 rewind (`pos -= got` in `ntio_do_read`) that this commit also adds. The rewind is correct and is kept.
9. **`cc755fa`** (`src/kernel/nt/handle.c:533`, the `NtDuplicateObject` undo path): the undo path calls `ObCloseHandle` on the duplicate. If the caller passes `OBJ_PROTECT_FROM_CLOSE` in the duplicate's attributes, that close is refused, so the new handle and its object reference leak. The test covers only attributes 0.
10. **`d4a33f2`** (`src/kernel/fs/binfmt_pe.c:55`, the ntdll reloc branch of `pe_map_ntdll`): on the ntdll path, a relocation directory above `PE_MAX_RELOC_BYTES` is skipped silently, so the image can load with no relocations applied. The exe path returns `-ENOEXEC`. The ntdll path should probably do the same.

## SUSPECT: third-pass resolution

Date: 2026-10-12. Branch `haiku/suspects2`, worktree `ntpers-suspects2`. Each SUSPECT above was re-read against its diff and audit note. Fixes are separate commits; the rest are decided here.

| Commit | Resolution | Hash |
|---|---|---|
| `667d788` | accepted-with-reason: the 300-character create is rejected by the total-length check first (STATUS_NAME_TOO_LONG); the 255-character case still asserts STATUS_OBJECT_NAME_INVALID exactly. Still a rejection, and the commit message gives the reason | none |
| `eef2786` | resolved OK: `build/gen_ntdll_def.py` run with the WinPython interpreter reproduces `src/ntdll/ntdll.def` byte for byte | none |
| `1f334ce` | needs-owner: should `sprintf`/`vsprintf` keep the old 8191-byte bound, or silently cut output at 1023 bytes as now? The cut is silent, so a caller that formats 1024 or more bytes gets short output with no error | none |
| `27030e1` | resolved OK: regenerated `ntdll.def` matches the checked-in copy; `_DllMainCRTStartup` is defined undecorated in `ntdll_main.c` and the shims, and the six `.def` copies agree | none |
| `fd86cb3` | needs-owner: does Windows `VirtualAlloc` accept `flProtect` 0 with `MEM_RESERVE` alone? If it does, this commit makes a valid call fail (the fix would ignore protection for reserve-only). No Windows reference on this host, and `kernel32.c` is MinGW-only | none |
| `e6d152f` | accepted-with-reason: the NULL branch cannot run, because `nt_svc_NtCreateThread` already reads `thr->tid` (proc_thread.c:1290) before calling `nt_setup_child_regs`. A `void` helper that returns silently is still fail-open; an error return needs a caller cleanup change, not made here | none |
| `38b1b0a` | accepted-with-reason: the probe before the count change removes the fault in the normal case. The residual race is a user page unmapped between probe and `copy_to_user`; a rollback needs a second lock and can race other suspend/resume calls. Revisit if Windows-atomic semantics are wanted | none |
| `b3dcef5` | accepted-with-reason: the IO-17 rewind (`pos -= got`) is correct. `kshim_kernel_read` advances `pos` by `got`, and a failed copy leaves the reported total unchanged. The commit message omits it (message-only gap) | none |
| `cc755fa` | needs-owner: a duplicate created with `OBJ_PROTECT_FROM_CLOSE` cannot be undone, because the undo `ObCloseHandle` is refused (handle.c:371), leaving the handle and its reference. The fix is a design choice: refuse before creating when the source is protected, or remove the undo entry without the protection check | none |
| `d4a33f2` | fixed: the ntdll reloc branch now returns `-ENOEXEC` for a directory above `PE_MAX_RELOC_BYTES`, as the exe path does. Not host-tested: `pe_map_ntdll` is static and needs the kernel file shims | `3daa005` |

Also fixed in this pass, from the notes below: the decommit path in `arch/x86/nt/mm.c` had no wrap guard (same defect as `d5728cf`), now ported from `993c3ee`. Fixed in `ca5a2e0`.

Checks on the fixed tree: `python -m unittest discover tests/unit`: 509 run, OK (skipped 1). `make -C tests/host test`, `make -C tests/ntdll-host test`, `make -C tests/kernel-host check`: all pass. `bash tests/syntax/check.sh`: 32 passed, 5 failed, 452 warnings, the same five files as before.

Notes outside the table:

- `arch/x86/nt/mm.c` (`NtFreeVirtualMemory`, decommit) had no wrap guard (same defect as `d5728cf`). Fixed in `ca5a2e0` (see the third-pass table).
- `f942cb9`'s test (`src/kernel/nt/mm_test.c`) frees the section struct directly instead of closing the handle, so the handle table keeps an entry for freed memory. It is OK for the audited change but should be cleaned up.

## Per-commit audit

Newest first. Files: basenames of every file the commit touches. Location: `file:line` is the first changed line in the post-image of that commit (not of HEAD), for the first in-scope file the commit touches.

| Commit | Files | Verdict | Location | Reason |
|---|---|---|---|---|
| `122f91d` | nt_service_list.h, nt_service_list.h, syscall_table.def, test_syscall_table_argtypes.py | OK | `arch/x86/nt/nt_service_list.h:85` | NtSetTimer arity 8 to 7 matches its seven parameters; the five ArgTypes lists match the kernel handlers or NT prototypes (table text only) |
| `619463c` | ntdll.md, unwind_pure.h, test_unwind_pure.c | OK | `src/ntdll/unwind_pure.h:24` | epilog decoder reads only the n bytes passed and checks every index; no caller yet (UNW-2 prerequisite) |
| `949cf00` | nt_io.c, io.c | OK | `arch/x86/nt/nt_io.c:778` | component loop rejects a component past NAME_MAX-1 before the copy; kernel branch frees resolved on the error path |
| `667d788` | io_test.c | SUSPECT | `src/kernel/nt/io_test.c:855` | 300-character create case now also accepts STATUS_NAME_TOO_LONG (io_test.c long_name_rejected); still a rejection, but the expectation was relaxed, so confirm it is intended |
| `e86e643` | x64-unwind-rules.md, unwind.c, test_unwind_pure.c | OK | `src/ntdll/unwind.c:163` | chained codes now applied secondary-first as the x64 chain rule states; new chained-order test covers it |
| `17fe71b` | check_syscall_tables.py | OK | `tools/check_syscall_tables.py:` | mode-only change (executable bit on a shebang script) |
| `6f592ff` | run_all_checks.sh | OK | `tools/run_all_checks.sh:9` | run_all_checks.sh adds the syscall parity step; no gating removed |
| `4a5e4c3` | io_test.c | OK | `src/kernel/nt/io_test.c:845` | test-only: adds the 255/300-character failing case expecting STATUS_OBJECT_NAME_INVALID, as the fix requires |
| `6560e0d` | except.c | OK | `src/ntdll/except.c:412` | vectored continue handlers run on the resume path; frame-path order unchanged |
| `35c6a9c` | Makefile, test_except_pure.c | OK | `tests/ntdll-host/Makefile:12` | test-only: vectored list order and resume decisions |
| `542d3aa` | except.c, except_pure.h | OK | `src/ntdll/except.c:12` | refactor: exc_list_insert/remove, exc_veh_resumes (ret == -1), exc_depth_exceeded (depth > 16) reproduce the old conditions |
| `00e4093` | gen_ntdll_def.py | OK | `build/gen_ntdll_def.py:32` | gen_ntdll_def.py drops the repeated NtRaiseException; printed count 31 to 30 matches the fixed set |
| `8c7a28d` | test_gen_ntdll_def.py | OK | `tests/unit/test_gen_ntdll_def.py:16` | test expectation follows 00e4093 (fixed set 11 to 10) |
| `eef2786` | ntdll.def | SUSPECT | `src/ntdll/ntdll.def:343` | hand edit of checked-in src/ntdll/ntdll.def, a copy of generator output; generator not re-run here (no Python on this host) |
| `1823f69` | Makefile, test_unwind_pure.c | OK | `tests/ntdll-host/Makefile:10` | test-only: one case per UWOP_* code, prolog state and chained info |
| `50cf960` | static_analyze.sh | OK | `tools/static_analyze.sh:` | mode-only change |
| `460791f` | test_check_syscall_tables.py | OK | `tests/unit/test_check_syscall_tables.py:1` | test-only: unit tests for check_syscall_tables.py; not run here (no Python) |
| `8587cb9` | check_syscall_tables.py | OK | `tools/check_syscall_tables.py:1` | new parity checker; parses the same NT_SYSCALL and NT_SERVICE forms the headers use; not run here (no Python) |
| `ef172d9` | unwind.c, unwind_pure.h | OK | `src/ntdll/unwind.c:25` | refactor: each uwp_* helper reproduces the expression it replaced (module range, prolog start, alloc sizes, chain slots) |
| `a275418` | i2_test.c, object_test.c | BROKEN | `src/kernel/nt/i2_test.c:348` | bare return in pseudo_handles and handle_table_lifetime counts neither pass nor fail, so a broken fixture passes silently; fixed in 6db83e7 (ASSERT) |
| `02f989c` | ntdll_runtime.c | OK | `src/ntdll/ntdll_runtime.c:188` | #undef errno before the definition in the freestanding build only |
| `7c66a22` | registry.c | OK | `src/kernel/nt/registry.c:1122` | NULL KeyValueInformation rejected after the length check and before any write |
| `f64e3ec` | .gitignore, static_analyze.sh | OK | `tools/static_analyze.sh:1` | informational analyzer; exit status does not gate the run |
| `df5fbd8` | _paths.py, c_coverage.sh | OK | `tests/harness/_paths.py:` | mode-only change |
| `f942cb9` | mm.c, mm.c, mm_test.c | OK | `arch/x86/nt/mm.c:1580` | raw *ViewSize checked before rounding in both arch and src copies; regression test fails before the change |
| `8e7e190` | ntstatus_to_dos.c, test_nt_types.c, test_ntstatus_to_dos.c | OK | `src/ntdll/ntstatus_to_dos.c:867` | header-value comments and DLL_NOT_FOUND expectation (599 to 126) match ERROR_MOD_NOT_FOUND; table rows unchanged |
| `b7e1edc` | c_coverage.sh | OK | `tools/c_coverage.sh:67` | tools only: runs only the ntdll test binaries |
| `b616826` | Makefile, registry_ops_test.c | OK | `tests/kernel-host/Makefile:58` | test-only: registry value operations |
| `c4bffb1` | object.c, proc_thread.c | OK | `src/kernel/nt/object.c:771` | ObRegisterThread is idempotent under the list lock; the duplicate node is freed in both branches |
| `3a03885` | test_pe_parse.c | OK | `tests/host/test_pe_parse.c:1017` | test-only: loader accessor branches |
| `c5ee8ad` | Makefile, handle_edge_test.c | OK | `tests/kernel-host/Makefile:58` | test-only: handle table edge cases |
| `7c8fff4` | .gitignore, Makefile, Makefile, Makefile, c_coverage.sh | OK | `tests/host/Makefile:35` | COVERAGE=1 mode added; default LDFLAGS and LDLIBS keep their values |
| `ef83425` | Makefile, exports.c, exports.def, test_real_pe.c | OK | `tests/host/Makefile:67` | test-only: DLL fixture with named, ordinal, DATA exports and forwarders |
| `1937b5f` | Makefile, minimal.c, test_real_pe.c | OK | `tests/host/Makefile:58` | test-only: PE fixture builder and parser test |
| `7fba2cb` | nt_io.c | OK | `arch/x86/nt/nt_io.c:1666` | removes the stray pos -= got; the in-branch rewind on copy failure is kept (nt_io.c) |
| `ee49ebe` | object_test.c | OK | `src/kernel/nt/object_test.c:619` | test-only: one registry node per thread; fails before c4bffb1 and passes now |
| `1f334ce` | msvcrt.c, msvcrt_policy.h, test_msvcrt_policy.c | SUSPECT | `src/ntdll/msvcrt.c:299` | sprintf output now capped at 1023 characters (was 8191 before, silently truncated); caller buffers under 1024 bytes still overflow, as the commit says |
| `1224d00` | msvcrt.c, msvcrt_policy.h, Makefile, test_msvcrt_policy.c | OK | `src/ntdll/msvcrt.c:9` | calloc product checked by ms_mul_size; *out unchanged on failure; test covers wrap cases |
| `1f5e08e` | test_gen_ntdll_def.py | OK | `tests/unit/test_gen_ntdll_def.py:63` | test expects undecorated _DllMainCRTStartup, following DEF-1 |
| `90b9bc6` | msvcrt.c | OK | `src/ntdll/msvcrt.c:78` | atexit table bounded at 32; exit runs handlers last-in first-out; _exit and _Exit skip them |
| `e5f85a7` | ldr.c | OK | `src/ntdll/ldr.c:1804` | ANSI_STRING copied by Length into a terminated buffer; freed on every path |
| `235067a` | kernel32.c | OK | `src/ntdll/kernel32.c:473` | class 14 position get and set, class 5 standard info; error paths set last error |
| `8dec432` | kernel32.c | OK | `src/ntdll/kernel32.c:553` | NtDuplicateObject argument order matches its prototype; Win32 option values kept |
| `a0e9450` | kernel32.c | OK | `src/ntdll/kernel32.c:141` | len 0x7FFF rejected; 0x7FFE gives Length 0xFFFC and MaximumLength 0xFFFE, both fit USHORT |
| `fd86cb3` | kernel32.c | SUSPECT | `src/ntdll/kernel32.c:193` | unsupported protection now fails with ERROR_INVALID_PARAMETER, including protect 0 with MEM_RESERVE; confirm Windows behaviour for protect 0 on reserve |
| `862d261` | kernel32.c | OK | `src/ntdll/kernel32.c:45` | NtQueryPerformanceCounter is in the syscall table and ntdll.def; NT_SUCCESS checked |
| `03f0653` | user32.c | OK | `src/ntdll/user32.c:25` | wvsprintfA and wvsprintfW now format; va_end moved to the callers |
| `5afcedb` | user32.c | OK | `src/ntdll/user32.c:34` | %s wide in wsprintfW; %S and %hs narrow; NULL handled |
| `4001a61` | user32.c | OK | `src/ntdll/user32.c:108` | %c in wsprintfA writes one character, matching wsprintfW |
| `3a0db32` | ntdll_main.c | OK | `src/ntdll/ntdll_main.c:376` | bare-name, /tmp and root candidates removed; system directory only |
| `2f4eae7` | run_all_checks.sh | OK | `tools/run_all_checks.sh:9` | run_all_checks.sh wiring for the doc-count check |
| `9010421` | ntdll_main.c | OK | `src/ntdll/ntdll_main.c:346` | depth counter capped at 16; decremented on every return path of the wrapper |
| `2913b8c` | ntdll_main.c | OK | `src/ntdll/ntdll_main.c:331` | header page released with RegionSize 0, as MEM_RELEASE requires |
| `079509b` | debugfs.h, percpu.h, printk.h | OK | `tests/syntax/shim/linux/debugfs.h:16` | syntax-check shim stubs only |
| `27030e1` | gen_ntdll_def.py, ntdll-syscalls.md, advapi32.def, kernel32.def, kernelbase.def, msvcrt.def, ntdll.def, ucrtbase.def, user32.def, check_exports.sh | SUSPECT | `build/gen_ntdll_def.py:38` | _DllMainCRTStartup@12 removed from the generator and six checked-in .def copies; out/ntdll.def not regenerated (commit message says so) |
| `ded5f9f` | ldr.c | OK | `src/ntdll/ldr.c:1535` | init stack bound checked before the write |
| `b40d234` | ldr.c | OK | `src/ntdll/ldr.c:955` | file buffer freed on image allocation failure; goto not used, so no double free |
| `a141aea` | ldr.c | OK | `src/ntdll/ldr.c:996` | wide base name no longer freed twice; the fail path frees FullDllName once |
| `0688cf7` | test_update_doc_counts.py | OK | `tests/unit/test_update_doc_counts.py:1` | test-only: update_doc_counts unit tests; not run here (no Python) |
| `04c679a` | ldr.c | OK | `src/ntdll/ldr.c:1212` | strcmp is case-sensitive, as Windows export lookup is |
| `71d1778` | update_doc_counts.py | OK | `tools/update_doc_counts.py:1` | tool; --check only reads, --write rewrites; not run here (no Python) |
| `39e6015` | msvcrt.c | OK | `src/ntdll/msvcrt.c:315` | __C_specific_handler returns ExceptionContinueSearch (1) instead of ContinueExecution (0) |
| `cdcc7ce` | msvcrt.c | OK | `src/ntdll/msvcrt.c:220` | NUL written only when outsz is nonzero; index stays below outsz |
| `3a8f7d1` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1141` | trampoline entered at sp+8 (16-byte aligned); direct StartRoutine fallback keeps sp |
| `2a499a7` | msvcrt.c | OK | `src/ntdll/msvcrt.c:83` | realloc copies min(old, new) and keeps ptr on failure |
| `10a9418` | mm.c | OK | `arch/x86/nt/mm.c:52` | kallsyms lookup removed; no stale len use remains in kshim_mprotect |
| `e8dd37b` | mm.c | OK | `arch/x86/nt/mm.c:18` | core_initcall initialises the VAD table; lazy checks kept |
| `92c14f5` | ntdll_runtime.c, test_runtime.c | OK | `src/ntdll/ntdll_runtime.c:144` | 16-byte header stores the block size; realloc copies the old size; malloc bound checked |
| `075e040` | nt_io.c | OK | `arch/x86/nt/nt_io.c:549` | delete-on-close moved to ntio_file_release; NtClose no longer unlinks |
| `325b55c` | nt_io.c | BROKEN | `arch/x86/nt/nt_io.c:1663` | stray unconditional pos -= got after every chunk; reads over 64 KiB re-read the first chunk; fixed in 7fba2cb |
| `b3dcef5` | nt_io.c | SUSPECT | `arch/x86/nt/nt_io.c:1525` | IOSB probe is correct, but the message omits the IO-17 rewind this commit also adds (nt_io.c:1660); kept |
| `6ae7bb7` | ntdll-except.md, except.c | OK | `src/ntdll/except.c:590` | RtlUnwindExWrapper had no caller; docs updated |
| `224f6a6` | kernel-nt.md, dispatch.c | OK | `src/kernel/nt/dispatch.c:114` | per-call syscall trace demoted to pr_debug |
| `366715b` | ntdll_main.c, unwind.c, unwind.h, test_unwind.c | OK | `src/ntdll/ntdll_main.c:815` | RtlDeleteFunctionTable removes the entry; a second delete reports failure |
| `dae81ab` | kernel-nt.md, security.c | OK | `src/kernel/nt/security.c:873` | ACL header bounds checked before AclSize is read; cleanup matches the following branch |
| `a1b6bd8` | kernel-nt.md, sync.c | OK | `src/kernel/nt/sync.c:1832` | wait-on-address operands copied with copy_from_user; a fault maps to STATUS_ACCESS_VIOLATION |
| `360c366` | unwind.c, test_except.c, test_unwind.c | OK | `src/ntdll/unwind.c:239` | leaf frame pops the return address without a module lookup (unwind.c) |
| `d730c42` | weak.c | OK | `arch/x86/nt/weak.c:15` | stray RtlUserThreadStart global removed; no remaining symbol references |
| `38b1b0a` | proc_thread.c | SUSPECT | `arch/x86/nt/proc_thread.c:1356` | unchanged from the earlier decision: count is changed before the copy-out (proc_thread.c:1356); see the resolution section |
| `7e1a710` | except.c, unwind.c, test_unwind.c | OK | `src/ntdll/except.c:502` | RtlVirtualUnwind returns only the requested handler kind; except.c passes EHANDLER; the other callers ignore the handler |
| `0fe2af8` | unwind.c, test_unwind.c | OK | `src/ntdll/unwind.c:111` | x64 RUNTIME_FUNCTION EndAddress is not flagged; lookup uses the full value |
| `a4ddf86` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1391` | rcu_read_lock around find_vpid and pid_task in suspend, resume and terminate |
| `d6e5ab7` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1201` | cross-process creation refused before any object is created |
| `e6d152f` | proc_thread.c | SUSPECT | `arch/x86/nt/proc_thread.c:1118` | NULL thr returns before setting child registers, so the child runs the parent's return path; caller ignores the result (unreachable today) |
| `0f85fea` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1110` | nt_get_ntdll_RtlUserThreadStart always returned 0; probe and its user reads removed |
| `d5728cf` | kernel-nt.md, mm.c | BROKEN | `src/kernel/nt/mm.c:1207` | end check ran after rounding; a size within a page of SIZE_MAX rounded to 0 and passed (src/kernel/nt/mm.c, protect and decommit); fixed in 993c3ee |
| `0b869fd` | proc_thread.c | OK | `arch/x86/nt/proc_thread.c:1241` | creation and suspend/resume logs demoted to pr_debug; NtResumeThread labels corrected; no logic change |
| `27e20ab` | kernel-nt.md, mm.c, mm_test.c | OK | `src/kernel/nt/mm.c:1462` | file-backed sections refused with STATUS_NOT_IMPLEMENTED; no caller in the tree |
| `bdbf6c1` | kernel-nt.md, mm.c, mm_test.c | OK | `src/kernel/nt/mm.c:855` | RegionSize wrap refused before round-up; test covers it |
| `bdae5f7` | nt_io.c | OK | `arch/x86/nt/nt_io.c:2245` | negative EndOfFile rejected before truncate |
| `2adcc11` | nt_io.c | OK | `arch/x86/nt/nt_io.c:36` | core_initcall initialises the I/O locks in the kernel build; userspace constructor unchanged |
| `9ab1528` | kernel-nt.md, object.c | OK | `src/kernel/nt/object.c:1136` | registry insert result checked; failure path destroys the new object |
| `4a0a0e3` | nt_io.c | OK | `arch/x86/nt/nt_io.c:373` | output pointer probed before a slot is reserved; get_user and put_user write back the value read |
| `c240201` | kernel-nt.md, object.c | OK | `src/kernel/nt/object.c:674` | permanent flags cleared before the object can be freed |
| `09da3f2` | kernel-nt.md, registry.c | OK | `src/kernel/nt/registry.c:610` | p == 0 checked before buf[p-1] |
| `6666521` | kernel-nt.md, registry.c | OK | `src/kernel/nt/registry.c:182` | Length above 0xFFFD rejected before the terminator is added |
| `9bbbb2c` | kernel-nt.md, object.c | OK | `src/kernel/nt/object.c:68` | same USHORT guard in nt_dup_unicodestring |
| `649f184` | mm.c | OK | `arch/x86/nt/mm.c:1626` | private VADs refused; section views keep MEM_MAPPED or MEM_IMAGE |
| `e0e5989` | mm.c | OK | `arch/x86/nt/mm.c:876` | size checks before rounding in allocate and protect (arch copy) |
| `5cced27` | mm.c | OK | `arch/x86/nt/mm.c:963` | reservation unmapped when the commit step fails |
| `4094946` | kernel-nt.md, object.c, object_test.c | OK | `src/kernel/nt/object.c:312` | symlink depth bounded at 32; ObpResolveSymlink passes attributes as before |
| `8f7d873` | nt_services.c | OK | `arch/x86/nt/nt_services.c:342` | copy size 24 equals the reported length; struct zeroed first |
| `f63ca2d` | nt_services.c | OK | `arch/x86/nt/nt_services.c:159` | PreviousState probed before the event state changes; probe writes back the value it read |
| `246e300` | kernel-nt.md, object.h | OK | `src/kernel/nt/object.h:82` | comment-only change in object.h; contract matches the okay_to_close call site under the table lock |
| `c351f77` | kernel-nt.md, handle.c | OK | `src/kernel/nt/handle.c:145` | entry returned by the allocator; no second lookup |
| `cc755fa` | kernel-nt.md, handle.c, object_test.c | SUSPECT | `src/kernel/nt/handle.c:533` | undo path calls ObCloseHandle on the duplicate; if the caller passes OBJ_PROTECT_FROM_CLOSE the close is refused and the new handle and reference leak (handle.c:536-541) |
| `35a1e46` | dispatch.c | OK | `arch/x86/nt/dispatch.c:382` | debugfs_lookup reference dropped with dput |
| `8ccc960` | dispatch.c | OK | `arch/x86/nt/dispatch.c:113` | per-call pr_info removed from the syscall path |
| `1fd3821` | dispatch.c | OK | `arch/x86/nt/dispatch.c:311` | missing-service log rate-limited |
| `ce0ec9a` | dispatch.c | OK | `arch/x86/nt/dispatch.c:119` | this_cpu_add on a preemptible path |
| `d022f12` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:876` | LdrInitializeThunk export scan capped; thunk RVA checked against the image and an executable section |
| `d4a33f2` | binfmt_pe.c | SUSPECT | `src/kernel/fs/binfmt_pe.c:55` | relocation directory above 16 MiB is skipped silently on the ntdll path (binfmt_pe.c:806) instead of failing the load |
| `c75cda6` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:705` | allocation rounded to pages; read exactly the bytes needed |
| `e9012fa` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:668` | ntdll header reads bounded to 24 bytes; optional header checked to [112, 240] |
| `c3647ea` | pe_parse.c, test_pe_parse.c | OK | `src/loader/pe_parse.c:1226` | types 6 and 11 to 15 rejected; test covers 6, 11 and 15 |

# Third pass: new code after fd26716

Date: 2026-10-12. Branch `haiku/audit3`, worktree `ntpers-audit3`. This pass audits the new service implementations and the fix commits that landed after `fd26716`, the HEAD of the second pass. Scope: every non-merge commit after `fd26716` that touches `src/kernel/nt`, `src/kernel/fs`, `arch/x86/nt`, `src/ntdll` or `tests`. That is 64 commits, listed below newest first. Docs-only commits, and commits outside those paths, are not in the table.

## Method and limits

- Each diff was read in context (`git show -U3` to `-U8`). Called symbols and struct layouts were checked by grep at HEAD. The checks were (a) user pointers probed and copied, (b) handle and object references balanced, (c) lock order and unlock on error paths, (d) size arithmetic, (e) uninitialised memory returned, (f) NT status codes, (g) tests that cannot fail, (h) `__KERNEL__` branches.
- Removed assertions were checked with `git log -p` over the in-scope paths. The only removed `ASSERT` lines are moves (the IoCompletion tests in `18d429d`). No assertion was removed or weakened.
- Host limits: `access_ok` is `1` in the host shim (`src/kernel/nt/kshim.h:74`), so a bad user pointer cannot be made on the host. Copy faults and allocation failures have no injection hook. Fixes for those paths are not host-tested, and the commit messages say so.
- The Linux kernel build was not run (no kernel headers on this host). Kernel-only code was checked by `tests/syntax/check.sh` only.

## Results

| Verdict | Count |
|---|---|
| OK | 48 |
| SUSPECT | 7 |
| BROKEN | 9 (all fixed, see below) |
| Audited | 64 |

Two BROKEN rows (`993c3ee`, `ca5a2e0`) share one fix commit (`e1486e9`). The table counts each audited hash once.

## Fixes applied

One commit per item, in branch order. Each references the audited hash.

- **bfd5d93** `fix(nt/mm): refuse a page range that wraps in NtFlushVirtualMemory`, for `8c0b86c`. The wrap guard underflowed for a base in the top page, so an unmapped range returned success. The new test `test_top_of_space_range_refused` (`src/kernel/nt/mm_test.c`) fails before the change (flush returned `0x0`) and passes after it.
- **e1486e9** `fix(nt/mm): close the wrap guard for a base in the top page`, for `993c3ee` and `ca5a2e0`. The protect and decommit guards underflow the same way for a base in the top page. The change is in `src/kernel/nt/mm.c` and `arch/x86/nt/mm.c`. The host test does not distinguish it: protect returned `STATUS_INVALID_PARAMETER` for that range before the change too.
- **b5e7ecf** `fix(nt/io): zero FILE_BASIC_INFORMATION before NtQueryAttributesFile`, for `cc1bbde`. Four stack bytes went to the caller. Not host-tested.
- **0caabcd** `fix(nt/io): zero FILE_NETWORK_OPEN_INFORMATION before NtQueryFullAttributesFile`, for `64476d7`. Same defect. Not host-tested.
- **1c8145e** `fix(nt/sync): close the new timer handle when the handle copy-out faults`, for `b891048`. Not host-tested.
- **6b45e31** `fix(nt/sync): close the keyed-event handle when the handle copy-out faults`, for `b9661c2`. Not host-tested.
- **0fad804** `fix(nt/sync): probe the timer output before NtSetTimer or NtCancelTimer changes the timer`, for `10ddb43`. Not host-tested (the host `access_ok` is always 1).
- **bb51ab6** `fix(nt/registry): keep the old value when NtSetValueKey cannot allocate`, for `0520a92`. The old code could leave a value with `data` NULL and `data_len` set, and the new snapshot then copied from NULL. Not host-tested (no allocation injection).

## Checks on the fixed tree

- `make -C tests/kernel-host check`: all programs exit 0, including `mm_test` at 30/30 (was 29/30 before the new test).
- Python unit suite (`python -m unittest discover tests/unit`): 509 run, OK (skipped 1). Same as the second pass.
- `bash tests/syntax/check.sh`: passed 35, failed 5, warnings 480. The five failures are the same files as before (`src/kernel/nt/dispatch.c`, `src/kernel/nt/kshim.c`, `src/kernel/fs/binfmt_pe.c`, `src/kernel/fs/binfmt_pe_test.c`, `arch/x86/nt/dispatch.c`). The pass count rose by three because new files were added since the second pass. The warning count rose from 452 with them; the new warnings were not classified individually.

After the SUSPECT resolution (branch `haiku/suspects3`): `make -C tests/kernel-host check` runs all 13 programs with rc 0 (`sync_extra_test` 30/30, `mm_test` 30/30, `security_extra_test` 24/24, `io_test` 29/29). Python unit suite: 509 run, OK (skipped 1). `tests/syntax/check.sh`: passed 35, failed 5 (the same five files as above), warnings 479, the same per file as before the pass. `tools/update_doc_counts.py --write`: counts already current.

## SUSPECT: for review

These need a decision. None was changed in this pass, except where a BROKEN fix above covers the same line.

1. **`2393db8`** (`src/kernel/nt/io_test.c:946`): the 255-character create is skipped when `PATH_MAX` is 260, which is the case on this Windows host. Lookup still covers 255. Decide whether the create case should use a shorter scratch root instead of being skipped.
2. **`0258238`** (`src/kernel/nt/sync.c:2708`): `NtSignalAndWaitForSingleObject` references both handles with access `0` and a `NULL` type. No `SYNCHRONIZE` or `EVENT_MODIFY_STATE` (or the mutant and semaphore equivalents) is checked. Decide the rights, then add the check.
3. **`b9661c2`** (`src/kernel/nt/sync.c:2857`, `keyed_event_op`): WAIT and RELEASE pass access `0`, so `KEYEDEVENT_WAIT` and `KEYEDEVENT_WAKE` are not checked. The copy-out part of this commit is fixed in `6b45e31`.
4. **`1e8f0dd`** (`src/kernel/nt/security.c:2174`): `TokenStatistics` returns the low 32 bits of a kernel pointer as `TokenId.LowPart`. That is a partial KASLR leak. The replacement needs a per-token identifier, which is a design choice, so it was not changed. The fill pass of `ntsec_query_token` (`security.c:2201`) also has no bound against the measured size. Latent today, because token counts do not change.
5. **`c1934e9`** (`src/kernel/nt/sync.c:867` and `1709`): `timer_expiry_100ns` computes `now - due`, which overflows for `due == INT64_MIN`.
6. **`4260f62`** (`src/kernel/nt/security.c:2530`): `ntsec_sr_length` adds the `ULONG` offset and size before the 64-bit comparison, so the total can wrap. `sd_from_self_relative` then rejects the descriptor, so the outcome is fail-closed.
7. **`a90862b`** (`src/kernel/nt/mm.c:1534`): `ntmm_transfer` uses a 4 KiB kernel stack buffer. Kernel builds warn above 2 KiB frames. The same class of 4 KiB stack path buffer appears in `NtQueryAttributesFile`, `NtDeleteFile` and `NtQueryFullAttributesFile` (`src/kernel/nt/io.c:2723`, `3222`, `3258`). Decide one bound for kernel stack use.
8. **`9bb018d`**: `test_allocate_ex` accepts either `0x10000` or `0x1000` (`src/kernel/nt/mm_test.c:1378`), a relaxed expectation. The dispatch wrapper (`src/kernel/nt/dispatch.c:182`) writes base and size back on any status except `STATUS_NOT_SUPPORTED`, as the other wrappers do.

Resolution of the SUSPECT items (branch `haiku/suspects3`, 2026-10-12). Each fix is one commit. Each has a failing test first where the host can run it.

1. **`2393db8`**: documented in `1267511`, skip kept. The create of a 255-byte component runs only when root + 1 + 255 fits PATH_MAX, which is 260 with the MinGW headers, so any root longer than 3 bytes skips it on this host. The comment now says so, and that it runs on Linux (PATH_MAX 4096). A relative root was tried and rejected: the path fits, but a 255-byte relative file cannot be removed afterwards on this host (MAX_PATH), so it would be left in the test directory.
2. **`0258238`**: fixed in `accf1ce`. The signal handle needs its type's modify right (EVENT_MODIFY_STATE, MUTANT_MODIFY_STATE, SEMAPHORE_MODIFY_STATE) and the wait handle needs SYNCHRONIZE, through the existing desired-access check. Test `signal_and_wait_checks_rights` failed before the change.
3. **`b9661c2`** (WAIT and RELEASE part): fixed in `7c6f281`. NtWaitForKeyedEvent needs KEYEDEVENT_WAIT (0x1) and NtReleaseKeyedEvent needs KEYEDEVENT_WAKE (0x2). Test `keyed_event_checks_rights` failed before the change.
4. **`1e8f0dd`**: fixed in `222f64d`. Each token has a LUID (`token_id`) from a monotonically increasing counter, set on create and on duplicate, and TokenStatistics returns it. Test `token_statistics_id_is_counter_not_address` failed before the change. Fill-pass bound, accepted with reason: the group and privilege counts are set only on a new or private duplicate before publish (`security.c:942`, `959`, `1078`, `1091`, `2320`, `2327`), and nothing changes them afterwards, so the measured and filled lengths agree.
5. **`c1934e9`**: fixed in `0589b08`. Both `timer_expiry_100ns` variants call `nt_relative_expiry_100ns`, which takes the magnitude as uint64 and saturates the expiry at INT64_MAX. Test `set_timer_int64_min_due_does_not_fire` failed before the change (the timer signalled within 300 ms). The kernel branch is checked by the syntax check only.
6. **`4260f62`**: fixed in `de11c20`. `ntsec_sr_extend` sums the end in 64 bits and refuses a result past the 64 KiB limit. The outcome was already fail-closed. Test `sr_extend_refuses_ulong_wrap` calls the helper directly. It was not seen failing, because the helper did not exist before the change, and through NtAccessCheck the read at the offset faults on a 64-bit host first.
7. **`a90862b`**: `ntmm_transfer` fixed in `19291fe` (heap bounce buffer, same copy behaviour; mm_test 30/30). Needs owner: NtQueryAttributesFile, NtDeleteFile and NtQueryFullAttributesFile (`io.c:2723`, `3222`, `3258`) still use 4 KiB stack path buffers. Should those move to the heap too, or is one 4 KiB frame accepted on those syscall paths?
8. **`9bb018d`**: fixed in `646ce95`. The exact check `size == 0x1000` is restored. A NULL-base reserve is page-rounded, so 0x10000 cannot occur. mm_test 30/30 with the exact check.

Also observed, not in this pass's scope or not a fix target here:

- `0520a92` (`src/kernel/nt/registry.c:1045`): `snap_walk_key` recurses once per key level with no depth bound. A deep key tree can exhaust the kernel stack during `NtFlushKey`.
- Pre-existing (`5eb8026`): in `NtSetValueKey`, the create path frees only the new record when its data allocation fails (`registry.c`, near the `kfree(nv)` line), which leaks the name buffer. The replace path calls `kmalloc(GFP_KERNEL)` while `k->lock` is held for write, which can sleep in atomic context in a kernel build.

## Per-commit audit

Newest first. Files: basenames of the in-scope files each commit touches. Location: `file:line` in the post-image at HEAD, or at the commit where the line was first changed.

| Commit | Files | Verdict | Location | Reason |
|---|---|---|---|---|
| `2393db8` | io_test.c | SUSPECT | src/kernel/nt/io_test.c:946 | The 255-character create is skipped when PATH_MAX is 260 (this Windows host), so that length is only checked by lookup. Lookup and reopen still assert, but a skipped case can hide a failure here. |
| `25bc012` | io.c, nt_io.c | OK | src/kernel/nt/io.c:898 | Component buffers sized NAME_MAX+1; a component at or over NAME_MAX is rejected before the copy. |
| `05927a9` | test_criteria_trace.py | OK | tests/unit/test_criteria_trace.py:1 | Unit tests only; the Python unit suite passes (509 run, 1 skipped) on the fixed tree. |
| `8e70c92` | test_service_inventory.py | OK | tests/unit/test_service_inventory.py:1 | Tooling and tests only; covered by the unit suite run. |
| `42ee3d6` | i2_test.c, nt_services.c | OK | src/kernel/nt/nt_services.c:136 | NULL checked; kernel path uses access_ok and copy_to_user; unit conversion uses the 1601 epoch constant. |
| `fd00d2a` | handle.c, handle.h, i2_test.c, nt_obj.c | OK | src/kernel/nt/nt_obj.c:748 | Length and class checked; ObjectInformation probed with access_ok and read with copy_from_user; flags set under the table lock (handle.c:410). |
| `993c3ee` | mm.c, mm_test.c | BROKEN | src/kernel/nt/mm.c:1232 | Guard size > ~0 - PAGE - base underflows for a base in the top page, so a wrapping range passed. Fixed in e1486e9 (guard now also tests the base). |
| `cc1bbde` | io.c, io.h, io_test.c | BROKEN | src/kernel/nt/io.c:2742 | FILE_BASIC_INFORMATION copied out with 4 uninitialised tail bytes (kernel stack to user). Fixed in b5e7ecf. |
| `6db83e7` | i2_test.c, object_test.c | OK | src/kernel/nt/object_test.c:400 | Bare returns that counted as neither pass nor fail are now ASSERT. |
| `f9698f5` | Makefile, fuzz_pe.c, fuzz_plat.c, fuzz_plat.h | OK | tests/fuzz/fuzz_pe.c:1098 | Deterministic fuzzer; exits 1 when a crash class is found, so a finding is not hidden. |
| `b891048` | Makefile, sync.c, sync_extra_test.c | BROKEN | src/kernel/nt/sync.c:2484 | On a handle copy-out fault the new or opened timer handle stays in the process table. Fixed in 1c8145e (ObCloseHandle on that path, lines 2484 and 2511). |
| `3047209` | test_pe_parse.c | OK | tests/host/test_pe_parse.c:1 | Test-only additions; no assertion removed. |
| `10ddb43` | sync.c, sync_extra_test.c | BROKEN | src/kernel/nt/sync.c:2557 | PreviousState (NtSetTimer) and CurrentState (NtCancelTimer) were probed after the timer changed, so a bad pointer returned ACCESS_VIOLATION with the timer armed or cancelled. Fixed in 0fad804. |
| `a164b43` | kshim.c | OK | tests/kernel-host/shim/*:1 | KSHIM_POISON fills freed blocks with 0xDD; debug-only. |
| `50dbf92` | Makefile, README.md, stress_common.h, stress_handle_test.c, stress_object_test.c, stress_sync_test.c | OK | tests/kernel-host/stress_handle_test.c:167 | Stress programs; violations counted by STRESS_CHECK and returned as a nonzero exit. Run by make stress, not check. |
| `663b877` | fuzz_pe.c, fuzz_plat.c | OK | tests/fuzz/Makefile:1 | Build define only. |
| `ca5a2e0` | mm.c | BROKEN | arch/x86/nt/mm.c:1300 | Same underflowing guard in the arch decommit path. Fixed in e1486e9. |
| `8ea698f` | sync.c, sync.h, sync_extra_test.c | OK | src/kernel/nt/sync.c:2643 | Class 0 only; length and output probed; the timer is referenced and released on every path. |
| `3daa005` | binfmt_pe.c | OK | src/kernel/fs/binfmt_pe.c:55 | ntdll reloc branch returns -ENOEXEC above PE_MAX_RELOC_BYTES, matching the exe path. |
| `0258238` | sync.c, sync_extra_test.c | SUSPECT | src/kernel/nt/sync.c:2708 | Both handles are referenced with access 0 and a NULL type, so no SYNCHRONIZE or EVENT_MODIFY_STATE check is made. Type checks run after the reference, and both references are released on every path. |
| `b9661c2` | sync.c, sync_extra_test.c | BROKEN | src/kernel/nt/sync.c:2799 | On a handle copy-out fault the keyed-event handle stays open. Fixed in 6b45e31. SUSPECT part: keyed_event_op passes access 0 (sync.c:2857), so the WAIT and RELEASE rights are not checked. |
| `f8a0644` | sync.c, sync_extra_test.c | OK | src/kernel/nt/sync.c:2884 | Always STATUS_SUCCESS; no user memory touched. |
| `ecc7f83` | README.md, object.c | OK | src/kernel/nt/object.c:315 | parent_dir re-checked under the directory lock, so only one caller drops the directory reference. |
| `7f7e5d1` | test_pe_parse.c | OK | src/loader/pe_parse.c:897 | Import walk bounded by totals (PE_MAX_IMPORT_DESCRIPTORS, PE_MAX_IMPORT_THUNKS); fails closed with PE_ERR_CORRUPT; tests in tests/host. |
| `5c5b570` | Makefile | OK | tests/kernel-host/Makefile:1 | Build change only: sync_test links nt_obj. |
| `a9fc20b` | kshim.h | OK | src/kernel/nt/kshim.h:1 | NT_TRACE and NT_TRACE_ERR compile out; trace arguments checked for side effects. |
| `b2b3e3b` | nt_obj.c | OK | src/kernel/nt/nt_obj.c:1 | Trace calls gated behind NT_DEBUG_TRACE; no logic change. |
| `8140aec` | proc_thread.c | OK | src/kernel/nt/proc_thread.c:1290 | Trace calls gated behind NT_DEBUG_TRACE; no logic change. |
| `0520a92` | registry.c, registry.h, registry_test.c | BROKEN | src/kernel/nt/registry.c:987 | snap_put memcpy from NULL: NtSetValueKey freed the old data before allocating, so a failed allocation left data NULL with data_len set. Fixed in bb51ab6 (allocate first). SUSPECT part: snap_walk_key recursion (registry.c:1045) has no depth bound. |
| `3f63828` | registry.c, registry_test.c | OK | src/kernel/nt/registry.c:760 | Nested delete finds the parent with registry_find_key; path buffer size checked. |
| `f2ce759` | registry.c, registry_test.c | OK | src/kernel/nt/registry.c:1390 | KeyNodeInformation: needed size computed and length checked before any write. |
| `3e98e64` | README.md | OK | tests/kernel-host/Makefile:1 | Comment and build flag only. |
| `295b44f` | Makefile, object.c, object.h, security.c, security_extra_test.c | OK | src/kernel/nt/security.c:1003 | ObCreateTokenHandle takes its own reference on the token and drops the creation reference on every path. |
| `2676f9e` | registry.c, registry_test.c | OK | src/kernel/nt/registry.c:1231 | value_info_layout and value_info_write: needed size checked by callers, padding zeroed, data copied only within the computed length. |
| `2a73077` | object.c, object.h, security.c, security.h, security_extra_test.c | OK | src/kernel/nt/object.c:1175 | Default primary token created with the process; the early-failure ObDestroyProcessObject is safe (kzalloc, unregister is a no-op for an unregistered process). |
| `c56cc62` | security.c, security.h, security_extra_test.c | OK | src/kernel/nt/security.c:1858 | Token opened with the access the object DACL gives; caller token taken from the impersonation token when OpenAsSelf is FALSE; handle creation failure checked. |
| `1e8f0dd` | security.c, security.h, security_extra_test.c | SUSPECT | src/kernel/nt/security.c:2174 | TokenStatistics sets TokenId.LowPart to the low 32 bits of a kernel address (a partial KASLR leak). Owner decision: use a per-token id. Also the fill pass (ntsec_query_token, 2201) has no bound against the measured size. |
| `ec4c86d` | security.c, security_extra_test.c | OK | src/kernel/nt/security.c:2317 | token_duplicate deep-copies the SIDs, so ntsec_keep_effective frees each SID once; type checked; OA attributes masked. |
| `5014421` | nt_obj.c, sync.c, sync_extra_test.c | OK | src/kernel/nt/sync.c:2275 | Timer body begins with the dispatcher header (sync.h:199), so the type check is sound. |
| `c1934e9` | sync.c, sync.h, sync_extra_test.c | SUSPECT | src/kernel/nt/sync.c:867 | timer_expiry_100ns computes now - due, which overflows for due = INT64_MIN (also sync.c:1709). Otherwise the timer thread and the cancel wait are consistent. |
| `8f099f2` | sync.c, sync_extra_test.c | OK | src/kernel/nt/sync.c:2552 | User APC refused before the timer is cancelled or armed (HEAD). |
| `b3adda7` | sync.c, sync_extra_test.c | OK | src/kernel/nt/sync.c:1036 | Keyed release: lock order g_keyed_lock then ctx.lock; matched and timeout decided under the lock; stack record removed before return. |
| `1f292a1` | sync_extra_test.c | OK | tests/kernel-host/sync_extra_test.c:1 | Comment only. |
| `4260f62` | security.c, security.h, security_extra_test.c | SUSPECT | src/kernel/nt/security.c:2530 | ntsec_sr_length adds ULONG offset and size before the 64-bit check, so the total can wrap. sd_from_self_relative rejects the result, so the outcome is fail-closed. Kernel outputs are copied only on success and are initialised. |
| `00b5a3f` | mm.c, mm.h, mm_test.c | OK | src/kernel/nt/mm.c:1362 | MemoryRegionInformation reads one VAD under the lock; dispatch.c:262-268 zeroes the kernel buffer and copies out only on success. |
| `a90862b` | dispatch.c, mm.c, mm.h, mm_test.c | SUSPECT | src/kernel/nt/mm.c:1534 | ntmm_transfer uses a 4 KiB kernel stack buffer (kernel builds warn above 2 KiB frames). Otherwise the copies are probed and bounded, and the wrap clamp is correct. |
| `18d429d` | io_test.c, iocp.c, iocp.h | OK | src/kernel/nt/io_test.c:1858 | IoCompletion open and query; the moved tests were not weakened. |
| `eb0ede7` | io.c, io.h, io_test.c | OK | src/kernel/nt/io.c:2271 | Query classes check the length before the write; padding zeroed where a struct is copied out. |
| `6e8f5bf` | registry.c, registry_test.c | OK | src/kernel/nt/registry.c:698 | CREATE_LINK refused before any side effect. |
| `bc82d49` | io.c, io.h, io_test.c, posix_host.c | OK | src/kernel/nt/io.c:2616 | Allocation growth refused; disposition and rename Ex flags checked; the rename collision check compares dev and ino. |
| `e0b4227` | io.c, io.h, io_test.c | OK | src/kernel/nt/io.c:2989 | Unknown codes refused; IOSB probed and written with copy_to_user (kernel) or directly (shim). |
| `6b71651` | io.c, io.h, io_test.c | OK | src/kernel/nt/io.c:3068 | No EAs: queries report STATUS_NO_EAS_ON_FILE; IOSB probed. |
| `7e90685` | io.c, io.h, io_test.c | OK | src/kernel/nt/io.c:3148 | Flags validated; parameters refused; the no-sync path completes without a flush. |
| `760d12e` | io.c, io.h, io_test.c | OK | src/kernel/nt/io.c:3179 | Cancel reports STATUS_NOT_FOUND when nothing is pending; IOSB probed before use. |
| `64476d7` | io.c, io.h, io_test.c | BROKEN | src/kernel/nt/io.c:3278 | FILE_NETWORK_OPEN_INFORMATION copied out with 4 uninitialised tail bytes. Fixed in 0caabcd (zeroed first). |
| `7bc43a3` | io.c, io.h, io_test.c | OK | src/kernel/nt/io.c:3316 | Byte-range locks: start and length overflow guarded (NTIO_MAX_LOCK_OFFSET); user pointers read with access_ok and copy_from_user; kzalloc freed on conflict; locks dropped on release. |
| `8c0b86c` | dispatch.c, mm.c, mm.h, mm_test.c | BROKEN | src/kernel/nt/mm.c:1611 | Wrap guard underflowed for a base in the top page, so an unmapped range returned success. Fixed in bfd5d93, with a test that failed before the change. |
| `7e43c43` | io.c, io.h, io_test.c, khost_prelude.h, posix_host.c | OK | src/kernel/nt/io.c:3693 | Directory snapshot per scan; every entry is zeroed before it is filled and checked against the length; kernel branch returns NOT_SUPPORTED. |
| `c354ebf` | io.c, io.h, io_test.c | OK | src/kernel/nt/io.c:3816 | FileFsDeviceInformation length checked; output copied only on success; other classes NOT_IMPLEMENTED. |
| `4d4b483` | io.c, io.h, io_test.c | OK | src/kernel/nt/io.c:3865 | Valid classes NOT_IMPLEMENTED, unknown INVALID_INFO_CLASS; IOSB probed. |
| `9bb018d` | dispatch.c, mm.c, mm.h, mm_test.c | SUSPECT | src/kernel/nt/dispatch.c:182 | The wrapper writes base and size back on any status except NOT_SUPPORTED, as the existing wrappers do (same values on failure). test_allocate_ex accepts 0x10000 or 0x1000 (src/kernel/nt/mm_test.c:1378), a relaxed expectation. |
| `ef03f4b` | Makefile, nt_services.c, system_test.c | OK | src/kernel/nt/nt_services.c:144 | Resolution clamped; user outputs probed with access_ok and written with copy_to_user; the state update is unlocked (noted in the code). |
| `c8f479e` | Makefile, mm.c, mm.h, mm_test.c | OK | src/kernel/nt/mm.c:1769 | Section references balanced: the VAD takes one per copy (mm.c:1809) and nt_vad_free drops it; the lookup reference is released after mapping; creation failure paths are safe. |
| `c20dead` | nt_services.c, system_test.c | OK | src/kernel/nt/nt_services.c:163 | NtRaiseHardError refused; no user memory read or written; the count is checked. |


# Fourth pass: commits after 2393db8

Date: 2026-10-12. Branch `haiku/audit4`, worktree `ntpers-audit4`. This pass repeats the audit for every non-merge commit after `2393db8`, the newest audited row of the third pass, that touches `src`, `arch`, `build`, `tests` or `tools`. The range is `2393db8..e2d279e` (the HEAD at the start of this pass), which gives 98 commits. The table lists each once, newest first. Docs-only commits outside those paths are not in the table.

## Method and limits

- Focus areas were read line by line in context (`git show -U8`, callers checked by grep at the audited commit): the security descriptor slot (`bd5852e` and every `ObCreateObject` caller in `src`, `arch`, `tests`), the object namespace (directory, symlink, permanence, reference counting on lookup), thread priority, affinity and alert, the file-IO directory, lock, volume and rename paths, user-pointer copies, and error unwinds.
- Other commits (registry, loader, binfmt, tooling, tests, docs-like files) were checked by diff skim and by the host test suite. Rows marked `(skim)` were not read line by line.
- Verdicts from the third pass for commits in this range are repeated where they still apply. The pass-3 fixes are marked as such; they were not re-audited for a new defect beyond a re-read.
- Host limits, as in the earlier passes: `access_ok` is `1` in the host shim (`src/kernel/nt/kshim.h:73`), `copy_from_user` is `memcpy`, and there is no allocation-failure injection. Fixes for those paths are not host-tested. The commit messages say so.
- The kernel build was not run (no kernel headers on this host). `src/kernel/nt` is copied over `arch/x86/nt` by `vm/kbuild.sh:78-79`, so kernel-only code in `src/kernel/nt` was checked as kernel code.
- The Python unit suite was run with `-I` under the WinPython interpreter (`C:/Users/Jackson/AppData/Local/Programs/WinPython/WPy64-3.13.12.0/python/python.exe`), not `python3`.

## Results

| Verdict | Count |
|---|---|
| OK | 74 |
| SUSPECT | 15 |
| BROKEN | 9 (all fixed; 4 new in this pass, 5 carried from pass 3) |
| Audited | 98 |

Verdicts count each audited hash once. `bc82d49` and `22f2688` are BROKEN and also carry a SUSPECT note. The BROKEN rows for `c229350`, `74ca2e6`, `22f2688` and `bc82d49` are new in this pass. `ca5a2e0`, `10ddb43`, `b891048`, `cc1bbde` and `993c3ee` were BROKEN in pass 3 and are listed again because they are in this range; their fixes were already committed.

## Fixes applied

One commit per item, in branch order (oldest first). Each references the audited hash. None is pushed.

- **170edb9** `fix(nt/io): bound the rename name by the information buffer`, for `bc82d49`. `ntio_set_rename` read `FileNameLength` bytes without checking them against the buffer length, and truncated the length to USHORT. The new test `rename_name_length_bounded` (`src/kernel/nt/io_test.c`) fails before the change (`STATUS_ACCESS_DENIED`, not `STATUS_INFO_LENGTH_MISMATCH`) and passes after it.
- **520cf42** `fix(nt/io): release the RootDirectory reference in rename`, for `bc82d49`. `ntio_handle_lookup` takes a reference that the rename never dropped (`io.c:2693`). The new test `rename_root_directory_releases_ref` fails before the change (a `FILE_SHARE_NONE` open of the directory returned `STATUS_SHARING_VIOLATION`) and passes after it. The test does not check the rename status, see the SUSPECT list.
- **e3bf004** `fix(nt/io): allocate the new path before a rename succeeds`, for `bc82d49`. A failed `kstrdup` after the rename left `linux_path` NULL with `STATUS_SUCCESS` (`io.c:2731`). Not host-tested (no allocation injection).
- **dca4180** `fix(nt/obj): do not make a discarded NtDuplicateObject duplicate protected`, for `74ca2e6`. A duplicate made with `OBJ_PROTECT_FROM_CLOSE` and no `TargetHandle` could not be closed again, so the handle count rose on every call. The new case in `duplicate_null_target` (`src/kernel/nt/object_extra_test.c`) fails before the change and passes after it.
- **6c650e3** `fix(nt/proc_thread): copy the thread information value from user memory`, for `22f2688`. `NtSetInformationThread` dereferenced the user pointer `Info` directly. It now uses `access_ok` and `copy_from_user`. Not host-tested (the host shim cannot fault).
- **39e7e0a** `fix(nt/io_test): compile the scratch cleanup call only where it is declared`, for `c229350`. The syntax check failed on `src/kernel/nt/io_test.c` (implicit declaration). The prelude now defines `KHOST_SCRATCH_CLEANUP` and the call is guarded by it.

## Checks on the fixed tree

- `make -C tests/kernel-host check-quick`: all programs exit 0.
- `make -C tests/kernel-host check`: exit 0. Under `io_test`, 31 passed, 0 failed; under `object_extra_test`, 29 passed, 0 failed.
- Python unit suite (`-I -m unittest discover tests/unit`): 538 run, OK (skipped 1).
- `bash tests/syntax/check.sh`: passed 36, failed 5, warnings 483. The five failures are the same as the third pass (`src/kernel/nt/dispatch.c`, `src/kernel/nt/kshim.c`, `src/kernel/fs/binfmt_pe.c`, `src/kernel/fs/binfmt_pe_test.c`, `arch/x86/nt/dispatch.c`). The `io_test.c` failure from `c229350` is fixed in 39e7e0a.

## SUSPECT: for review

These need a decision. None was changed in this pass.

1. **`cbd67a7`** (`src/kernel/nt/nt_obj.c:957`): the userspace `NtCreateSymbolicLinkObject` does not check `LinkTarget->Buffer` before the copy. Host-only; the kernel branch checks it.
2. **`7a676a1`** (`src/kernel/nt/object.c:648`): `OBJECT_ATTRIBUTES.RootDirectory` is ignored by `ObInsertOrOpenNamedObject`, so a relative name fails. Decide whether to implement it. It is a missing feature, not an unsafe path.
3. **`6b86fe8`** (`src/kernel/nt/object.c:882`): `is_permanent` is set but never read. An anonymous `OBJ_PERMANENT` object is freed on its last handle close. Decide whether permanence should hold a reference.
4. **`3477776`** (`src/kernel/nt/security.c:2796`): `NtSetSecurityObject` on an object with no descriptor returns `STATUS_ACCESS_DENIED`, so the owner cannot attach one. The code comment states the choice. Confirm it is intended.
5. **`ef03f4b`** (`src/kernel/nt/nt_services.c:49`): one global timer-resolution request; a release by one caller resets it for all. Documented model limit; the state is not locked.
6. **`22f2688`** (`src/kernel/nt/proc_thread.c:2023`, `src/kernel/nt/sync.c:566`): the alert flag is split. `NtAlertThread` and `NtAlertResumeThread` set `nt_ethread.alerted`. Alertable waits test and clear `struct nt_thread.alerted`, and nothing in production sets that field (`nt_thread_alert` is only called from its own test). So an alert does not satisfy an alertable wait. `NtTestAlert` (`iocp.c:769`) does see the `nt_ethread` flag. This needs one flag for both paths.
7. **`9bb018d`** (`src/kernel/nt/dispatch.c:348`): if `NtAllocateVirtualMemoryEx` succeeds and the copy-out of base or size faults, the region stays committed and the caller never gets its address.
8. **`7e43c43`** (`src/kernel/nt/io.c:3593`): directory entry names are widened one byte per character, so non-ASCII host names are wrong. `opendir` failure maps to `STATUS_ACCESS_DENIED`.
9. **`7bc43a3`** (`src/kernel/nt/io.c:621`): the lock conflict test computes `start + length` and `l->start + l->length` without an overflow check. A plain read or write near a file offset of 2^63 gives undefined arithmetic. No unsafe access was found.
10. **`bc82d49`** (`src/kernel/nt/io.c:2703`): a `RootDirectory` name goes through `kshim_ntpath_to_linux`, which treats it as absolute, so the rename lands under the scratch root. `NtCreateFile` strips the root after translation (`io.c:1100-1130`); rename does not. Fix by following the create path. Relative renames do not work until then.
11. **`4260f62`** (`src/kernel/nt/security.c:2530`): `ntsec_sr_length` adds the offsets as `ULONG` after the 64-bit comparison, so a wrapped sum can give a small total. `sd_from_self_relative` rejects the result, so the outcome is fail-closed. Keep the total 64-bit. (Pass-3 SUSPECT 6, unchanged.)
12. **`c1934e9`** (`src/kernel/nt/sync.c:869`): `timer_expiry_100ns` computes `now - due` for a relative due time and overflows at `INT64_MIN`. (Pass-3 SUSPECT 5, unchanged.)
13. **`1e8f0dd`** (`src/kernel/nt/security.c:2174`): `TokenStatistics` returns the low 32 bits of a kernel pointer as `TokenId.LowPart`. The fill pass of `ntsec_query_token` (`security.c:2233`) has no bound against the measured size. (Pass-3 SUSPECT 4, unchanged.)
14. **`0520a92`** (`src/kernel/nt/registry.c:1045`): `snap_walk_key` recurses once per key level with no depth bound. (Pass-3 note, unchanged.)
15. **`ecc7f83`** (`src/kernel/nt/object.c:309`): the fix re-checks `parent_dir` under the directory lock, but the first read is unlocked. If `directory_delete` frees the directory between the read and the lock, the lock is taken on freed memory. A lock on the directory pointer is needed.
16. **`b9661c2`** (`src/kernel/nt/sync.c:2857`): `keyed_event_op` passes access 0 for WAIT and RELEASE. (Pass-3 SUSPECT 3, unchanged.)
17. **`0258238`** (`src/kernel/nt/sync.c:2708`): `NtSignalAndWaitForSingleObject` references both handles with access 0. (Pass-3 SUSPECT 2, unchanged.)

Observed, not a verdict:

- `proc_thread_test` prints `leaks 34` at exit and still returns 0. The line is the same before and after `6c650e3`.
- `io_test` syntax warnings rose from the third-pass count with the new tests. They were not classified individually.

## SUSPECT: fourth-pass resolution

Date: 2026-10-12. Branch `haiku/suspects4`, worktree `ntpers-suspects4`. Each of the 17 SUSPECT items above was re-read against its diff, its audit note and the code at HEAD. Several were already fixed by commits that landed on this branch after the audited HEAD (`e2d279e`); those are listed with the fixing hash. Every fix is its own commit, and each has a failing test first where the host can run it.

| Item | Commit | Resolution | Hash |
|---|---|---|---|
| 1 | `cbd67a7` | fixed: `ObCreateSymbolicLinkObject` refuses a target with a NULL buffer (`STATUS_INVALID_PARAMETER`). Test `create_symlink_null_target_buffer` segfaulted before, passes after | `cac7296` |
| 2 | `7a676a1` | needs-owner: should `ObInsertOrOpenNamedObject` honour `OBJECT_ATTRIBUTES.RootDirectory` (object.c:648), or keep refusing relative names with `PATH_SYNTAX_BAD`? Missing feature, not an unsafe path | none |
| 3 | `6b86fe8` | needs-owner: should an anonymous `OBJ_PERMANENT` object keep a reference until it is made temporary? `is_permanent` is set (object.c:304) and never read; the only reader is the test | none |
| 4 | `3477776` | needs-owner: confirm that an object with no descriptor refuses every set. The code says "unprotected, and nothing can change it" (security.c:2817) and the test asserts it (security_extra_test.c:1449). NT would let a WRITE_DAC holder attach one | none |
| 5 | `ef03f4b` | accepted-with-reason: a documented model limit (nt_services.c:44-47). It is one aligned ULONG that only sets an advisory resolution; no unsafe access results | none |
| 6 | `22f2688` | fixed: one alert flag, on the sync thread that alertable waits test. `NtAlertThread`, `NtAlertResumeThread` and `NtTestAlert` use it; the kernel `NtDelayExecution` honours it when alertable. Test `test_alert_satisfies_alertable_wait` failed before (STATUS_TIMEOUT), passes after. The kernel delay path is not host-testable | `a1d128c` |
| 7 | `9bb018d` | fixed: `NtAllocateVirtualMemoryEx` releases the region when the base or size copy-out faults. Not host-testable (host `copy_to_user` cannot fault) | `f583b05`, `165bca4` |
| 8 | `7e43c43` | fixed: entry names are decoded from UTF-8 to UTF-16 (test `query_directory_utf8_name`); a listing whose path stopped being a directory returns `STATUS_NOT_A_DIRECTORY` (test `query_directory_replaced_by_file`). Both failed before | `3b187d5` |
| 9 | `7bc43a3` | fixed: the byte-range end is saturated at LLONG_MAX in the conflict test. Test `lock_conflict_near_offset_limit` failed before (STATUS_UNSUCCESSFUL, expected STATUS_FILE_LOCK_CONFLICT), passes after | `fcca36a` |
| 10 | `bc82d49` | fixed: a relative rename with RootDirectory resolves inside that directory (test `rename_relative_to_root_directory` failed before with STATUS_ACCESS_DENIED). The earlier three items of this commit were fixed in 170edb9, 520cf42 and e3bf004 | `32816fb` |
| 11 | `4260f62` | already fixed: the self-relative length is summed in 64 bits (`ntsec_sr_extend`) | `de11c20` |
| 12 | `c1934e9` | already fixed: `nt_relative_expiry_100ns` saturates for a relative due time of INT64_MIN | `0589b08` |
| 13 | `1e8f0dd` | already fixed: each token has a LUID from a counter as its TokenId, so no kernel address is returned. The fill pass is accepted-with-reason: tokens are immutable after creation, so the measure pass fixes the length the fill pass writes | `222f64d` |
| 14 | `0520a92` | needs-owner: what depth bound should `snap_walk_key` (registry.c:1045) apply, and which status should a deeper tree return? The recursion has no bound today | none |
| 15 | `ecc7f83` | needs-owner: `ObpRemoveFromDirectory` reads `parent_dir` before taking the directory lock (object.c:312). Choose one: each entry holds a reference on its directory (it must then not be cyclic with `directory_delete`), or `parent_dir` is protected by one global lock | none |
| 16 | `b9661c2` | already fixed: keyed-event wait and release check KEYEDEVENT_WAIT and KEYEDEVENT_WAKE | `7c6f281` |
| 17 | `0258238` | already fixed: `NtSignalAndWaitForSingleObject` checks the signal and wait rights | `accf1ce` |

Checks on the fixed tree (`haiku/suspects4` at the last fix commit):

- `make -C tests/kernel-host check-quick`: exit 0.
- `make -C tests/kernel-host check`: exit 0; all 15 programs return 0.
- Python unit suite (`-I -m unittest discover tests/unit`): 546 run, OK (skipped 1).
- `bash tests/syntax/check.sh`: passed 36, failed 5, warnings 482. The five failing files are the same as the third pass (`src/kernel/nt/dispatch.c`, `src/kernel/nt/kshim.c`, `src/kernel/fs/binfmt_pe.c`, `src/kernel/fs/binfmt_pe_test.c`, `arch/x86/nt/dispatch.c`). `dispatch.c` gained no new error; the `MEM_RELEASE` name from `f583b05` was fixed in `165bca4`.
- `tools/update_doc_counts.py --write`: counts already current.

Not covered by the host: the kernel branches of `sync.c` (`nt_thread_alert_pending`, `nt_thread_consume_alert`), `nt_obj.c` (`NtDelayExecution`) and `dispatch.c` (the copy-out unwind) need a kernel build, which this host cannot run. The syntax check compiles the shim branch only.


## Per-commit audit

Newest first. Files: basenames of every file the commit touches. Location: `file:line` in the post-image at HEAD (`e2d279e` for unchanged code), or at the commit where the line was first changed.

| Commit | Files | Verdict | Location | Reason |
|---|---|---|---|---|
| `74ca2e6` | nt_obj.c, object_extra_test.c | BROKEN | `src/kernel/nt/nt_obj.c:619-623` | Userspace path with NULL TargetHandle closes a duplicate made with OBJ_PROTECT_FROM_CLOSE; ObCloseHandle refuses (handle.c:371), so the handle count rose by one. Fixed in dca4180. |
| `fec4217` | handle.c, handle.h, nt_obj.c, object_extra_test.c | OK | `src/kernel/nt/handle.c:410` | Class 4 read per handle with length check; both branches. Cosmetic: the "Set the Inherit..." comment now sits above ObGetHandleFlags and ObSetHandleFlags has none. |
| `6c153ff` | nt_obj.c, object_extra_test.c | OK | `src/kernel/nt/nt_obj.c:970` | Kernel branch checks access_ok for the caller's string and copies the target through a bounded kernel buffer; userspace branch checks MaximumLength before memcpy. |
| `934d67b` | nt_obj.c, object.c, object_extra_test.c | OK | `src/kernel/nt/object.c:852` | OBJ_OPENLINK only for the symlink type; the type check at object.c:444-453 covers the other types. |
| `cbd67a7` | nt_obj.c, object_extra_test.c | SUSPECT | `src/kernel/nt/nt_obj.c:957` | Userspace branch passes LinkTarget to ObCreateSymbolicLinkObject without checking Buffer; a nonzero Length with NULL Buffer faults on the host. The kernel branch checks it. |
| `455346f` | nt_obj.c, object.h, object_extra_test.c | OK | `src/kernel/nt/nt_obj.c:143` | nt_dir_fill: two passes under one read lock; stage bound rec*(take+2)+strs+sz checked before each take; records 8-aligned; kernel branch stages at most NT_DIR_QUERY_MAX. |
| `ef074ff` | nt_obj.c, object_extra_test.c | OK | `src/kernel/nt/nt_obj.c:1000` | Opens through ObpParsePath with type directory; no handle leak on type mismatch (checked). |
| `7a676a1` | nt_obj.c, object.c, object.h, object_extra_test.c | SUSPECT | `src/kernel/nt/object.c:648` | RootDirectory in OBJECT_ATTRIBUTES is ignored by ObInsertOrOpenNamedObject; a relative name fails as PATH_SYNTAX_BAD. Not an unsafe path, a missing NT feature. Decide whether to implement. |
| `6b86fe8` | Makefile, nt_obj.c, object.c, object.h, object_extra_test.c, security.c, security.h | SUSPECT | `src/kernel/nt/object.c:882` | is_permanent is set (object.c:301, 891) but nothing reads it. An anonymous OBJ_PERMANENT object is freed on its last handle close, so permanence holds only while the object sits in a directory. |
| `6c194aa` | security.c, security_extra_test.c | OK | `src/kernel/nt/security.c:3080` | Audit is a no-op by design (documented in the comment); the access decision uses the same core as NtAccessCheck. |
| `94efa84` | DEVELOPMENT.md, KNOWN-ISSUES.md, README.md, SERVICES.md, registry.md, service_inventory.py, test_service_inventory.py | OK | `tools/service_inventory.py` | Tooling and docs only; covered by the Python unit run. |
| `efa987d` | Makefile, README.md | OK | `tests/kernel-host/Makefile` | Build target and docs only. |
| `07afc54` | Makefile, sync_test.c | OK | `tests/kernel-host/sync_test.c` | Test iteration count from environment; default unchanged. |
| `37685de` | security.c, security_extra_test.c | OK | `src/kernel/nt/security.c:2800` | Owner, group and DACL set under the object's current descriptor; SACL and no-descriptor cases return before any change. |
| `c229350` | Makefile, io_test.c, khost_prelude.h, posix_host.c | BROKEN | `src/kernel/nt/io_test.c:82` | Calls khost_scratch_arm_cleanup, declared only in the kernel-host prelude; the syntax check (tests/syntax/check.sh) failed on io_test.c. Fixed in 39e7e0a. |
| `bb51ab6` | registry.c | OK | `src/kernel/nt/registry.c` | Pass-3 fix (old value kept when allocation fails); re-read, no defect. |
| `0fad804` | sync.c | OK | `src/kernel/nt/sync.c` | Pass-3 fix (probe before the timer changes); re-read, no defect. |
| `6b45e31` | sync.c | OK | `src/kernel/nt/sync.c` | Pass-3 fix (keyed-event handle closed on copy-out fault); re-read, no defect. |
| `1c8145e` | sync.c | OK | `src/kernel/nt/sync.c` | Pass-3 fix (timer handle closed on copy-out fault); re-read, no defect. |
| `0caabcd` | io.c | OK | `src/kernel/nt/io.c` | Pass-3 fix (zeroed FILE_NETWORK_OPEN_INFORMATION); re-read, no defect. |
| `b5e7ecf` | io.c | OK | `src/kernel/nt/io.c` | Pass-3 fix (zeroed FILE_BASIC_INFORMATION); re-read, no defect. |
| `e1486e9` | mm.c | OK | `src/kernel/nt/mm.c` | Pass-3 fix (top-page wrap guard); re-read, no defect. |
| `bfd5d93` | mm.c, mm_test.c | OK | `src/kernel/nt/mm.c` | Pass-3 fix (flush wrap guard); re-read, no defect. |
| `3477776` | object.c, object.h, security.c, security.h, security_extra_test.c | SUSPECT | `src/kernel/nt/security.c:2796` | A descriptor-less object refuses every set with ACCESS_DENIED (the owner cannot set one). The code comment states the choice. Confirm it is intended. |
| `2c79f14` | proc_thread.c, proc_thread.md, proc_thread_test.c | OK | `src/kernel/nt/proc_thread.c` | Terminate and suspend status codes; error paths unchanged in shape. |
| `c20dead` | nt_services.c, system_test.c | OK | `src/kernel/nt/nt_services.c` | Refusal after argument validation; no state change. |
| `c8f479e` | Makefile, mm.c, mm.h, mm_test.c | OK | `src/kernel/nt/mm.c` | Section handles through the object table (ADR 0001); section struct is freed by the object delete path. |
| `ef03f4b` | Makefile, nt_services.c, system_test.c | SUSPECT | `src/kernel/nt/nt_services.c:49` | One global timer-resolution request. Release resets it for every caller, and the state is not locked (the commit says so). Model limit, not an unsafe access. |
| `22f2688` | io_test.c, iocp.c, proc_thread.c, proc_thread.h, proc_thread_test.c | BROKEN | `src/kernel/nt/proc_thread.c:2033` | NtSetInformationThread dereferenced the user pointer Info directly; in the kernel build (src/kernel/nt copied over arch/x86/nt by vm/kbuild.sh) that is an unchecked user access. Fixed in 6c650e3. Also SUSPECT: NtAlertThread sets nt_ethread.alerted (proc_thread.c:2023) while the wait paths test struct nt_thread.alerted (sync.c:566); nothing in production sets that field, so an alert does not satisfy an alertable wait. |
| `bd5852e` | object.c, object.h, object_test.c | OK | `src/kernel/nt/object.c:746` | Set copies outside the lock and swaps under ob_sd_lock; old copy freed after unlock; freed once in ObDereferenceObject (object.c:735). All ObCreateObject callers leave SecurityDescriptor NULL (grep: nt_obj.c, sync.c, registry.c, mm.c, security.c, object.c), so nothing borrowed is lost. |
| `9bb018d` | dispatch.c, mm.c, mm.h, mm_test.c | SUSPECT | `src/kernel/nt/dispatch.c:348` | If NtAllocateVirtualMemoryEx allocates and then copy_to_user of the base or size fails, the region stays committed and the caller never learns its address. |
| `4d4b483` | io.c, io.h, io_test.c | OK | `src/kernel/nt/io.c` | Refuses every class with the documented status; no buffer use. |
| `c354ebf` | io.c, io.h, io_test.c | OK | `src/kernel/nt/io.c` | FileFsDeviceInformation copied with length check; other classes return before any copy. Output fields all set. |
| `7e43c43` | io.c, io.h, io_test.c, khost_prelude.h, posix_host.c | SUSPECT | `src/kernel/nt/io.c:3593` | Entry names are host bytes widened one byte per WCHAR (wname[i]=(WCHAR)(unsigned char)name[i]); non-ASCII names come out wrong. Also opendir failure maps to STATUS_ACCESS_DENIED, not NOT_A_DIRECTORY. Buffer bounds are correct (cur_off+size checked before each fill). |
| `d34f367` | object.h | OK | `src/kernel/nt/object.h` | Fields appended at the end; no offsets used outside object.c. |
| `8c0b86c` | dispatch.c, mm.c, mm.h, mm_test.c | OK | `src/kernel/nt/mm.c` | NtFlushVirtualMemory; the top-page guard is the one fixed in bfd5d93. |
| `7bc43a3` | io.c, io.h, io_test.c | SUSPECT | `src/kernel/nt/io.c:621` | ntio_lock_conflicts computes start + length and l->start + l->length without an overflow check. The lock path bounds start+len (io.c:3386 caller), but a plain read or write at a file offset near 2^63 can overflow in the conflict test. Undefined arithmetic only; no unsafe access found. |
| `64476d7` | io.c, io.h, io_test.c | OK | `src/kernel/nt/io.c` | Pass-3 fix (zeroed FILE_NETWORK_OPEN_INFORMATION); re-read, no defect. |
| `760d12e` | io.c, io.h, io_test.c | OK | `src/kernel/nt/io.c` | CancelIoFile; reads pending list under the file lock; no reference left behind (skim). |
| `7e90685` | io.c, io.h, io_test.c | OK | `src/kernel/nt/io.c` | FlushBuffersFileEx; delegates to the existing flush path (skim). |
| `6b71651` | io.c, io.h, io_test.c | OK | `src/kernel/nt/io.c` | EA query and set: length and class checks before copy (skim). |
| `e0b4227` | io.c, io.h, io_test.c | OK | `src/kernel/nt/io.c` | DeviceIoControl and FsControl: unsupported codes refuse before copy (skim). |
| `bc82d49` | io.c, io.h, io_test.c, posix_host.c | BROKEN | `src/kernel/nt/io.c:2675` | FileNameLength was not checked against the buffer length, so the name was read past the caller's buffer and the length was truncated to USHORT. Fixed in 170edb9. The RootDirectory reference was never released (io.c:2693). Fixed in 520cf42. The new path was duplicated after the rename (io.c:2731), so a failed allocation left the file moved with linux_path NULL and SUCCESS. Fixed in e3bf004. Also SUSPECT: a RootDirectory name goes to kshim_ntpath_to_linux (io.c:2703), which treats it as absolute, so the rename lands under the scratch root. NtCreateFile strips the root after translation (io.c:1100-1130); rename does not. |
| `6e8f5bf` | registry.c, registry_test.c | OK | `src/kernel/nt/registry.c` | REG_OPTION_CREATE_LINK refused before any state change (skim). |
| `eb0ede7` | io.c, io.h, io_test.c | OK | `src/kernel/nt/io.c` | Query classes: each length checked before copy-out; unknown classes refused (skim). |
| `18d429d` | io_test.c, iocp.c, iocp.h | OK | `src/kernel/nt/iocp.c` | NtQueryIoCompletion looks the port up, drops the reference before the copy-out, and both branches copy with checks. |
| `a90862b` | dispatch.c, mm.c, mm.h, mm_test.c | OK | `src/kernel/nt/mm.c:1533` | ntmm_transfer: wrap of addr+size clamped; each run checked by ntmm_access_run; copy failure breaks with the partial count. The 4 KiB bounce buffer on the kernel stack is the pass-3 SUSPECT 7 class. |
| `00b5a3f` | mm.c, mm.h, mm_test.c | OK | `src/kernel/nt/mm.c` | MemoryRegionInformation; WS and filename classes refused (skim). |
| `4260f62` | security.c, security.h, security_extra_test.c | SUSPECT | `src/kernel/nt/security.c:2530` | ntsec_sr_length: the offset is added as ULONG (total = (ULONG)(offs[i]+size)) after a 64-bit comparison, so a wrapped sum can give a small total. sd_from_self_relative rejects the descriptor, so the outcome is fail-closed (pass-3 SUSPECT 6). Fix: keep total 64-bit. |
| `1f292a1` | sync_extra_test.c | OK | `src/kernel/nt/sync_extra_test.c` | Comment only. |
| `b3adda7` | sync.c, sync_extra_test.c | OK | `src/kernel/nt/sync.c` | NtReleaseKeyedEvent waits for a waiter to take the release; no reference change (skim). |
| `8f099f2` | sync.c, sync_extra_test.c | OK | `src/kernel/nt/sync.c:1012` | NtSetTimer refuses a user APC routine before any state change. |
| `c1934e9` | sync.c, sync.h, sync_extra_test.c | SUSPECT | `src/kernel/nt/sync.c:869` | Timer thread now waits on timer_cond to an absolute deadline; cancel waits for thread exit. timer_expiry_100ns still computes now - due for a negative (relative) due time, which overflows for INT64_MIN (pass-3 SUSPECT 5, unchanged). |
| `5014421` | nt_obj.c, sync.c, sync_extra_test.c | OK | `src/kernel/nt/sync.c` | Timers accepted by the wait services; the wait path does not reference the timer beyond the wait block (skim). |
| `ec4c86d` | security.c, security_extra_test.c | OK | `src/kernel/nt/security.c` | NtDuplicateToken: access and type checked; new token referenced by the handle (skim). |
| `1e8f0dd` | security.c, security.h, security_extra_test.c | SUSPECT | `src/kernel/nt/security.c:2174` | TokenStatistics returns the low 32 bits of a kernel pointer as TokenId.LowPart (partial address disclosure). The fill pass of ntsec_query_token (security.c:2201) has no bound against the measured size. Pass-3 SUSPECT 4, unchanged. |
| `c56cc62` | security.c, security.h, security_extra_test.c | OK | `src/kernel/nt/security.c` | NtOpenProcessToken(Ex) and NtOpenThreadToken(Ex): access and reference checked (skim). |
| `2a73077` | object.c, object.h, security.c, security.h, security_extra_test.c | OK | `src/kernel/nt/object.c` | Primary token and impersonation slots: the old value is dereferenced on replace (skim). |
| `2676f9e` | registry.c, registry_test.c | OK | `src/kernel/nt/registry.c` | Align64 information classes; buffer lengths checked (skim). |
| `295b44f` | Makefile, object.c, object.h, security.c, security_extra_test.c | OK | `src/kernel/nt/object.c` | Token object type and handle helper; reference taken by ObCreateTokenHandle (skim). |
| `3e98e64` | README.md | OK | `tests/kernel-host/README.md` | Docs only. |
| `f2ce759` | registry.c, registry_test.c | OK | `src/kernel/nt/registry.c` | KeyNodeInformation length checked before the copy (skim). |
| `3f63828` | registry.c, registry_test.c | OK | `src/kernel/nt/registry.c:950` | NtDeleteKey nested removal: registry_find_key returns a referenced key, released on every path; the subkey entry's reference is dropped under the parent lock. |
| `0520a92` | registry.c, registry.h, registry_test.c | SUSPECT | `src/kernel/nt/registry.c:1045` | snap_walk_key recurses once per key level with no depth bound; a deep key tree can exhaust the kernel stack in NtFlushKey (pass-3 note, unchanged). |
| `8140aec` | proc_thread.c | OK | `src/kernel/nt/proc_thread.c` | Per-call prints behind NT_DEBUG_TRACE; no logic change. |
| `b2b3e3b` | nt_obj.c | OK | `src/kernel/nt/nt_obj.c` | Per-call prints behind NT_DEBUG_TRACE; no logic change. |
| `a9fc20b` | kshim.h | OK | `src/kernel/nt/kshim.h` | NT_TRACE macros; no logic change. |
| `5c5b570` | Makefile | OK | `tests/kernel-host/Makefile` | Build wiring only. |
| `7f7e5d1` | 0036-loader-import-walk-budget.md, pe_parse.c, pe_parse.h, test_pe_parse.c | OK | `src/loader/pe_parse.c` | Import walk total bounded across descriptors (skim). |
| `ecc7f83` | README.md, object.c | SUSPECT | `src/kernel/nt/object.c:309` | The fix re-checks parent_dir under the directory lock, but the first read of obj->parent_dir (object.c:309) is unlocked. If the directory is freed by directory_delete between that read and the lock, the lock is taken on freed memory. Needs a lock on the directory pointer. |
| `f8a0644` | sync.c, sync_extra_test.c | OK | `src/kernel/nt/sync.c` | NtYieldExecution; no state (skim). |
| `b9661c2` | sync.c, sync_extra_test.c | SUSPECT | `src/kernel/nt/sync.c:2857` | keyed_event_op passes access 0 for WAIT and RELEASE, so KEYEDEVENT_WAIT and KEYEDEVENT_WAKE are not checked (pass-3 SUSPECT 3, unchanged). |
| `0258238` | sync.c, sync_extra_test.c | SUSPECT | `src/kernel/nt/sync.c:2708` | NtSignalAndWaitForSingleObject references both handles with access 0 (pass-3 SUSPECT 2, unchanged). |
| `3daa005` | binfmt_pe.c | OK | `src/kernel/fs/binfmt_pe.c:55` | Relocation directory size bounded before the walk (pass 3 verdict). |
| `8ea698f` | sync.c, sync.h, sync_extra_test.c | OK | `src/kernel/nt/sync.c` | NtQueryTimer class 0 only; length and output checked (pass 3 verdict). |
| `ca5a2e0` | mm.c | BROKEN | `arch/x86/nt/mm.c:1300` | Decommit wrap guard underflowed for a base in the top page (pass 3). Fixed in e1486e9. |
| `06c28f3` | DEVELOPMENT.md, ci.yml, run_all_checks.sh | OK | `build/ci.yml` | CI wiring only. |
| `663b877` | fuzz_pe.c, fuzz_plat.c | OK | `tests/fuzz/Makefile` | Build define only. |
| `b448667` | pe_parse.c | OK | `src/loader/pe_parse.c` | Walker RVAs at or above SizeOfImage rejected (skim). |
| `50dbf92` | Makefile, README.md, stress_common.h, stress_handle_test.c, stress_object_test.c, stress_sync_test.c | OK | `tests/kernel-host/stress_handle_test.c` | Stress programs; counts violations and exits nonzero. Not part of check. |
| `a164b43` | kshim.c | OK | `tests/kernel-host/shim/kshim.c` | KSHIM_POISON debug option only. |
| `10ddb43` | sync.c, sync_extra_test.c | BROKEN | `src/kernel/nt/sync.c:2557` | PreviousState and CurrentState probed after the timer changed (pass 3). Fixed in 0fad804. |
| `3047209` | test_pe_parse.c | OK | `tests/host/test_pe_parse.c` | Test additions only. |
| `b891048` | Makefile, sync.c, sync_extra_test.c | BROKEN | `src/kernel/nt/sync.c:2484` | Timer handle left in the process table when the copy-out faults (pass 3). Fixed in 1c8145e. |
| `f9698f5` | .gitignore, Makefile, fuzz_pe.c, fuzz_plat.c, fuzz_plat.h | OK | `tests/fuzz/fuzz_pe.c` | Deterministic fuzzer; exits 1 on a finding. |
| `41b0102` | service_inventory.py | OK | `tools` | Executable bit only. |
| `d5e0b2e` | README.md, service_inventory.py, syscall-dispatch.md | OK | `docs and tools` | Counts and docs. |
| `6db83e7` | i2_test.c, object_test.c | OK | `src/kernel/nt/object_test.c:400` | Bare returns now assert. |
| `cc1bbde` | SERVICES.md, io.c, io.h, io_test.c | BROKEN | `src/kernel/nt/io.c:2742` | FILE_BASIC_INFORMATION copied out with 4 uninitialised tail bytes (pass 3). Fixed in b5e7ecf. |
| `993c3ee` | mm.c, mm_test.c | BROKEN | `src/kernel/nt/mm.c:1232` | Protect and decommit size guard ran after rounding; a size within a page of SIZE_MAX passed (pass 3). Fixed in e1486e9. |
| `fd00d2a` | SERVICES.md, handle.c, handle.h, i2_test.c, nt_obj.c | OK | `src/kernel/nt/nt_obj.c:748` | ObjectInformation probed and copied; flags set under the table lock (pass 3 verdict). |
| `42ee3d6` | SERVICES.md, i2_test.c, nt_services.c | OK | `src/kernel/nt/nt_services.c:136` | NULL checked; 1601 epoch constant (pass 3 verdict). |
| `8e70c92` | DEVELOPMENT.md, KNOWN-ISSUES.md, README.md, SERVICES.md, ci.yml, service_inventory.py, test_service_inventory.py | OK | `tools/service_inventory.py` | Tooling; Python unit run passes. |
| `53469cc` | README.md | OK | `tools/criteria_trace.py` | Docs only. |
| `05927a9` | test_criteria_trace.py | OK | `tests/unit/test_criteria_trace.py` | Unit tests; suite passes. |
| `67df457` | criteria_trace.py | OK | `tools/criteria_trace.py` | Tooling. |
| `25bc012` | io.c, nt_io.c | OK | `src/kernel/nt/io.c:898` | Component buffers sized NAME_MAX+1 (pass 3 verdict). |
| `122f91d` | nt_service_list.h, syscall_table.def, test_syscall_table_argtypes.py | OK | `src/kernel/nt/nt_service_list.h` | ArgTypes table entries; the Python argtypes test covers them (skim). |
