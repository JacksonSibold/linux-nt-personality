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
