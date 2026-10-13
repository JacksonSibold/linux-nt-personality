# tests/kernel-host - src/kernel/nt unit tests on the Windows host

Builds the `src/kernel/nt` unit tests with the UCRT gcc from MSYS2
(`/c/msys64/ucrt64/bin`) and runs them on the host against
`kshim.c`, using the userspace branch (`-DUSERSPACE_KSHIM`, pthreads).

```sh
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
cd tests/kernel-host
make check          # build and run the programs listed in TESTS
make                # build only (includes BUILD_ONLY, currently empty)
```

`TEMP_DIR` is taken from the environment if set, else `TEMP`, `TMP`,
`LOCALAPPDATA/Temp`, and finally MSYS `/tmp`. The Makefile exports `TMP`,
`TEMP` and `TMPDIR` from it for the recipes. Under make on this machine the
environment is sometimes not inherited, so the MSYS `/tmp` fallback is the
one that resolves (to `C:/msys64/tmp` there), and it is writable.

Outputs go to `build/` (ignored by git). Test programs run with `build/` as
their working directory, so the scratch files they create stay there.

## Make targets

| Target | What it does |
|---|---|
| `make all` (default) | Builds every program in `TESTS`, `STRESS_TESTS`, `BUILD_ONLY` and `SHIM_TESTS` into `build/<name>.exe`. Runs nothing. |
| `make build/<name>.exe` | Builds one program, e.g. `make build/io_test.exe`. |
| `make check` | Builds, then runs `TESTS` and `mman_test`. `sync_test` gets `SYNC_TEST_ITERATIONS=$(SYNC_CHECK_ITERATIONS)` (default 2000). Exits nonzero if any program fails. |
| `make check-quick` | Like `check`, but leaves out `sync_test`, the slowest program. Use it for the edit-test loop. |
| `make check-full` | Like `check`, with `sync_test` at the CI count `SYNC_FULL_ITERATIONS` (default 20000, the program's own default). Takes about 5 to 6 minutes on this host. |
| `make stress` | Runs `STRESS_TESTS` once per seed in `STRESS_SEEDS`, with `KSHIM_POISON=1`. Not part of any `check` target. |
| `make clean` | Removes `$(BUILD)/`. |
| `make clean-scratch` | Removes `ntio_test_*` directories older than one day from the test temp dir (`TEMP_DIR`). Nothing else in that dir is matched. |
| `make ... TIMEOUT=n` | Per-program wall-clock limit in seconds for the check targets (default 3000). |

Variables that the targets read:

- `SYNC_TEST_ITERATIONS` (environment, read by `sync_test` itself). Count for
  the `wait_on_address_extended` loop. Unset means 20000. Set it to a value in
  1..10000000; anything else makes `sync_test` exit with status 2. The
  `check` targets set it through the Makefile variables above.
- `SYNC_CHECK_ITERATIONS`, `SYNC_FULL_ITERATIONS` (Makefile) - the counts
  used by `check` and `check-quick`, and by `check-full`.
- `KEEP_SCRATCH=1` (environment, read by the shim). Keeps a test's scratch
  root instead of removing it at exit. The program prints the kept path.
- `STRESS_SEEDS`, `STRESS_THREADS`, `STRESS_OPS`, `STRESS_TIMEOUT` (Makefile) -
  the stress run; see "Stress tests" below.
- `COVERAGE=1`, `OPT=-O2`, `BUILD=...` (Makefile) - coverage and optimiser builds,
  kept in their own directories (`build-cov/`, `build-*/`).
- `TEMP_DIR` (Makefile) - the temp dir, see "Scratch root" under Known deviations.

## Layout

- `Makefile` - build rules. Each program links its test object with the module
  objects it needs (`<prog>_OBJS`). Module objects are built from
  `src/kernel/nt/*.c` with `-iquote`, so the sources' `#include "io.h"` keeps
  resolving to the module header. (`-I` would let the UCRT `<io.h>` pick up
  the module's `io.h`.)
- `shim/` - host headers and units for the POSIX pieces the toolchain lacks:
  - `khost_prelude.h` - force-included (`-include`) into every module build.
    Defines `loff_t`, `O_DIRECTORY`, `O_SYNC`, and maps `open`, `unlink`,
    `rmdir`, `rename`, `system`, `mkdir`, `pread`, `pwrite`, `fsync` to
    `khost_*` functions.
  - `sys/stat.h` - Linux-layout `struct stat` (`st_atim` etc. are
    `struct timespec`), `stat`/`fstat`, `S_IS*`.
  - `sys/mman.h`, `mman_win.c` - `mmap`/`munmap`/`mprotect`/`madvise` on
    `VirtualAlloc`. Anonymous mappings only. `MAP_FIXED` and
    `MAP_FIXED_NOREPLACE` are emulated per `VirtualQuery` region: a
    MEM_RESERVE part left by a partial `munmap` is a hole and can be mapped
    again (Linux semantics). `shim/mman_test.c` checks this.
  - `sys/syscall.h` - empty stub so `proc_thread.c` parses.
  - `posix_host.c` - `stat`/`fstat` through `GetFileInformationByHandle`,
    `open` through `CreateFileW` with `FILE_SHARE_DELETE` (so unlink-while-open
    works), directory opens, `\\?\` paths (so trailing dots are kept), `/tmp`
    mapped to the scratch root (see below), `system()` limited to `mkdir -p`, `rm -rf` and `&&`
    chains, and an unbuffered `stdout`.
  - `posix_raw.c` - `pread`, `pwrite`, `fsync`, `mkdir` on `_read`, `_write`,
    `FlushFileBuffers`, `_mkdir`. Built without the shim include directory so
    it sees the toolchain's own headers.
- `tools/ntstatus_inc.sh` - generates `build/ntstatus_table.inc` from
  `src/ntdll/ntstatus_to_dos.c`. See "Known deviations".

## Status

**Current state (2026-10-12): the build succeeds.** `make clean`, then each target
built on its own, links with no errors. Commit `213d333` restored the
`USERSPACE_KSHIM` branch of the `KSHIM_STAT_*` macros, so `kshim.c` builds
against the shim's `st_atim` layout.

`TESTS` lists nine programs: the eight kernel module tests and `proc_thread_test`
(added to `TESTS` in `a3278a3`). `SHIM_TESTS` is `mman_test`. `make check` runs both.

Counts from a clean rebuild and run, 2026-10-12:

| Program | Result |
|---|---|
| `object_test` | builds, 9/9 pass |
| `security_test` | builds, 41/41 ntsec and 609/609 negative pass |
| `registry_test` | builds, 10/10 pass |
| `i2_test` | builds, 7/7 pass |
| `io_test` | 13/13 pass, 50 of 50 consecutive runs at -O0 and -O2, also with `KSHIM_POISON=1` (see below) |
| `mm_test` | builds, 14/14 pass |
| `sync_test` | builds, 21/21 pass, no leaks (slow, see below) |
| `mman_test` | shim self-test (`shim/mman_test.c`), 3/3 pass |
| `proc_thread_test` | 12/12 pass, 10 of 10 consecutive runs, no leak at exit |

`mman_test` passes 100 of 100 runs after the fix below (before it: 79 of 100).

### mman_test flake: test bug, fixed in the test

- **Cause: the test assumed the 32K past its own 64K mapping was free.**
  `test_hole_plus_free` mapped 64K at NULL, then asked for a 64K range starting
  32K into it, so the last 32K was never reserved by the test. Where Windows
  placed the 64K block decided what sat next to it. In the failing runs a
  committed block (0x22000 bytes) sat right after it, so
  `NOREPLACE over hole + free` correctly returned EEXIST (`shim/mman_test.c:57`).
  The cleanup `munmap(a, 0x18000)` then decommitted that foreign block, which
  caused the segfault at exit (rc 139). The shim was not wrong: its
  `map_fixed_range` and `munmap` behaved as designed in every run. 21 of 100
  runs failed this way.
- **Fix.** The test now reserves 128K, releases it, and maps the first 64K back
  at that base. The rest of the 128K is known to be free, and nothing runs
  between the steps, so the mixed hole-plus-free range is deterministic.
  `shim/mman_win.c` is unchanged.

## Results of the 2026-10-12 investigation

- **mm_test: shim bug, fixed in the shim.** Cause: `munmap` on part of an
  allocation decommits it, and Windows keeps the decommitted part reserved
  (VirtualQuery reports MEM_RESERVE). The shim then refused
  `MAP_FIXED_NOREPLACE` over that hole with EEXIST, but mm.c had already freed
  the range, so the next reserve failed with `STATUS_NO_MEMORY`. Reproduced
  with plain `mmap`/`munmap` and no mm.c code. `mmap` now maps a range one
  VirtualQuery region at a time, treating MEM_RESERVE holes as unmapped.
  `shim/mman_test.c` covers this and fails on the old shim. mm.c was not
  changed; its VAD invariants held throughout.
- **io_test: the flake did not reproduce.** 20 runs of the trailing-dot check
  all passed. The lookup is deterministic: `posix_host.c` opens paths with the
  `\\?\` prefix, which keeps the trailing dot, so `trail.txt.` is not found.
  The earlier one-off success is not explained. A separate, deterministic
  failure was found and fixed: since 1d160e8, `byte_round_trip` read through a
  write-only handle and failed on every run (10/11). It now opens read-write.
- **proc_thread_test: real leak, fixed.** The leaked allocation is the
  `nt_thread` that `thread_trampoline` creates (`sync.c:134`, found by the
  allocation-site tags in `kshim.h`). Self-termination (`pthread_exit`) did
  not reach the trampoline's cleanup, so the `nt_thread` was never freed.
  `thread_sync_release()` in `proc_thread.c` now frees it on every exit path.
  `test_suspend_counts` also waited on a sleeper thread that was still running
  at teardown; it now waits for the thread's exit path.

### Results of the io_test flake investigation (2026-10-12, second pass)

- **Cause: the scratch root depended on the launch environment.** The root
  came from TEMP, then TMP, then `.`. A process that saw neither variable got a
  relative root (`./ntio_test_<pid>`). The shim adds the `\\?\` prefix only to
  absolute drive paths, so Win32 then stripped the dot from `trail.txt.` and
  `path_parsing` failed at io_test.c:558 (expected NOT_FOUND, got SUCCESS).
  Reproduced on demand: with the msys `env` wrapper (after `export PATH` with
  the MSYS directories first) the original shim failed 50 of 50 runs, and
  `env -u TEMP -u TMP ./io_test.exe` failed too. Each failing run left a
  relative `ntio_test_<pid>` root in the build directory (63 in `build-o2/`,
  49 in `build/`) and none in `%TEMP%`.
- **Fix.** `host_temp_dir()` in `shim/posix_host.c` takes TMP or TEMP when it is
  an absolute drive path (MSYS `/c/...` form included), otherwise the current
  directory, which is always absolute. `GetTempPath` was rejected: with TMP,
  TEMP and USERPROFILE unset it returns `C:\WINDOWS\`, which is not writable.
  Relative paths given to the shim are now made absolute before the `\\?\`
  prefix is added. `io_test` runs `scratch_root_keeps_trailing_dot` first and
  fails there with a clear message if the root drops dots.
- **Cleanup.** `io_test` removes its per-pid root after a clean run. Failing
  runs keep it. A caller-supplied `NTIO_TEST_ROOT` is never removed.
- **Uninitialised memory: not found.** `KSHIM_POISON=1` (0xA5 fill of every
  `kmalloc` result, in `kshim_alloc_tagged`) passes at -O0 and -O2 once the
  root is fixed. The build has no `-Wuninitialized` warnings beyond the
  known pointer-cast ones. The earlier poisoned failures were the root
  problem, not a poisoned read.
- **Shared temp-dir state: not found.** No file named `trail.txt.` exists in
  any of the ~160 leftover roots in `%TEMP%`, and each run uses its own pid.

Knobs added: `KSHIM_POISON=1` (poison kmalloc results) and `OPT=-O2` with a
separate `BUILD=build-o2` directory (`build-*/` is git-ignored).

## Known failures and limits

- **sync_test** (slow). `wait_on_address_extended` runs `SYNC_TEST_ITERATIONS`
  iterations (default 20000), each with a `pthread_create` and `usleep(2000)`.
  At 20000 it took about 5 minutes on the 2026-10-12 clean run. `check` and
  `check-quick` use 2000 (about 45 s); `check-full` uses 20000. `TIMEOUT`
  (default 3000 s) bounds each program in the check targets.
- **Trailing dots in unlink, rename, mkdir, rmdir.** `khost_unlink`,
  `khost_rmdir`, `khost_rename` and `khost_raw_mkdir` call the CRT directly,
  not through the `\\?\` path. A name ending in `.` can therefore be stripped
  to the wrong file. No test exercises that yet.
- **mman_test intermittent failure: fixed** (test bug, see "mman_test flake" above).
- **Stress findings still open (`make stress` is not green).** Each is recorded in `docs/review/kernel-nt.md`, with the seed that reproduces it:
  - `stress_registry_test` faults under `KSHIM_POISON=1` (every seed): `NtSetValueKey` persists a value after releasing the key lock, and `NtDeleteValueKey` can free that value first (REG-1).
  - `stress_namespace_test` faults in a libwinpthread lock about one run in three under `KSHIM_POISON=1` (OB-1). Without poison it passed every run tried.
  - `stress_sync_test` fails about half the runs at seed 2 with 16 workers, both with and without poison. The base commit before this work (`45a70b4`) fails the same way (SYNC-3, not minimised).
  - Opening an event through a path that crosses a symbolic link always fails with `STATUS_OBJECT_PATH_NOT_FOUND` (OB-2). The namespace test keeps that probe, so its counter `thru_link` stays 0.

## Modules that cannot be built here

These do not appear in `TESTS` or `BUILD_ONLY`.

- `debugfs_tests.c` - an in-kernel self-test module. Entry point
  `nt_debugfs_init()` registers debugfs files. It has no `main`. Needs
  `linux/debugfs.h`, `linux/seq_file.h`, `linux/fs.h`, `linux/uaccess.h`,
  `linux/slab.h`, and a running kernel to call `nt_debugfs_init()`.
- `dispatch.c` - syscall entry dispatch with per-CPU counters and
  `TIF_NTABI`/ptrace state. Needs `linux/kernel.h`, `linux/sched.h`,
  `linux/thread_info.h`, `linux/ptrace.h`, `linux/nospec.h`, `linux/percpu.h`,
  and `linux/debugfs.h`. Also `struct pt_regs` from the arch headers. Nothing
  in `kshim.h` provides them.
- `weak.c` - `__weak` default hooks (`nt_handle_table_destroy`,
  `nt_abandon_owned_mutants`) and `RtlUserThreadStart`. Needs `linux/sched.h`
  for `struct task_struct`. It is a kernel link-time fallback, so there is
  nothing to test on the host.
- `repro_mem_vad.c` - a standalone reproducer with `main`, not a `*_test.c`
  module test. Not built here.

## Known deviations from the kernel build

- **ntstatus table.** `ntstatus_kernel.c` includes `ntstatus_table.inc`, which
  `src/kernel/nt/gen_ntstatus_table.py` produces. That script needs `python3`,
  which this machine lacks (`python3` resolves to a Store alias stub).
  `tools/ntstatus_inc.sh` extracts the same entry lines with awk and sed into
  `build/`. On 2026-10-12 it yields 520 entries, the same entry lines as the tracked
  `src/kernel/nt/ntstatus_table.inc`. The generator has no hard-coded expected
  count (`ed17a53`).
- **Pointer width.** Win64 is LLP64, so `long` is 32 bits. The test sources
  cast pointers to `unsigned long`, which gives `-Wpointer-to-int-cast`
  warnings. `-Wall -Wextra` is on and the warnings are left as they are.
- **Sanitizers.** `-fsanitize=address` and `-fsanitize=undefined` do not link:
  the UCRT64 install has no `libasan` or `libubsan`. Not installed here.
- **nt_svc_NtClose.** `io.c` and `nt_obj.c` both define a strong
  `nt_svc_NtClose(HANDLE)` in their userspace branches. Linking both fails with
  a multiple-definition error. `io_test` links `io.c`'s version, which is the
  file-aware one, by running `objcopy --weaken-symbol` on `nt_obj.o`. The
  kernel Makefile (`arch/x86/nt/Makefile`) lists `nt_io.o`, not `io.o`, and
  there is no `nt_io.c` in `src/kernel/nt`; `arch/x86/nt/nt_io.c` is the arch copy. Worth a look in the sources.
- **Object identity.** `st_dev`, `st_ino` and `st_nlink` come from
  `GetFileInformationByHandle`. The UCRT `ino_t` is `unsigned short` and
  `dev_t` is `unsigned int`, so `st_ino` keeps only the low 16 bits of the
  Win32 file index. Two files that differ only above bit 15 would compare equal.
- **Scratch root.** Tests use `/tmp`. It maps to TMP, else TEMP (absolute
  drive path only), else the current directory. It is always absolute, which
  the `\\?\` prefix requires. Directory opens use `FILE_FLAG_BACKUP_SEMANTICS`.
- **Scratch cleanup.** A test that creates a root under `/tmp` calls
  `khost_scratch_arm_cleanup()` (`shim/posix_host.c`). The root is removed at
  exit after a pass or a failure, so a failing run leaves nothing behind.
  `KEEP_SCRATCH=1` keeps it for debugging. Only `io_test` creates such a root
  (`ntio_test_<pid>`). Roots left by runs before this change, or by a process
  killed before exit, are removed by `make clean-scratch` once they are a day old.

## Stress tests (not part of `check`)

Nine concurrency stress programs drive the userspace kshim build of the
object, handle, sync, token, section, namespace, security-descriptor, registry
and timer code with several threads at once. They are built by
`make all` and run only by `make stress`. `check` does not run them.

| Program | Sources linked | Covers |
|---|---|---|
| `stress_handle_test` | `object handle kshim` | create, close, duplicate (same and cross process, with move), ref and deref through handles, and a race round where every worker calls `ObCloseHandle` on one value |
| `stress_object_test` | `object handle kshim` | named create and insert, open, reference by name, delete (`ObMakeTemporaryObject`), unnamed ref and deref, and a regression round for concurrent make-temporary |
| `stress_sync_test` | `object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel` | `nt_svc_*` events, mutants, and semaphores with 0 to 2 ms waits, recursive mutant ownership, duplicate and close, named events, and a race round where every worker calls `NtClose` on one value |
| `stress_token_test` | `security object handle kshim` | process-token open with random access masks, duplicate (of a shared token and of a worker's own), disable-all-privileges and TokenUser query, checked against the access each handle was granted; a race round of closes on one duplicate |
| `stress_section_test` | `mm security object handle io iocp nt_obj_weak sync kshim` | anonymous sections with a private RW view and a tag round trip, file-backed read-only views of four 64 KiB files, named sections created and opened by name, view unmap and handle close in independent orders, delete by name; a race round of closes |
| `stress_namespace_test` | `security object handle nt_obj sync kshim` | directories (permanent and temporary names), events inside them, symbolic links to directory paths, open of links and of events by path, permanent and temporary changes; a race round where every worker opens one event by path while it is deleted |
| `stress_sd_test` | `security object handle nt_obj sync kshim` | `NtSetSecurityObject` with one of four DACL variants and `NtQuerySecurityObject` into large and small buffers on shared events; each query must parse and match an installed variant; a race round of set-or-query then close |
| `stress_registry_test` | `security registry object handle nt_obj sync kshim` | key create, open, delete and flush under `\Registry\User\Software`, DWORD value set, query and delete; every result must be in the call's allowed set; a race round where every worker opens a key that is deleted at the same moment |
| `stress_timer_test` | `security object handle nt_obj nt_services sync proc_thread kshim ntstatus_kernel` | real nt threads: waitable timers set (some periodic) and cancelled, waits with 0 to 2 ms timeouts, keyed-event release and wait with per-worker keys, alertable waits on an event that is never signalled, `NtAlertThread` between workers and on self |

Usage (run from `tests/kernel-host`, or from `build/`):

```sh
make stress                                   # seeds 1 2 3, 12 threads, per-program op counts
make stress STRESS_SEEDS=42 STRESS_THREADS=16 # one seed, 16 workers
./build/stress_handle_test.exe <seed> <threads 8..16> <ops per thread>
```

Each program takes `seed`, `threads` (clamped to 8..16), and `ops` as
positional arguments. The defaults are 65536 ops for `stress_handle_test`,
16384 for `stress_object_test`, and 4096 for `stress_sync_test`. Each worker's
PRNG stream is derived from the seed and the worker index, so a failure
reproduces from its seed. Every program exits 0 on success and 1 on any
violation. Violations print the seed, file, line, and the failing condition.

Every run also checks these invariants at the end:

- no leaked allocations: `kshim_alloc_count` returns to its value before the
  object manager was initialised; `kshim_leak_report()` lists survivors
- handle tables are empty, and each pool object has `ref_count == 1` and
  `handle_count == 0`
- in each race round exactly one concurrent close succeeds and the others
  return `STATUS_INVALID_HANDLE`

`make stress` sets `KSHIM_POISON=1`, which fills every allocation with `0xA5`
on allocation and `0xDD` on free (see `src/kernel/nt/kshim.c`). A read of freed
memory then produces a visibly wrong value or a fault.

A watchdog thread in each program aborts the process with a report if no worker
has completed an operation for 60 seconds. That is treated as a deadlock.

Measured on this machine (2026-10-12, UCRT gcc, Windows 11, 16 workers, `KSHIM_POISON=1`, seed 2, default op counts): `stress_token_test` 1 s, `stress_handle_test` 2 s, `stress_section_test` 2 s, `stress_sd_test` 3 s, `stress_namespace_test` 11 s, `stress_object_test` 18 s, `stress_sync_test` 16 s (the timed waits dominate, and it fails about half the runs, see below), `stress_timer_test` 28 s, `stress_registry_test` faults in under 1 s (see below). A full `make stress STRESS_THREADS=16` (three seeds, nine programs) took about 2 min 40 s on the 2026-10-12 run, with the failures listed below. `make stress` writes each program's output to
`build/<program>.seed<N>.log`. `nt_obj.c` and `proc_thread.c` print debug lines
(`W1_INSTR`, `NtWaitForSingleObject:`) on every call. Those lines are now compiled out unless `NT_DEBUG_TRACE` is defined (`src/kernel/nt/kshim.h`), so the logs are small by default.
