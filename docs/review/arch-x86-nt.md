# Review: arch/x86/nt (NT personality: dispatch, VM, I/O, processes, services)

Scope: every file in `arch/x86/nt/`: `Makefile`, `dispatch.c`, `kernel_stubs.c`, `mm.c`, `mm.h`, `nt_internal.h`, `nt_io.c`, `nt_service_list.h`, `nt_services.c`, `proc_thread.c`, `weak.c`.
Method: read-only static review. Findings were checked against the cited lines. Nothing was compiled or run. Kernel-side code (`#ifdef __KERNEL__` branches) was the focus. The userspace-shim branches were read only as far as needed to confirm the build split.

Severity: High / Medium / Low / Info.

Important: several of these files depend on headers and sources that live in `src/kernel/nt/`, and `vm/kbuild.sh` and `vm/rebuild_kdir.sh` copy one tree over the other before building (see B-3). Findings are for the copies in `arch/x86/nt/`. Where the copy in `src/kernel/nt/` differs, that is noted.

## Summary

| Severity | Count |
|---|---|
| High | 14 |
| Medium | 24 |
| Low | 24 |
| Info | 1 |

Total: 63 findings.

Most High items fall into four groups: information disclosure to user space (kernel heap or stack, kernel pointers), missing access control on processes and handles, state that diverges between the VAD table and real mappings, and kernel out-of-bounds reads in the file layer.

## Build

### B-1 [High] kernel_stubs.c does not compile
- `arch/x86/nt/kernel_stubs.c:1` contains only the token `test` followed by CRLF. `arch/x86/nt/Makefile:36` builds `kernel_stubs.o` whenever `CONFIG_NT_PERSONALITY` is set, so the build fails.
- No `kernel_stubs.c` exists in `src/kernel/nt/`, so nothing overrides it.
- Fix: delete the file and its Makefile line, or restore the intended content.

### B-2 [Medium] Makefile lists objects with no source here, and suppresses all warnings
- `arch/x86/nt/Makefile:19` sets `ccflags-y += -w`, which hides every compiler warning for the directory.
- `Makefile:20-35` list `handle.o`, `kshim.o`, `iocp.o`, `object.o`, `sync.o`, `security.o`, `nt_obj.o`, `ntstatus_kernel.o`, `debugfs_tests.o`, `stubs.o`. None of these sources exist in `arch/x86/nt/`. The directory is only buildable after the copy step described in B-3.
- `weak.c:3,5` and `dispatch.c:19-21` include `sync.h`, `object.h`, `handle.h`, which exist only under `src/kernel/nt/`.
- Fix: keep `-Wall -Werror` (or at least `-Wall`), and either make the directory self-contained or document that it is a partial overlay.

### B-3 [Medium] Two build scripts pick different copies of the same sources
- `vm/kbuild.sh:73-79` copies `src/kernel/nt/.` over `$KDIR/arch/x86/nt/`, so the `src/` copy wins for every file, including `dispatch.c` and `mm.c`.
- `vm/rebuild_kdir.sh:138-151` copies `src/kernel/nt/` but skips `dispatch.c`, `nt_io.c`, `nt_services.c`, `proc_thread.c`, `nt_internal.h`, `weak.c`, and `Makefile` (the "arch wins" list). `mm.c` is not on that list, so the `src/` copy of `mm.c` wins there too.
- The two copies differ: `arch/x86/nt/mm.c` vs `src/kernel/nt/mm.c` has about 155 differing lines. `dispatch.c` differs by one line: `arch/x86/nt/dispatch.c:304` calls `nt_set_current_thread(NULL)` in `nt_exit_personality`, `src/kernel/nt/dispatch.c` does not.
- Consequence: the reviewed `mm.c` may not be the one that runs, and the build result depends on which script was used. Findings M-1 to M-13 apply to the `arch/x86/nt/mm.c` text. Re-check them against the `src/` copy before acting.
- Fix: keep one canonical copy. Make both scripts use the same rule and delete the other copy.

### B-4 [Low] Weak fallbacks and a stray global
- `arch/x86/nt/weak.c:7-15`: `nt_handle_table_destroy` and `nt_abandon_owned_mutants` are weak no-ops. If the strong definitions are not linked, handle tables and owned mutants are never cleaned up, with no warning.
- `weak.c:17`: `char RtlUserThreadStart = 0;` defines a global `char` symbol with a name that also appears as a user-space routine in comments. Nothing in this tree uses it.
- Fix: make missing strong symbols a link error, or log at init. Remove the stray symbol.

## dispatch.c

### D-1 [High] NtQueryVirtualMemory wrapper copies uninitialised kernel heap to user space
- `arch/x86/nt/dispatch.c:258`: `kmalloc(MemoryInformationLength, GFP_KERNEL)` is used without zeroing, and the length comes straight from user space (no upper bound).
- `dispatch.c:262-264`: `NtQueryVirtualMemory` (`mm.c:1345-1459`) writes only `sizeof(MEMORY_BASIC_INFORMATION)` bytes. The wrapper then does `copy_to_user(MemoryInformation, kbuf, MemoryInformationLength)`, which copies the full user-requested length, including the uninitialised tail.
- Result: up to the requested length of kernel heap contents is returned to the caller. The same bug is in `src/kernel/nt/dispatch.c:258`.
- Fix: use `kzalloc`, cap the length (for example to `sizeof(MEMORY_BASIC_INFORMATION)` for class 0), and copy back only the bytes the service reported in `kret`.

