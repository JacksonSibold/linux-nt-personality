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

## SUSPECT: for human review

- **b3dcef5**: the IOSB probe is correct, but the commit also adds the IO-17 in-branch rewind to `ntio_do_read`, which its message does not mention. The behaviour is correct; the history is misleading.
- **d1f866c**: `src/kernel/nt/ntstatus_table.inc` now maps `0xC0000228` and `0xC0000233` where `DLL_NOT_FOUND` and `ENTRYPOINT_NOT_FOUND` belong. Their real values are `0xC0000135` and `0xC0000139` (`src/ntdll/ntstatus.h`). The values come from `src/ntdll/ntstatus_to_dos.c`, whose comments say the header names are unverified. The table owner must decide, so this was not changed.
- **d921888** and **40f6db0**: `NtMapViewOfSection` rounds `*ViewSize` up before the bounds test. A size above `SIZE_MAX - 4095` wraps to 0 and passes. The natural fix is to compare the raw size with `sec->size - offset` before rounding, but the error code for that case is an owner decision, so it was not applied.
- **38b1b0a**: the suspend and resume previous-count probe is a snapshot. If the copy-out faults after the count has changed, the call returns `STATUS_ACCESS_VIOLATION` with the suspend already applied.

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
| `b3dcef5` | nt_io.c | SUSPECT | `arch/x86/nt/nt_io.c:1525` | IOSB probe is correct, but the commit also adds the IO-17 in-branch rewind to ntio_do_read, which the message does not mention |
| `d730c42` | weak.c | OK | `arch/x86/nt/weak.c:15` | stray RtlUserThreadStart global removed; only comments refer to it now |
| `38b1b0a` | proc_thread.c | SUSPECT | `arch/x86/nt/proc_thread.c:1356` | probe, then count change, then copy-out after unlock: a fault after the count changed returns STATUS_ACCESS_VIOLATION with the suspend already applied (nt_put_prev_count) |
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
| `d1f866c` | gen_ntstatus_table.py, ntstatus_table.inc | SUSPECT | `src/kernel/nt/gen_ntstatus_table.py:68` | parser change is sound, but ntstatus_table.inc now maps 0xC0000228 and 0xC0000233 where DLL_NOT_FOUND (0xC0000135) and ENTRYPOINT_NOT_FOUND (0xC0000139) belong (src/ntdll/ntstatus.h); not in the message |
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
| `40f6db0` | mm.c | SUSPECT | `src/kernel/nt/mm.c:1524` | src port of d921888; same wrap in the ntmm_round_up(*ViewSize) step |
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
| `d921888` | mm.c | SUSPECT | `arch/x86/nt/mm.c:1566` | offset and size checked against the section, but the round-up of a *ViewSize above SIZE_MAX-4095 wraps to 0, which then passes the size check; check the raw size before rounding |
| `79d1de2` | nt_io.c | OK | `arch/x86/nt/nt_io.c:2266` | FileNameLength checked for even length and buffer bounds before the rename target is built |
| `6da43d2` | nt_io.c | OK | `arch/x86/nt/nt_io.c:1898` | early return removed so NtReadFile performs the read and fills the IOSB |

