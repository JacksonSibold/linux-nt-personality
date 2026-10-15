# Linux run: sanitizer and portability findings

Date: 2026-10-13. Branch `haiku/guesttests`, worktree `ntpers-guesttests`, based on `haiku/kbuild`.
Host: Debian 12 x86_64 guest under QEMU (TCG, 4 vCPU, of which at most 2 used here; another agent
builds a kernel in the same guest). Toolchain: gcc 12.2.0, glibc 2.36, Python 3.11.2, binutils 2.40,
Debian `gcc-mingw-w64-x86-64` 12.2 (fixtures), libasan/libubsan/libtsan from gcc 12.
Source copy: `~/guesttests` in the guest (sync with `GUEST_DEST=guesttests tools/linux-env/sync-repo.sh`, which now ships LF).

Every finding has an id (`LNX-nn`). Each fix is one commit, named in its message. Where a test was
red before the fix, the red run is recorded under the finding. Open race sites are listed in the
last section with their sanitizer evidence; they are not fixed here.

## Summary

| Id | Area | Kind | Found by | Status | Commit |
|---|---|---|---|---|---|
| LNX-01 | tools/check_repo.py | portability: NUL byte in .py gives ValueError on Python 3.11 | unit suite on Linux (error) | fixed | fb92b90 |
| LNX-02 | tools/, tests/differential | 12 tracked scripts lack the exec bit | Windows unit suite, and a Linux clone would not run them | fixed | 29b2df4 |
| LNX-03 | tests/host real-PE layout | test pins binutils 2.47 layout; Debian has 2.40 | host suite on Linux, 5 of 10 red | fixed (test profile; parser agrees with objdump) | d0ac0e0 |
| LNX-04 | src/ntdll/except.c, except.h | POSIX branch never compiled (feature macros, kshim/g_nt_mm refs, BOOLEAN) | ntdll-host build, 30 errors | fixed | d1b4894 |
| LNX-05 | src/ntdll/except.c | reads %gs:0x30 (Windows TEB) on every SIGSEGV; crashes in a Linux process | ASan, SEGV at except.c:190 | fixed (guard pages not reported on this path) | 1bca416 (red), 8f87cfe |
| LNX-06 | tests/ntdll-host Makefile | ASan replaces the freestanding malloc; test_runtime aborts | ASan | fixed (build config) | 071151a |
| LNX-07 | tools/linux-env/sync-repo.sh | CRLF scripts reach the guest; generator fails "set: Illegal option -" | kernel-host build | fixed | f02e7d7 |
| LNX-08 | src/kernel/nt/mm_test.c | WCHAR (16-bit) fed to L"" literal and libc wcslen (wchar_t, 32-bit) | gcc diagnostic on Linux | fixed | 44b8538 |
| LNX-09 | src/kernel/nt/sync_test.c | wake lost after fixed 2 ms sleep; uninitialised field, segfault | sync_test on Linux, rc=1 x3 | fixed (test) | 05f9344 |
| LNX-10 | src/kernel/nt/registry.c | last_write_time written outside the key lock (3 call sites) | TSan, stress_registry_test | fixed | 325cd68 |

Open (real, not fixed in this pass): O-1 object.c flag races, below.

## Test results on Linux (final tree at the time of each run)

- `python3 -m unittest discover tests/unit` (Linux): 665 run, 1 error before LNX-01, then clean.
- Windows `python -m unittest discover tests/unit` (WinPython 3.13): 665 run, 1 skip (WSL), OK, after LNX-02.
- `make -C tests/host test` with ASan+UBSan: pe_parse 48/48, real_pe 10/10 (fixtures built by
  Debian mingw, `REAL_PE_LAYOUT_DEBIAN_MINGW`).
- `make -C tests/ntdll-host test` with ASan+UBSan: 13 suites, all pass (after LNX-04..06).
- `make -C tests/fuzz smoke` plain and `SANITIZE=1`: 0 violations, 0 faults.
- `make -C tests/fuzz SANITIZE=1 ITERS=100000 SEED=1..3 run`: 21 file-runs, 0 violations, 0 faults,
  no ASan or UBSan reports.
- `make -C tests/kernel-host check` on Linux (`KHOST_OS=linux`): 22 programs, all rc=0, 333 s,
  after LNX-09 and the Linux shim.