### D-2 [Medium] Per-CPU dispatch counter is not atomic with respect to preemption
- `arch/x86/nt/dispatch.c:117-119`: `__this_cpu_add(nt_dispatch_per_ssn[ssn], 1)` is called from the syscall path, where preemption is enabled. A task can migrate between computing the per-CPU address and the add, losing or misattributing counts. The comment at 116 calls the counts "exact", which they are not.
- Fix: use `this_cpu_add` (or disable preemption around the `__this_cpu_` operations).

### D-3 [Medium] User-triggerable logging at info level for every missing service
- `arch/x86/nt/dispatch.c:126-129` calls `nt_report_missing_service` (`dispatch.c:309-312`, `pr_info`) on every call to an unimplemented SSN. Any process can call this in a loop.
- Fix: `pr_info_ratelimited`, or count only and report through debugfs.

### D-4 [Low] Debug logging left in the hot path
- `arch/x86/nt/dispatch.c:113-115` prints for six specific SSNs on every call.
- Fix: remove, or gate behind a debug option.

### D-5 [Low] Thread object leaked or lost when there is no ethread
- `arch/x86/nt/dispatch.c:275-293` (`nt_enter_personality`): if `nt_current_ethread()` returns NULL, the newly created `t` is made current (line 289) but never stored, so it is leaked on exit. A failed `nt_thread_create` is ignored silently.
- Fix: store or free `t` on that path, and log the failure.

### D-6 [Low] debugfs_lookup reference not released
- `arch/x86/nt/dispatch.c:379-385`: `debugfs_lookup` returns a referenced dentry that is never `dput`.
- Fix: `dput` after use, or cache the dentry.

### D-7 [Low] noinstr function calls instrumented code
- `arch/x86/nt/dispatch.c:107`: `do_nt_syscall` is `noinstr` but calls `pr_info` (line 114, 127) and `nt_thread_check_suspend` (line 145). objtool will flag these.
- Fix: move the logging out of the noinstr region or call through `instrumentation_begin/end` correctly.

### D-8 [Low] Output parameters written back even when the service fails
- `arch/x86/nt/dispatch.c:178-181`, `205-208`, `234-239`: BaseAddress and RegionSize are copied back after the NT call returns an error. NT semantics only define these on success. Callers may act on stale values.
- Fix: write back only on success, or document the behavior.

Checked and found correct in dispatch.c: argument fetch from the user stack (`dispatch.c:89-105`) checks `access_ok` and uses `__get_user`; the syscall number is bounded with `ssn >= NT_NR_SERVICES` and `array_index_nospec` (`dispatch.c:120-124`); `argc` values in `nt_service_list.h` are at most 11, within `NT_MAX_ARGS` (12); the largest SSN is `0x1db`, below `NT_NR_SERVICES` (476).

## mm.c (virtual memory manager)

### M-1 [High] One global VAD table for every process; ProcessHandle is ignored
- `arch/x86/nt/mm.c:167`: `struct nt_mm g_nt_mm;` is a single address space. The comment at 165 calls it "singleton for tests".
- Every service ignores its process handle: `(void)ProcessHandle;` at `mm.c:836` (NtAllocateVirtualMemory), `1133` (NtFreeVirtualMemory), `1271` (NtProtectVirtualMemory), `1353` (NtQueryVirtualMemory), `1553` (NtMapViewOfSection), `1610` (NtUnmapViewOfSection).
- Operations therefore act on the caller's address space while being recorded in the shared table. Cross-process operations are not possible, and the table of one process is visible to all others.
- `ntmm_reset` (`mm.c:774`) and `ntmm_destroy` are never called from exec or process exit in this tree. The only caller of `ntmm_reset` is `src/kernel/nt/debugfs_tests.c`.
- Fix: put the `nt_mm` in the per-process `nt_eprocess`, resolve `ProcessHandle` through the object manager, and reset on exec.

### M-2 [High] Section handle is a kernel pointer; handles are dereferenced without a lookup
- `arch/x86/nt/mm.c:1540`: `*SectionHandle = (HANDLE)sec;` hands user space the address of a `kzalloc` object. This is a kernel address disclosure (defeats KASLR) and the handle is forgeable.
- `mm.c:1549`: `NtMapViewOfSection` does `struct nt_section *sec = (struct nt_section *)SectionHandle;` and then dereferences `sec->attributes` (1550-1568). Any integer passed as the handle is treated as a kernel pointer. Only a NULL check is applied.
- Reachability: the service table lists `NtCreateSection` and `NtMapViewOfSection` (`nt_service_list.h:31`, `34`). The `nt_svc_*` wrappers that call these functions are not in this directory, so their handle handling was not reviewed.
- Fix: allocate section handles through the object manager and resolve them with a type-checked lookup (`ObReferenceObjectByHandle`) in `NtMapViewOfSection`.

