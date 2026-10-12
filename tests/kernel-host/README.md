# tests/kernel-host - src/kernel/nt unit tests on the Windows host

Builds the `src/kernel/nt` unit tests with the UCRT gcc from MSYS2
(`C:/Users/Jackson/repos/TOOLCHAIN.md`) and runs them on the host against
`kshim.c`, using the userspace branch (`-DUSERSPACE_KSHIM`, pthreads).

```sh
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
cd tests/kernel-host
make check          # build and run the programs listed in TESTS
make                # build only (includes BUILD_ONLY)
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
    `VirtualAlloc`. Anonymous mappings only. `MAP_FIXED_NOREPLACE` is emulated
    with `VirtualQuery`.
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

| Program | Result |
|---|---|
| `object_test` | builds, 9/9 pass |
| `security_test` | builds, 41/41 ntsec and 609/609 negative pass |
| `registry_test` | builds, 10/10 pass |
| `i2_test` | builds, 7/7 pass |
| `io_test` | builds, 11/11 in most runs (see below) |
| `mm_test` | builds, 12/14 pass (2 fail, see below) |
| `sync_test` | builds, 21/21 pass, no leaks (slow, see below) |
| `proc_thread_test` | builds (BUILD_ONLY); exits 0 before printing its summary (see below) |

`check` runs `TESTS`. `proc_thread_test` is kept out of `TESTS` because its
exit status (0) would look like a pass.

## Known failures and limits

- **mm_test** (2 of 14). `test_vad_split_coalesce_invariants` (mm_test.c:756)
  and `test_probe_direct_windows_rule` (mm_test.c:863) fail on
  `NtAllocateVirtualMemory(MEM_RESERVE)` with `STATUS_NO_MEMORY`. Cause is
  likely `munmap` on a partial range. Windows can only decommit a part of a
  reservation, so that part stays reserved and a later
  `MAP_FIXED_NOREPLACE` at that address is refused. Whole allocations are
  released. Not fixed.
- **io_test** (intermittent). `io_test.c:505` expects `OBJECT_NAME_NOT_FOUND`
  for `trail.txt.` when only `trail.txt` exists. One run returned success, and
  the next run passed. Not investigated further.
- **proc_thread_test** (build only). The program returns 0 after
  `RUN test_mutant_abandon_peer_terminate` and never prints its summary. The
  peer `NtTerminateThread` path calls `pthread_cancel` (`proc_thread.c:573`).
  The likely cause is winpthreads cancellation of the wrong thread. Not
  confirmed.
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
  `build/`. It yields 519 entries. The generator's own sanity check expects
  527, so the count differs from what the generator would say. Not verified
  against the generator's output.
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
  there is no `nt_io.c` in the tree. Worth a look in the sources.
- **Object identity.** `st_dev`, `st_ino` and `st_nlink` come from
  `GetFileInformationByHandle`. The UCRT `ino_t` is `unsigned short` and
  `dev_t` is `unsigned int`, so `st_ino` keeps only the low 16 bits of the
  Win32 file index. Two files that differ only above bit 15 would compare equal.
- **Scratch root.** Tests use `/tmp`, which maps to `%TEMP%`. Directory
  opens use `FILE_FLAG_BACKUP_SEMANTICS`.
