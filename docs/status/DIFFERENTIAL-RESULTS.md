# Differential probes inside the NT personality kernel: results

Date: 2026-10-13. Kernel: Linux 6.18.56 with the NT personality (`docs/status/KERNEL-BOOT.md`), booted under
host QEMU with `-kernel`. Oracle: host-captured Windows goldens `tests/differential/golden/windows-26200.9457/`.
Comparator: the repo's own `tests/differential/dualrun.py` (`normalize_json`, `diff_jsons`, `evaluate_probe`),
called by `tools/linux-env/probe_results.py compare`.

## Exception dispatch, 2026-10-15 (branch `haiku/seh`)

NONVM probes on the rebuilt kernel (`docs/status/KERNEL-BOOT.md`, section "User-mode exception delivery"). All 15 reach
`NTPERS_ALL_DONE` and exit 0. Comparator: `probe_results.py compare`, golden `windows-26200.9457`.

| probe | matched / golden | verdict |
|---|---|---|
| probe_directory | 19 / 67 | FAIL |
| probe_exception | 61 / 76 | FAIL (was 0 / 76, exit 139) |
| probe_fileinfo | 183 / 221 | FAIL |
| probe_handles | 82 / 111 | FAIL |
| probe_namedpipe | 3 / 45 | FAIL |
| probe_ntstatus | 2191 / 2199 | FAIL |
| probe_path | 55105 / 72523 | FAIL |
| probe_peloader | 33 / 34 | FAIL |
| probe_process_thread | 0 / 182 | FAIL |
| probe_registry | 128 / 140 | FAIL |
| probe_section | 128 / 129 | FAIL |
| probe_sharemode | 458857 / 458857 | PASS |
| probe_sync | 0 / 74 | FAIL |
| probe_teb_peb | 81 / 104 | FAIL |
| probe_virtualquery | 128 / 139 | FAIL |

Verdict level: 1 of 15 (`probe_sharemode`), unchanged. Field level: no probe lower than the previous run. The
`probe_exception` remaining gaps (guard page code, FP state in CONTEXT, TestAlert) are listed in KERNEL-BOOT.md.

## Five-line report

1. Probes run in the personality: 8 of 8 VM-capable probes (`*_direct`). All 8 ran to exit status 0 in the final boot. Not run: the 14 non-direct probes, which have no VM branch and need ntdll (`binfmt_pe`: "ntdll not mapped").
2. Probes matching goldens: 0 of 8 at the verdict level (the repo's policy allows 0 S1). Field level, `probe_sharemode_direct` matches 462437 of 462451 fields and `probe_sync_direct` could not be compared (see below).
3. Top divergence causes: (a) the NT namespace has no `C:\tmp`, so every scratch-file test returns `STATUS_OBJECT_PATH_NOT_FOUND` (0xC000003A); (b) `NtCreateProcessEx` and other services are not implemented (`STATUS_NOT_IMPLEMENTED`), which empties the cross-process and handle-query sections; (c) VirtualQuery region coalescing and status codes differ (`probe_virtualquery_direct`: 1279 S1 fields).
4. Fixed one kernel bug (`src/kernel/fs/binfmt_pe.c`): the second PE in one boot always failed to load (`pe_map_sections failed after exec commit (err -99)`). Before: 1 probe ran, 7 killed by SIGSEGV. After: 8 of 8 ran. M0 still passes (`m0-serial-after-binfmt-fix.log`).
5. Harness: `tools/linux-env/probes-initramfs.sh` builds the probes and the initramfs; `boot-m0-host.ps1 -Initrd -Serial -Append` boots them; `probe_results.py split|compare` produces the verdicts.

## Thread exit status fix, probe_process_thread_direct (2026-10-14)

Before = the kernel of the table above (`bzImage` from `kfast/linux-src`, built 2026-10-14 04:17). After = the
same guest tree with `src/kernel/nt/object.c` changed (`thread_delete` now wired in the kernel build too). Same
probe initramfs (`probes-initramfs.sh ... probe_process_thread_direct`) and boot command for both
(`log_buf_len=16M`). Compared with `probe_results.py compare` against `windows-26200.9457`.

Cause: the kernel build never set `nt_thread_type.ops.delete_obj`, so a closed thread kept its registry node
bound to its header address. A later thread object at the same address was matched first by
`ObFindThreadByHeader`, which returned the closed thread's exit status (0, from the probe's explicit
`NtTerminateThread(th,0)` on the sleeper threads). The serial log shows the three thread objects sharing header
`c5c6822d`.

| run | matched / golden leaves | mismatched | exit_codes in the probe JSON |
|---|---|---|---|
| before | 177 / 184 | 7 | thread_before_wait 0, thread_after 0, create_suspended_before 0, create_suspended_after 0 |
| after | 180 / 184 | 4 | thread_before_wait 777, thread_after 777, create_suspended_before 259, create_suspended_after 777 |

Remaining mismatches after the fix:
- `process_info[9].retLen` (202 vs 120) and `process_info[9].image_name` (host build path): environmental.
- `teb_peb.teb_peb` (guest address vs golden address): environmental.
- `exit_codes.thread_before_wait` (259 vs 777): **not fixed**. The probe creates a thread that returns at once,
  then queries it. On this single-CPU guest the child runs and exits before the parent's query, so the query
  sees the terminated status. The kernel status path is correct (blocked and suspended threads report 259,
  checked by `tests/kernel-host/thread_exit_test.c`). Making this read 259 would need a delay or a change to
  scheduling, which is not done here.

Regression on the same rebuilt kernel (the fixed `bzImage`, initramfs images from `~/ntpers`): M0 exit 42
(`M0 PASS`); S1, S2, S3 and S4 each exit 0 (`STEP PASS`).

## Progress after the divergence fixes (2026-10-13)

Before = run 4 (`probes-serial-run4.log`, the kernel of the table below). After = run 6 (`probes-divfix2-serial.log`, the kernel built from the commits listed underneath). Matched/total are `diff_jsons` field counts against the Windows golden. Run 5 (`probes-divfix-serial.log`, sync fix only, no `/tmp`, no exit fix) changed only `probe_sync_direct`, so the other rows are the same as run 4 there.