### M-3 [High] Section view offset bounds check wraps
- `arch/x86/nt/mm.c:1566`: `actual_size = sec->size - offset;` is computed with no check that `offset <= sec->size`. If `offset > sec->size`, the subtraction wraps to a huge value.
- `mm.c:1568`: `if (offset + actual_size > sec->size)` then wraps back below `sec->size` and passes.
- Result: a view can be mapped that extends past the section, with a size chosen by the caller.
- Fix: `if (offset > sec->size || actual_size > sec->size - offset) return STATUS_INVALID_VIEW_SIZE;`.

### M-4 [High] Commit-failure rollback is empty; VAD says committed, memory is not
- `arch/x86/nt/mm.c:1063-1076`: pieces of the VAD are switched to `MEM_COMMIT` (`mid->state = MEM_COMMIT;`, line 1074) before the real mapping is made.
- `mm.c:1081`: `kshim_mmap_commit` fails. The rollback block at `mm.c:1082-1094` loops over the range and contains only a comment and no state change. It then returns `STATUS_NO_MEMORY` (line 1095).
- Result: the VAD keeps reporting `MEM_COMMIT` for pages that are not mapped. Later accesses fault, and `NtQueryVirtualMemory` lies about the state.
- Fix: record the previous state of each piece and restore it in the rollback, or change the VAD only after the mapping succeeds.

### M-5 [Medium] VAD protection changed before the page protection; failure leaves them apart
- `arch/x86/nt/mm.c:1018-1019` (commit of an existing range) and `mm.c:1325-1326` (NtProtectVirtualMemory): the VAD is updated (`mid->protect = Protect` at `mm.c:1010`, `mid->protect = NewProtect` at `mm.c:1306`) before `kshim_mprotect`. On failure the function returns `STATUS_INVALID_PAGE_PROTECTION` and the VAD is not restored.
- `mm.c:1257`: `kshim_mmap_commit((void *)base, size, PROT_NONE)` in MEM_DECOMMIT is called and its result ignored, while the VAD was already changed to `MEM_RESERVE`.
- Fix: change the page protection first and update the VAD only on success, or roll the VAD back on failure. Check the return value at 1257.

### M-6 [Medium] Unsynchronised lazy initialisation of the global table
- `arch/x86/nt/mm.c:840`, `1139`, `1284`, `1356`, `1569`, `1611`, `1665`: `if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }` with no lock. Two first calls on different CPUs both run `ntmm_init`, which resets the VAD tree under a concurrent user.
- Fix: initialise at module load (`core_initcall`), or use a lock with a double check.

### M-7 [Medium] Emergency-level logging on every protect call; dereference before NULL check
- `arch/x86/nt/mm.c:72`: `printk(KERN_EMERG "kshim_mprotect: ...")` on every call to `kshim_mprotect`.
- `mm.c:1272`: `printk(KERN_EMERG ..., *BaseAddress, (unsigned long)*RegionSize, ...)` runs before the NULL check at `mm.c:1282`. A NULL `BaseAddress` or `RegionSize` faults here, and every call is logged at emergency level, which a user can drive from a loop.
- Fix: remove or demote both logs to `pr_debug`, and move the argument checks to the top of `NtProtectVirtualMemory`.

### M-8 [Medium] Calls an unexported kernel function found through kallsyms
- `arch/x86/nt/mm.c:52-58`: `kallsyms_lookup_name("do_mprotect_pkey")` and an indirect call through the result. The function is internal and not part of any stable interface. If it returns non-zero, the code falls through (line 61-62) and repeats the change through `mprotect_fixup` on the same range.
- Fix: use the supported `vm_mprotect` or `mprotect_fixup` path under the mmap lock only. Remove the kallsyms lookup.

### M-9 [Medium] PAGE_GUARD state is a single global; earlier guard regions are forgotten
- `arch/x86/nt/mm.c:940-944`, `972-976`, `1022-1026`, `1104-1108`, `1329-1333`: each allocation or protect that sets `PAGE_GUARD` overwrites the one `kshim_guard` record (`guard_base`, `guard_size`, `base_protect`). Earlier guard regions are no longer recognised by the fault path (`NtMmHandleGuardFault`, `mm.c:1460-1490`).
- The VAD already has a per-region `guard` flag (for example `mm.c:1016`), so the global is redundant.
- Fix: drop the global and use the VAD flag in the fault path.

### M-10 [Medium] Reservation leaked when the commit mapping fails
- `arch/x86/nt/mm.c:963-969` (commit-only path): `kshim_mmap_reserve(...) == NULL || kshim_mmap_commit(...) != 0` removes the VAD and returns `STATUS_NO_MEMORY`, but when the reserve succeeded and the commit failed, the `PROT_NONE` reservation is never unmapped. The reserve path at `mm.c:931-937` does unmap in the same situation.
- Fix: `kshim_munmap` the reservation before returning.

