# NT personality kernel: build, boot and M0 status

## Device bridge and S13 (branch `drvkernel`, 2026-10-27)

**What landed.** `NtCreateFile` and `NtOpenFile` try the driver host first (`io.c`, `ntio_driver_open`). The driver host
(`driver/drvhost_svc.c`, `dh_io_open_device`) copies the object name and opens it with its own `NtCreateFile`. A name it
does not own returns `STATUS_OBJECT_NAME_NOT_FOUND`, and the io layer falls through to its own open. The file object
carries the driver handle (`nt_file_object.drv_handle`). `NtDeviceIoControlFile` on such a handle bounces the buffers
through kernel memory and calls the driver host (`ntio_driver_ioctl`). `NtClose` releases the handle through the file
release (`dh_io_device_close`). The hooks are weak, so builds without the driver host keep the old io path.
`dh_user_load_driver` now calls `drvhost_init` first (it sets `\SystemRoot\`), installs the boot-time service record
(see below), runs `NtLoadDriver`, then one PnP enumeration of the service (`AddDevice` creates the device).

**Boot-time service record.** User mode cannot write HKLM (`registry.c` denies `KEY_SET_VALUE` and `KEY_CREATE_SUB_KEY`
under `\Registry\Machine`), so the record a driver needs cannot be created by a user PE. The kernel command line
`ntpers_driver=NAME:\SystemRoot\...` installs it once, through `drvhost_service_add`. S13 is booted with that parameter.

**S13 client** (`tests/kernel/s13/console_s13.c`, `make -C tests/kernel s13`): `NtLoadDriver`, `NtCreateFile` on
`\??\HelloDrv`, `IOCTL_HELLO_PING` (expects `'HELO'`, version `0x00010000`), `IOCTL_HELLO_ADD` (`0xFFFFFFFF + 1`,
expects `0x100000000`), `NtClose`, `NtUnloadDriver`. Expected values are in the source. Exit 0 on pass; 12, 13, 14, 15,
16, 17 name the first failing step.

**S13 result: PASSES** (`tests/kernel/s13/console_s13.c`, exit 0). The client logs `S13 <step> begin` and `S13 <step> end
status <n>` for load, open, ping, add, close and unload to the serial console, then `S13 PASS`. Every step returns 0, and
the IOCTL values are checked against constants written in the source (`'HELO'`, `0x00010000`, `0x100000000`).

**Root cause of the earlier oops (fixed).** S13 unloads the driver before the process exits. `vfree` returned the image
pages with their direct-map aliases still read-only: `set_memory_*` on the vmalloc image changes those aliases, and
`vzalloc` does not restore them unless the area is flagged `VM_FLUSH_RESET_PERMS`. The freed pages were then reused for a
page table, and the exit path's write faulted. Fix (`driver/drvhost_plat.c`): `dh_map_image` calls
`set_vm_flush_reset_perms`, and `dh_protect_rx` uses `set_memory_rox` for text and `ro`+`nx` or `rw`+`x`/`nx` otherwise,
one module-style call per combination, so no page is ever writable and executable. The execute bit is not skipped.

**Boots on the final kernel** (`bzImage` sha256 `6ee36f930060f2cd...`, copied to `C:\Users\Jackson\repos\linux-env\kernel-drvkernel`):
M0 42 (expected 42) PASS; S1 (no DLL) 0 PASS; S2 to S4 and S6 to S13 0 PASS; S5 53 PASS; no `Oops` or `BUG:` in any
serial log; NONVM probes `NTPERS_ALL_DONE`, 15 of 15 exit 0, match counts unchanged from the previous section.

**Boots on this kernel.** The full boot set (M0, S1 with no DLL, S2 to S12, NONVM probes) ran on the kernel with the
bridge, before the final load-path change, which only S13 exercises. M0 42 PASS, S1 0 PASS, S2 to S4 and S6 to S12 PASS
(S5 53), NONVM probes 15 of 15 exit 0 with the same match counts as the previous section. S13 PASS on the final kernel (see the S13 result above).

## Driver host in the kernel, partial (branch `drvkernel`, 2026-10-27)

**Scope.** The NT driver host (`src/kernel/nt/driver/`) is now compiled into the personality kernel, and
`NtLoadDriver` and `NtUnloadDriver` are wired as service entries. The IOCTL path is not: `NtCreateFile` and
`NtDeviceIoControlFile` do not reach driver devices yet, so S13 (hello_drv round trip) is not added. The driver host
is reached only through the two load and unload entries.

**Kernel build.** `vm/incremental-kernel.sh` under `flock ~/.kbuild.lock` (guest `kfast/linux-src`, `GUEST_REPO=drvkernel`).
`bzImage` sha256 `034ec21eba848a29...` (copied to `C:\Users\Jackson\repos\linux-env\kernel-drvkernel\`). Build:
`INCREMENTAL_OK`. kpatch: 94 applied, 26 applicable before the run, 0 blocked, 0 todo.

**What changed.**
- `drvhost_ex.c`: `__thread dh_irql` and `dh_tls_thread` are per-task slots in the kernel (`drvhost_plat.c`, 256 slots
  keyed by `current`; a kthread releases its slot at exit). `setjmp`/`longjmp` in thread exit are replaced by
  `thread_finish` and `dh_thread_exit_current` (`kthread_exit`). The host build keeps `__thread` and `setjmp`.
- Bugcheck panics in the kernel; the debug sink uses `pr_info`.
- `drvhost_libc.h` (new): the libc names the driver files use, from linux headers in the kernel build.
- `drvhost_rtl.c`: `MmIsAddressValid` uses `copy_from_kernel_nofault` in the kernel.
- `drvhost_svcdb.c`: `nt_types.h` and `ntstatus.h` by bare name (kpatch copies them beside it).
- `tools/ntpers_kpatch.py`: the walk searches `src/kernel/nt/driver/`, `src/loader/` and `src/ntdll/`. The label for
  a canonical copy names its real source. `arch/x86/nt/Makefile` lists the 12 driver objects, `pe_parse.o` and
  `nt_driver_svc.o`. `vm/incremental-kernel.sh` stages `src/loader/pe_parse.*`.
- `src/kernel/nt/nt_driver_svc.c` (new): `nt_svc_NtLoadDriver` and `nt_svc_NtUnloadDriver`, with weak fallbacks that
  return `STATUS_NOT_IMPLEMENTED`. `driver/drvhost_svc.c` copies the user `UNICODE_STRING` and calls the driver host.
- `nt_service_list.h`: `NT_SERVICE(0x0106, NtLoadDriver, 1)` and `NT_SERVICE(0x0107, NtUnloadDriver, 1)`.
  **The two numbers are repo-assigned and unverified.** The reference `ntdll.dll` on the guest
  (`~/base-dlls/windows/system32/`, 95 KB) is the repo's own build, not a Windows binary, and the repo has no source for
  these numbers. `NtDeviceIoControlFile` (0x0007) and `NtCreateFile` (0x0055) read from the same file match the table.
  Check the two numbers against a Windows build before any parity claim.
- `docs/status/SERVICES.md` and `TRACEABILITY.md` regenerated with `tools/service_inventory.py` and
  `tools/criteria_trace.py`. Total 171 to 173; IMPLEMENTED 68 to 70.

**Tests.**
- Host, guest (Debian 12): `tests/kernel-drivers` `make check`: `driver_unit_test` 101 checks, 0 failed;
  `driver_host_test` 97 checks, 0 failed. `tests/ntdll-host` `make test`: rc 0. `tests/kernel-host` `make check`:
  rc 0 (`kernel-host` after the `drvhost_svcdb.o` include-path fix). `tests/unit/test_ntpers_kpatch.py`: 20 OK.
- The 12 driver objects compile under the kernel flags with 0 warnings (out-of-tree `make M=`).

**Boots** (host QEMU, `boot-m0-host.ps1`, `-KernelDir kernel-drvkernel`, `log_buf_len=16M`, final kernel above):

| step | exit (expected) | verdict |
|---|---|---|
| M0 | 42 (42) | PASS |
| S1 (no DLL in the image) | 0 (0) | PASS |
| S2 | 0 (0) | PASS |
| S3 | 0 (0) | PASS |
| S4 | 0 (0) | PASS |
| S5 | 53 (53) | PASS |
| S6 | 0 (0) | PASS |
| S7 | 0 (0) | PASS |
| S8 | 0 (0) | PASS |
| S9 | 0 (0) | PASS |
| S10 | 0 (0) | PASS |
| S11 | 0 (0) | PASS (a rerun; the first run timed out under host load) |
| S12 | 0 (0) | PASS |
| S13 | not built | not added |

The S1 image must not carry the DLLs: with them packed, `binfmt_pe` loads `ntdll` and S1 exits 5. That was the
image build, not the kernel.

NONVM probes (`NONVM=1 probes-initramfs.sh`, one boot, `NTPERS_ALL_DONE` printed, all 15 exit 0). Compared with the
goldens (`tests/differential/golden/windows-26200.9457`) and the recorded table in `DIFFERENTIAL-RESULTS.md`:

| probe | recorded matched | this boot matched | verdict |
|---|---|---|---|
| probe_directory | 19 / 67 | 64 / 67 | FAIL |
| probe_exception | 61 / 76 | 61 / 76 | FAIL |
| probe_fileinfo | 183 / 221 | 183 / 221 | FAIL |
| probe_handles | 82 / 111 | 99 / 111 | FAIL |
| probe_namedpipe | 3 / 45 | 3 / 45 | FAIL |
| probe_ntstatus | 2191 / 2199 | 2191 / 2199 | FAIL |
| probe_path | 55105 / 72523 | 55105 / 72523 | FAIL |
| probe_peloader | 33 / 34 | 33 / 34 | FAIL |
| probe_process_thread | 0 / 182 | 0 / 182 | FAIL |
| probe_registry | 128 / 140 | 128 / 140 | FAIL |
| probe_section | 128 / 129 | 128 / 129 | FAIL |
| probe_sharemode | 458857 / 458857 | 458857 / 458857 | PASS |
| probe_sync | 0 / 74 | 0 / 74 | FAIL |
| probe_teb_peb | 81 / 104 | 81 / 104 | FAIL |
| probe_virtualquery | 128 / 139 | 139 / 139 | PASS |

No probe went down. Three rose (directory, handles, virtualquery). The recorded table predates later kernel commits,
so the rises are not attributed to this change. A strict A/B against a kernel without the driver host was not run.

**proc_thread_test (kernel-host) intermittent exit 139: root cause and fix.** Reproduced under load (8 parallel loops,
`SYNC_TEST_ITERATIONS=2000`): 2 crashes in 240 runs, then 1 in 480. It is a use-after-free of a mutant header in the
userspace model. `nt_abandon_owned_mutants` (`sync.c`) set `signal_state` under the header lock, dropped the lock, and
only then woke waiters. A waiter that woke on the signal could drop its reference and free the header in that gap; the
abandoner then touched the freed header (`wake_waiters_for_header`). The freed block was reused by a later allocation,
so the corruption later showed up in `ObDestroyHandleTable` (`handle.c:240`), in the next test's setup. The fix holds a
reference across the wake. `SANITIZE=address` reports the use-after-free with the free and allocation stacks, and the
new regression test `test_mutant_abandon_refcount_race` (`proc_thread_test.c`) reproduces it deterministically through a
test seam (`nt_test_abandon_gap_hook`, NULL in production, userspace model only). Unfixed: heap-use-after-free in
`wake_waiters_for_header`, every run. Fixed: passes. Stress after the fix: 480 of 480 runs rc 0 on the normal build and
480 of 480 on the ASan build, both under 8-way load.

**Remaining, in order.**
1. S13: an `hello_drv.sys` image (`tests/kernel-drivers`, MinGW) and a PE client that creates the service key, calls
   `NtLoadDriver`, opens the device, round-trips an IOCTL and calls `NtUnloadDriver`. Needs a bridge from the kernel
   handle table to driver-host file objects (`NtCreateFile`, `NtDeviceIoControlFile`), so it is a separate slice.
2. Verify `0x0106` and `0x0107` against a Windows build.
3. (done) The proc_thread_test exit 139 is fixed in its own slice, see above.
4. Per-task slot release at process exit. Slots are released for kthreads only; a user task keeps its slot until the
   table fills (256), and then the kernel panics with a clear message.

## User-mode exception delivery (branch `haiku/seh`, 2026-10-15)

Kernel: `bzImage` sha256 `993ed32c3566391c2f1638c9837d37063f14a17004e4106c447331e5cb4eacec` (1,135,616 bytes), built from
`~/syncfix/linux-src` under `flock ~/.kbuild.lock` with `tools/ntpers_kpatch.py` (80 applied, 15 applicable before the
run; 95 edits). Probes: `probes-nonvm.cpio.gz` sha256 `7915a31f79d6e6758da4a2d41d64980c6349a1d65b69d67276ac1f6d56b495c3`,
the 15 NONVM probes built with `make -C src/ntdll all kernel32 msvcrt advapi32 user32` and `NONVM=1 probes-initramfs.sh`.

**What changed.** A user-mode page fault (`arch/x86/mm/fault.c`, `__bad_area_nosemaphore`), a user trap (`arch/x86/kernel/traps.c`,
`do_trap`: #DE, #OF, #BP, #UD) of an NT task is no longer a SIGSEGV. `src/kernel/nt/proc_thread.c` writes an EXCEPTION_RECORD
and a CONTEXT to the user stack (layout in `src/kernel/nt/exc_frame_pure.h`) and resumes the thread at
`KiUserExceptionDispatcher`, whose address `binfmt_pe.c` records per task. `NtContinue` and `NtRaiseException` now rewrite the
trap frame instead of returning STATUS_NOT_IMPLEMENTED; `do_nt_syscall` keeps the RAX they set, and `nt_can_sysret` takes the IRET
path for a non-user RIP. `KiUserExceptionDispatcher` (`src/ntdll/ntdll_main.c`) calls `NtContinue` when a handler resumes, and
ends the process with the exception code otherwise.

**Tests.** `tests/kernel-host/exc_deliver_test` (guest, gcc): trap and access-type mapping, frame alignment and
non-overlap over 200000 stack pointers, EFLAGS merge, Windows CONTEXT (0x4D0) and EXCEPTION_RECORD (0x98) sizes: 6 groups, 0 failed.
`tests/unit/test_ntpers_kpatch.py`: 20 OK (new anchors asserted). Host unit suite: 836 tests OK.
Not compiled here: the `__KERNEL__` branch of `proc_thread.c` and `dispatch.c` (compiled only by the kernel build, which passed).

**Boot (host QEMU, `boot-m0-host.ps1 -Mem 2048 -Append "... log_buf_len=16M"`):** all 15 probes reach `NTPERS_ALL_DONE`
and every probe exits 0. Before this change `probe_exception` exited 139.

| probe | matched / golden | verdict | before |
|---|---|---|---|
| probe_exception | 61 / 76 | FAIL | 0 / 76, exit 139 |
| probe_fileinfo | 183 / 221 | FAIL | 165 / 221 |
| probe_path | 55105 / 72523 | FAIL | 51355 / 72523 |
| probe_teb_peb | 81 / 104 | FAIL | 79 / 104 |
| probe_sharemode | 458857 / 458857 | PASS | same |

No probe went down; the rest are the same as the last recorded run. Full table in `DIFFERENTIAL-RESULTS.md`.

**Not done (remaining probe_exception gaps):** guard pages (`PAGE_GUARD`) are still reported as ACCESS_VIOLATION, not
GUARD_PAGE_VIOLATION (`is_guard_page_hit` is 0 on this path); the FP/XMM state is not carried in the CONTEXT (CONTEXT_CONTROL and
INTEGER only); `TestAlert` is not modelled in the kernel branch of NtContinue; `RtlRaiseException` still dispatches in-process
(`unwind.c`), not through the kernel.

Kernel: Linux **6.18.56** (newest 6.18.y in kernel.org `sha256sums.asc`), built in the Debian 12 guest
(`~/ntpers/linux-6.18.56`) with the NT wiring from `tools/ntpers_kpatch.py`. Session date 2026-10-13.

## Handle release at process exit, S11 (2026-10-15)

Branch `haiku/hndl` on main `3fac7e9`. Fixes the exhaustion left open by S10: each PE exec took up to three of the
1024 global handle slots and nothing freed them, so about 320 execs failed with ENOMEM. A PE's pipe ends stayed open.

**What changed.**
- `io.c`: each handle slot records its owner, the calling process id (`tgid` in the kernel; the process id in the
  userspace shim). The id is read without creating a process object, because creating one during exec captured a
  null PEB in an earlier attempt. `ntio_release_owned_handles(owner)` frees every slot the owner holds, including the
  standard handles and leaked ones, and drops each object's reference the way NtClose does (file put, delete on close,
  IOCP put). A file's last reference closes it, so a pipe's ends close with it.
- kernel hook: `tools/ntpers_kpatch.py` inserts `nt_process_exit_handles()` into `kernel/exit.c` right after
  `group_dead = atomic_dec_and_test(&tsk->signal->live)`, so it runs once, when the last thread of the group goes.
- `dispatch.c`: debugfs `ntpers/handles_in_use` (slots in use). S11 reads it before and after the runs.
- `weak.c`: weak default for `nt_exc_user_trap` and `nt_exc_user_fault`. The shared kernel tree had call sites in
  `arch/x86/kernel/traps.c` and `fault.c` with no definition, so the link failed. A strong definition replaces the default.
- ADR 0007 status updated: owner recorded and freed at exit; the lookup ownership check and duplicate rule are still open.

**Tests.**
- Kernel-host (`io_test`, new): `process_exit_releases_handles` (1100 cycles of 3 std handles with no close, 3300
  slots, more than the table), `process_exit_closes_file`, `process_exit_spares_other_owners`. `io_test` 40 of 40 on the
  host; `io_test rc=0` in the guest. `make -C tests/kernel-host check-quick` in the guest: `KH_RC=0`.
- ntdll-host, in the guest: `make test` exits 0, all tests passed (run before the pid-keyed owner change; ntdll code is unchanged).
- Host unit suite: `python -m unittest discover tests/unit`: 836 tests OK, 1 skipped (WSL). The kpatch fixture
  `kernel/exit.c` gained the `group_dead` line so the new hook can be checked.
- Booted S11 (`tests/kernel/s11/s11.step`, `STEP_SCRIPT` in `step-initramfs.sh`): `console_s11.exe` runs 1500 times
  from the init shell loop, each must exit 0, and the handle count must return to the baseline after the loop and after
  the pipe run. `pipe_s11.exe` runs as `echo pipe-payload | pipe_s11.exe` and asserts `GetFileType == FILE_TYPE_PIPE`
  (3), reads the 13-byte payload, and exits 0.

**Boots (host QEMU, `boot-m0-host.ps1`, bzImage built from `~/syncfix/linux-src` with `vm/incremental-kernel.sh --all`
under `flock ~/.kbuild.lock`).**

| step | expected | result | serial line |
|---|---|---|---|
| M0 | 42 | 42, PASS | `M0 exit status: 42 (expected 42)`, `M0 PASS` |
| S1 | 0 | 0, PASS | `hello from NT`; image without the ntdll DLLs (see below) |
| S2 | 0 | 0, PASS | `STEP PASS` |
| S3 | 0 | 0, PASS | `STEP PASS` |
| S4 | 0 | 0, PASS | `STEP PASS` |
| S5 | 53 | 53, PASS | `STEP exit status: 53 (expected 53)` |
| S6 | 0 | 0, PASS | `STEP PASS` |
| S7 | 0 | 0, PASS | `STEP PASS` |
| S8 | 0 | 0, PASS | `STEP PASS` |
| S9 | 0 | 0, PASS | `STEP PASS` |
| S10 | 0 | 0, PASS | `STEP PASS` |
| S11 | 0 | 0, PASS | `S11 PASS` |

S1 is the raw-syscall test on the no-ntdll path. Its image must not contain `windows/system32/*.dll`: with ntdll
present the loader takes the other path, and S1 read a null `ProcessParameters` (segfault at 0x28). The first
boot batch packed the DLLs into every image, which caused that failure. It is an image error, not a kernel
regression, and the S1 image was rebuilt without them.

## Standard handles: fd 0, 1, 2 at exec, S10 (2026-10-14)

Branch `haiku/stdio` on main `4b012b4` (rebased from `256571a`). Kernel: `bzImage` sha256 `936c64e7d5e67617bc806ff8641b8679ffd212586d62147dd52f7e9a6e3916c9`
(1,135,616 bytes), built in the guest from `~/syncfix/linux-src` with `vm/incremental-kernel.sh` on the rebased tree
(CCACHE=0, JOBS=4): 2 NT files copied, `CC fs/binfmt_pe.o` and `CC arch/x86/nt/io.o`, `INCREMENTAL_OK`. (The first build on `256571a` copied all 88 and had the sha256 `a9372827...`.)

**What changed.**
- `binfmt_pe.c`: at exec, the task's fd 0, 1 and 2 each become a File handle. They are written to
  `RTL_USER_PROCESS_PARAMETERS.StandardInput/StandardOutput/StandardError` (+0x20, +0x28, +0x30). An fd that is
  not open leaves its field 0. If a later step fails, the handles already installed are released
  (`ntio_release_std_handle`).
- `io.c`: a standard handle's granted access is FILE_GENERIC_READ and/or FILE_GENERIC_WRITE from the fd's mode, so
  a write to a read-only stdin fails. FileFsDeviceInformation reports a named pipe (FIFO or socket), a console
  (character device) or disk. kernel32 maps these to FILE_TYPE_PIPE, FILE_TYPE_CHAR and FILE_TYPE_DISK.
- `msvcrt.c` / `msvcrt.def`: stdout no longer falls back to the stdin handle. fprintf, vfprintf, fputc and fputs send
  `&g_iob[2]` (stderr) to the standard error handle. `fread` on stdin is new (reads the input handle).
- kernel32 (`GetStdHandle`, `SetStdHandle`, `ReadFile`, `WriteFile`, `WriteConsole`, `GetFileType`) already read the
  PEB slots and needed no change.

**Limits, not fixed.**
- The handle table is still global (`ntio_handles` in `io.c`), not per process. Each PE exec takes up to three of its
  1024 slots (64 are reserved), and nothing releases them at process exit. About 320 PE execs fill the table, and then
  exec fails with ENOMEM. A PE's pipe ends stay open after it exits.
- No runtime test covers a pipe standard handle (FILE_TYPE_PIPE). The boots use a regular-file stdin and the console.
- The kernel-host test `std_handle_install_release` covers install, read through the handle, and release. It cannot
  check the device type or the access split, because the host model keeps no `f_mode` or inode.

**Tests.**
- Kernel-host, in the guest: `make -C tests/kernel-host check` exits 0. `io_test rc=0` (including the new test),
  `binfmt_pe_test rc=0`, 25 programs rc=0 in total.
- ntdll-host, in the guest: `make -C tests/ntdll-host test` exits 0, all tests passed.
- Host unit suite: `python -m unittest discover tests/unit`, 836 tests OK, 1 skipped (WSL).
- Guest builds: `make -C src/ntdll all kernel32 msvcrt advapi32 user32` and `make -C tests/kernel all s2..s10` exit 0.

**Boots (host QEMU, `boot-m0-host.ps1`, the bzImage above).**

| step | expected | result | serial line |
|---|---|---|---|
| M0 | 42 | 42, PASS | `M0 exit status: 42 (expected 42)` |
| S1 | 0 | 0, PASS | `hello from NT` |
| S2 | 0 | 0, PASS | `hello from NT via ntdll` |
| S3 | 0 | 0, PASS | `hello from kernel32`, `file payload from S3` |
| S4 | 0 | 0, PASS | `hello from msvcrt 4 printf`, `hello from msvcrt puts` |
| S5 | 53 | 53, PASS | `STEP exit status: 53 (expected 53)` |
| S6 | 0 | 0, PASS | `S6 GetProcAddress kernel32 GetCurrentProcessId: match` |
| S7 | 0 | 0, PASS | `STEP PASS` |
| S8 | 0 | 0, PASS | `S8 command line: 5 args ok; environment NTPERS_S8 ok` |
| S9 | 0 | 0, PASS | `S9 environment: get, set, delete, expand, command line ok` |
| S10 | 0 | 0, PASS | `S10 out: alpha` / `S10 err: alpha` (and the same for `beta gamma` and `3`), then `S10 stdin/stdout/stderr round trip ok` |

S10 is `tests/kernel/s10/console_s10.c`. Its stdin is `tests/kernel/s10/console_s10.stdin` (19 bytes, three lines),
fed through the new `STEP_STDIN` option of `tools/linux-env/step-initramfs.sh`. It checks the file types, reads to
end of input, echoes each line to stdout and stderr, checks that a write to stdin fails, and checks
`SetStdHandle`. Exit codes 10 to 25 name the failed check.

The first S2 to S10 images were packed without `ntdll.dll`, `kernel32.dll`, `msvcrt.dll`, `advapi32.dll` and
`user32.dll`. Those boots exited 139. The images were rebuilt with the five DLLs under `/windows/system32`, the
recipe these steps use, and all of them pass. The 139s were a packing error, not the kernel change.

**NONVM probes (`NONVM=1 probes-initramfs.sh`, `log_buf_len=16M`, host timeout 1800 s).** All 15 reach
`NTPERS_ALL_DONE`. `probe_peloader` now receives its three stdin paths and returns three records. Before this change its
output was empty. Counts against `tests/differential/golden/windows-26200.9457` (`probe_results.py split` and `compare`)
on main `4b012b4` (`probe_fileinfo` 183 and `probe_ntstatus` 2191 are from that commit; the other 13 equal the 252fa28 table above):

| probe | matched / golden | verdict |
|---|---|---|
| probe_sharemode | 458857 / 458857 | PASS |
| probe_peloader | 33 / 34 | FAIL (`S1 divergences 1 > allowed 0`) |
| probe_path | 51355 / 72523 | FAIL |
| probe_ntstatus | 2191 / 2199 | FAIL |
| the other 11 | see `compare.json` | FAIL |

**probe_peloader is not a match, and its 33 of 34 is not a meaningful count.** The one divergence is the
`vacuous` guard in `tests/differential/dualrun.py`. Its mandatory key for this probe is `tests`, which neither side
has. The comparator then does not descend into the `results` list. The first record shows the difference: the Windows
golden for `C:/Windows/System32/notepad.exe` has `sec_image_accept: true`, `loadlibrary_accept: true` and
`bin_type: 6`. The VM record has `false`, `false` and `0`, with `file_open_error: 2`. The image does not ship those
Windows files, and the batch lists them. So the probe reads its stdin correctly, and it cannot match the golden with
this image. Thresholds, goldens and the comparator are unchanged.

## kernel32 environment API, msvcrt argv, probe argv (2026-10-14)

Branch `haiku/k32a` on the kernel boot at `252fa28` (same bzImage, sha256 starts `16b98d6e887d6a45`, unchanged kernel).
Userspace only: no kernel source changed.

**kernel32 environment API (`src/ntdll/kernel32_env.c`, rules in `k32_env_pure.h`).** Added GetCommandLineA,
GetEnvironmentStringsA, FreeEnvironmentStringsA/W, GetEnvironmentVariableA/W, SetEnvironmentVariableA/W,
ExpandEnvironmentStringsA/W. ANSI means CP_ACP, which is Windows-1252 in this shim (same as MultiByteToWideChar),
not UTF-8. Divergences, documented in the file: GetEnvironmentStringsW returns the live block (S8 asserts that),
and FreeEnvironmentStringsW accepts only that block. Set replaces a variable in place, appends new ones, and
replaces the process block with a heap copy as RtlSetEnvironmentVariable does.

**msvcrt argv (`src/ntdll/msvcrt.c`, `ms_split_cmdline` in `msvcrt_policy.h`).** `__getmainargs` hard-coded
argc=1, so no program ever saw its arguments. It now splits the command line by the Windows quoting rules.
**msvcrt stdin.** `fgets` was a stub that returned NULL. It now reads the standard input handle.

**Probe argv (`tools/linux-env/probes-argv.manifest`, `probes-initramfs.sh`).** One line per probe gives argv
and a stdin file. probe_peloader runs as `--batch` with the three golden inputs on stdin.

**Results (host QEMU, `boot-m0-host.ps1`, bzImage `252fa28`):**

| step | expected | result |
|---|---|---|
| S4 console_s4 (msvcrt startup change) | 0 | 0, `STEP PASS` |
| S8 console_s8 | 0 | 0, `STEP PASS` |
| S9 console_s9 (new) | 0 | 0, `STEP PASS` |
| NONVM probes | all 15 reach `NTPERS_ALL_DONE` | yes; 14 exit 0, probe_exception 139 as before |

Golden match (`probe_results.py compare`), the same counts as the 252fa28 section for every probe:
probe_sharemode 458857 of 458857 (PASS); the other 14 FAIL, with the same matched counts as before.

**probe_peloader is not matched.** Its argv now reaches `main` and it enters `--batch`. Its stdin
does not: `binfmt_pe` leaves `PEB->ProcessParameters->StandardInput` at 0 until a per-process handle table exists
(`src/kernel/fs/binfmt_pe.c`, the comment at the StandardOutput block). So `fgets` gets no input and the
result list is empty. The 33 of 34 match is the empty result against the golden, so it is not a pass. Wiring
stdin needs a kernel change and a rebuild, which this change does not make.

**Tests run.** ntdll-host suite in the Linux guest (`make -C tests/ntdll-host test`, ASan and UBSan),
all suites pass, including `test_k32_env_pure` and the new `test_msvcrt_policy` split cases. The Windows-host
build of `test_k32_env_pure.exe` and `test_unwind_pure.exe` was refused at run time by Application Control
(the MAIN-1 policy, `src/ntdll/Makefile`); those binaries were not renamed or moved. Host unit suite
`python -m unittest discover tests/unit`: 832 tests OK, 1 skipped (WSL).


## 252fa28: PE process parameters, boot verification (2026-10-14)

Commit `252fa28` (loader: fill PEB process parameters) changed `binfmt_pe.c`: `pe_build_process_parameters`
fills ImagePathName, CommandLine and Environment from argv and envp (read from `mm->arg_start`). The kernel
had compiled but never been booted. This section boots it.

Kernel: `bzImage` built incrementally in the guest at `~/loader` (the `~/kfast/linux-src` tree, rebuilt
16:55 with `vm/incremental-kernel.sh`), sha256 `16b98d6e887d6a45c5efeb50e760d234cb1eb97fefe5534e78ad648b7de5e6dc`,
1,135,616 bytes. Its NT sources match `252fa28`: all 87 entries of the guest manifest hash to the same
content as the blobs at `252fa28` (CRLF removed). Userspace PEs and DLLs were built in `~/loader/repo` from
the same tree (`make -C src/ntdll all kernel32 msvcrt advapi32 user32`, `make -C tests/kernel all s2..s8`).
Host boots: `tools/linux-env/boot-m0-host.ps1` with `-KernelDir` set to a copy of the bzImage and the
initramfs images (`C:\Users\Jackson\repos\linux-env\kernel-252fa28`), QEMU TCG, `-nic none`.

| step | PE | exit status (expected) | verdict | serial line |
|---|---|---|---|---|
| M0 | `minimal.exe` | 42 (42) | PASS | `M0 exit status: 42 (expected 42)`, `M0 PASS` |
| S1 | `console_s1.exe` | 0 (0) | PASS | `hello from NT` |
| S2 | `console_s2.exe` | 0 (0) | PASS | `hello from NT via ntdll` |
| S3 | `console_s3.exe` | 0 (0) | PASS | `hello from kernel32`, `file payload from S3` |
| S4 | `console_s4.exe` | 0 (0) | PASS | `hello from msvcrt 4 printf`, `hello from msvcrt puts` |
| S5 | `console_s5.exe` | 53 (53) | PASS | `STEP exit status: 53 (expected 53)` |
| S6 | `console_s6.exe` | 0 (0) | PASS | `S6 GetProcAddress kernel32 GetCurrentProcessId: match` |
| S7 | `console_s7.exe` | 0 (0) | PASS | `STEP exit status: 0 (expected 0)`, `STEP PASS` |
| S8 | `console_s8.exe` (new, below) | 0 (0) | PASS | `S8 command line: 5 args ok; environment NTPERS_S8 ok` |

S1-S7 were booted with `log_buf_len=16M` appended to the kernel command line (an extra argument in the
loop, not the S-step default). It only sizes the printk ring. S8 and M0 used the default command line.

**S8: the exe sees its own command line and environment.** New fixture `tests/kernel/s8/console_s8.c`
(`make -C tests/kernel s8`). It starts with argv `alpha 'beta gamma' 3 'say"hi'` and the environment
`NTPERS_S8=alpha-env` (both through `STEP_ARGS` and `STEP_ENV` in `tools/linux-env/step-initramfs.sh`).
The exe reads `PEB->ProcessParameters` (`%gs:0x60`, `+0x20`) and checks, with values written in the
source and not read from a recorded output: five arguments split by the Windows rules; argv[0] ends with
`console_s8.exe`; argv[1..4] are `alpha`, `beta gamma` (quoted by the kernel), `3`, and `say"hi` (the
kernel writes the quote as `\"`); the environment block contains `NTPERS_S8=alpha-env` and its
`EnvironmentSize` covers the block up to its double NUL; ImagePathName ends with `console_s8.exe`;
`GetCommandLineW` and `GetEnvironmentStringsW` return the same buffers as the PEB. Exit codes 10 to 22
name the failed check. Negative control: the same image with `STEP_ENV` unset exits 18 (the environment
check fails and says so), as expected.

**Gap found and fixed.** The kernel32 shim did not export `GetCommandLineW` or `GetEnvironmentStringsW`,
so a PE that called them could not link. Added in `src/ntdll/kernel32.c` (they return
`PEB->ProcessParameters` CommandLine.Buffer and Environment) and in `src/ntdll/kernel32.def`.
`GetCommandLineA` is still missing. The S8 image was rebuilt with the new kernel32.dll and booted again
(PASS above).

**NONVM probes** (`NONVM=1 tools/linux-env/probes-initramfs.sh`, `log_buf_len=16M`, host timeout 1800 s).
All 15 reach `NTPERS_ALL_DONE`; 14 exit 0 and `probe_exception` exits 139, as before. Compared with the
goldens in `tests/differential/golden/windows-26200.9457` (`probe_results.py split` and `compare`):

| probe | matched / golden | 0d19a62 |
|---|---|---|
| probe_directory | 19 / 67 | 19 / 67 |
| probe_exception | 0 / 76 | 0 / 76 |
| probe_fileinfo | 165 / 221 | 165 / 221 |
| probe_handles | 82 / 111 | 82 / 111 |
| probe_namedpipe | 3 / 45 | 3 / 45 |
| probe_ntstatus | 2190 / 2199 | 2190 / 2199 |
| probe_path | 51355 / 72523 | 51355 / 72523 |
| probe_peloader | 33 / 34 | 33 / 34 |
| probe_process_thread | 0 / 182 | 0 / 182 |
| probe_registry | 128 / 140 | 128 / 140 |
| probe_section | 128 / 129 | 128 / 129 |
| probe_sharemode | 458857 / 458857 (PASS) | 458857 / 458857 (PASS) |
| probe_sync | 0 / 74 | 0 / 74 |
| probe_teb_peb | 81 / 104 | 79 / 104 |
| probe_virtualquery | 128 / 139 | 128 / 139 |

No regression. `probe_teb_peb` gained two fields. The NONVM image was packed with the kernel32.dll built
before the two new exports; the exports do not change any probe's path.

**Finding, not fixed: `probe_peloader` runs without an argument.** Its output is
`no path provided, usage: probe_pe...`, so its one-field match (33 of 34) compares an empty result with the
golden and is vacuous (`compare.json` marks the field `vacuous`, severity S1). The NONVM framework starts
every probe with no arguments. A probe that needs an argument can only be given one by extending the
probes init, which is out of scope here.

## Min profile (2026-10-14): smaller kernel, same M0 and S1-S4 result

The default `.config` is now `PROFILE=min` (`vm/make-kernel-config.sh`, BUILD-RECIPE 6). It drops the
networking stack, the block layer, the 9p and virtio transports, BPF, VT and the input stack, the extra
decompressors and CPU vendors, and modules. Measured in a separate guest tree, `~/kfast/linux-src`, extracted
from the same `linux-6.18.56.tar.xz` (the other agents' trees were not touched):

| | full | min |
|---|---|---|
| enabled options | 611 | 469 |
| compile steps, `make bzImage` (unique objects, dry run) | 1044 | 608 |
| wall time, `make -j2`, ccache cold | estimate about 3 h 40 min | 7,720 s (607 compile lines, 2 h 8 min) |
| `bzImage` | 1.9 MB (earlier) | 1.1 MB |

Boots on host QEMU (`tools/linux-env/boot-m0-host.ps1`, TCG, `-kernel`, the initramfs images from `~/ntpers`,
`step-initramfs.sh` and `m0-initramfs.sh`), run twice: once after the first build, and again on the image
produced by the incremental rebuild (BUILD-RECIPE 8a), which is the final state:

| Test | Initramfs | Expected | Result (final image) | Serial line |
|---|---|---|---|---|
| M0 | `m0-initramfs.cpio.gz` | 42 | PASS | `M0 exit status: 42 (expected 42)`, `M0 PASS` |
| S1 | `s1-initramfs.cpio.gz` | 0 | PASS | `hello from NT` |
| S2 | `s2-initramfs.cpio.gz` | 0 | PASS | `hello from NT via ntdll` |
| S3 | `s3-initramfs.cpio.gz` | 0 | PASS | `hello from kernel32`, `file payload from S3` |
| S4 | `s4-initramfs.cpio.gz` | 0 | PASS | `hello from msvcrt 4 printf`, `hello from msvcrt puts` |

The userspace images (PEs, ntdll, kernel32, msvcrt) were not rebuilt; they are the ones that passed S1-S4 on the
full kernel. Only the kernel changed.

Incremental rebuild (`vm/incremental-kernel.sh`): a one-file edit in `src/kernel/nt/handle.c`, then the revert:
2 min 39 s each run, one NT object recompiled, and the manifest brought the guest back to the committed source
(the first version of the script compared with git and missed the revert; fixed before the timed run).

Unit suite (`python3 -m unittest discover tests/unit`, in the guest): 758 tests, 1 failure, 1 skipped (WSL).
The failure is `test_golden_accepted_builds.ShippedAcceptedListTest.test_shipped_list_validates_and_matches_corpus`.
The accepted list records the sha256 of the CRLF form of `windows-26200.9457/version.json` and
`manifest.json` (`822d95...`). The guest copy is LF, because `tools/linux-env/sync-repo.sh` normalises
line endings, so it fails there. The same test passes on a CRLF copy of the pre-change commit `6582899`.
This is not caused by the profile change, but the test needs a CRLF checkout to pass.

The completeness gate (`tests/kernel/check_kernel_complete.sh`) was not run on the min image; the open
threshold question in the section below is unchanged.

## Summary (the five-line report)

1. Downloaded: yes. `linux-6.18.56.tar.xz`, SHA256 matches the kernel.org checksum file. The PGP signature on that file is **not** verified (see LINUX-ENV.md).
2. Patched: yes. `ntpers_kpatch.py --check` reports 75 applicable, 0 blocked, 0 todo; the apply wrote 69 files.
3. Configured: yes. `vm/make-kernel-config.sh` (tinyconfig base, 64-bit, PASS).
4. Built: yes. `arch/x86/boot/bzImage` (1.9 MB) links. The repo's completeness gate `tests/kernel/check_kernel_complete.sh` **fails** (see "Completeness gate").
5. Booted: yes, under the host QEMU with `-kernel`. Banner: yes. M0: **PASS** (exit status 42).

## Boot and M0 result (serial, verbatim)

From `C:\Users\Jackson\repos\linux-env\kernel-6.18.56\m0-serial.log`, the boot that passed:

```
NT personality: ntpers v1.0 banner
Run /init as init process
=== M0 initramfs init (no 9p share) ===
binfmt_pe: headers ok imagebase=0x140000000 entry=0x1000 size=0x6000 sections=5 stack_res=0x200000
binfmt_pe: ntdll not mapped, falling back to exe entry
binfmt_pe: loading PE /tests_kernel/minimal.exe pid=25
ObRegisterThread: thr=00000000bfaeda1c tid=25 obj=00000000a4f0359c
binfmt_pe: after nt_enter TIF_NTABI=1
M0 exit status: 42 (expected 42)
M0 PASS
sysrq: Power Off
reboot: Power down
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000000
```

The final panic line is printed because `/init` exits as the power-off completes. It comes after the
verdict and does not change it.

## Deviations from the recipe (and why)

- **Init and runner.** `vm/mkinitramfs.sh` and `vm/runvm.sh` need the QEMU 9p share, which QEMU for
  Windows does not provide (LINUX-ENV.md blocker 4). The M0 run used `tools/linux-env/m0-initramfs.sh`,
  whose `/init` runs `/tests_kernel/minimal.exe`, prints the status, and powers off. It boots with
  `tools/linux-env/boot-m0-host.ps1` (host QEMU, `-kernel`, TCG, `-nic none`). `vm/check_m0.sh` and
  `vm/test_m0.sh` were **not** run; their result file is written to the 9p share.
  Later, `VM_TRANSPORT=initrd-serial` and `tools/linux-env/run-vm.ps1` ran the same M0 payload
  through the repository's own test script and collector; see `docs/vm/TRANSPORTS.md`.
- **Test binary.** `tests/kernel/bin/minimal.exe` built in the guest with `x86_64-w64-mingw32-gcc`
  (PE32+, x86-64). The guest copy of the repo was LF-normalised before the build.
- **Config base.** tinyconfig with the required list, not defconfig (`BASE` in the script; BUILD-RECIPE G19).

## Fixes made in this session (one commit each)

| Commit | Change | Why |
|---|---|---|
| 9512d20 | `kpatch`: NT branch uses `syscall_enter_from_user_mode()`; enter.c todo removed | G3/G6: old branch left IRQs off and skipped seccomp, ptrace and audit; 6.18 declares `syscall_trace_enter()` |
| efa2ae7 | `vm/make-kernel-config.sh`: tinyconfig base, 64BIT, `bash scripts/config`, PRINTK/TTY/SHMEM/VIRTIO_MENU/NET/BLOCK | G19; tinyconfig is 32-bit and omits these |
| 31010f4 | `src/kernel/nt/kshim.h`: userspace `kstrdup` guarded by `!__KERNEL__` | conflicting `kstrdup` prototype (`int` vs `gfp_t`) |
| 5b32fca | `src/kernel/fs/binfmt_pe.c`: `register_binfmt()` returns void in 6.18 | compile error: void value not ignored |
| 883e59e | `job.c`, `debugobj.c`: `<stddef.h>`/`<string.h>` guarded | not available in a kernel build |
| 11f4fdd | `arch/x86/nt/nt_services.c`: `NT_MAX_HARDERROR_PARAMETERS` moved out of the userspace guard; `TRUE`/`FALSE` removed from the kernel branch | undeclared identifiers |
| c905982 | `src/kernel/nt/sync.c`: `bool equal;` declared | undeclared identifier |
| fbe6add | `src/kernel/nt/kshim.h`: `#include <linux/uaccess.h>` | link: undefined `copy_to_user`, `copy_from_user`, `access_ok` (warnings hidden by `-w`) |
| 743e4f3 | `src/kernel/nt/nt_obj.c`: kernel copy of `k_copy_oa` | link: undefined `k_copy_oa` (its only definition is static in unbuilt `registry.c`, G9) |
| 6a9da7f | `arch/x86/nt/nt_io.c`: `ntio_file_section_size`/`_extend` | link: undefined; the definitions were only in unbuilt `io.c` |
| c2813ac | `vm/make-kernel-config.sh`: `BINFMT_SCRIPT` | `/init` is a shell script; exec returned ENOEXEC (-8) |
| d9465a3 | `tools/linux-env/m0-initramfs.sh`, `boot-m0-host.ps1` | the 9p-free M0 path |

Docs: 7eeb614 (download log), 6836970 (BUILD-RECIPE: G1 partly, G3 and G4 closed, G19, 5i, 5l).

## Completeness gate (`tests/kernel/check_kernel_complete.sh`): FAIL

Run against `vmlinux` in the guest (`KDIR=~/ntpers/linux-6.18.56`). Exit status 1.

```
OK: do_nt_syscall ... OK: nt_banner_init            (14 symbols OK)
nt_svc_* count: 131 (expected >=350)
MISSING: nt_svc_* count 131 < 350 (expected 355)
built-in.a object count: 20 (expected 17)
OK object: dispatch.o ... OK object: kernel_stubs.o   (all 17 named objects OK)
ERROR: NT kernel incomplete - missing 1 symbols/objects: nt_svc_count:131
```

Analysis, not yet acted on:

- The service table `arch/x86/nt/dispatch.c` is generated from `arch/x86/nt/nt_service_list.h`, which has
  **171** `NT_SERVICE` lines. The gate expects 355. The two numbers disagree in the repo, so the threshold
  cannot be met from this list alone (similar to G8).
- 131 `nt_svc_*` symbols are defined. 43 listed services are weak, undefined, and therefore NULL in the
  table; the dispatcher returns `STATUS_NOT_IMPLEMENTED` for them (`dispatch.c`, `!svc->fn`). The M0 services
  (`NtTerminateProcess`, `NtWriteFile`, `NtClose`, `NtCreateFile`, `NtReadFile`) are all defined.
- `built-in.a` has 20 objects (the `arch/x86/nt/Makefile` list). The gate's 17 is the G8 disagreement.

Decision needed (owner): fix the threshold and object list to match the repo, or implement the missing services.
This session did not change the gate.

## Completeness gate (after)

The gate was rewritten to derive its expectations from the repository (commits
`f030cf2`, `62543ae`, `ec51f20`, `9272761`, `2d60b0e`); see `tools/kernel_gate.py`,
the header of `tests/kernel/check_kernel_complete.sh` and BUILD-RECIPE 7. Run in the
guest on 2026-10-13: `cd ~/gate && bash tests/kernel/check_kernel_complete.sh
~/ntpers/linux-6.18.56/vmlinux`. `~/gate` is a `git archive` of `tools/`,
`src/kernel/nt/`, `arch/x86/nt/`, `tests/kernel/` and `docs/status/SERVICES.md` at
`2d60b0e`. The kernel tree was only read. Exit status 1.

```
Checking NT personality in /home/ntpers/ntpers/linux-6.18.56/vmlinux (KDIR=/home/ntpers/ntpers/linux-6.18.56, repo=/home/ntpers/gate) ...
NT kernel completeness gate
  repo:     /home/ntpers/gate
  nm:       /home/ntpers/ntpers/linux-6.18.56/vmlinux
  table:    171 services (src/kernel/nt/nt_service_list.h)
  expected: 159 kernel-branch strong handlers (IMPLEMENTED 154, REFUSES 0, STUB-NOT-IMPLEMENTED 5)
  present:  124 strong nt_svc_* symbols in the binary
SHORTFALL: 35 expected service(s) not strong in the binary
  NtAlertThread                    IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtAlertThreadByThreadId          IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtAlertResumeThread              IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtWaitForAlertByThreadId         IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtFlushBuffersFileEx             IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryDirectoryFile             IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryDirectoryFileEx           IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryVolumeInformationFile     IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtSetVolumeInformationFile       IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryEaFile                    IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtSetEaFile                      IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtLockFile                       IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtUnlockFile                     IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtCancelIoFile                   IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtCancelIoFileEx                 IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtDeviceIoControlFile            IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtFsControlFile                  IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtDeleteFile                     IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryAttributesFile            IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryFullAttributesFile        IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtCreateKey                      IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtOpenKey                        IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtOpenKeyEx                      IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtDeleteKey                      IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtFlushKey                       IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtEnumerateKey                   IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtEnumerateValueKey              IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtQueryKey                       IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtQueryValueKey                  IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtSetValueKey                    IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtDeleteValueKey                 IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtQuerySystemTime                IMPLEMENTED  absent                   src/kernel/nt/nt_services.c
  NtContinue                       IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtContinueEx                     IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtRaiseException                 IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
KERNEL GAPS: 12 service(s) with no kernel-branch strong handler (not failing)
  NtAllocateVirtualMemory          WEAK-FALLBACK            defined, weak binding
  NtAllocateVirtualMemoryEx        WEAK-FALLBACK            absent
  NtFreeVirtualMemory              WEAK-FALLBACK            defined, weak binding
  NtProtectVirtualMemory           WEAK-FALLBACK            defined, weak binding
  NtQueryVirtualMemory             WEAK-FALLBACK            defined, weak binding
  NtReadVirtualMemory              WEAK-FALLBACK            absent
  NtWriteVirtualMemory             WEAK-FALLBACK            absent
  NtFlushVirtualMemory             WEAK-FALLBACK            absent
  NtLockVirtualMemory              WEAK-FALLBACK            absent
  NtUnlockVirtualMemory            WEAK-FALLBACK            absent
  NtReplyWaitReceivePortEx         MISSING                  absent
  NtCloseObjectAuditAlarm          MISSING                  absent
KERNEL STUBS: 5 expected service(s) that return not-implemented (not failing)
  NtCreateProcess                  src/kernel/nt/proc_thread.c
  NtCreateProcessEx                src/kernel/nt/proc_thread.c
  NtGetContextThread               src/kernel/nt/proc_thread.c
  NtSetContextThread               src/kernel/nt/proc_thread.c
  NtExtendSection                  src/kernel/nt/mm.c
note: 3 strong nt_svc_* symbol(s) not in the table: NtOpenProcess NtOpenThread NtSetInformationDebugObject
entry points: 9 expected, 0 not strong
built-in.a: 20 expected objects from arch/x86/nt/Makefile, 0 missing
VERDICT: FAIL (35 expected service symbol(s) not strong in the binary)
EXIT=1
```

Reading the result:

- The gate now FAILS, as the rule says it should: 35 services that the kernel-branch
  inventory classifies as implemented are not in vmlinux. The object check passes:
  all 20 `arch/x86/nt/Makefile` objects are in `built-in.a`. The entry points are strong.
- The 48 services the kernel does not implement are 35 shortfalls, 8 with no handler
  symbol (6 weak-only handlers absent from the build, 2 with no handler in the
  repository), and 5 stubs. Four `dispatch.c` virtual-memory handlers run through
  weak binding. Pointers and the causes
  are in [KERNEL-GAPS.md](KERNEL-GAPS.md): 27 services sit in `io.c` and `registry.c`,
  which the kernel build does not compile; 8 are in `proc_thread.c` and `nt_services.c`,
  where the build uses the `arch/` copies.
- The old numbers could not pass. 355 matches no count the repository produces: there are
  171 table entries, and 1217 lines in the handler and arch sources mention `nt_svc_`.
  17 matches no list either: `arch/x86/nt/Makefile` has 20 objects. Two inventory defects had also inflated the
  expectation: the per-branch class was not computed, and a definition after the weak
  `NT_SERVICE` declaration in `dispatch.c` was counted as strong, which vmlinux shows as
  `W`. Both are fixed (`f030cf2`, `62543ae`).

## Remaining failures and gaps

1. **Completeness gate** now derives its expectations from the repository and FAILS on 35 expected
   handlers that vmlinux does not link. See "Completeness gate (after)" and [KERNEL-GAPS.md](KERNEL-GAPS.md).
   Closing those gaps is the open work; the gate itself is no longer the blocker (G8 closed).
2. **Warnings hidden.** `arch/x86/nt/Makefile` sets `ccflags-y += -w`. That hid the implicit-declaration
   errors that the link later reported. A build with warnings on (drop `-w`) is needed before trusting the
   NT sources. Not done here.
3. **G24 (which copy is canonical).** `src/kernel/nt/mm.c`, `mm.h` and `nt_service_list.h` overwrite
   `arch/x86/nt/` copies in the kpatch run (WARN G24). The kernel uses the `src` versions. Not decided.
4. **Seccomp self-test** from patch 0002 (`tools/testing/selftests/ntpers/seccomp_arch.c`) does not exist.
   The NT path now runs seccomp, but nothing tests it.
5. **Signature of `sha256sums.asc`** is not verified (LINUX-ENV.md).
6. **Userspace shim check** (`make -C tests/kernel-host check-quick`) fails **before and after** this session:
   at base commit `30f7e2b` it fails with the same nine errors: eight in `shim/khost_prelude.h` (`pread`/`pwrite`
   macros) and one in `shim/sys/stat.h` (`redefinition of 'struct stat'`) against glibc. Not caused by this
   session. The host has no make/gcc, so the check ran in the guest. Not fixed here.
7. **Boot path.** `vm/runvm.sh`, `vm/check_m0.sh`, `vm/freshness.sh` (stamp) and `vm/guest-bootstrap.sh`
   were not used. The freshness stamp (BUILD-RECIPE 10) was not written.
8. **Unit suite** (`python3 -m unittest discover tests/unit`, run in the guest, Python 3.11.2): 665 tests,
   1 error, 1 skipped (WSL). The error is `test_check_repo_extra.PerFileTests.test_nul_byte_past_probe_is_a_compile_failure_without_line_none`:
   `tools/check_repo.py` line 105 calls `compile()`, which raises `ValueError: source code string cannot contain
   null bytes` on this Python (3.11). The check only reports `SyntaxError`, so the error escapes instead of
   becoming a `py-compile` failure. (Python 3.12 and later are reported to raise `SyntaxError` here; not checked
   in this session.) Neither file changed in this session
   (`git diff 30f7e2b HEAD` is empty for both). The kpatch tests (19) pass, including the rewritten G3 and
   todo tests.

## Resume commands

Guest (SSH via `tools/linux-env/ssh-guest.sh`; the build runs detached, log in `~/ntpers/`):

```sh
cd ~/$GUEST_DEST                                        # sync first: GUEST_DEST=<name> tools/linux-env/sync-repo.sh <repo>
sed -i 's/\r$//' vm/make-kernel-config.sh tests/kernel/check_kernel_complete.sh
python3 tools/ntpers_kpatch.py --tree ~/ntpers/linux-6.18.56          # idempotent; copies changed sources
KDIR=$HOME/ntpers/linux-6.18.56 bash vm/make-kernel-config.sh         # PASS expected
cd ~/ntpers/linux-6.18.56 && make -k -j4 ARCH=x86_64 bzImage          # the pass that compiled the remaining ~400 objects took about 1 h under TCG; reruns recompile only changed objects
KDIR=$HOME/ntpers/linux-6.18.56 bash ~/ntpers-research/tests/kernel/check_kernel_complete.sh ~/ntpers/linux-6.18.56/vmlinux
sh ~/ntpers-research/tools/linux-env/m0-initramfs.sh ~/ntpers-research ~/ntpers/m0-initramfs.cpio.gz
```

Host (copy `bzImage` and `m0-initramfs.cpio.gz` from the guest with `scp -P 2222` into
`C:\Users\Jackson\repos\linux-env\kernel-6.18.56\`, then):

```powershell
powershell -ExecutionPolicy Bypass -File tools\linux-env\boot-m0-host.ps1 -TimeoutSec 240
```

Expected last lines: `M0 exit status: 42 (expected 42)` and `M0 PASS`.

## Unification (2026-10-13)

Branch `haiku/kgaps`, after the merge of `origin/main` (`37f21c5`). The kernel was built in
`~/kgaps/linux-6.18.56` (a private copy of the kernel tree, `nice -j2`), with
`tools/ntpers_kpatch.py` applied. `src/kernel/nt` is the only NT source tree; the `arch/x86/nt`
duplicates are deleted (see [tree-divergence.md](../analysis/tree-divergence.md), section 7).

**Completeness gate: PASS** (`tests/kernel/check_kernel_complete.sh`, `EXIT=0`):

```
table:    171 services (src/kernel/nt/nt_service_list.h)
expected: 171 kernel-branch strong handlers (IMPLEMENTED 160, REFUSES 0, STUB-NOT-IMPLEMENTED 11)
present:  171 strong nt_svc_* symbols in the binary
SHORTFALL: 0 expected service(s) not strong in the binary
KERNEL GAPS: 0 service(s) with no kernel-branch strong handler
KERNEL STUBS: 11 expected service(s) that return not-implemented (explicit refusals)
built-in.a: 21 expected objects from arch/x86/nt/Makefile, 0 missing
VERDICT: PASS
```

Services in the kernel: **171 of 171** have a strong handler in vmlinux. 160 are implemented. 11 are
explicit refusals: NtCreateProcess, NtCreateProcessEx, NtGetContextThread, NtSetContextThread,
NtExtendSection, NtAllocateVirtualMemoryEx, NtReadVirtualMemory, NtWriteVirtualMemory,
NtFlushVirtualMemory, NtReplyWaitReceivePortEx and NtCloseObjectAuditAlarm. They return
`STATUS_NOT_SUPPORTED`; the reasons are in `src/kernel/nt/dispatch.c` and `docs/status/KERNEL-GAPS.md`.

**M0: PASS** (`M0 exit status: 42 (expected 42)`, `M0 PASS`), booted on the host with
`tools/linux-env/boot-m0-host.ps1`.

**Console steps: S1 PASS, S2 PASS, S3 PASS, S4 PASS** (`STEP exit status: 0 (expected 0)`, `STEP PASS`),
each booted from `tools/linux-env/step-initramfs.sh` on the same `bzImage`. The initramfs for S2–S4 packs
`ntdll.dll`, `kernel32.dll` and `msvcrt.dll` under `/windows/system32`.

**Decisions recorded in this change:**
- Registry: in memory only in the kernel build (`NT_REGISTRY_HOST_FILES 0`). It does not survive a reboot.
- VAD coalescing: off, the src behaviour (matches the probe oracle). Owner decision, delegated.
- Virtual-memory services that have no kernel implementation refuse (see above).

**Unit suite** (`python3 -m unittest discover tests/unit`, guest, Python 3.11): 789 tests. Three failures,
none in the kernel:
- `test_check_doc_links`: the link to this section was missing; fixed by this section.
- `test_service_inventory`: `docs/status/SERVICES.md` was stale; regenerated.
- `test_golden_accepted_builds`: the accepted-list hash for `windows-26200.9457/version.json` matches the
  CRLF bytes of the Windows worktree, and the committed blob is LF. The test fails in the Linux guest
  on the base commit too. Not changed here.

## Merged kernel (kgaps + main)

Branch `haiku/integrate5`: merge `haiku/kgaps` (`ada9ce0`) into `main` (`6582899`), merge commit `6f91c3f`, then `db4ee08`
(README file count). Conflicts: `arch/x86/nt/proc_thread.c` (deleted by kgaps, modified on main) took the deletion after
checking that `src/kernel/nt/proc_thread.c` already has main's TEB-in-user-memory fix (`nt_alloc_teb_stack_kernel`,
`nt_new_thread_peb`, `nt_teb_set_tid`, same bodies as the arch copy). `README.md` took main and was regenerated.
Regenerated: `SERVICES.md`, `TRACEABILITY.md`, `CRITERIA.md`, `CRITERIA-ROADMAP.md` (the roadmap changed: three
`WI-POLICY-DOCS` rows now `HAS-HARNESS-ENTRY`), and the README counts.

Build: private tree `~/integ5/linux` (a copy of the built `~/kgaps/linux-6.18.56`), `tools/ntpers_kpatch.py --tree`
(74 applied, 11 applicable, 10 files written), `vm/make-kernel-config.sh` PASS, `make -k -j20 ARCH=x86_64 bzImage`
(incremental after the copy). Test PEs and DLLs built in `~/integ5/ntpers-research` (`tests/kernel`, `src/ntdll`).

**Completeness gate: PASS** (`tests/kernel/check_kernel_complete.sh`, exit 0): 171 services in the table, 0 shortfall,
0 kernel gaps, 11 explicit refusals, 21 built-in objects present, 9 entry points strong.

Boots (host `tools/linux-env/boot-m0-host.ps1`, the QEMU VM runs the NT kernel and the PE inside it; the Windows host
does not execute any PE):

| step | exit status (expected) | verdict |
|---|---|---|
| M0 | 42 (42) | PASS |
| S1 console_s1 | 0 (0) | PASS |
| S2 console_s2 | 0 (0) | PASS |
| S3 console_s3 | 0 (0) | PASS |
| S4 console_s4 | 0 (0) | PASS |
| S5 console_s5 | 53 (53), `STATUS_DLL_NOT_FOUND` | PASS |
| S6 console_s6 | 0 (0) | PASS |
| S7 console_s7 | 0 (0) | PASS |

NONVM probes (`NONVM=1 tools/linux-env/probes-initramfs.sh`, boot with `log_buf_len=16M`): all 15 run to
`NTPERS_ALL_DONE`. 14 exit 0; `probe_exception` exits 139 (as before). Matched at the verdict level: **1 of 15**
(`probe_sharemode`, 458857 of 458857 fields). Against the last recorded main run (the "after" table above) there is no
regression: every field count is the same or higher. Improved: `probe_directory` 0 to 19 of 67, `probe_fileinfo` 129 to
165 of 221, `probe_peloader` 0 to 33 of 34, `probe_registry` 0 to 128 of 140.

Other checks on the merged tree: unit suite `python -m unittest discover -s tests/unit -p "test_*.py"` 789 tests OK;
`check_doc_links.py` 0 broken; `update_doc_counts.py --check` current; `check_syscall_tables.py` parity for 171 services;
`tests/harness/lint_integrity.py` PASS; `dodrun.py`: DOD-SEC-A008 PASS, 3 ERROR (DOD-ARCH-A001, A002, B001: WSL not
installed on the host, environmental, not regressions), 0 regressions.

## Main 0d19a62: flake fixes and reproof (2026-10-14)

Source: `main` at `0d19a62` (flake fixes `675cc54`, merged as `069c9f5`; thread-exit hook `d7edb90`). Main has since moved (`252fa28` and later are not in this build).
Kernel: a private copy of `~/integ5/linux` in the guest (`~/main-proof-kernel/linux-6.18.56`), with
`tools/ntpers_kpatch.py` applied from `~/main-proof` at `0d19a62` (76 applied, 0 blocked, 0 todo) and
`vm/make-kernel-config.sh` (PROFILE=min, PASS). Built `make -j20 ARCH=x86_64 bzImage` (BUILD_RC=0).
Userspace PEs and DLLs built from the same tree (ntdll, kernel32, msvcrt, advapi32, user32 with mingw,
tests/kernel, probes). Boots on host QEMU through `tools/linux-env/boot-m0-host.ps1`.

**Flake fixes (userspace shim, `src/kernel/nt`).** The wait engine lost a wakeup when a signal landed
between a failed acquire and the wait-block link, and dropped waiters beyond 64 on one object. Thread-exit
ordering published `terminated` before the mutants were abandoned. Both fixes apply to the kernel branch of
`sync.c` too; the proc_thread ordering fix is userspace only (see follow-ups).

Loops (`build/port_test.exe` and `build/proc_thread_test.exe` in the guest, 4 in parallel, 60 s per run):

| test | before | after |
|---|---|---|
| port_test, 200 runs | 9 of 40 runs deadlocked (rc 124) | 200 of 200 pass |
| proc_thread_test, 200 runs (run 1) | not measured | 11 of 200 fail (`self_terminate`, `thread_exit`) |
| proc_thread_test, 200 runs (run 2) | 11 of 200 fail | 200 of 200 pass |

Regression tests: `sync_extra_test` `wake_all_waiters_beyond_64` and `auto_reset_handoff_loop` fail on the
old engine (`FAIL wake_all: 64 of 100 waiters woke`, `FAIL handoff: lost wakeup`) and pass on the new one.
`make -C tests/kernel-host check-quick` (Linux guest, before the thread-exit merge): all 22 programs rc=0.

**Completeness gate (`tests/kernel/check_kernel_complete.sh`): PASS** (exit 0, 171 services, 0 shortfall,
0 kernel gaps, 9 entry points strong, 21 built-in objects).

**Boots (host QEMU, serial logs in the private Windows dir `linux-env\kernel-d7edb90`):**

| step | exit status (expected) | verdict |
|---|---|---|
| M0 | 42 (42) | PASS |
| S1 console_s1 | 0 (0) | PASS |
| S2 console_s2 | 0 (0) | PASS |
| S3 console_s3 | 0 (0) | PASS |
| S4 console_s4 | 0 (0) | PASS |
| S5 console_s5 | 53 (53) | PASS |
| S6 console_s6 | 0 (0) | PASS |
| S7 console_s7 | 0 (0) | PASS |

**NONVM probes** (`NONVM=1 tools/linux-env/probes-initramfs.sh`, boot with `log_buf_len=16M`): all 15 reach
`NTPERS_ALL_DONE`. 14 exit 0; `probe_exception` exits 139 (as before).

Matched against `tests/differential/golden/windows-26200.9457` (`probe_results.py split` and `compare`), fields
matched of golden leaves:

| probe | matched / golden | verdict |
|---|---|---|
| probe_directory | 19 / 67 | FAIL |
| probe_exception | 0 / 76 | FAIL |
| probe_fileinfo | 165 / 221 | FAIL |
| probe_handles | 82 / 111 | FAIL |
| probe_namedpipe | 3 / 45 | FAIL |
| probe_ntstatus | 2191 / 2199 | FAIL |
| probe_path | 51355 / 72523 | FAIL |
| probe_peloader | 33 / 34 | FAIL |
| probe_process_thread | 0 / 182 | FAIL |
| probe_registry | 128 / 140 | FAIL |
| probe_section | 128 / 129 | FAIL |
| probe_sharemode | 458857 / 458857 | PASS |
| probe_sync | 0 / 74 | FAIL |
| probe_teb_peb | 79 / 104 | FAIL |
| probe_virtualquery | 128 / 139 | FAIL |

Verdict level: **1 of 15** (`probe_sharemode`), the same as the last recorded main run. For the four probes
named in the previous section (directory, fileinfo, peloader, registry) the counts are the same as recorded there.

**Follow-ups (not fixed here; all three fixed on `haiku/syncfix`, see the next section):**
- Kernel branch of `NtTerminateThread`/`do_exit` still publishes `terminated` before abandoning mutants.
  The userspace fix is in `proc_thread.c`; the kernel path needs the same ordering and a kernel build check.
- Userspace `NtTerminateThread` on a peer thread calls `nt_abandon_for_thread`, which abandons the caller's
  mutants, not the target's. Mutants of the peer are abandoned by its cancel cleanup, which runs after
  `terminated` is published.
- `nt_pulse_event` has a 64-entry snapshot in the same shape as the fixed wake path. Not changed.

## Kernel follow-ups fixed (branch `haiku/syncfix`, 2026-10-14)

Base: `main` at `099f353`. The three follow-ups above are fixed; each has a kernel-host regression test that
fails on the old code and passes on the new.

| follow-up | fix | regression test (fails before) |
|---|---|---|
| kernel `NtTerminateThread` publishes `terminated` before abandoning mutants | self and peer paths of the `__KERNEL__` branch in `proc_thread.c` abandon the target's sync thread before `terminated` is set; the peer path does it under `thr->lock` | `thread_exit_test` `test_kernel_terminate_abandons_before_publish` (source-order check; the `__KERNEL__` branch cannot be compiled on the host, so this is not a runtime race test) |
| userspace peer terminate abandons the caller's mutants | `thread_trampoline` publishes its sync thread on the ethread, `thread_sync_release` unpublishes it under `thr->lock`, and the peer path abandons `thr->sync_thread` under `thr->lock` before publishing | `thread_exit_test` `test_peer_terminate_abandons_target_mutant` (waiter sees `0x102` TIMEOUT instead of `0x80` ABANDONED) |
| `nt_pulse_event` releases at most 64 waiters | the pulse releases every linked waiter (both builds). A WaitAny waiter on a manual-reset event is marked as released, and its wait returns success even after the 1 ms reset (a plain wake was not enough: 7 of 100 woke under that reset) | `sync_extra_test` `test_pulse_wakes_all_beyond_64` (`FAIL pulse: 64 of 100 waiters woke`) |

**Loops (guest, `tests/kernel-host` binaries, 200 runs each, 4 in parallel):** `thread_exit_test` 200 of 200
pass; `sync_extra_test` 200 of 200 pass (zero non-zero exits, zero `FAIL` lines).

**Gates:** `make -C tests/kernel-host check-quick` (guest, KHOST_OS=linux): 25 programs rc=0.
`make -C tests/ntdll-host test` (guest): all tests passed. `python -m unittest discover tests/unit` (Windows
host): 832 tests OK. `sync_test` (not in check-quick) also passes.

**Not verified here:**
- The Windows host cannot link C test programs: Windows Application Control blocks `collect2.exe` in the
  MSYS2 UCRT64 toolchain ("An Application Control policy has blocked this file"), so `tests/kernel-host`
  and `tests/ntdll-host` were run in the guest instead.
- The `__KERNEL__` changes are proved only by the incremental kernel build, not by a runtime test. Build:
  `vm/incremental-kernel.sh` (CCACHE=0, JOBS=20, private guest tree `~/syncfix/linux-src`, copied from
  `~/kfast/linux-src`): full pass over 87 NT files, `INCREMENTAL_OK`, bzImage up to date; rerun on the final
  tree after one comment-only edit, 1 file copied, `INCREMENTAL_OK`.

**New follow-ups found while fixing:**
- `nt_abandon_owned_mutants` (both builds) still snapshots at most 64 owned mutants per thread. A thread that
  owns more than 64 mutants leaves the rest owned. Not changed here.
- The userspace pulse still resets the event after a 1 ms sleep. Late WaitAny waiters that arrive in that
  window still succeed, and WaitAll waiters rely on the window rather than on a mark. Not changed here.

## Kernel directory enumeration (haiku/npipe, 2026-10-15)

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