| probe | matched before | matched after | mismatched before to after | verdict after |
|---|---|---|---|---|
| probe_handles_direct | 93 / 121 | 114 / 121 | 28 to 7 | FAIL |
| probe_ntstatus_direct | 3510 / 3977 | 2904 / 3977 | 467 to 1073 (regression) | FAIL |
| probe_path_direct | 121507 / 237589 | 122713 / 237589 | 116082 to 114876 | FAIL |
| probe_process_thread_direct | 177 / 184 | 177 / 184 | 7 to 7 | FAIL |
| probe_section_direct | 106 / 129 | 103 / 129 | 23 to 26 (regression) | FAIL |
| probe_sharemode_direct | 462437 / 462451 | 462439 / 462451 | 14 to 12 | FAIL |
| probe_sync_direct | not compared (guard) | 74 / 74 | 0 after (5 in run 5) | **PASS** |
| probe_virtualquery_direct | 47902 / 49181 | 47902 / 49181 | 1279 to 1279 | FAIL |

Probes matching their goldens at the verdict level: 1 of 8 (`probe_sync_direct`). All 8 exit 0 and reach `NTPERS_ALL_DONE`.

Commits in the "after" kernel, in order:

- `8d23a6a` comparator: `probe_sync_direct` mandatory keys match the probe (the sync probe is now compared).
- `d4dbbb3` kernel: the sync thread is per task (`src/kernel/nt/sync.h`). It was one process-wide global, so threads shared one sync thread.
- `2673694` kernel: a task's owned mutants are abandoned at exit (`arch/x86/nt/dispatch.c` `nt_thread_exit_abandon`, called from `do_exit` by `tools/ntpers_kpatch.py`). Nothing abandoned them on a normal thread return. This is the change that makes the sync probe match.
- `db518ef` harness: `probes-initramfs.sh` mounts tmpfs on `/tmp`. The kernel maps the NT path `C:\tmp` to `/tmp`; the initramfs had no `/tmp`, so scratch creates returned `STATUS_OBJECT_PATH_NOT_FOUND` (0xC000003A).

The `/tmp` and exit-abandon changes were measured together, so the handles, path and ntstatus rows cannot be attributed to one of them. The two regressions (ntstatus, section) are not explained yet. The likely cause is the `/tmp` mount changing which paths succeed, but this is a hypothesis.

Regression checks on the same kernels: M0 exit 42 (PASS) and S1 `hello from NT` exit 0 (PASS), before and after the exit-abandon change.

Not done: the 14 non-VM probes (ntdll packing), VirtualQuery (1279 S1 fields, unchanged), and the two regressions.

## Fix cycles after the first table (runs 7 to 10, 2026-10-13)

| run | kernel (commit) | initramfs | result |
|---|---|---|---|
| 7 | build 2 + exit abandon (`2673694`) | no `/tmp` (old) | ntstatus 3510, section 106: the same as run 4. The two regressions come from the `/tmp` mount, not the exit change. |
| 8 | build 4 (adds `3cd5e5f`) | `/tmp` | 7 of 8 probes killed (SIGKILL). Cause: a kernel SMAP fault on the user `AllocationSize` read in `NtCreateFile`. Fixed in `87087ff` and `510d8d4`. |
| 9 | build 5 (SMAP read fixed) | `/tmp` | all 8 exit 0. Same numbers as run 6. |
| 10 | build 6 (adds `236e741`) | `/tmp` | all 8 exit 0. `probe_section_direct` 104/129 (was 103); `sec_file_commit` now matches the golden. |

Run 10 per probe (matched / golden leaves): handles 114/121, ntstatus 2904/3977, path 122713/237589, process_thread 177/184, section 104/129, sharemode 462439/462451, sync 74/74 (PASS), virtualquery 47902/49181.

Still open:

- **ntstatus regression (467 to 1073 mismatches):** after `/tmp` exists, the probe reaches real NT statuses where it used to hit `OBJECT_PATH_NOT_FOUND` everywhere, so its distinct-status list is positionally shifted. This is a real S1 divergence exposed by `/tmp`, not a kernel regression. Not fixed.
- **section (26 mismatches):** pagefile `NtMapViewOfSection` at offset 8192 (Windows 0xC0000220, VM 0); `map_file_commit.base` (an address the normaliser does not ignore; ASLR); `sec_image` (Windows 0xC000012F, VM 0xC0000002). Not fixed.
- **Non-VM probes:** see the section "Non-VM probes (advapi cycle)". The loader now fails the load on an unbound import, and advapi32 is in place. All 15 probes fail cleanly on 62 missing kernel32 exports; none runs to completion yet.
- **VirtualQuery** (1279 S1 fields), **NtCreateProcessEx / sharemode** (12 fields): not started.

Commits added in this cycle: `2834e75` (exit helper moved into `src/kernel/nt/sync.c`), `3cd5e5f` and `cba9ff9` (create AllocationSize, src and arch mirror), `87087ff` and `510d8d4` (SMAP-safe read of the user pointer, src and arch mirror), `236e741` (explicit zero maximum size), and the non-VM initramfs script.

## Non-VM probes (advapi cycle, 2026-10-13)

Scope: the 15 probes with no `NTPERS_VM` branch (`tools/linux-env/probes-initramfs.sh NONVM=1`). They are built
with the default MinGW CRT and resolve ntdll through `GetProcAddress`. The image carries `ntdll.dll`,
`kernel32.dll`, `msvcrt.dll`, `advapi32.dll` and `user32.dll` under `/windows/system32`, built from `src/ntdll`.
The kernel is the divfix build (`bzImage`, 2026-10-13 09:20), copied and not changed. Serial log: host
`linux-env\advapi-run\probes-nonvm-serial.log` (not committed).

What changed in this cycle, one commit each:

1. Loader strictness (`5bb668b`): every binder path ends the process on an import it cannot bind, with
   STATUS_DLL_NOT_FOUND or STATUS_ENTRYPOINT_NOT_FOUND. Before, the first call of an unbound import jumped to its
   RVA (the segfaults). S5 covers the DLL case. S2, S3 and S4 still pass.
2. advapi32 shim (`b9284a7`): the Reg*A calls the probes import, on the NT registry services, for
   `\Registry\Machine` and `\Registry\User`. `RevertToSelf` returns TRUE. `ImpersonateNamedPipeClient` fails with
   ERROR_CALL_NOT_IMPLEMENTED: the kernel has no named-pipe impersonation service yet.
3. Initramfs (`probes-initramfs.sh NONVM=1`): packs the four shims and user32, builds the probes plain, and
   checks each probe's imports against the packed DLLs.

Result of the boot: no probe runs to completion. Every probe now fails at load, cleanly, on the first kernel32
export the shim lacks. The loader writes the name to stderr and the serial console, and each probe exits with 57
(0xC0000139, the low byte). ADVAPI32 binds completely in every probe.