### M-11 [Medium] VAD published before the mapping exists; lock released in between
- `arch/x86/nt/mm.c:919-922` (reserve) and `961-969` (commit-only): the VAD is inserted under `ntmm_lock`, the lock is released, and only then is the mapping created. Between those steps another thread can run `NtProtectVirtualMemory`, `NtQueryVirtualMemory` or `NtFreeVirtualMemory` on a region that is not yet mapped.
- Fix: hold the lock across the mapping (taking `mmap_write_lock` inside is acceptable), or mark the VAD as pending and reject operations on pending VADs.

### M-12 [Low] Size arithmetic can overflow; NtProtect end address unchecked
- `arch/x86/nt/mm.c:881`, `891`: `ntmm_round_up(orig_size + delta, NTMM_PAGE_SIZE)` with a user-chosen `orig_size` near `SIZE_MAX` wraps to a small or zero size. A zero-length VAD is then inserted and removed again on the failed mapping. There is no upper bound check (for example against `TASK_SIZE`).
- `mm.c:1289`: `ULONG_PTR end = base + size;` is not checked for wrap in `NtProtectVirtualMemory`.
- Fix: reject sizes above the user address range before the arithmetic, and check `end > base`.

### M-13 [Low] NtUnmapViewOfSection unmaps any allocation, not only section views
- `arch/x86/nt/mm.c:1615-1625`: the only check is `vad->alloc_base != base`. It does not verify `vad->type` (MEM_MAPPED or MEM_IMAGE) or `vad->section`. A private committed allocation can be unmapped through this call, which should return `STATUS_NOT_MAPPED_VIEW`.
- Fix: check the VAD type and reject non-section VADs.

### M-14 [Low] Kernel stub for file-backed sections drops the file, and the handle is cast to a path
- `arch/x86/nt/mm.c:144`: `kshim_section_create_file` is an unconditional stub in the kernel build. It ignores `path` and never opens or maps the file. The section therefore has no backing and the mapping is anonymous.
- `mm.c:1517` and `1527`: `(const char *)FileHandle` passes a handle value as a path pointer. In the shim it is dereferenced; in the kernel it is ignored.
- Fix: resolve `FileHandle` through the NT handle table to a file object and implement kernel-side backing (or fail with `STATUS_NOT_SUPPORTED` until it exists).

Checked and found correct in mm.c: `NtAllocateVirtualMemory` argument validation (`mm.c:823-840`) and the alignment and rounding rules for reserve and commit; `NtFreeVirtualMemory` MEM_RELEASE requires the allocation base (`mm.c:1148`); `NtProtectVirtualMemory` rejects invalid protections (`mm.c:1283`).

## proc_thread.c (processes and threads)

### P-1 [High] Object open and handle creation have no access checks
- `arch/x86/nt/proc_thread.c:1264`: `ObCreateHandle(cur_proc, thr->obj, 0x1FFFFF, 0, &h)`. The caller's `DesiredAccess` is ignored (`(void)DesiredAccess;` at line 1208), so every new thread handle carries full access.
- `proc_thread.c:1589-1592` (NtOpenProcess) and `1611-1614` (NtOpenThread): `ObFindProcessByPid(pid)` / `ObFindThreadByTid(tid)` locate any process or thread by ID, and the handle is created with `DesiredAccess ? DesiredAccess : 0x1FFFFF`. The requested rights are granted verbatim, with no owner or security check, and a zero mask means full access.
- Result: any NT process can open and terminate any other NT process by ID (`NtTerminateProcess`, `proc_thread.c:1545-1572`).
- Fix: check the caller against the target's owner or a security descriptor, map generic rights, and grant only what was requested.

### P-2 [High] TEB is kernel heap memory; it is used as the GS base and returned to user space
- `arch/x86/nt/proc_thread.c:1007-1010`: the TEB is `kzalloc(2 * PAGE_SIZE)`, and `thr->teb_base` is set to that kernel address.
- `proc_thread.c:1316`: `nt_write_gsbase(child, thr->teb_base)` installs that kernel address as the child's GS base. Every GS-relative access (TEB, TLS) in user space then faults. `binfmt_pe.c:590` allocates the initial thread's TEB with `vm_mmap`, so the two paths disagree.
- `proc_thread.c:1817`: `NtQueryInformationThread` returns `target->teb_base` in `TebBaseAddress` to user space. That leaks a kernel heap address.
- Fix: allocate the TEB in user space with `vm_mmap` and copy the initial fields to it, as `binfmt_pe.c` does. Return the user address.

### P-3 [High] Thread is started before the output handle is validated; on failure it keeps running
- `arch/x86/nt/proc_thread.c:1331`: `wake_up_new_task(child)` runs first. The handle is registered at 1344, and only then does `copy_to_user(ThreadHandle, &h, sizeof(h))` run at 1348.
- If that copy fails (for example an unmapped user pointer that passes `access_ok` at line 1213), the function closes the handle and returns `STATUS_ACCESS_VIOLATION` at 1352. The comment at 1351 says the child "will exit via termination path", but nothing terminates it.
- Result: a thread keeps running with no handle and no owner. Repeating the call exhausts thread resources.
- Fix: probe `ThreadHandle` for writability before creating the thread (for example `put_user(0, ThreadHandle)`), or terminate the child on this path.