## Findings

### LNX-01 check_repo: NUL bytes on Python 3.11 (fixed, fb92b90)
`compile()` raises `ValueError` for a NUL byte on Python 3.11 and older, `SyntaxError` on 3.12+. The
check caught only `SyntaxError`, so `test_nul_byte_past_probe_is_a_compile_failure_without_line_none`
errored on Debian. The check now reports both the same way.

### LNX-02 exec bits (fixed, 29b2df4)
Twelve tracked files start with `#!` but are mode 100644. The Windows checkout cannot record the
exec bit, and a Linux clone would not run `./script`. `test_script_modes` fails on Windows for the
same reason. Set mode 100755 in the index.

### LNX-03 real-PE layout is toolchain-specific (fixed, d0ac0e0)
The MinGW fixtures link differently under binutils 2.40. Five of ten `test_real_pe` checks failed.
`objdump -h`/`-p` on the same images agrees with `pe_parse` on every value (SizeOfImage 0x6000,
no `.rdata`, export directory at 0x5000, RUNTIME_FUNCTION end 0x1011). The parser is correct; the
pinned constants were for binutils 2.47. The Windows values remain the default; a Debian profile
is selected by the Makefile on non-MSYS hosts.

### LNX-04 POSIX exception path never compiled (fixed, d1b4894)
`src/ntdll/except.c` (the `#ifndef __MINGW32__` branch) did not build on glibc: `REG_*`,
`mcontext_t.gregs`, `siginfo_t` and `sigaction` need a feature macro under `-std=c11`. The branch
also referenced `g_nt_mm` and `struct kshim_guard_state`, which exist only in the kernel build. And
`NtRaiseException` was defined with `ULONG FirstChance` while `except.h` declares `BOOLEAN`. The
fix defines `_GNU_SOURCE` for the glibc branch (in the header and on the ntdll-host compile line),
removes the kshim/`g_nt_mm` reads, and matches the signature.

### LNX-05 `%gs:0x30` read in the SIGSEGV path (fixed, 1bca416 red, 8f87cfe fix)
`is_guard_page_hit()` read the Windows TEB stack limits through `%gs:0x30` on every SIGSEGV with a
non-null fault address. A plain Linux process has no TEB and `%gs` has base 0, so the read faults
inside the signal handler. ASan reported `SEGV on unknown address 0x000000000030` at
`except.c:190` (the red run). The block is removed; the function returns "not a guard hit". Guard-page
reporting on this path is therefore not implemented: a fault is reported as an access violation.
Test: `sigsegv_maps_access_violation_without_teb`.

### LNX-06 ASan and the freestanding allocator (fixed, 071151a)
`src/ntdll/ntdll_runtime.c` replaces `malloc`, `calloc` and `free` for the whole process. ASan
interposes the same symbols, and `test_runtime` aborted in its interceptor ("calloc-overflow").
Without sanitizers all 13 suites pass. The runtime objects and `test_runtime` are built without
sanitizer flags; every other target keeps ASan and UBSan. The test source is unchanged.

### LNX-07 CRLF scripts in the guest (fixed, f02e7d7)
With `core.autocrlf=true` the Windows working tree holds text files as CRLF, while git stores LF.
`sync-repo.sh` copied the working tree, so the guest got CRLF shell scripts. The kernel-host
generator failed with `sh: set: Illegal option -`. Files git stores as LF are now converted back to
LF in a staging copy. This is a sync-tool fix, not a repo change.

### LNX-08 WCHAR is not wchar_t (fixed, 44b8538)
`mm_test.c` initialised `WCHAR` (`uint16_t`) arrays from `L"..."` (`wchar_t`, 32 bits on Linux) and
measured one with the libc `wcslen`. Both are wrong for 16-bit data on glibc. The arrays now use
`u"..."` and the length is counted in 16-bit units. The compiler diagnostic
(`-Wincompatible-pointer-types`) was the red evidence.