| probe | first unbound import | exit | JSON | matched / golden leaves |
|---|---|---|---|---|
| probe_directory | KERNEL32.dll!DeleteCriticalSection | 57 | no | not run |
| probe_exception | KERNEL32.dll!AddVectoredContinueHandler | 57 | no | not run |
| probe_fileinfo | KERNEL32.dll!DeleteCriticalSection | 57 | no | not run |
| probe_handles | KERNEL32.dll!CreateEventA | 57 | no | not run |
| probe_namedpipe | KERNEL32.dll!ConnectNamedPipe | 57 | no | not run |
| probe_ntstatus | KERNEL32.dll!DeleteCriticalSection | 57 | no | not run |
| probe_path | KERNEL32.dll!DeleteCriticalSection | 57 | no | not run |
| probe_peloader | KERNEL32.dll!CreateFileMappingA | 57 | no | not run |
| probe_process_thread | KERNEL32.dll!CreateProcessA | 57 | no | not run |
| probe_registry | KERNEL32.dll!DeleteCriticalSection | 57 | no | not run |
| probe_section | KERNEL32.dll!CreateFileMappingA | 57 | no | not run |
| probe_sharemode | KERNEL32.dll!DeleteCriticalSection | 57 | no | not run |
| probe_sync | KERNEL32.dll!CreateEventA | 57 | no | not run |
| probe_teb_peb | KERNEL32.dll!DeleteCriticalSection | 57 | no | not run |
| probe_virtualquery | KERNEL32.dll!DeleteCriticalSection | 57 | no | not run |

The first-failure column is the first unbound name in each probe's import table, so it advances as shims are
added. Before this cycle, all 15 segfaulted, with `ip` 0xf5fc or 0x10566 (the unbound IAT RVA).

Still missing, from the import tables of the 15 probes (not in `src/ntdll/kernel32.def`, 62 names): the
critical-section, event, mutex and semaphore calls; named pipes (`CreateNamedPipeA`, `ConnectNamedPipe`,
`PeekNamedPipe`, `WaitNamedPipeA`, `DisconnectNamedPipe`); `CreateProcessA`; `CreateThread`;
`CreateFileMappingA` and `MapViewOfFile` and `UnmapViewOfFile`; the vectored handlers; the file-find calls;
`GetTempPathA`, `GetTempFileNameA`; `MultiByteToWideChar` and `WideCharToMultiByte`; the timers and priorities.
Missing in msvcrt.def (10): `_acmdln`, `_onexit`, `_setjmp`, `_snwprintf`, `fwrite`, `getenv`, `longjmp`, `memset`,
`setvbuf`, `strncpy`. Each is an implementation, not a loader or kernel divergence, so this cycle does not
fix them: none of them is small, and a probe can only run once all of its imports exist.

Not compared: no probe produced JSON, so `probe_results.py compare` has nothing to diff against the goldens.

## Non-VM probes (k32full cycle, 2026-10-13)

Scope: the same 15 non-VM probes, now against the shims for every name their import tables need. The
missing set is 62 kernel32 names and 10 msvcrt names (the advapi cycle's list, rechecked against the parsed
import tables of the 15 probes: the 62 and 10 match exactly). All 62 and all 10 are exported now.

Commits, one per family (`haiku/k32full`):

1. `84cbea0` sync: critical sections on an auto-reset NT event (RTL protocol), `WaitForMultipleObjects` on
   `NtWaitForMultipleObjects` (it returned 0), `GetTickCount` from the performance counter. Host tests:
   `test_k32_sync_pure` (8 cases).
2. `3f2775d` mapping: `CreateFileMappingA/W`, `MapViewOfFile`, `UnmapViewOfFile` on `NtCreateSection` and
   `NtMapViewOfSection` (the fake handle and the always-failing view are gone). `test_k32_mm_pure` (8 cases).
3. `3257b5f` threads: `CreateThread` on `NtCreateThreadEx` (it returned 120), `Suspend`/`ResumeThread`,
   `TerminateThread`, the exit codes from the basic information classes (they returned 0). `CreateProcessA/W`
   are `ERROR_CALL_NOT_IMPLEMENTED`. `test_k32_thread_pure` (4 cases).