### P-4 [Medium] Uninitialised structures copied to user space (kernel stack)
- `arch/x86/nt/proc_thread.c:1649-1656`: `PROCESS_BASIC_INFORMATION pbi;` has no initialiser. The `src/kernel/nt/proc_thread.h:27-34` layout has 4 bytes of padding after `ExitStatus` and 4 bytes after `BasePriority`. Those 8 bytes are copied to user space at line 1656.
- `proc_thread.c:1814-1824`: `THREAD_BASIC_INFORMATION tbi;` (`proc_thread.h:36-43`) has 4 bytes of padding after `ExitStatus`, copied at line 1824.
- Fix: `= {0}` on both.

### P-5 [Medium] Synthetic trampoline page is RWX and never unmapped
- `arch/x86/nt/proc_thread.c:1076`: `vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC, ...)`. The comment at 1058-1066 acknowledges the W^X violation.
- `proc_thread.c:1049-1058` (`nt_free_teb_stack_kernel`) frees the TEB and the stack but not the trampoline page, which is never recorded anywhere. Each thread creation leaks one RWX user page.
- Fix: map `PROT_READ|PROT_EXEC` after writing the stub, track the address in `nt_ethread`, and `vm_munmap` it on thread exit.

### P-6 [Medium] Thread entry stack misaligned by 8 bytes
- `arch/x86/nt/proc_thread.c:1154`: `sp &= ~0xF; sp -= 8;`. The trampoline is entered with `rsp` congruent to 8 (mod 16).
- The trampoline (`proc_thread.c:1084-1100`) performs `call *%rax` with no adjustment, which pushes another 8 bytes. `StartRoutine` is therefore entered with `rsp` congruent to 0 (mod 16). The ABI requires `rsp + 8` to be 16-byte aligned at function entry.
- Fix: enter the trampoline with `rsp` congruent to 0 (mod 16) (remove `sp -= 8`), or adjust the stub.

### P-7 [Medium] pid_task(find_vpid(...)) used without RCU
- `arch/x86/nt/proc_thread.c:1415`, `1458`, `1540`: `pid_task(find_vpid(thr->tid), PIDTYPE_PID)` is called under `thr->lock`, not under `rcu_read_lock()`. The file contains no RCU read-side section at all (`grep rcu` finds none).
- Consequence: the `task_struct` can be freed while `set_tsk_thread_flag` or `wake_up_process` runs on it.
- Fix: take `rcu_read_lock()` around the lookup and `get_task_struct` before use, or keep a counted reference on `nt_ethread`.

### P-8 [Medium] Thread creation ignores the target process
- `arch/x86/nt/proc_thread.c:1215-1222`: `ProcessHandle` is resolved to `target`, which is then used only for `ObCreateThreadObject` and the TEB/stack helper arguments.
- `proc_thread.c:1269-1272`: the new task is created with `CLONE_VM` from `current`, so the thread always runs in the caller's address space, and `StartRoutine` is executed there regardless of the requested process.
- Fix: return `STATUS_NOT_SUPPORTED` when `target != cur_proc` until cross-process creation exists.

### P-9 [Low] Output copied to user space while holding a spinlock
- `arch/x86/nt/proc_thread.c:1410` and `1451`: `copy_to_user` runs inside `spin_lock(&thr->lock)`. A page fault in the user buffer fails the copy (atomic context) even when the page could be faulted in.
- Fix: copy the value under the lock, release it, then copy to user.

### P-10 [Low] NULL process not checked
- `arch/x86/nt/proc_thread.c:1472` (NtSuspendProcess) and `1487` (NtResumeProcess): `proc = nt_current_eprocess();` is used without the NULL check that the thread services have (`1390`, `1506`).
- Fix: add the check.

### P-11 [Low] Handle leaked when the output copy fails
- `arch/x86/nt/proc_thread.c:1592-1594` (NtOpenProcess) and `1614-1616` (NtOpenThread): the handle is created, and if `copy_to_user` fails the function returns without `ObCloseHandle`.
- Fix: close the handle on the error path.

### P-12 [Low] Dead ntdll probe that reads user memory on every thread creation
- `arch/x86/nt/proc_thread.c:1114-1141`: `nt_get_ntdll_RtlUserThreadStart` probes fixed user addresses and always returns 0. The branch that uses its result (`proc_thread.c:1162-1171`) is therefore dead.
- Fix: remove the probe, or finish it, and document the synthetic trampoline as the only path.

### P-13 [Low] Pointer dereferenced before NULL check
- `arch/x86/nt/proc_thread.c:1151`: `READ_ONCE(thr->stack_base)` runs before the check `if (!thr || !thr->stack_base)` at about line 1169. `proc_thread.c:1159-1160` also reads `thr->tid` in a `pr_debug` before any check.
- Fix: check `thr` first.

### P-14 [Low] Noisy info-level logging; mislabelled messages
- `arch/x86/nt/proc_thread.c:1234` and the `W1_INSTR` lines from about 1307 to 1330 print at info level for every thread creation.
- `proc_thread.c:1436` (inside `nt_svc_NtResumeThread`) prints "NtSuspendThread" in its message. Copy-paste error that will mislead debugging.
- Fix: `pr_debug`, and correct the label.

