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

**Current state (2026-10-12): the build fails.** `make` stops at `build/kshim.o`.
`src/kernel/nt/kshim.c:410-412` reads `st_atime`, `st_mtime` and `st_ctime`, because
the non-`__linux__` branch of the `KSHIM_STAT_*` macros uses the MinGW names. The host
`shim/sys/stat.h` declares only the Linux-layout `st_atim`, `st_mtim` and `st_ctim`.
Commit `6efb8b7` made this change after the results below were recorded. The other
module objects compile, but no test program links. The results below were recorded
before `6efb8b7` and were not re-run.

| Program | Result (recorded before `6efb8b7`) |
|---|---|
| `object_test` | builds, 9/9 pass |
| `security_test` | builds, 41/41 ntsec and 609/609 negative pass |
| `registry_test` | builds, 10/10 pass |
| `i2_test` | builds, 7/7 pass |
| `io_test` | builds; `access_denied_on_valid_handle` passes; path_parsing lookup checks are flaky (see below) |
| `mm_test` | builds, 14/14 pass |
| `sync_test` | builds, 21/21 pass, no leaks (slow, see below) |
| `mman_test` | shim self-test (`shim/mman_test.c`), 3/3 pass |
| `proc_thread_test` | 12/12 pass, 10 of 10 consecutive runs, no leak at exit |

`check` runs `TESTS` and `SHIM_TESTS`.

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
  checks in `path_parsing` sometimes pass and sometimes fail across runs with
  the same binary (seen at io_test.c lines 506 and 558). Not investigated;
  `make check` can fail on it.
- **sync_test** (slow). `wait_on_address_extended` runs 20000 iterations, each
  with a `pthread_create` and `usleep(2000)`. On Windows that takes minutes.
  `TIMEOUT` (default 3000 s) bounds each program in `check`.

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