### LNX-09 wait-on-address test (fixed, 05f9344)
`wait_on_address_extended` woke one waiter after a fixed 2 ms sleep. A wake with no queued waiter is
lost by design. Under emulation the waiter was not queued yet and the test failed at iteration 8
(`FAIL woa extended 8 got 8`, rc=1 three times). With a 50 ms sleep all 300 iterations passed, so
the race was the test's timing, not the wait. The test now repeats the wake until the waiter has
returned, and the waiter array's new `done` field is initialised (it was a segfault on the first
version of the fix). Verified: three runs of 2000 iterations, 21 passed, 0 failed.

### LNX-10 `last_write_time` written outside the key lock (fixed, 325cd68)
TSan (stress_registry_test, SANITIZE=thread) reported `registry.c:882` (NtCreateKey, parent key
timestamp after unlock) and `registry.c:1383` (NtDeleteValueKey, timestamp after unlock). Both are
now written inside the lock that protects the list. After the fix the registry stress run (seeds
1-3) has no `registry.c` report.

## Open: real races reported by TSan (not fixed here)

These are real. The code writes object flags with no lock while other threads read them.

| Site | Function | Reports (all stress logs) | Note |
|---|---|---|---|
| object.c:997-998 | ObMakeTemporaryObject (`obj->flags`, `obj->is_permanent`) | 46 | written without `ob_ns_lock`; read by ObpInsertDirectoryEntry and others |
| object.c:1006-1007 | ObMakePermanentObject | 11 | same |
| object.c:350-351 | ObpInsertDirectoryEntry (read of `obj->flags` after `ob_ns_lock` is released) | 6 | read outside the lock that protects the write |
| object.c:953 | ObOpenObjectByName (`obj->is_exclusive`) | 7 | written on first open without a lock |
| object.c:1648 | ObRegisterObjectType (type list walk) | 3 | list walked without the type-list lock |

A fix needs one rule for `flags` and `is_permanent`/`is_exclusive`: either take `ob_ns_lock` around
every write and read, or make them atomic. That changes the directory insert and exclusive-open
semantics, so it needs its own failing tests and a review. Not done here.

## Triaged as not a product defect

- `object.c:204` `test_close` (19 reports): the test object type's close callback sets
  `body->close_pending` from several threads closing handles to the same object. The field is test
  state only, the value is always `true`, and no production type reads it. The dispatcher calls
  `ops.close` outside the handle-table lock by design. Benign for the product; the stress test is
  the only user of `nt_test_body`. A test-side atomic would silence it; not changed here.
- `stress_timer_test` (TSan rc=132): `ThreadSanitizer: CHECK failed: tsan_rtl_thread.cpp:307
  "tid < kMaxTid"`. TSan has a fixed limit on threads it has seen (about 8128), and this test
  creates and destroys threads faster than that allows. A tool limit, not a race report.

## Valgrind

Clean (no reports, rc=0) for `test_pe_parse`, `test_real_pe`, `test_except_pure`, `test_unwind_pure`,
`test_ldr_sched`, `test_k32_io_pure`, built without sanitizers. `test_except` reports one
`Syscall param msync(start) points to unaddressable byte(s)`: `decode_access_type` probes the
instruction at the faulting RIP with `msync`, and the test sets RIP to an unmapped address. The
probe is designed to fail on unmapped addresses (it returns ENOMEM), so this is intended, not a
defect. `test_runtime` is not run under valgrind because it replaces malloc.

## Static analysis and coverage

`tools/static_analyze.sh`: 35 informational diagnostics (mostly `-Wsign-conversion` in `except.c`,
`ldr.c`, `ntstatus.h`), 42 units not buildable under the three profiles. The script does not gate CI,
and nothing was changed for it.

`tools/c_coverage.sh` (COVERAGE=1 builds of kernel-host check, host test and ntdll-host): all three
suites pass; per-file line coverage in the report, for example `io.c` 76%, `mm.c` 85%, `iocp.c` 55%,
`nt_services.c` 23%.

## Not run in this pass

Anything not listed under "Test results" is recorded in `docs/status/LINUX-TEST-MATRIX.md` with its
status. The TSan stress run over all programs was terminated (SIGTERM) at about 33 minutes, after
seed 2 and part of seed 3 (handle, object, sync and token finished seed 3; section, namespace, sd,
registry and timer did not). Those seed-3 results are missing. stress_registry_test was rerun after LNX-10 with all three seeds.