### P-15 [Low] Long image path silently truncated
- `arch/x86/nt/proc_thread.c:1761`: `us.Length = (USHORT)(wlen * sizeof(WCHAR));` truncates for paths of 16384 or more characters, while `MaximumLength` is computed from `(wlen+1)` and also truncated.
- Fix: reject paths that do not fit in `USHORT`.

Checked and found correct in proc_thread.c: `NtQueryInformationProcess` class 0 requires an exact length (`proc_thread.c:1646-1656`), and the query classes follow one copy-in and copy-out pattern.

## nt_io.c (files, handles, paths)

### IO-1 [High] FileNameLength not checked against the buffer; kernel heap read leaks into a file name
- `arch/x86/nt/nt_io.c:2272`: `uname.Length = (USHORT)ri->FileNameLength;` where `ri` points into `kbuf`, a kernel buffer of `Length` bytes allocated at `nt_io.c:2318`. `ntio_set_rename` (`nt_io.c:2259-2270`) checks only `len >= sizeof(...) - sizeof(WCHAR)`.
- `kshim_ntpath_to_linux` (`nt_io.c:172-190`) then reads `us->Length / 2` wide characters from `us->Buffer`. A `FileNameLength` larger than `len - 20` reads up to about 64 KiB past the allocation.
- The bytes read are converted into the rename target name (`nt_io.c:2280-2286`), so kernel heap contents become visible through the filesystem.
- Fix: require `FileNameLength <= len - offsetof(FILE_RENAME_INFORMATION, FileName)` and that the length is even, before building `uname`.

### IO-2 [High] NtReadFile never reads; it always returns success
- `arch/x86/nt/nt_io.c:1894-1904`: after the `delete_pending` check, an unconditional block runs `ntio_file_put(file); return STATUS_SUCCESS;`. The following call to `ntio_do_read` (line 1904) and the IOSB update are unreachable.
- Result: every `NtReadFile` returns `STATUS_SUCCESS` with the buffer untouched and no IO_STATUS_BLOCK written. The caller's IOSB is left with stale values.
- Fix: delete the early block. Make the delete check return only in its own branch.

### IO-3 [High] Handle table is global; handles are shared across all processes
- `arch/x86/nt/nt_io.c:348`: `static struct ntio_handle_entry ntio_handles[NTIO_MAX_HANDLES];` is one table for the whole system. `ntio_handle_lookup` (`nt_io.c:402-430`), `ntio_handle_close` (`nt_io.c:488-505`) and `NtClose` have no owner check.
- Any NT process can read, write, query, close or duplicate another process's file handles if it knows the number.
- Fix: put the table in `nt_eprocess` and check the owner on every lookup.

### IO-4 [High] No access rights enforced on read, write, query, set, flush, or disposition
- `arch/x86/nt/nt_io.c:1890` (NtReadFile), `1962` (NtWriteFile), `2150` (NtQueryInformationFile), `2327` (NtSetInformationFile), `2416` (NtFlushBuffersFile): `ntio_handle_lookup(FileHandle, 0)` passes `required = 0`, so the granted access on the handle is never checked.
- A handle opened with only `SYNCHRONIZE` can be written. `ntio_set_disposition` (`nt_io.c:2247-2257`) sets `delete_pending` without any `DELETE` check.
- Fix: pass the right needed by each operation (`FILE_READ_DATA`, `FILE_WRITE_DATA | FILE_APPEND_DATA`, `FILE_READ_ATTRIBUTES`, `FILE_WRITE_ATTRIBUTES`, `DELETE`) to `ntio_handle_lookup`.

### IO-5 [High] FILE_SUPERSEDE deletes the existing file before the open can succeed
- `arch/x86/nt/nt_io.c:1106-1108` (kernel branch): `kshim_vfs_unlink(path)` runs in `ntio_apply_disposition` for `FILE_SUPERSEDE`. That function is called at `nt_io.c:1303`, before the share check (`1489`) and before the open succeeds.
- If the share check or the open then fails, the original file has already been deleted.
- Fix: open the existing file first and replace it only after success, or use a rename of the old inode into a temporary name that is removed on success.

### IO-6 [Medium] Handle slot consumed before the output copy; table exhaustion
- `arch/x86/nt/nt_io.c:375-395` (`ntio_handle_create`): the slot is marked `used` (line 380) and the object reference is kept. If `copy_to_user(out, &h, ...)` (line 389) fails, the function returns `STATUS_ACCESS_VIOLATION` and never frees the slot.
- The table is global and has 1024 entries (`NTIO_MAX_HANDLES`), with 64 reserved. A process that passes bad output pointers can exhaust file handles for every process.
- Fix: validate or probe `out` before reserving the slot, or release the slot on the copy-failure path.

