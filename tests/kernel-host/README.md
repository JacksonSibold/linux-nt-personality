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
    mapped to `%TEMP%`, `system()` limited to `mkdir -p`, `rm -rf` and `&&`
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
| `object_test` | 9/9 pass, rc 0 |
| `security_test` | 41/41 ntsec and 609/609 negative pass, rc 0 |
| `registry_test` | 10/10 pass, rc 0. The Windows Application Control block described in earlier notes was not hit on this run, so no policy workaround is in the tree |
| `i2_test` | 7/7 pass, rc 0 |
| `io_test` | 12/12 pass in 12 of 13 runs. One run failed `path_parsing` at `io_test.c:558` (see "io_test lookup flake") |
| `mm_test` | 14/14 pass, rc 0 |
| `sync_test` | 21/21 pass, rc 0, no leaks. Slowest program; about 5 minutes on the clean run |
| `proc_thread_test` | 12/12 pass, rc 0. Its `leaks 27` line is the live count printed before teardown; the at-exit count is 0 and the program returns 0 only when it is |
| `mman_test` | 3/3 pass on the clean run; intermittent, see below |

`mman_test` is intermittent on this machine: 2 of 14 runs failed. In one, the
`NOREPLACE over hole + free` check failed (`shim/mman_test.c:57`, EEXIST) and the
process then segfaulted. In the other, the process segfaulted (rc 139) with no
check failure printed. The other 12 runs passed. The cause is not investigated.

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

## Known failures and limits

- **sync_test** (slow). `wait_on_address_extended` runs 20000 iterations, each
  with a `pthread_create` and `usleep(2000)`. On Windows that takes minutes.
  `TIMEOUT` (default 3000 s) bounds each program in `check`.
- **io_test lookup flake.** The trailing-dot, `\\?\` probe and mixed-case
  checks in `path_parsing` sometimes fail across runs with the same binary
  (seen at io_test.c lines 506 and 558). Status on 2026-10-12: 1 failure in 13
  runs (`io_test.c:558`, expected `STATUS_OBJECT_NAME_NOT_FOUND`, got success)
  and 12 passes. Not root-caused; `make check` can fail on it.
- **mman_test intermittent failure.** See the status table above.
- **sync_test** (slow). `wait_on_address_extended` runs 20000 iterations, each
  with a `pthread_create` and `usleep(2000)`. On Windows it took about 5 minutes
  on the 2026-10-12 clean run. `TIMEOUT` (default 3000 s) bounds each program
  in `check`.

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
- **Scratch root.** Tests use `/tmp`, which maps to `%TEMP%`. Directory
  opens use `FILE_FLAG_BACKUP_SEMANTICS`.
