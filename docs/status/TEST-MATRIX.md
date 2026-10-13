# Test matrix

Recorded by `tools/run_all_checks.sh` (see [DEVELOPMENT.md](../DEVELOPMENT.md#running-every-local-check-at-once)).

- Date: 2026-10-12
- Commit: fd26716 (clean tree at run time)
- Host: Windows 11, Git Bash, MSYS2 UCRT64 gcc 16.2.0, WinPython 3.13.12
- Result: PASS=19 FAIL=1 KNOWN-FAIL=0 SKIP=0 (exit 1 because of the syntax row)

```
CHECK                         STATUS      NOTE
unit                          PASS
check_repo                    PASS
check_doc_links               PASS
syscall_tables                PASS
doc_counts                    PASS
provenance                    PASS
lint                          PASS
syntax                        FAIL        exit 5 (5 files, see below)
host                          PASS
kernel-host:object_test       PASS
kernel-host:security_test     PASS
kernel-host:registry_test     PASS
kernel-host:i2_test           PASS
kernel-host:io_test           PASS
kernel-host:mm_test           PASS
kernel-host:sync_test         PASS
kernel-host:proc_thread_test  PASS
kernel-host:handle_edge_test  PASS
kernel-host:registry_ops_test PASS
kernel-host:mman_test         PASS
```

## Notes

- **syntax (FAIL).** Five files fail `gcc -fsyntax-only`: `src/kernel/fs/binfmt_pe.c`,
  `src/kernel/fs/binfmt_pe_test.c`, `arch/x86/nt/dispatch.c`,
  `arch/x86/nt/kernel_stubs.c` and `src/kernel/nt/dispatch.c`. This matches the
  "Remaining failures (5)" list in `tests/syntax/README.md`. They are model and
  shim gaps, not stale expectations, so they are left as FAIL. The syntax log
  reports 32 passed, 5 failed, 452 warnings.
- **kernel-host.** Every module passed in this run. The earlier KNOWN-FAIL rows
  for `io_test` and `mm_test` did not reproduce, and the `known-fail:` list in
  `tests/kernel-host/README.md` was not used for any row. `handle_edge_test`,
  `registry_ops_test` and `mman_test` are now part of the run.
- `proc_thread_test` is in the kernel-host run; the earlier note that it is
  build-only no longer applies to this matrix.
