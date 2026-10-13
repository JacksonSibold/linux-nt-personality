# Linux test matrix

Recorded 2026-10-13 on branch `haiku/guesttests`. Findings and fixes are in
[docs/review/linux-sanitizers.md](../review/linux-sanitizers.md) (ids `LNX-nn`).

- Host: Debian 12 (bookworm) x86_64 guest under QEMU/TCG, 4 vCPU (this run used at most 2), 3.9 GB RAM.
- Toolchain: gcc 12.2.0, glibc 2.36, GNU make 4.3, Python 3.11.2, binutils 2.40,
  `gcc-mingw-w64-x86-64` 12.2 (for real PE fixtures), libasan/libubsan/libtsan (gcc 12), valgrind.
- Source: `~/guesttests` in the guest, synced from the worktree with `tools/linux-env/sync-repo.sh`.
- Durations are wall-clock times recorded by the runs (TCG, so slower than native).

## Matrix

| Suite | Command | Sanitizer | Result | Duration | Notes |
|---|---|---|---|---|---|
| unit (Python) | `python3 -m unittest discover tests/unit` | none | PASS: 665 run, 1 skip (WSL) | 189 s | LNX-01 (1 error before fix) |
| unit (Python), Windows host | `python -m unittest discover tests/unit` (WinPython 3.13) | none | PASS: 665 run, 1 skip (WSL) | 40 s | LNX-02 (1 failure before fix) |
| host | `make -C tests/host test` | ASan+UBSan (auto) | PASS: pe_parse 48/48, real_pe 10/10 | ~50 s | real PE built by Debian mingw; LNX-03 |
| ntdll-host | `make -C tests/ntdll-host test` | ASan+UBSan (auto) | PASS: 13 suites | ~60 s | LNX-04, LNX-05, LNX-06 |
| ntdll-host | `make -C tests/ntdll-host SANITIZE=0 test` | none | PASS: 13 suites | ~15 s | |
| fuzz smoke | `make -C tests/fuzz smoke` | none | PASS: 0 violations | 19 s | 2000 iterations x 7 files |
| fuzz smoke | `make -C tests/fuzz SANITIZE=1 smoke` | ASan+UBSan | PASS: 0 violations, no reports | 40 s | |
| fuzz large | `make -C tests/fuzz SANITIZE=1 ITERS=100000 SEED=1 run` | ASan+UBSan | PASS: 0 violations, 0 faults | 200 s | 7 file-runs |
| fuzz large | same, SEED=2 | ASan+UBSan | PASS: 0 violations, 0 faults | 200 s | |
| fuzz large | same, SEED=3 | ASan+UBSan | PASS: 0 violations, 0 faults | 290 s | |
| kernel-host | `make -C tests/kernel-host check` (KHOST_OS=linux) | none | PASS: 22 programs, rc=0 | 333 s | LNX-07, LNX-08, LNX-09 |
| kernel-host build | `make -C tests/kernel-host SANITIZE=thread all` | TSan | build OK | 338 s | |
| kernel-host stress | `make SANITIZE=thread stress` (seeds 1-3) | TSan | partial: seed 1 and 2 rc=66 (race reports) in 7 of 9 programs, rc=0 in sync, token, sd; seed 3 partial; terminated at ~1980 s | ~1980 s | object.c races open (O-1); timer hits TSan thread limit; see review doc |
| kernel-host stress | `stress_registry_test` seeds 1-3, after LNX-10 | TSan | no registry.c reports; object.c:997-998 still reported | ~ | LNX-10 |
| kernel-host | `make -C tests/kernel-host SANITIZE=address check` | ASan+UBSan | not run | | |
| static analysis | `sh tools/static_analyze.sh` | (analyzer, -fanalyzer) | rc=0 (informational): 35 diagnostics, 42 units UNBUILDABLE under the three profiles | 297 s | mostly -Wsign-conversion in except.c, ldr.c, ntstatus.h; not gated |
| coverage | `sh tools/c_coverage.sh` (kernel-host check, host test, ntdll-host) | gcov | rc=0: all three suites pass under COVERAGE=1 | 556 s | e.g. io.c 76%, mm.c 85%, iocp.c 55%, nt_services.c 23%; uncovered list in the report |
| valgrind | `valgrind -q --leak-check=full` on host test binaries | valgrind | test_pe_parse, test_real_pe: clean | | `test_except`: one report, the intended msync probe on an unmapped RIP (false positive) |
| valgrind | `valgrind -q` on ntdll-host test binaries (except_pure, unwind_pure, ldr_sched, k32_io_pure) | valgrind | clean (rc=0 each) | | test_runtime skipped: it replaces malloc; test_except as above |

## Notes

- **Red runs.** Each LNX fix records the failing run that preceded it in its commit message:
  LNX-01 (unit error), LNX-02 (Windows unit failure), LNX-03 (5 real-PE failures), LNX-04 (30
  compile errors), LNX-05 (ASan SEGV), LNX-06 (ASan calloc abort), LNX-07 (generator "Illegal
  option"), LNX-08 (compiler diagnostic), LNX-09 (sync_test rc=1 x3), LNX-10 (TSan reports).
- **Stress is partial.** The TSan stress run was terminated (SIGTERM) at about 1980 s, after seed 2
  and part of seed 3. Seed-3 results for section, namespace, sd, registry and timer are missing.
  Re-run with `make SANITIZE=thread STRESS_SEEDS=3 stress` to complete them.
- **Not stress failures.** `stress_timer_test` exits 132 under TSan because TSan's thread-id limit
  (about 8128 threads) is exceeded. This is a TSan limit, not a product defect.
- **Race evidence.** Open TSan race sites (not fixed in this pass): see the review doc, section
  "Open: real races reported by TSan".
