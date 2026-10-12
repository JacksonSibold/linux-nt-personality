# C line coverage (host suites)

Measured 2026-10-12 with `sh tools/c_coverage.sh` on branch `haiku/gcov`,
after the new tests `handle_edge_test`, `registry_ops_test` and the loader
accessor cases in `test_pe_parse` were added. Method: gcov over the `-O0`
`--coverage` builds (`COVERAGE=1`) of `tests/kernel-host`, `tests/host` and
`tests/ntdll-host`. Counts are per source file, summed over the object files
that compile it. Test drivers (`*_test.c`, `*tests.c`, `repro_mem_vad.c`) are
excluded.

## Suite results in this run

| Suite | Result |
|---|---|
| kernel-host (`make check`) | pass, 11 programs rc=0 (includes `handle_edge_test`, `registry_ops_test`) |
| host (`make test`) | pass, 41/41 |
| ntdll-host | `test_ntstatus_to_dos`, `test_unwind`, `test_runtime` pass. `test_except`, `test_win32_policy`, `test_ldr_pure` **not run**: the OS refused to execute them (bash `Permission denied`, rc=126). |

The three refused binaries are a host execution-policy block, not a test
failure, and they were not worked around. `test_except` therefore reports 0%
for `except.c`, and the `win32_policy.h` and `ldr_pure.h` headers are not
measured. `test_pe_bounds` is build-only on this host (see `docs/review/ntdll.md`,
MAIN-1).

## Per-file line coverage

| File | Suite | Lines executed | Lines | Line coverage |
|---|---|---:|---:|---:|
| `src/kernel/nt/dispatch.c` | not built | - | - | - |
| `src/kernel/nt/handle.c` | kernel-host | 268 | 310 | 86.45% |
| `src/kernel/nt/io.c` | kernel-host | 528 | 908 | 58.15% |
| `src/kernel/nt/iocp.c` | kernel-host | 124 | 271 | 45.76% |
| `src/kernel/nt/kshim.c` | kernel-host | 195 | 255 | 76.47% |
| `src/kernel/nt/mm.c` | kernel-host | 607 | 756 | 80.29% |
| `src/kernel/nt/nt_obj.c` | kernel-host | 294 | 441 | 66.67% |
| `src/kernel/nt/nt_services.c` | kernel-host | 0 | 81 | 0.00% |
| `src/kernel/nt/ntstatus_kernel.c` | kernel-host | 23 | 26 | 88.46% |
| `src/kernel/nt/object.c` | kernel-host | 635 | 760 | 83.55% |
| `src/kernel/nt/proc_thread.c` | kernel-host | 346 | 546 | 63.37% |
| `src/kernel/nt/registry.c` | kernel-host | 659 | 934 | 70.56% |
| `src/kernel/nt/security.c` | kernel-host | 740 | 1042 | 71.02% |
| `src/kernel/nt/sync.c` | kernel-host | 644 | 813 | 79.21% |
| `src/kernel/nt/weak.c` | not built | - | - | - |
| `src/loader/pe_parse.c` | host | 727 | 749 | 97.06% |
| `src/loader/pe_prot.c` | not built | - | - | - |
| `src/ntdll/advapi32.c` | not built | - | - | - |
| `src/ntdll/except.c` | ntdll-host | 0 | 190 | 0.00% |
| `src/ntdll/kernel32.c` | not built | - | - | - |
| `src/ntdll/kernelbase.c` | not built | - | - | - |
| `src/ntdll/ldr.c` | not built | - | - | - |
| `src/ntdll/msvcrt.c` | not built | - | - | - |
| `src/ntdll/ntdll_main.c` | not built | - | - | - |
| `src/ntdll/ntdll_runtime.c` | ntdll-host | 39 | 130 | 30.00% |
| `src/ntdll/ntstatus_to_dos.c` | ntdll-host | 10 | 22 | 45.45% |
| `src/ntdll/unwind.c` | ntdll-host | 35 | 365 | 9.59% |
| `src/ntdll/user32.c` | not built | - | - | - |

"not built" means no host suite compiles the file (kernel-only, Win32 shim,
or needs headers this host lacks; `src/kernel/nt/dispatch.c` and `weak.c` are
listed as unbuildable in `tests/kernel-host/README.md`). `pe_prot.c` is a
loader file that no host suite compiles yet.

## Top 15 uncovered functions by size

Size is the number of executable lines gcov counts in the function. Only
functions with 0 lines executed are listed.

| # | Function | File | Executable lines |
|---:|---|---|---:|
| 1 | `unwind_one_frame` | `src/ntdll/unwind.c` | 209 |
| 2 | `nt_svc_NtRemoveIoCompletionEx` | `src/kernel/nt/iocp.c` | 66 |
| 3 | `RtlDispatchException` | `src/ntdll/except.c` | 57 |
| 4 | `RtlUnwindExWrapper` | `src/ntdll/except.c` | 47 |
| 5 | `RtlVirtualUnwind` | `src/ntdll/unwind.c` | 33 |
| 6 | `undo_consume_locked` | `src/kernel/nt/sync.c` | 26 |
| 7 | `ntio_set_rename` | `src/kernel/nt/io.c` | 25 |
| 8 | `ntio_query_all` | `src/kernel/nt/io.c` | 23 |
| 9 | `wait_on_eprocess` | `src/kernel/nt/nt_obj.c` | 23 |
| 10 | `ntmm_add_region` | `src/kernel/nt/mm.c` | 21 |
| 11 | `nt_svc_NtQueryMutant` | `src/kernel/nt/nt_services.c` | 21 |
| 12 | `RtlUnwindEx` | `src/ntdll/unwind.c` | 20 |
| 13 | `nt_svc_NtQueryEvent` | `src/kernel/nt/nt_services.c` | 19 |
| 14 | `nt_svc_NtQuerySemaphore` | `src/kernel/nt/nt_services.c` | 19 |
| 15 | `build_chain` | `src/ntdll/unwind.c` | 19 |

Items 1, 4 to 5, 12 and 15 are unwinding code. `test_except` and the
unwind-execution paths need a run that the OS currently blocks, so their 0% is
partly an execution gap, not an absence of tests.

## Reproduce

```
sh tools/c_coverage.sh             # all three suites, then the report (kernel-host takes minutes)
sh tools/c_coverage.sh --no-run    # report from the existing counters
```

Raw gcov output is not kept; the script removes its temporary files.