4. `e664f6f` files: `FindFirstFileA/FindNextFileA/FindClose` on `NtQueryDirectoryFile`, `GetFileTime`,
   `GetFileInformationByHandle`, `GetTempPathA` (TMP, TEMP, else `C:\tmp\`), `GetTempFileNameA` (creates the
   file). `test_k32_find_pure` (8 cases).
5. `4a0fc38` misc: the vectored handler functions forward to ntdll's lists (exported under the Win32 names
   with `.def` aliases); the named pipe calls are `ERROR_CALL_NOT_IMPLEMENTED`; `GetStartupInfoA` sets `cb`.
6. `e63392c` system: priority classes and thread priorities on the information classes, handle flags on
   `NtQueryObject`, `GetBinaryTypeA` from the PE header (it returned 6 for every file),
   `GetProcessHandleCount` is `ERROR_CALL_NOT_IMPLEMENTED` (it returned 5). `VirtualQuery`'s
   `MEMORY_BASIC_INFORMATION` lacked `PartitionId`, which put `RegionSize` at offset 24, not 32.
   `test_k32_sys_pure` (5 cases).
7. `ebd418d` strings: `MultiByteToWideChar` and `WideCharToMultiByte` with UTF-8 (validated), Windows-1252
   for `CP_ACP`, ASCII for `CP_OEMCP` (code page 437's high bytes are not built yet). `test_k32_str_pure`
   (12 cases).
8. `314fa39` msvcrt: `_setjmp`/`longjmp` (x64 assembly, XMM registers not saved), `fwrite`, `getenv` (a
   snapshot of the environment), `setvbuf` (mode check only), `strncpy`, `_snwprintf` (a wide formatter,
   `%ls` is `%s` here), `_onexit` (it was a stub), `_acmdln` (it was never set; now the narrowed command
   line, a DATA export).

Build and checks (in the guest; the Windows host has no Python, so the `.def` generator runs there):
`make -C src/ntdll all kernel32 msvcrt advapi32 user32` builds; `make check` passes (373 ntdll exports
match, LdrLoadDll is not the stub); `check_exports.sh` passes for kernel32 (124 names) and msvcrt (74).
The host tests pass in the guest, with `test_except`, `test_except_pure` and `test_unwind*` not built there
(the Linux glibc headers differ from the MinGW layout that `except.c` expects; this is unchanged from the
advapi cycle). `test_runtime` aborts under AddressSanitizer on its own calloc overflow case, also unchanged.

Boot (`probes-nonvm-serial.log`, 2026-10-13 11:17, kernel `bzImage` copied from `advapi-run`, the
initramfs from `probes-initramfs.sh NONVM=1` on this branch). Every one of the 15 probes now runs to its end
marker with an exit status. The advapi cycle had all 15 fail at load with status 57. Per-probe results
(`probe_results.py split` and `compare` against `tests/differential/golden/windows-26200.9457`):

| probe | ran | exit | JSON | golden leaves | matched | mismatched | S1 | first divergence |
|---|---|---|---|---|---|---|---|---|
| probe_directory | yes | 139 (SIGSEGV, ip 0) | no | 67 | 0 | 67 | 1 | NULL call: GetProcAddress failed (loader registration, below) |
| probe_exception | yes | 139 (read at 0, ip 0x140001780) | no | 76 | 0 | 76 | 1 | the probe's own NULL read is not dispatched to its handler (not investigated) |
| probe_fileinfo | yes | 139 (ip 0) | no | 221 | 0 | 221 | 1 | NULL call (loader registration) |
| probe_handles | yes | 0 | yes | 111 | 68 | 43 | 6 | `create_status` handles and `duplicate_handle` lengths (as in the VM cycle) |
| probe_namedpipe | yes | 0 | yes | 45 | 3 | 42 | 5 | named pipe services not implemented (`ERROR_CALL_NOT_IMPLEMENTED`) |
| probe_ntstatus | yes | 1 | no | 2199 | 0 | 2199 | 1 | no JSON; not investigated |
| probe_path | yes | 139 (ip 0) | no | 72523 | 0 | 72523 | 1 | NULL call (loader registration) |
| probe_peloader | yes | 0 | no | 34 | 0 | 34 | 1 | no JSON; section image mapping (`SEC_IMAGE`) not investigated |
| probe_process_thread | yes | 0 | no | 182 | 0 | 182 | 1 | no JSON; process creation is not implemented |
| probe_registry | yes | 0 | no | 140 | 0 | 140 | 1 | kernel prints "missing service 18" (NtOpenKey) into the JSON stream |
| probe_section | yes | 0 | yes | 129 | 128 | 1 | 1 | `vacuous`: the NT section calls return -1 (GetProcAddress failed, loader registration) |
| probe_sharemode | yes | 139 (ip 0) | no | 458857 | 0 | 458857 | 1 | NULL call (loader registration) |
| probe_sync | yes | 139 (ip in the thread, `%gs` not a TEB) | no | 74 | 0 | 74 | 1 | a created thread has no TEB: `%gs:0x30` faults (kernel thread creation) |
| probe_teb_peb | yes | 0 | yes | 104 | 77 | 27 | 3 | thread stack bounds and `DeallocationStack` differ (kernel stack layout) |
| probe_virtualquery | yes | 0 | yes | 139 | 128 | 11 | 11 | region count (5 against 4) and 4 KB against 8 KB region sizes (kernel VAD granularity) |

Counts: 15 of 15 ran to an exit status; 6 crashed (SIGSEGV); 5 produced JSON; 0 match at the verdict level.

Root causes found in this cycle (diagnosed, not fixed; each needs more than a shim change):

1. **Startup DLLs are not in the loader's module list.** `LdrInitializeThunk` (`src/ntdll/ntdll_main.c`) binds
   the image's imports by mapping each DLL with `ldr_nt_map_dll` (an NT section) and never enters it into the
   context (`winldr_ctx`) module list or name hash. Only `LdrLoadDll` (`src/ntdll/ldr.c`, `ldr_map_and_load`)
   does that, and it reads and maps the file itself. The result: `GetModuleHandleA("kernel32.dll")`,
   `GetModuleHandleA("ntdll.dll")` and `GetModuleHandleA("msvcrt.dll")` all fail with `ERROR_MOD_NOT_FOUND`
   in the guest, so `GetProcAddress` returns NULL. A probe that calls the result then jumps to address 0
   (directory, fileinfo, path, sharemode), and the section probe's NT calls report -1. The evidence is an
   instrumented `LdrGetDllHandle` that returns the load-list and bucket counts: both are 0 for `ntdll.dll`
   while the context is valid (published in `PEB.SparePointers[0]`, read by kernel32 correctly). The fix is
   to register the startup modules: export parsing for `GetProcAddress` needs the file's PE headers, and the
   address must come from the NT mapping (DllBase). That is the next task on this path.
2. **A created thread has no TEB.** `CreateThread` (`NtCreateThreadEx` in `src/kernel/nt/proc_thread.c`)
   starts the routine with a `%gs` base that is not the thread's TEB, so the first `%gs:0x30` read faults
   (probe_sync, at `ObRegisterThread` for tid 2004 onwards). Kernel side.
3. **The registry service is missing from this kernel build.** `NtOpenKey` (service 0x12) is in
   `nt_service_list.h` and `registry.c` defines it, but the dispatcher reports the handler as missing
   ("missing service 18"). This kernel build (the one copied from `advapi-run`) does not link `registry.c`
   into the NT service table. Kernel build configuration. The message also lands in the middle of the JSON
   stream, which is why probe_registry produces no JSON.

Not done in this cycle, and still a divergence source for the probes: named pipes (no NPFS service),
`CreateProcessA` (needs `NtCreateUserProcess` with a parameters block), `GetProcessHandleCount` (the kernel
does not answer `ProcessHandleCount`), `QueryDosDeviceW` (fails with `ERROR_FILE_NOT_FOUND`), the thread
priority of a new thread (the kernel's default), and msvcrt's `__getmainargs` (argc is always 1).

## Non-VM probes (startup modules and TEB cycle, 2026-10-13)

Scope: the same 15 non-VM probes, after two fixes on `haiku/modlist`:

1. `7cc0429` ntdll: the startup modules (exe, ntdll and every DLL the binder maps) enter the loader list
   (`winldr_register_mapped`). `GetModuleHandle`, `GetProcAddress`, `LdrGetDllHandle` and
   `LdrGetProcedureAddress` now see them, with forwarders (`ldr_mapped_export`, `ldr_pure.h`). Step S6.
2. `306d6af` kernel: a thread made by `NtCreateThreadEx` has a user-mode TEB (`nt_alloc_teb_stack_kernel`
   was `kzalloc`, a kernel address). Step S7.

Boot: kernel built from this branch (`~/ntpers/linux-modlist-teb`, `bzImage` copied to
`linux-env\modlist-run\bzImage`, the original kernel untouched), initramfs from
`probes-initramfs.sh NONVM=1` on this branch. Serial log: host `linux-env\modlist-run\probes-nonvm-after-serial.log`
(not committed). Before = the k32full table above (`probes-nonvm-serial.log`).

Per probe, before to after (matched / golden leaves; exit status):

| probe | before | after |
|---|---|---|
| probe_directory | 0 / 67 (139, NULL call) | 0 / 67 (0) |
| probe_exception | 0 / 76 (139, own NULL read) | 0 / 76 (139, unchanged) |
| probe_fileinfo | 0 / 221 (139, NULL call) | 129 / 221 (0) |
| probe_handles | 68 / 111 (0) | 82 / 111 (0) |
| probe_namedpipe | 3 / 45 (0) | 3 / 45 (0) |
| probe_ntstatus | 0 / 2199 (1, no JSON) | 2190 / 2199 (0) |
| probe_path | 0 / 72523 (139, NULL call) | 51355 / 72523 (0) |
| probe_peloader | 0 / 34 (0, no JSON) | 0 / 34 (0) |
| probe_process_thread | 0 / 182 (0, no JSON) | 0 / 182 (0) |
| probe_registry | 0 / 140 (0, no JSON) | 0 / 140 (0) |
| probe_section | 128 / 129 (0, vacuous) | 128 / 129 (0) |
| probe_sharemode | 0 / 458857 (139, NULL call) | **458857 / 458857 (0), PASS** |
| probe_sync | 0 / 74 (139, thread has no TEB) | 0 / 74 (0, runs; not matched yet) |
| probe_teb_peb | 77 / 104 (0) | 79 / 104 (0) |
| probe_virtualquery | 128 / 139 (11) | 128 / 139 (11) |

Summary after: 15 of 15 run to the end marker. 14 exit 0; `probe_exception` exits 139 (its own NULL read is not
dispatched to its handler, not changed here). 1 of 15 matches its golden at the verdict level
(`probe_sharemode`). Before: 6 crashed (139), 5 produced JSON, 0 matched. The `probe_sharemode` jump is the
`NtCreateProcessEx` and `GetProcAddress` path: its NULL call is gone.

Not done: the remaining mismatches (path, ntstatus, fileinfo, sync, process_thread, registry, peloader) are not
diagnosed in this cycle, and `probe_sync` is not compared (0 of 74).

Checks on the branch, per the cycle's rules: `make -C src/ntdll check` passes; `make -C tests/ntdll-host` passes in
the guest for every binary except `test_except` and `test_unwind*` (not built on glibc, as before) and with
`SANITIZE=0` (ASan aborts `test_runtime` on its own calloc case, as before); `test_ldr_mapped_pure` 7 of 7;
`tools/check_doc_links.py` 0 broken; `update_doc_counts.py` tracked count 711. Not verified: the unit suite has
one error (`test_check_repo_extra`, a Python null-byte `ValueError` in `tools/check_repo.py`, not touched here);
`make -C tests/kernel-host check-quick` cannot run here (it runs on the Windows host, which is prohibited; the
guest build fails in the shim's `sys/stat.h`). The host compile of the kernel-host tests passes.

## How results leave the VM

- Each probe is built in the guest with `-DNTPERS_VM -DVM_BUILD` and `-Dlinux_open=ntpers_vm_open -Dlinux_write=ntpers_vm_write -Dlinux_close=ntpers_vm_close`. The probe source is unchanged. The shim `tools/linux-env/ntpers_vm_stdout.c` routes the probe's stdout to NT console handle -11, which the kernel prints through `printk` (`arch/x86/nt/nt_io.c`, `ntio_console_write`). The probe's own `linux_write(1, ...)` is a raw `syscall` with rax=1, which the NT ABI dispatches as an NT service number, so it cannot reach stdout.
- `/init` runs each probe under `timeout 900` and frames it with `NTPERS_PROBE_BEGIN/RC/END` markers written to `/dev/kmsg`, so the markers share the printk sequence with the probe output.
- Limits found on the way: printk truncates a record at about 1 KB, so stdout is written in chunks of 700 bytes. The printk ring is 128 KB by default (`CONFIG_LOG_BUF_SHIFT=17`). Boot with `log_buf_len=16M`. Probes writing more than one copy of their JSON (`probe_virtualquery_direct` writes it through `linux_write(1)` and again through `NtWriteFile(-11)`) are handled by taking the first complete JSON object.

## Per-probe results (final boot, `probes-serial-run4.log`)

Golden leaves are the normalised leaf count of the Windows golden. Matched and mismatched are field counts from `diff_jsons` (S4 normalisations excluded).

| probe | ran | exit | JSON | matched / golden leaves | mismatched fields | verdict |
|---|---|---|---|---|---|---|
| probe_handles_direct | yes | 0 | yes | 93 / 121 | 28 | FAIL (8 S1) |
| probe_ntstatus_direct | yes | 0 | yes | 3510 / 3977 | 467 | FAIL (463 S1) |
| probe_path_direct | yes | 0 | yes | 121507 / 237589 | 116082 | FAIL (42363 S1) |
| probe_process_thread_direct | yes | 0 | yes | 177 / 184 | 7 | FAIL (1 S1) |
| probe_section_direct | yes | 0 | yes | 106 / 129 | 23 | FAIL (4 S1) |
| probe_sharemode_direct | yes | 0 | yes | 462437 / 462451 | 14 | FAIL (6 S1) |
| probe_sync_direct | yes | 0 | yes | not compared | 1 | FAIL (comparator guard, see below) |
| probe_virtualquery_direct | yes | 0 | yes | 47902 / 49181 | 1279 | FAIL (1279 S1) |

No probe crashed or hung in the final boot. Before the kernel fix, 7 of 8 died with exit status 139.

### First mismatches (from `compare.json`)

- **probe_handles_direct**: `create_status.h1` (Windows 0, VM 0xC000003A), `create_status.h1_hex`, `create_status.h2`, `create_status.h2_hex`, `create_status.hFile2`. Then `duplicate_handle` and `nt_query_object` arrays are shorter on the VM side (len 1 vs 7 and 1 vs 6), and `handles[0..3].value` differ (Windows 236/240/244/248, VM 0/0/200/204).
- **probe_ntstatus_direct**: `distinct_summary.distinct_list` (len 15 vs 1), `distinct_list[0]` (0xC0000003 vs 0xC000003A), `distinct_via_real_syscalls` (15 vs 1), `total_real_cases` (15 vs 1), `fallback_tests[2].dos` (128 vs 317).
- **probe_path_direct**: `tests[0].dos` (2 vs 3), `tests[0].hex` (0xC0000034 vs 0xC000003A), `tests[0].nt_path` (`\??\<root>\...` vs `\??\C:\tmp\...`), `tests[0].ntstatus`, `tests[10000].dos` (3 vs 123).
- **probe_process_thread_direct**: `process_info[9].retLen` (202 vs 120), `exit_codes.create_suspended_after` (777 vs 0), `exit_codes.create_suspended_before` (259 vs 0), `exit_codes.thread_after` (777 vs 0), `exit_codes.thread_before_wait` (259 vs 0).
- **probe_section_direct**: `map_commit_offset_8192.hex` (0xC0000220 vs 0x0), `map_commit_offset_8192.ntstatus`, `map_real_image.viewSize` (2519040 vs 2514944), `vquery_commit[1].RegionSize` (61440 vs 65536), `map_commit.base` (0x23662f20000 vs 0x20000, an address field not in the normaliser's key list).
- **probe_sharemode_direct**: `cross_process` (Windows len 512, VM len 0), `cross_process_missing_hex` (0 vs 0xC0000002), `cross_process_missing_service` (empty vs `NtCreateProcessEx`), `cross_process_missing_ssn` (0 vs 77), `cross_process_missing_status` (0 vs -1073741822).
- **probe_sync_direct**: the comparator's vacuous guard fires on both sides, so no field was compared. `MANDATORY_KEYS["probe_sync_direct"]` is `["tests"]`, but the probe emits `events` and `mutant` and no `tests`. This is a comparator configuration gap in `tests/differential/dualrun.py`, not a kernel result.
- **probe_virtualquery_direct**: `negatives[0].status` and `negatives[1].status` (Windows 0, VM 0xC000000D), `release.status` (0xC00000A0 vs 0), `release.success` (False vs True), `transitions[1281].mbi.RegionSize` (4096 vs 8192).

## Root-cause hypotheses (not yet proven)

1. **No `C:\tmp` in the NT namespace** (handles, ntstatus, path). The scratch path `\??\C:\tmp\...` is not found, so the create returns `STATUS_OBJECT_PATH_NOT_FOUND` (0xC000003A) where Windows returns success or `STATUS_OBJECT_NAME_NOT_FOUND` (0xC0000034). This cascades into the handle and distinct-status arrays. Likely the kernel FS/namespace layer (`src/kernel/fs`, `src/kernel/nt/io.c`). Not fixed.
2. **Missing services** (sharemode cross-process: `NtCreateProcessEx`, SSN 77, returns `STATUS_NOT_IMPLEMENTED`; handle query: `NtQueryObject` and `DuplicateHandle` arrays truncated). `docs/status/KERNEL-BOOT.md` already records 43 listed services as unimplemented.
3. **Probe-side table, not kernel** (ntstatus `fallback_tests[2].dos`). The VM build of `probe_ntstatus_direct` maps `RtlNtStatusToDosError` through a hard-coded switch with default 317. The VM value is the probe's own stub, so it cannot be a kernel result. Do not attribute it to the kernel.
4. **Thread exit codes** (process_thread). The kernel reports 0 where Windows reports `STILL_ACTIVE` (259) or 777 for a live thread. Likely `GetExitCodeThread` returns the stored exit code before the thread has exited. Not fixed.
5. **VirtualQuery coalescing and argument validation** (virtualquery, section). Region sizes and statuses differ at specific transitions, and invalid arguments are accepted (`negatives`). Likely `src/kernel/nt/mm.c` VAD coalescing and argument checks. Not fixed.
6. **Comparator gap** (sync). Listed above; this is in the repo's comparator, not the kernel.

## The kernel fix (before and after)

`src/kernel/fs/binfmt_pe.c`: `load_pe_binary` now calls `ntmm_reset()` right after `begin_new_exec` commits, under `CONFIG_NT_PERSONALITY`.

Cause: the kernel keeps one NT VAD map (`ntmm_global_space` in `src/kernel/nt/mm.c`). Only the userspace shim destroys a process space on exit. The second PE in a boot found the first image's header and section regions still registered, so `ntmm_add_region` returned `STATUS_CONFLICTING_ADDRESSES` (reported as `-EADDRNOTAVAIL`, -99) and the exec was killed after commit.

- Before (`probes-serial-run2.log`, old bzImage): `probe_handles_direct` exit 0, the other 7 exit 139 at `pe_map_sections failed after exec commit (err -99)`.
- After (`probes-serial-run4.log`, new bzImage): all 8 exit 0.
- M0 regression, after the fix (`m0-serial-after-binfmt-fix.log`): `M0 exit status: 42`, `M0 PASS`.

Limit: the VAD map is still global. Two PEs running at the same time would share it. Per-process VAD maps in the kernel are the real fix and are not done here.

## Not run

The 14 probes without an NTPERS_VM branch (`probe_directory`, `probe_direct_simple`, `probe_exception`, `probe_fileinfo`, `probe_handles`, `probe_namedpipe`, `probe_ntstatus`, `probe_path`, `probe_peloader`, `probe_process_thread`, `probe_registry`, `probe_section`, `probe_sharemode`, `probe_sync`, `probe_teb_peb`) resolve ntdll through `GetProcAddress`. The personality maps no ntdll, so they cannot run until ntdll is loaded. Their `*_direct` variants are the ones run here.

## NONVM field work: ntstatus fix and the four target probes (diff3, 2026-10-14)

Build: `NONVM=1` probes (`probe_*.c` without an NTPERS_VM branch) and their DLLs (`make -C src/ntdll all kernel32 msvcrt advapi32 user32`, rebuilt in the guest), on the kernel `bzImage` of `vm/incremental-kernel.sh` over `~/syncfix/linux-src`. Boot: host QEMU, `boot-m0-host.ps1 -Mem 2048 -Initrd probes-nonvm.cpio.gz`, `log_buf_len=16M`, `NTPERS_ALL_DONE` reached, all 15 probes exit as before (`probe_exception` 139).

**A/B on the same initramfs (only the kernel differs):**

| probe | before (main `io.c`) | after (this change) | golden leaves |
|---|---|---|---|
| probe_ntstatus | 2190 | **2191** | 2199 |
| probe_fileinfo | 165 | **183** | 221 |
| all other 13 probes | unchanged | unchanged | |

`probe_sharemode` stays PASS (458857 of 458857). No probe lost a match.

The `teb_peb` count is 81 of 104 in both runs. The earlier 79 came from the DLLs built before this cycle, not from the kernel change.

**Fix (`src/kernel/nt/io.c`, `nt_svc_NtQueryInformationFile`):** `IoStatusBlock.Information` followed the wrong rule. The old code always wrote `retlen`, so it stored 0 on every error and never reported bytes on `STATUS_BUFFER_OVERFLOW`. The golden (probe_ntstatus `size_probe`, Windows 26200.9457) shows:

- `STATUS_BUFFER_OVERFLOW` reports the bytes copied, which is the whole buffer (row `buffer_len 8`: Windows 8, old VM 0).
- A `STATUS_INFO_LENGTH_MISMATCH` query leaves the caller's `Information` unchanged (rows `buffer_len 0/4`, `FileBasicInformation`: Windows keeps 94 from the previous successful query).

The change is `ntio_store_query_status`: it writes `Information` on success and on `BUFFER_OVERFLOW` only, and the overflow case reports `Length`. The `io_test` case `query_status_information` covers both rules, and the io_test suite passes on the host (36 passed, 0 failed). The unit suite (`python -m unittest discover tests/unit`) passes.

**Field-by-field root cause (the four target probes):**

- **probe_section, 1 field (`vacuous`, S1): not fixable from the kernel.** `dualrun.py` `MANDATORY_KEYS["probe_section"] = ["tests"]`. Neither the golden nor the VM output has a `tests` key (checked: `probe_section.json` keys are `sec_commit`, `map_commit`, and so on). The NT section calls do run in the VM (`sec_commit` status 0, `map_commit` 0). Clearing the flag needs a golden or comparator change, both forbidden here. Left at 128/129.
- **probe_ntstatus, 9 fields (`size_probe[0..8].info`): 1 fixed, 8 left.**
  - `[2]` (buffer 8, overflow): fixed by this change.
  - `[0]`, `[1]` (buffer 0 and 4): Windows reports 623483279872, which is uninitialised stack in the Windows capture (the `iosb` is not set before the call). Not reproducible from the VM. Environmental.
  - `[3]` (buffer 512): Windows reports 94, which is the NT name of the host temp file `\Users\Jackson\AppData\Local\Temp\ntpXXXX.tmp` (45 characters). The VM reports 38, the length of the faux NT name for the guest's temp file. Environmental (host path).
  - `[4..8]`: the value carried from `[3]`, so they follow `[3]`. Environmental.
- **probe_virtualquery, 11 fields: not investigated in this cycle.** Open items from the table above: region count 5 against 4, and 4 KB against 8 KB region sizes.
- **probe_handles, 29 fields: not investigated in this cycle.** Open items: `GetProcessHandleCount` 0 against 63, and the `duplicate_handle` length 6 against 7.

Left unfixed: section (blocked, above), ntstatus 8 environmental fields, virtualquery 11, handles 29.

## NONVM path and registry cycle: wide formatter (pathreg, 2026-10-14)

Before = `main` at 3fac7e9 (NONVM initramfs built from that tree in the guest, `~/pathreg-base`). After = the same tree
with `src/ntdll/msvcrt.c` and `src/ntdll/msvcrt_format.h` changed (`~/pathreg`). Kernel `bzImage` unchanged for both
(`kernel-stdio/bzImage`). Both booted with `boot-m0-host.ps1 -Mem 2048 -Initrd probes-nonvm.cpio.gz`, `log_buf_len=16M`,
both reached `NTPERS_ALL_DONE` with 15 of 15 probes ended. Compared with `probe_results.py compare` against
`windows-26200.9457`.

Cause (found and fixed): `_snwprintf` (`ms_wformat`, now `src/ntdll/msvcrt_wformat.h`) did not parse length modifiers,
so `%ls` was copied out as the literal text `%ls`. The path probe builds every NT name that way
(`tests/differential/probes/probe_path.c`, `build_prefixes`), so the names carried `%ls` instead of the root directory.
The narrow formatter (`msvcrt_format.h`) treated `%ls` as a narrow string. Both are fixed; `test_msvcrt_policy` has 5 new
cases (16 of 16 pass on the host).

| probe | before matched / golden | after matched / golden | verdict after |
|---|---|---|---|
| probe_path | 51355 / 72523 | **55105 / 72523** (+3750) | FAIL |
| all other 14 probes | unchanged | unchanged | |

Path mismatches after the fix (17418 fields, 21168 before), by prefix (decoded from the generator index):

- `\?\` prefix: 0 mismatches. Bare `C:\` prefix: 0 (was 1250, now matches 3B). `\??\` prefix: 440 status fields
  are `3A` in Windows and `33` in the VM; the cause is a lone `/` treated as a separator in the kernel
  (`src/kernel/nt/io.c`, `kshim_ntpath_to_linux`). Object-namespace prefix (`\dir\`): 1053 `3A` vs `33`, double separator
  rejected before the first lookup. `\Device\HarddiskVolume` prefix: 563, mostly the same `/` cause.
- Handle-relative prefix (`RootDirectory`): 1250 tests return `0xC0000008` (invalid handle) in the VM. The probe's
  directory handle from `CreateFileA(dir, FILE_FLAG_BACKUP_SEMANTICS)` is probably invalid. Not diagnosed.
- `nt_path` display field: 7500 fields differ because the probe's MinGW `printf("%lc")` output loses every
  non-backslash character in the guest. This is in the probe's printf path, not the NT name; not fixed.

Kernel path rules (not in this commit): the `/` and double-separator rules in `kshim_ntpath_to_linux` were changed
(`/` is an ordinary character and rejected with `PATH_NOT_FOUND`, `\` in an object name gives `PATH_NOT_FOUND`), with
`io_test` cases. They were not booted: the shared kernel tree (`~/syncfix/linux-src`) does not build from `main`
(undefined `nt_exc_user_trap`, `nt_exc_user_fault`, `nt_process_exit_handles` in another agent's uncommitted hooks in
`arch/x86/kernel/traps.c`, `arch/x86/mm/fault.c`, `kernel/exit.c`). The patch is kept outside the repo; it needs a
boot to confirm the expected gain (about 2000 status fields) before it is committed.

Registry (128 / 140, unchanged): 12 remaining S1 fields are environmental. `nt_query_key` retLen and `test_key` depend on the
PID and tick digits in the probe's key name (a 10-byte difference per 5 digits). `nt_enumerate.key_idx0` reports an
existing HKLM child on the golden host; the guest registry is empty. No registry semantic change in this cycle.

Checks: `make -C tests/ntdll-host test_msvcrt_policy` 16 of 16; `python -m unittest discover tests/unit` 840 tests OK
(one skip, WSL); `tests/unit/test_sync_repo_guard.py` covers the `GUEST_DEST` guard in `tools/linux-env/sync-repo.sh`.

## Reproduce

Guest (SSH via `tools/linux-env/ssh-guest.sh`, repo synced with `GUEST_DEST=dualrun tools/linux-env/sync-repo.sh`):

```sh
sh tools/linux-env/probes-initramfs.sh ~/dualrun ~/dualrun-probes.cpio.gz
python3 tools/ntpers_kpatch.py --tree ~/ntpers/linux-6.18.56 && cd ~/ntpers/linux-6.18.56 && make -j2 ARCH=x86_64 bzImage
python3 tools/linux-env/probe_results.py split <serial.log> <outdir>
python3 tools/linux-env/probe_results.py compare <outdir> tests/differential/golden/windows-26200.9457
```

Host (copy `bzImage` and `probes-initramfs.cpio.gz` into `C:\Users\Jackson\repos\linux-env\kernel-6.18.56\`):

```powershell
powershell -ExecutionPolicy Bypass -File tools/linux-env/boot-m0-host.ps1 -TimeoutSec 1500 -Initrd probes-initramfs.cpio.gz -Serial probes-serial.log -Append 'console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 panic=-1 log_buf_len=16M'
```

Serial logs are kept on the host in `C:\Users\Jackson\repos\linux-env\kernel-6.18.56\` (`probes-serial-run1..4.log`, `m0-serial-after-binfmt-fix.log`). They are not committed (about 60 MB).

## Kernel directory enumeration (haiku/npipe, 2026-10-15): probe_directory 56 of 67

Source: `main` at `e141334` plus the directory commit on `haiku/npipe` (`NtQueryDirectoryFile(Ex)` kernel branch in `src/kernel/nt/io.c`, `FILE_ID_BOTH_DIRECTORY_INFORMATION` in `io.h`, a FileIdBoth check in `io_test.c`).

**What changed.** The `__KERNEL__` branch of `ntio_dir_service` no longer returns `STATUS_NOT_SUPPORTED`. It snapshots the directory with `iterate_dir()` (SEEK_SET first, then one pass; `.` and `..` added first, as in the host build), applies the same wildcard and restart rules, and formats entries with the helpers the host build uses. Formatting goes into a kernel bounce buffer, capped at 1 MiB, and is copied out with `copy_to_user`. The search pattern (`FileName`) is copied in from user space before use. Classes: FileDirectoryInformation, FileFullDirectoryInformation, FileBothDirectoryInformation, FileNamesInformation and FileIdBothDirectoryInformation (37).

**Dispatch.** Service 53 (0x0035) is `NtQueryDirectoryFile` in `nt_service_list.h` and `syscall_table.def`. In the built tree `arch/x86/nt/nt_service_list.h` has the same entry, and `nt_svc_NtQueryDirectoryFile` is present in `io.c`. The probe serial logs contain no `nt: missing service` line. I could not reproduce the earlier "missing service 53" message from the source; the stale build that produced it was not inspected.

**Two kernel faults found by the probe and fixed before the reported run.**
- First boot: `probe_directory` oopsed (`BUG: unable to handle page fault`, exit 137). The kernel read the caller's `UNICODE_STRING` pointer directly in `ntio_dir_pattern`. Fixed by copying the string and its buffer in (`ntio_dir_pattern_user`).
- Shared lock: while the rebased build finished, another agent's job held `~/.kbuild.lock` (`seh/kb3.sh`), so the manifest write queued behind it. The build steps themselves ran under the lock.

**Builds (guest, `vm/incremental-kernel.sh` under `flock ~/.kbuild.lock`).** Shared tree `~/kfast/linux-src`, shared manifest. Final bzImage: build `#7` (rebased on `e141334`), `INCREMENTAL_OK`, sha256 `1727aa9f...`. The NONVM initramfs was built from a fresh copy of the rebased worktree at `~/dirq-repo`, sha256 `4e1a3bb1...`.

**Host unit tests.** `make -C tests/kernel-host KHOST_OS=linux build/io_test.exe`, run in the guest on the rebased tree: 40 passed, 0 failed. The new FileIdBoth case checks that the FileId of `qdir/b.txt` equals its host inode.

**NONVM probes** (`NONVM=1 probes-initramfs.sh`, boot with `log_buf_len=16M`, host QEMU through `boot-m0-host.ps1`): all 15 reach `NTPERS_ALL_DONE`. `probe_exception` exits 139 as before, `probe_process_thread`, `probe_sync` do not produce JSON (as before). Every other probe exits 0, and `probe_directory` now exits 0 (was 137 on the first boot).

Matched fields against `tests/differential/golden/windows-26200.9457`, from `probe_results.py split` and `compare` (matched = golden leaves minus mismatched fields):

| probe | matched / golden | verdict | last recorded (main 0d19a62) |
|---|---|---|---|
| probe_directory | 56 / 67 | FAIL (11 S1) | 19 / 67 |
| probe_exception | 0 / 76 | FAIL | 0 / 76 |
| probe_fileinfo | 183 / 221 | FAIL | 165 / 221 |
| probe_handles | 82 / 111 | FAIL | 82 / 111 |
| probe_namedpipe | 3 / 45 | FAIL | 3 / 45 |
| probe_ntstatus | 2191 / 2199 | FAIL | 2191 / 2199 |
| probe_path | 55105 / 72523 | FAIL | 51355 / 72523 |
| probe_peloader | 33 / 34 | FAIL | 33 / 34 |
| probe_process_thread | 0 / 182 | FAIL | 0 / 182 |
| probe_registry | 128 / 140 | FAIL | 128 / 140 |
| probe_section | 128 / 129 | FAIL | 128 / 129 |
| probe_sharemode | 458857 / 458857 | PASS | 458857 / 458857 |
| probe_sync | 0 / 74 | FAIL | 0 / 74 |
| probe_teb_peb | 81 / 104 | FAIL | 79 / 104 |
| probe_virtualquery | 128 / 139 | FAIL | 128 / 139 |

Verdict level: **1 of 15** (`probe_sharemode`), unchanged. The "last recorded" column is main at `0d19a62`, not a same-tree baseline: main has moved since, so the `probe_path`, `probe_fileinfo` and `probe_teb_peb` gains are not all from this change. No probe that was matched before lost a field.

**probe_directory still fails.** The 11 S1 mismatches are the NULL-buffer and undersized-buffer statuses: `query_tests[*].null_hex` windows `0xC0000004` (STATUS_INFO_LENGTH_MISMATCH), vm `0xC0000005` (our access violation); `small_hex` windows `0xC0000004`, vm `0x80000005` (buffer overflow). Changing these is a status-mapping change and is not made here.

**Path-rule patch (`pathreg-run/unverified-kernel-path-rules.patch`): not folded in.** It applies with an offset. Its `io_test` literals were mis-escaped; I corrected them, then the userspace case `\??\C:\path\sub/deep.txt` returned `0xC0000033` instead of PATH_NOT_FOUND, so `io_test` reported 39 passed, 1 failed. Per the rule to drop anything that regresses, it was reverted and no kernel boot was taken for it.

**Gaps.** `probe_directory` at 56 / 67 still needs the status mapping above. The `FileIdBothDirectoryInformation` path is covered by the host test only; no probe reads class 37.