### IO-7 [Medium] Delete-on-close runs on every handle close, not on the last reference
- `arch/x86/nt/nt_io.c:2474-2476` (kernel `nt_svc_NtClose`): `if (file->delete_on_close || file->delete_pending) kshim_vfs_unlink(file->linux_path);` runs on each `NtClose`. A duplicated handle still refers to the file after the first close has removed it.
- The userspace branch at `nt_io.c:2506` does the same.
- Fix: perform the unlink in `ntio_file_release` when the reference count reaches zero.

### IO-8 [Medium] Reference leak when renaming relative to a root directory
- `arch/x86/nt/nt_io.c:2276`: `ntio_handle_lookup(ri->RootDirectory, 0)` takes a reference (`ntio_file_get` on the lookup path, `nt_io.c:420`). The rename branch (`2276-2287`) never calls `ntio_file_put(root)`.
- Fix: `ntio_file_put(root)` on every return path.

### IO-9 [Medium] Kernel heap copied to user in NtQueryInformationFile
- `arch/x86/nt/nt_io.c:2144`: `kbuf = kmalloc(Length, GFP_KERNEL);` is not zeroed. The per-class helpers (`ntio_query_basic` 2001-2023, `ntio_query_standard` 2025-2045, `ntio_query_all` 2089-2123) fill fields but not the padding and not every field of the structure.
- `nt_io.c:2180-2182`: `to_copy = retlen` bytes are copied to user space, so the uninitialised bytes are returned.
- Fix: `kzalloc`, or zero the buffer before the helper runs.

### IO-10 [Medium] Kernel locks are never initialised
- `arch/x86/nt/nt_io.c:349` (`ntio_handle_lock`) and `563` (`ntio_share_mutex`) are `spinlock_t` objects. `spin_lock_init` is called only from `ntio_handle_table_init` (`nt_io.c:362`) and `ntio_share_init` (`nt_io.c:574`), which run only from the userspace `constructor` at `nt_io.c:2519-2523` (`#ifndef __KERNEL__`).
- In the kernel build the locks are zero-initialised statics. That works on common x86 configurations but is not a valid initialisation, and lock debugging (`CONFIG_DEBUG_SPINLOCK`, lockdep) reports them as bad.
- Fix: initialise both locks from a kernel `core_initcall` (or `__init`) function.

### IO-11 [Medium] Path mapping is a string prefix; symlinks escape the root; default root is the whole host
- `arch/x86/nt/nt_io.c:74`: `char g_kshim_root[PATH_MAX] = "/";`. With this default, every NT path maps into the host filesystem root. Nothing in the personality limits a process to a subtree.
- `kshim_ntpath_to_linux` (`nt_io.c:172-323`) rejects `..` components (correctly) but then builds an ordinary path string under `g_kshim_root`. The later open (`kshim_filp_open`) follows symlinks and does not use `RESOLVE_BENEATH` or `O_NOFOLLOW`. A symlink inside the root can therefore lead outside it.
- Fix: default `g_kshim_root` to a dedicated directory, and open with `openat2` using `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`.

### IO-12 [Medium] Case-insensitive resolution scans whole directories under a spinlock on every open
- `arch/x86/nt/nt_io.c:857-921` (`ntio_resolve_ci`, kernel branch) runs on every open (`nt_io.c:1029`). For each component that does not exist, `ntio_casefold_lookup` (`nt_io.c:838-850`) walks all children of the parent with `hlist_for_each_entry` while holding `parent->d_lock`.
- For large directories this holds a spinlock for an unbounded time (soft lockup).
- Also at `nt_io.c:861-865`: `component[NAME_MAX]` is written by `while (*p && *p != '/') component[i++] = *p++;` with no bound. It is safe only because the caller already rejected components longer than `NAME_MAX` (`kshim_ntpath_to_linux`, `nt_io.c:265`). That is an invariant that is not enforced locally.
- Fix: use the filesystem's lookup (`lookup_one_len` with a case-insensitive fallback) instead of a locked scan, and bound the copy loop.

### IO-13 [Low] ReplaceIfExists ignored
- `arch/x86/nt/nt_io.c:2259-2300` (`ntio_set_rename`): the `ReplaceIfExists` field is never read. A rename onto an existing file always replaces it, where NT requires `STATUS_OBJECT_NAME_COLLISION` when the flag is FALSE.
- Fix: check the target first and honour the flag.

### IO-14 [Low] Handle created before the IOSB copy; failure leaves the handle live
- `arch/x86/nt/nt_io.c:1518` (`ntio_handle_create`) writes the handle to user space. If the IOSB copy at `nt_io.c:1521-1525` then fails, the function returns `STATUS_ACCESS_VIOLATION` while the handle is live and already returned.
- Fix: copy the IOSB before creating the handle, or close the handle on this path.

### IO-15 [Low] Non-ASCII NT names collapse to '?'
- `arch/x86/nt/nt_io.c:189`: `if (wc >= 0x80) wpath[i] = '?';`. Distinct names such as `é.txt` and `?.txt` map to the same Linux path.
- Fix: reject non-ASCII names, or encode them (for example UTF-8) consistently.

