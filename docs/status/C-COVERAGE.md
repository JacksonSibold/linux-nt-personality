# C line coverage (host suites)

Measured 2026-10-12 with `sh tools/c_coverage.sh` on branch `haiku/statusrefresh` at commit `fd26716`. Method: gcov over the `-O0` `--coverage` builds (`COVERAGE=1`) of `tests/kernel-host`, `tests/host` and `tests/ntdll-host`. Counts are per source file, summed over the object files that compile it. Test drivers (`*_test.c`, `*tests.c`, `repro_mem_vad.c`) are excluded.

## Suite results in this run

| Suite | Result |
|---|---|
| kernel-host (`make check`) | pass, all programs rc=0 |
| host (`make test`) | pass |
| ntdll-host | all nine test binaries pass: `test_ntstatus_to_dos`, `test_unwind`, `test_unwind_pure`, `test_except`, `test_except_pure`, `test_runtime`, `test_win32_policy`, `test_msvcrt_policy`, `test_ldr_pure` |

No binary was refused by the host (no rc=126 this run). The earlier run's three refused binaries (`test_except`, `test_win32_policy`, `test_ldr_pure`) now execute and are measured. `src/ntdll/kernel32.c`, `user32.c`, `advapi32.c`, `kernelbase.c`, `msvcrt.c`, `ntdll_main.c` and `ldr.c` are MinGW-only or are not compiled by any host suite, so they show as "not built".

## Per-file line coverage

Source: the per-file table printed by `tools/c_coverage.sh` in this run.

| File | Suite | Lines executed | Lines | Line coverage |
|---|---|---:|---:|---:|
| `src/kernel/nt/dispatch.c` | not built | - | - | - |
| `src/kernel/nt/handle.c` | kernel-host | 273 | 315 | 86.67% |
| `src/kernel/nt/io.c` | kernel-host | 538 | 912 | 58.99% |
| `src/kernel/nt/iocp.c` | kernel-host | 124 | 271 | 45.76% |
| `src/kernel/nt/kshim.c` | kernel-host | 197 | 255 | 77.25% |
| `src/kernel/nt/mm.c` | kernel-host | 612 | 761 | 80.42% |
| `src/kernel/nt/nt_obj.c` | kernel-host | 294 | 441 | 66.67% |
| `src/kernel/nt/nt_services.c` | kernel-host | 0 | 81 | 0.00% |
| `src/kernel/nt/ntstatus_kernel.c` | kernel-host | 23 | 26 | 88.46% |
| `src/kernel/nt/object.c` | kernel-host | 661 | 770 | 85.84% |
| `src/kernel/nt/proc_thread.c` | kernel-host | 346 | 546 | 63.37% |
| `src/kernel/nt/registry.c` | kernel-host | 661 | 936 | 70.62% |
| `src/kernel/nt/security.c` | kernel-host | 741 | 1048 | 70.71% |
| `src/kernel/nt/sync.c` | kernel-host | 643 | 813 | 79.09% |
| `src/kernel/nt/weak.c` | not built | - | - | - |
| `src/loader/pe_parse.c` | host | 730 | 751 | 97.20% |
| `src/loader/pe_prot.c` | not built | - | - | - |
| `src/ntdll/advapi32.c` | not built | - | - | - |
| `src/ntdll/except.c` | ntdll-host | 57 | 114 | 50.00% |
| `src/ntdll/kernel32.c` | not built | - | - | - |
| `src/ntdll/kernelbase.c` | not built | - | - | - |
| `src/ntdll/ldr.c` | not built | - | - | - |
| `src/ntdll/msvcrt.c` | not built | - | - | - |
| `src/ntdll/ntdll_main.c` | not built | - | - | - |
| `src/ntdll/ntdll_runtime.c` | ntdll-host | 47 | 131 | 35.88% |
| `src/ntdll/ntstatus_to_dos.c` | ntdll-host | 10 | 22 | 45.45% |
| `src/ntdll/unwind.c` | ntdll-host | 247 | 325 | 76.00% |
| `src/ntdll/user32.c` | not built | - | - | - |

## Uncovered functions (0 lines executed)

| # | Function | File | Executable lines |
|---:|---|---|---:|
| 1 | `nt_svc_NtRemoveIoCompletionEx` | `src/kernel/nt/iocp.c` | 66 |
| 2 | `undo_consume_locked` | `src/kernel/nt/sync.c` | 26 |
| 3 | `ntio_set_rename` | `src/kernel/nt/io.c` | 25 |
| 4 | `ntio_query_all` | `src/kernel/nt/io.c` | 23 |
| 5 | `wait_on_eprocess` | `src/kernel/nt/nt_obj.c` | 23 |
| 6 | `ntmm_add_region` | `src/kernel/nt/mm.c` | 21 |
| 7 | `nt_svc_NtQueryMutant` | `src/kernel/nt/nt_services.c` | 21 |
| 8 | `RtlUnwindEx` | `src/ntdll/unwind.c` | 20 |
| 9 | `nt_svc_NtQueryEvent` | `src/kernel/nt/nt_services.c` | 19 |
| 10 | `nt_svc_NtQuerySemaphore` | `src/kernel/nt/nt_services.c` | 19 |
| 11 | `nt_svc_NtQuerySystemInformation` | `src/kernel/nt/proc_thread.c` | 17 |
| 12 | `ntio_handle_lookup_any` | `src/kernel/nt/io.c` | 16 |
| 13 | `ntio_query_basic` | `src/kernel/nt/io.c` | 16 |
| 14 | `nt_svc_NtFlushBuffersFile` | `src/kernel/nt/io.c` | 15 |
| 15 | `ntio_duplicate_file_handle` | `src/kernel/nt/io.c` | 15 |

`not built` means no host suite compiles the file. `nt_services.c` is compiled by kernel-host but no test calls its service wrappers, so it reports 0%.