### IO-16 [Low] Large stack buffers in the file path
- `arch/x86/nt/nt_io.c:1234` (`char path[PATH_MAX]` in `ntio_open_file`), `975` (`char rel[PATH_MAX]` in `ntio_resolve_path`), `2263` and `2279` (`newpath` and `rel` in `ntio_set_rename`). Each is 4 KiB on the kernel stack, and some are nested.
- Fix: use `kmalloc` for the path buffers.

### IO-17 [Low] Read loop advances the file position before a failed copy
- `arch/x86/nt/nt_io.c:1625-1645` (`ntio_do_read`, kernel branch): `kshim_kernel_read` advances `pos` (through `&pos`) before `copy_to_user` runs. If the copy fails after some data was read, the data is lost and the position stays advanced.
- Fix: copy first and advance the position only on success, or restore the position on failure.

### IO-18 [Low] EndOfFile not range-checked
- `arch/x86/nt/nt_io.c:2234-2245` (`ntio_set_eof`): negative `EndOfFile` makes the truncate fail, which is then reported as `STATUS_DISK_FULL`. A huge value grows the file as a sparse file with no limit.
- Fix: reject negative values with `STATUS_INVALID_PARAMETER` and cap the size.

Checked and found correct in nt_io.c: `kshim_ntpath_to_linux` rejects `..` components (`nt_io.c:~250-265`), `\\?\` prefixes, UNC forms, and double separators; `ntio_resolve_path` copies the object name and attributes from user space with `copy_from_user` (`nt_io.c:923-960`) and does not dereference user pointers directly; `ntio_do_write` and `ntio_do_read` check `access_ok` on the user buffer.

## nt_services.c (synchronisation services)

### N-1 [Medium] Object state changed before the output pointer is validated; failure reports an error after success
- `arch/x86/nt/nt_services.c:170-174` (NtSetEvent), `189-193` (NtResetEvent), `208-212` (NtPulseEvent), `227-231` (NtReleaseMutant), `247-251` (NtReleaseSemaphore): the event, mutant or semaphore is changed first (`nt_set_event`, `nt_release_semaphore` and so on), then `access_ok` and `copy_to_user` run on the previous-state pointer.
- If the pointer is bad, the function returns `STATUS_ACCESS_VIOLATION` after the release has happened. A caller that retries a failed `ReleaseSemaphore` will release twice.
- Fix: validate and probe the output pointer before changing state.

### N-2 [Low] Two padding bytes of NtQueryMutant output are uninitialised
- `arch/x86/nt/nt_services.c:325`: `struct { ULONG CurrentCount; BOOLEAN Owned; BOOLEAN Abandoned; UCHAR R[2]; ULONG OwnerTid; ULONG Reserved[2]; } kinfo;`. Lines 326-330 set every field except `kinfo.R[0]` and `kinfo.R[1]`, and the whole structure is copied to user space at line 331.
- Fix: `memset(&kinfo, 0, sizeof(kinfo))`.

### N-3 [Low] Reported length and bytes written differ for NtQueryMutant
- `arch/x86/nt/nt_services.c:311` and `331`: the function reports 24 bytes (`needed`) but copies `sizeof(kinfo)` = 20 bytes.
- Fix: make the copy size match the reported size, and zero the tail.

### N-4 [Info] Userspace branch writes user pointers directly
- `arch/x86/nt/nt_services.c:69`, `81`, `89-92`, `105-106`, `126-129` (`#ifndef __KERNEL__` branch): output pointers are written directly, without `access_ok` or copy helpers. This is the userspace shim only and is not built into the kernel. The file header at lines 6-8 says arguments "must not be dereferenced directly", which that branch does.

Checked and found correct in nt_services.c: kernel-side handle lookups use `ObReferenceObjectByHandle` and release every reference on each return path; `NtQueryEvent` and `NtQuerySemaphore` check output lengths against the expected structure before writing.

## nt_service_list.h

### (no finding)
- Every SSN is below `NT_NR_SERVICES` (476): the largest is `0x01db`. Every `argc` is at most 11, within `NT_MAX_ARGS` (12). The table is consistent with `dispatch.c:83-85`.

## Not verified / out of scope
- `ObReferenceObjectByHandle`, `ObFindProcessByHeader`, `ObFindThreadByHeader`, `ObFindThreadByTid`, `ObFindProcessByPid`, `ObCreateHandle`, `ObCreateThreadObject`: these live in `src/kernel/nt/object.c`, which was not read. Whether `Find*` returns a counted reference matters for the use-after-deref question at `proc_thread.c:1397-1398`, `1439-1440`, `1524-1525`, `1801-1802` (the thread pointer is used after `ObDereferenceObject(hdr)`). Not confirmed either way.
- The `nt_svc_*` wrappers for the VM and section services (M-2, M-3) are not in this directory.
- `arch/x86/nt/mm.h` (260 lines) was not read in full.
- `nt_io.c` lines 600-690 (share registry helpers), 694-737 (access-to-flags mapping), 1764-1855 (IOCP and console), and 2390-2420 (flush) were read only in part.
- The `src/kernel/nt/` copies of `mm.c` and `dispatch.c` were not reviewed in full (only compared, see B-3).
- No compile, no sanitizer, no runtime test. Lock-order and race claims (M-6, M-11, P-7, IO-10) are from reading only.
